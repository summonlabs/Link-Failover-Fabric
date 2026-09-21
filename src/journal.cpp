// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/journal.hpp"

#include "detail/fsutil.hpp"
#include "lff/hash.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#endif

namespace lff {
namespace {

struct RecordTypeName {
    RecordType value;
    std::string_view name;
};

constexpr RecordTypeName kRecordTypeNames[] = {
    {RecordType::Boot, "Boot"},
    {RecordType::PolicyCommit, "PolicyCommit"},
    {RecordType::TopologyCommit, "TopologyCommit"},
    {RecordType::AttemptCreate, "AttemptCreate"},
    {RecordType::AttemptUpdate, "AttemptUpdate"},
    {RecordType::DecisionCommit, "DecisionCommit"},
    {RecordType::Fence, "Fence"},
    {RecordType::Completion, "Completion"},
    {RecordType::Rollback, "Rollback"},
    {RecordType::IdempotencyIndex, "IdempotencyIndex"},
    {RecordType::Checkpoint, "Checkpoint"},
    {RecordType::Shutdown, "Shutdown"},
};

void store_u16(std::uint8_t* out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void store_u32(std::uint8_t* out, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
    }
}

void store_u64(std::uint8_t* out, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
    }
}

std::uint16_t load_u16(const std::uint8_t* in, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset]) |
                                      (static_cast<std::uint16_t>(in[offset + 1]) << 8));
}

std::uint32_t load_u32(const std::uint8_t* in, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= (static_cast<std::uint32_t>(in[offset + i]) << (i * 8u));
    }
    return value;
}

std::uint64_t load_u64(const std::uint8_t* in, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= (static_cast<std::uint64_t>(in[offset + i]) << (i * 8u));
    }
    return value;
}

std::array<std::uint8_t, kJournalHeaderSize> build_journal_header(LineageSeq base_seq) {
    std::array<std::uint8_t, kJournalHeaderSize> header{};
    store_u32(header.data(), 0, kJournalMagic);
    store_u16(header.data(), 4, kJournalFormatVersion);
    store_u16(header.data(), 6, 0);
    store_u64(header.data(), 8, base_seq.value());
    store_u64(header.data(), 16, 0);
    store_u32(header.data(), 24, 0);
    store_u32(header.data(), 28, crc32c(header.data(), 28));
    return header;
}

Status parse_journal_header(const std::uint8_t* data, std::size_t size, JournalHeader& out) {
    if (size < kJournalHeaderSize) {
        return Status::failure(Outcome::Invalid, ReasonCode::TruncatedFrame,
                               "journal header is truncated");
    }
    if (load_u32(data, 0) != kJournalMagic) {
        return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                               "journal magic mismatch");
    }
    const std::uint32_t expected_crc = load_u32(data, 28);
    if (crc32c(data, 28) != expected_crc) {
        return Status::failure(Outcome::Corrupt, ReasonCode::ChecksumMismatch,
                               "journal header checksum mismatch");
    }
    const std::uint16_t version = load_u16(data, 4);
    if (version != kJournalFormatVersion) {
        Status status = Status::failure(Outcome::Unsupported,
                                        ReasonCode::UnsupportedFormatVersion,
                                        "unsupported journal format version");
        status.add(ReasonCode::InvalidArgument, std::to_string(version));
        return status;
    }
    if (load_u16(data, 6) != 0) {
        return Status::failure(Outcome::Invalid, ReasonCode::ReservedBitsSet,
                               "journal header flags must be zero");
    }
    if (load_u64(data, 16) != 0 || load_u32(data, 24) != 0) {
        return Status::failure(Outcome::Invalid, ReasonCode::ReservedBitsSet,
                               "journal header reserved fields must be zero");
    }
    JournalHeader header;
    header.format_version = version;
    header.flags = 0;
    header.base_seq = LineageSeq::from(load_u64(data, 8));
    out = header;
    return Status::success();
}

struct RecordHeaderFields {
    std::uint16_t version{0};
    RecordType type{RecordType::Boot};
    std::uint32_t payload_length{0};
    std::uint64_t seq{0};
    std::uint32_t payload_crc{0};
};

Status parse_record_header(const std::uint8_t* data, RecordHeaderFields& out) {
    if (load_u32(data, 0) != kRecordMagic) {
        return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                               "record magic mismatch");
    }
    const std::uint32_t expected_crc = load_u32(data, 28);
    if (crc32c(data, 28) != expected_crc) {
        return Status::failure(Outcome::Corrupt, ReasonCode::ChecksumMismatch,
                               "record header checksum mismatch");
    }
    const std::uint16_t version = load_u16(data, 4);
    if (version != kJournalFormatVersion) {
        Status status = Status::failure(Outcome::Unsupported,
                                        ReasonCode::UnsupportedFormatVersion,
                                        "unsupported record format version");
        status.add(ReasonCode::InvalidArgument, std::to_string(version));
        return status;
    }
    const std::uint16_t raw_type = load_u16(data, 6);
    if (raw_type >= kRecordTypeCount) {
        Status status = Status::failure(Outcome::Corrupt, ReasonCode::UnsupportedValue,
                                        "unknown record type");
        status.add(ReasonCode::InvalidArgument, std::to_string(raw_type));
        return status;
    }
    const std::uint32_t length = load_u32(data, 8);
    if (length > kMaxRecordPayload) {
        Status status = Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                                        "record payload exceeds the supported bound");
        status.add(ReasonCode::InvalidArgument, std::to_string(length));
        return status;
    }
    if (load_u32(data, 24) != 0) {
        return Status::failure(Outcome::Invalid, ReasonCode::ReservedBitsSet,
                               "record header reserved field must be zero");
    }
    out.version = version;
    out.type = static_cast<RecordType>(raw_type);
    out.payload_length = length;
    out.seq = load_u64(data, 12);
    out.payload_crc = load_u32(data, 20);
    return Status::success();
}

std::array<std::uint8_t, kRecordHeaderSize> build_record_header(RecordType type,
                                                                std::uint32_t payload_length,
                                                                LineageSeq seq,
                                                                std::uint32_t payload_crc) {
    std::array<std::uint8_t, kRecordHeaderSize> header{};
    store_u32(header.data(), 0, kRecordMagic);
    store_u16(header.data(), 4, kJournalFormatVersion);
    store_u16(header.data(), 6, static_cast<std::uint16_t>(type));
    store_u32(header.data(), 8, payload_length);
    store_u64(header.data(), 12, seq.value());
    store_u32(header.data(), 20, payload_crc);
    store_u32(header.data(), 24, 0);
    store_u32(header.data(), 28, crc32c(header.data(), 28));
    return header;
}

Status open_stream(const std::filesystem::path& path, const char* mode, std::FILE** out) {
    std::FILE* stream = nullptr;
#if defined(_WIN32)
    if (::fopen_s(&stream, path.string().c_str(), mode) != 0) {
        stream = nullptr;
    }
#else
    stream = std::fopen(path.string().c_str(), mode);
#endif
    if (stream == nullptr) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "cannot open journal file");
    }
    *out = stream;
    return Status::success();
}

Status sync_stream(std::FILE* stream) {
    if (std::fflush(stream) != 0) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError, "journal flush failed");
    }
#if defined(_WIN32)
    const int descriptor = ::_fileno(stream);
    if (descriptor < 0 || ::_commit(descriptor) != 0) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "journal commit failed");
    }
#else
    const int descriptor = ::fileno(stream);
    if (descriptor < 0 || ::fsync(descriptor) != 0) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "journal fsync failed");
    }
#endif
    return Status::success();
}

}  // namespace

std::string_view record_type_name(RecordType value) noexcept {
    for (const RecordTypeName& entry : kRecordTypeNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedRecordType";
}

bool record_type_parse(std::string_view text, RecordType& out) noexcept {
    for (const RecordTypeName& entry : kRecordTypeNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// JournalWriter
// ---------------------------------------------------------------------------
struct JournalWriter::Impl {
    std::FILE* stream{nullptr};
    std::filesystem::path path;
    LineageSeq last_seq;
    bool failed{false};
    std::uint64_t bytes_written{0};
};

JournalWriter::~JournalWriter() {
    close();
}

JournalWriter::JournalWriter(JournalWriter&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

Result<JournalWriter> JournalWriter::open(const std::filesystem::path& path, LineageSeq base_seq,
                                          LineageSeq last_seq, bool create_new) {
    if (base_seq.value() != 0 && last_seq.value() < base_seq.value()) {
        return Result<JournalWriter>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::SequenceRegression,
                            "journal last sequence precedes its base sequence"));
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error) && !error;
    if (create_new && exists) {
        const Status removed = detail::remove_file(path);
        if (!removed.ok()) {
            return Result<JournalWriter>::failure(removed);
        }
    }
    if (!create_new) {
        if (!exists) {
            return Result<JournalWriter>::failure(
                Status::failure(Outcome::NotFound, ReasonCode::UnknownLink,
                                "journal file does not exist"));
        }
        JournalHeader header;
        const Result<JournalHeader> existing = JournalReader::read_header(path);
        if (!existing.ok()) {
            return Result<JournalWriter>::failure(existing.status());
        }
        header = existing.value();
        if (header.base_seq.value() != base_seq.value()) {
            Status status = Status::failure(Outcome::Conflict, ReasonCode::SequenceRegression,
                                            "journal base sequence does not match the snapshot");
            status.add(ReasonCode::InvalidArgument, std::to_string(header.base_seq.value()));
            return Result<JournalWriter>::failure(status);
        }
    }

    std::FILE* stream = nullptr;
    const Status opened = open_stream(path, create_new ? "wb" : "r+b", &stream);
    if (!opened.ok()) {
        return Result<JournalWriter>::failure(opened);
    }
    if (create_new) {
        const std::array<std::uint8_t, kJournalHeaderSize> header = build_journal_header(base_seq);
        if (std::fwrite(header.data(), 1, header.size(), stream) != header.size()) {
            std::fclose(stream);
            return Result<JournalWriter>::failure(
                Status::failure(Outcome::IoError, ReasonCode::InternalError,
                                "cannot write journal header"));
        }
        Status synced = sync_stream(stream);
        if (!synced.ok()) {
            std::fclose(stream);
            return Result<JournalWriter>::failure(synced);
        }
    } else if (std::fseek(stream, 0, SEEK_END) != 0) {
        std::fclose(stream);
        return Result<JournalWriter>::failure(
            Status::failure(Outcome::IoError, ReasonCode::InternalError,
                            "cannot seek the journal to its end"));
    }

    auto* impl = new Impl();
    impl->stream = stream;
    impl->path = path;
    impl->last_seq = create_new ? base_seq : last_seq;
    impl->bytes_written = create_new ? kJournalHeaderSize : 0;
    JournalWriter writer;
    writer.impl_ = impl;
    return Result<JournalWriter>::success(std::move(writer));
}

Status JournalWriter::append(RecordType type, LineageSeq seq,
                             const std::vector<std::uint8_t>& payload) {
    if (impl_ == nullptr || impl_->stream == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed,
                               "journal writer is not open");
    }
    if (impl_->failed) {
        return Status::failure(Outcome::IoError, ReasonCode::StickyDecodeFailure,
                               "journal writer has already failed");
    }
    if (payload.size() > kMaxRecordPayload) {
        return Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                               "record payload exceeds the supported bound");
    }
    if (!(impl_->last_seq < seq)) {
        impl_->failed = true;
        return Status::failure(Outcome::Conflict, ReasonCode::SequenceRegression,
                               "record sequence does not advance the lineage");
    }

    const std::uint32_t payload_crc = crc32c(payload.data(), payload.size());
    const std::array<std::uint8_t, kRecordHeaderSize> header =
        build_record_header(type, static_cast<std::uint32_t>(payload.size()), seq, payload_crc);

    if (std::fwrite(header.data(), 1, header.size(), impl_->stream) != header.size()) {
        impl_->failed = true;
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "cannot write record header");
    }
    if (!payload.empty() &&
        std::fwrite(payload.data(), 1, payload.size(), impl_->stream) != payload.size()) {
        impl_->failed = true;
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "cannot write record payload");
    }
    Status synced = sync_stream(impl_->stream);
    if (!synced.ok()) {
        impl_->failed = true;
        return synced;
    }
    impl_->last_seq = seq;
    impl_->bytes_written += header.size() + payload.size();
    return Status::success();
}

Status JournalWriter::flush() {
    if (impl_ == nullptr || impl_->stream == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed,
                               "journal writer is not open");
    }
    return sync_stream(impl_->stream);
}

bool JournalWriter::is_open() const noexcept {
    return impl_ != nullptr && impl_->stream != nullptr;
}

bool JournalWriter::failed() const noexcept {
    return impl_ == nullptr || impl_->failed;
}

LineageSeq JournalWriter::last_seq() const noexcept {
    return impl_ == nullptr ? LineageSeq{} : impl_->last_seq;
}

const std::filesystem::path& JournalWriter::path() const noexcept {
    static const std::filesystem::path kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->path;
}

std::uint64_t JournalWriter::bytes_written() const noexcept {
    return impl_ == nullptr ? 0 : impl_->bytes_written;
}

void JournalWriter::close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->stream != nullptr) {
        std::fflush(impl_->stream);
        std::fclose(impl_->stream);
        impl_->stream = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

// ---------------------------------------------------------------------------
// JournalReader
// ---------------------------------------------------------------------------
Result<JournalHeader> JournalReader::read_header(const std::filesystem::path& path) {
    const Result<std::vector<std::uint8_t>> bytes =
        detail::read_prefix(path, static_cast<std::uint64_t>(kJournalHeaderSize));
    if (!bytes.ok()) {
        return Result<JournalHeader>::failure(bytes.status());
    }
    if (bytes.value().size() < kJournalHeaderSize) {
        return Result<JournalHeader>::failure(
            Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                            "journal file is shorter than its header"));
    }
    JournalHeader header;
    const Status parsed = parse_journal_header(bytes.value().data(), bytes.value().size(), header);
    if (!parsed.ok()) {
        return Result<JournalHeader>::failure(parsed);
    }
    return Result<JournalHeader>::success(header);
}

Result<JournalOpenResult> JournalReader::replay(const std::filesystem::path& path,
                                                bool repair_torn_tail,
                                                const JournalReplayHooks& hooks) {
    if (!detail::path_exists(path)) {
        return Result<JournalOpenResult>::failure(
            Status::failure(Outcome::NotFound, ReasonCode::UnknownLink,
                            "journal file does not exist"));
    }
    const Result<std::vector<std::uint8_t>> bytes = detail::read_file(path, kMaxJournalBytes);
    if (!bytes.ok()) {
        return Result<JournalOpenResult>::failure(bytes.status());
    }
    const std::vector<std::uint8_t>& data = bytes.value();

    JournalOpenResult result;
    if (data.size() < kJournalHeaderSize) {
        if (!repair_torn_tail) {
            return Result<JournalOpenResult>::failure(
                Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                                "journal header is incomplete"));
        }
        const Status truncated = detail::truncate_file(path, 0);
        if (!truncated.ok()) {
            return Result<JournalOpenResult>::failure(truncated);
        }
        result.torn_tail_recovered = true;
        result.truncated_to_empty = true;
        result.bytes_discarded = data.size();
        return Result<JournalOpenResult>::success(result);
    }

    JournalHeader header;
    const Status header_status = parse_journal_header(data.data(), data.size(), header);
    if (!header_status.ok()) {
        return Result<JournalOpenResult>::failure(header_status);
    }
    result.header = header;
    result.last_seq = header.base_seq;

    std::size_t offset = kJournalHeaderSize;
    std::uint64_t records = 0;
    bool torn = false;
    std::size_t torn_offset = offset;

    while (offset < data.size()) {
        if (data.size() - offset < kRecordHeaderSize) {
            torn = true;
            torn_offset = offset;
            break;
        }
        RecordHeaderFields fields;
        const Status record_status = parse_record_header(data.data() + offset, fields);
        if (!record_status.ok()) {
            return Result<JournalOpenResult>::failure(record_status);
        }
        const std::size_t payload_offset = offset + kRecordHeaderSize;
        const std::size_t payload_size = static_cast<std::size_t>(fields.payload_length);
        if (data.size() - payload_offset < payload_size) {
            torn = true;
            torn_offset = offset;
            break;
        }
        if (crc32c(data.data() + payload_offset, payload_size) != fields.payload_crc) {
            Status status = Status::failure(Outcome::Corrupt, ReasonCode::ChecksumMismatch,
                                            "record payload checksum mismatch");
            status.add(ReasonCode::CorruptRecord, std::to_string(fields.seq));
            return Result<JournalOpenResult>::failure(status);
        }
        if (!(result.last_seq < LineageSeq::from(fields.seq))) {
            Status status = Status::failure(Outcome::Corrupt, ReasonCode::SequenceRegression,
                                            "record sequence does not advance the lineage");
            status.add(ReasonCode::CorruptRecord, std::to_string(fields.seq));
            return Result<JournalOpenResult>::failure(status);
        }
        RecordView view;
        view.type = fields.type;
        view.seq = LineageSeq::from(fields.seq);
        view.payload_length = fields.payload_length;
        view.payload = data.data() + payload_offset;
        if (hooks.on_record) {
            const Status applied = hooks.on_record(view);
            if (!applied.ok()) {
                return Result<JournalOpenResult>::failure(applied);
            }
        }
        result.last_seq = view.seq;
        ++records;
        offset = payload_offset + payload_size;
    }

    result.records_replayed = records;

    if (torn) {
        const std::uint64_t discarded = static_cast<std::uint64_t>(data.size() - torn_offset);
        if (!repair_torn_tail) {
            Status status = Status::failure(Outcome::Corrupt, ReasonCode::TornTailRecovered,
                                            "journal ends in a torn record");
            status.add(ReasonCode::TruncatedFrame, std::to_string(discarded));
            return Result<JournalOpenResult>::failure(status);
        }
        const Status truncated = detail::truncate_file(path, torn_offset);
        if (!truncated.ok()) {
            return Result<JournalOpenResult>::failure(truncated);
        }
        result.torn_tail_recovered = true;
        result.bytes_discarded = discarded;
    }

    return Result<JournalOpenResult>::success(result);
}

}  // namespace lff
