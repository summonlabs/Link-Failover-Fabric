// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/store.hpp"

#include "detail/fault.hpp"
#include "detail/fsutil.hpp"
#include "lff/bytes.hpp"
#include "lff/hash.hpp"
#include "lff/version.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace lff {
namespace {

constexpr std::uint32_t kSnapshotMagic = 0x31504E53u;  // "SNP1"
constexpr std::size_t kSnapshotHeaderSize = 32;
constexpr const char* kSnapshotFileName = "lineage.snap";
constexpr const char* kJournalFileName = "lineage.jrnl";
constexpr const char* kStagingFileName = "lineage.jrnl.new";

std::vector<std::uint8_t> frame_snapshot(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out(kSnapshotHeaderSize + payload.size(), 0);
    auto put_u16 = [&out](std::size_t offset, std::uint16_t value) {
        out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
        out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    };
    auto put_u32 = [&out](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) {
            out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
        }
    };
    put_u32(0, kSnapshotMagic);
    put_u16(4, kSnapshotFormatVersion);
    put_u16(6, 0);
    put_u32(8, static_cast<std::uint32_t>(payload.size()));
    put_u32(12, crc32c(payload.data(), payload.size()));
    if (!payload.empty()) {
        std::memcpy(out.data() + kSnapshotHeaderSize, payload.data(), payload.size());
    }
    put_u32(28, crc32c(out.data(), 28));
    return out;
}

Status unframe_snapshot(const std::vector<std::uint8_t>& framed,
                        std::vector<std::uint8_t>& payload) {
    if (framed.size() < kSnapshotHeaderSize) {
        return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                               "snapshot is shorter than its header");
    }
    auto load_u16 = [&framed](std::size_t offset) {
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(framed[offset]) |
                                          (static_cast<std::uint16_t>(framed[offset + 1]) << 8));
    };
    auto load_u32 = [&framed](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |= (static_cast<std::uint32_t>(framed[offset + i]) << (i * 8u));
        }
        return value;
    };
    if (load_u32(0) != kSnapshotMagic) {
        return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                               "snapshot magic mismatch");
    }
    if (crc32c(framed.data(), 28) != load_u32(28)) {
        return Status::failure(Outcome::Corrupt, ReasonCode::ChecksumMismatch,
                               "snapshot header checksum mismatch");
    }
    const std::uint16_t version = load_u16(4);
    if (version != kSnapshotFormatVersion) {
        Status status = Status::failure(Outcome::Unsupported,
                                        ReasonCode::UnsupportedFormatVersion,
                                        "unsupported snapshot format version");
        status.add(ReasonCode::InvalidArgument, std::to_string(version));
        return status;
    }
    if (load_u16(6) != 0) {
        return Status::failure(Outcome::Invalid, ReasonCode::ReservedBitsSet,
                               "snapshot flags must be zero");
    }
    const std::uint32_t length = load_u32(8);
    if (length > kMaxSnapshotBytes) {
        return Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                               "snapshot payload exceeds the supported bound");
    }
    if (framed.size() != kSnapshotHeaderSize + static_cast<std::size_t>(length)) {
        return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                               "snapshot length does not match its declared payload");
    }
    payload.assign(framed.begin() + static_cast<std::ptrdiff_t>(kSnapshotHeaderSize), framed.end());
    if (crc32c(payload.data(), payload.size()) != load_u32(12)) {
        return Status::failure(Outcome::Corrupt, ReasonCode::ChecksumMismatch,
                               "snapshot payload checksum mismatch");
    }
    return Status::success();
}

bool attempt_less(const AttemptRecord& a, const AttemptRecord& b) noexcept {
    if (a.subject < b.subject) {
        return true;
    }
    if (b.subject < a.subject) {
        return false;
    }
    return a.attempt_seq < b.attempt_seq;
}

bool fence_less(const FenceRecord& a, const FenceRecord& b) noexcept {
    return a.seq < b.seq;
}

}  // namespace

// ---------------------------------------------------------------------------
// Payload encoders
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encode_attempt_payload(const AttemptRecord& record) {
    Writer writer;
    record.encode(writer);
    return writer.take();
}

bool decode_attempt_payload(const std::uint8_t* data, std::size_t size, AttemptRecord& out) {
    Reader reader(data, size);
    AttemptRecord value;
    if (!AttemptRecord::decode(reader, value)) {
        return false;
    }
    if (!reader.at_end()) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> encode_policy_payload(const FailoverPolicy& policy) {
    Writer writer;
    policy.encode(writer);
    return writer.take();
}

bool decode_policy_payload(const std::uint8_t* data, std::size_t size, FailoverPolicy& out) {
    Reader reader(data, size);
    FailoverPolicy value;
    if (!FailoverPolicy::decode(reader, value)) {
        return false;
    }
    if (!reader.at_end()) {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::uint8_t> encode_topology_payload(const TopologySnapshot& topology) {
    Writer writer;
    topology.encode(writer);
    return writer.take();
}

bool decode_topology_payload(const std::uint8_t* data, std::size_t size, TopologySnapshot& out) {
    Reader reader(data, size);
    TopologySnapshot value;
    if (!TopologySnapshot::decode(reader, value)) {
        return false;
    }
    if (!reader.at_end()) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> encode_fence_payload(const FenceRecord& record) {
    Writer writer;
    record.encode(writer);
    return writer.take();
}

bool decode_fence_payload(const std::uint8_t* data, std::size_t size, FenceRecord& out) {
    Reader reader(data, size);
    FenceRecord value;
    if (!FenceRecord::decode(reader, value)) {
        return false;
    }
    if (!reader.at_end()) {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::uint8_t> encode_decision_payload(const FailoverDecision& decision) {
    Writer writer;
    decision.encode(writer);
    return writer.take();
}

bool decode_decision_payload(const std::uint8_t* data, std::size_t size, FailoverDecision& out) {
    Reader reader(data, size);
    FailoverDecision value;
    if (!FailoverDecision::decode(reader, value)) {
        return false;
    }
    if (!reader.at_end()) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> encode_idempotency_payload(const IdempotencyEntry& entry) {
    Writer writer;
    writer.u16(kIdentityFormatVersion);
    writer.digest(entry.key);
    writer.digest(entry.decision_id);
    writer.u16(static_cast<std::uint16_t>(entry.kind));
    writer.u16(static_cast<std::uint16_t>(entry.assertion));
    writer.u16(static_cast<std::uint16_t>(entry.outcome));
    writer.str(entry.subject.fabric.value());
    writer.str(entry.subject.link.value());
    writer.u64(entry.subject_generation.value());
    writer.bool8(entry.has_replacement);
    if (entry.has_replacement) {
        writer.str(entry.replacement.fabric.value());
        writer.str(entry.replacement.link.value());
        writer.u64(entry.replacement_generation.value());
    }
    writer.u64(entry.attempt_seq.value());
    writer.u64(entry.seq.value());
    writer.u32(static_cast<std::uint32_t>(entry.reasons.size()));
    for (const Reason& reason : entry.reasons) {
        writer.u16(static_cast<std::uint16_t>(reason.code));
        writer.str(reason.detail, static_cast<std::uint32_t>(kMaxReasonDetail));
    }
    return writer.take();
}

bool decode_idempotency_payload(const std::uint8_t* data, std::size_t size,
                                IdempotencyEntry& out) {
    Reader reader(data, size);
    const std::uint16_t format_version = reader.u16();
    IdempotencyEntry value;
    value.key = reader.digest();
    value.decision_id = reader.digest();
    const std::uint16_t kind = reader.u16();
    const std::uint16_t assertion = reader.u16();
    const std::uint16_t outcome = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const bool has_replacement = reader.bool8();
    std::string replacement_fabric;
    std::string replacement_link;
    std::uint64_t replacement_generation = 0;
    if (has_replacement) {
        replacement_fabric = reader.str();
        replacement_link = reader.str();
        replacement_generation = reader.u64();
    }
    const std::uint64_t attempt_seq = reader.u64();
    const std::uint64_t seq = reader.u64();
    const std::uint32_t reason_count = reader.u32();
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    if (kind >= kDecisionKindCount || assertion >= kAssertionCount || outcome >= kOutcomeCount ||
        reason_count > kMaxReasons) {
        return false;
    }
    const Result<FabricName> fabric_name = FabricName::parse(fabric);
    const Result<LinkName> link_name = LinkName::parse(link);
    if (!fabric_name.ok() || !link_name.ok()) {
        return false;
    }
    if (has_replacement) {
        const Result<FabricName> replacement_fabric_name = FabricName::parse(replacement_fabric);
        const Result<LinkName> replacement_link_name = LinkName::parse(replacement_link);
        if (!replacement_fabric_name.ok() || !replacement_link_name.ok()) {
            return false;
        }
        value.has_replacement = true;
        value.replacement.fabric = replacement_fabric_name.value();
        value.replacement.link = replacement_link_name.value();
        value.replacement_generation = LinkGeneration::from(replacement_generation);
    }
    value.kind = static_cast<DecisionKind>(kind);
    value.assertion = static_cast<Assertion>(assertion);
    value.outcome = static_cast<Outcome>(outcome);
    value.subject.fabric = fabric_name.value();
    value.subject.link = link_name.value();
    value.subject_generation = LinkGeneration::from(generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.seq = LineageSeq::from(seq);
    for (std::uint32_t i = 0; i < reason_count; ++i) {
        const std::uint16_t code = reader.u16();
        const std::string detail = reader.str(static_cast<std::uint32_t>(kMaxReasonDetail));
        if (!reader.ok() || code >= kReasonCodeCount) {
            return false;
        }
        value.reasons.emplace_back(static_cast<ReasonCode>(code), detail);
    }
    if (!reader.at_end()) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> encode_boot_payload(const BootPayload& payload) {
    Writer writer;
    writer.u16(kIdentityFormatVersion);
    writer.u64(payload.epoch.value());
    writer.incarnation(payload.coordinator);
    writer.u64(payload.boot_count);
    writer.u64(payload.epoch_advances);
    writer.str(payload.toolchain, 64);
    writer.str(payload.architecture, 64);
    return writer.take();
}

bool decode_boot_payload(const std::uint8_t* data, std::size_t size, BootPayload& out) {
    Reader reader(data, size);
    const std::uint16_t format_version = reader.u16();
    BootPayload value;
    value.epoch = Epoch::from(reader.u64());
    value.coordinator = reader.incarnation();
    value.boot_count = reader.u64();
    value.epoch_advances = reader.u64();
    value.toolchain = reader.str(64);
    value.architecture = reader.str(64);
    if (!reader.ok() || format_version != kIdentityFormatVersion || !reader.at_end()) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> encode_checkpoint_payload(const CheckpointPayload& payload) {
    Writer writer;
    writer.u16(kIdentityFormatVersion);
    writer.u64(payload.covers_seq.value());
    writer.u64(payload.record_count);
    writer.u64(payload.snapshot_bytes);
    return writer.take();
}

bool decode_checkpoint_payload(const std::uint8_t* data, std::size_t size,
                               CheckpointPayload& out) {
    Reader reader(data, size);
    const std::uint16_t format_version = reader.u16();
    CheckpointPayload value;
    value.covers_seq = LineageSeq::from(reader.u64());
    value.record_count = reader.u64();
    value.snapshot_bytes = reader.u64();
    if (!reader.ok() || format_version != kIdentityFormatVersion || !reader.at_end()) {
        return false;
    }
    out = value;
    return true;
}

// ---------------------------------------------------------------------------
// LineageStore
// ---------------------------------------------------------------------------
struct LineageStore::Impl {
    StoreOptions options;
    DurableState state;
    StoreStatus store_status;
    JournalWriter writer;
    std::filesystem::path snapshot_path;
    std::filesystem::path journal_path;
    std::filesystem::path staging_path;
    bool failed{false};
    std::uint64_t records_since_snapshot{0};

    // --- retention ---------------------------------------------------------
    void evict_attempts() {
        if (state.attempts.size() <= options.max_attempts) {
            return;
        }
        std::sort(state.attempts.begin(), state.attempts.end(), attempt_less);
        while (state.attempts.size() > options.max_attempts) {
            const auto victim = std::find_if(state.attempts.begin(), state.attempts.end(),
                                             [](const AttemptRecord& record) {
                                                 return attempt_phase_is_terminal(record.phase);
                                             });
            if (victim == state.attempts.end()) {
                break;
            }
            state.attempts.erase(victim);
            ++store_status.attempts_evicted;
        }
    }

    void evict_fences() {
        if (state.fences.size() <= options.max_fences) {
            return;
        }
        std::sort(state.fences.begin(), state.fences.end(), fence_less);
        const std::size_t excess = state.fences.size() - options.max_fences;
        state.fences.erase(state.fences.begin(),
                           state.fences.begin() + static_cast<std::ptrdiff_t>(excess));
        store_status.fences_evicted += excess;
    }

    void evict_idempotency() {
        if (state.idempotency.size() <= options.max_idempotency) {
            return;
        }
        const std::size_t excess = state.idempotency.size() - options.max_idempotency;
        state.idempotency.erase(state.idempotency.begin(),
                                state.idempotency.begin() + static_cast<std::ptrdiff_t>(excess));
        store_status.idempotency_evicted += excess;
    }

    void upsert_attempt(const AttemptRecord& record) {
        for (AttemptRecord& existing : state.attempts) {
            if (existing.subject == record.subject && existing.attempt_seq == record.attempt_seq) {
                existing = record;
                return;
            }
        }
        state.attempts.push_back(record);
    }

    Status apply_record(const RecordView& view) {
        switch (view.type) {
            case RecordType::Boot: {
                BootPayload payload;
                if (!decode_boot_payload(view.payload, view.payload_size(), payload)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                           "boot record payload is malformed");
                }
                state.epoch = payload.epoch;
                state.coordinator = payload.coordinator;
                state.boot_count = payload.boot_count;
                state.epoch_advances = payload.epoch_advances;
                return Status::success();
            }
            case RecordType::PolicyCommit: {
                FailoverPolicy policy;
                if (!decode_policy_payload(view.payload, view.payload_size(), policy)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                           "policy record payload is malformed");
                }
                state.policy = policy;
                return Status::success();
            }
            case RecordType::TopologyCommit: {
                TopologySnapshot topology;
                if (!decode_topology_payload(view.payload, view.payload_size(), topology)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                           "topology record payload is malformed");
                }
                state.topology = std::move(topology);
                return Status::success();
            }
            case RecordType::AttemptCreate:
            case RecordType::AttemptUpdate:
            case RecordType::Completion:
            case RecordType::Rollback: {
                AttemptRecord record;
                if (!decode_attempt_payload(view.payload, view.payload_size(), record)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                           "attempt record payload is malformed");
                }
                if (view.type == RecordType::AttemptCreate &&
                    record.phase != AttemptPhase::Evaluated &&
                    record.phase != AttemptPhase::Refused &&
                    record.phase != AttemptPhase::Indeterminate) {
                    return Status::failure(
                        Outcome::Corrupt, ReasonCode::CorruptRecord,
                        "AttemptCreate record carries a phase that is not an entry phase");
                }
                if (view.type == RecordType::Completion && record.phase != AttemptPhase::Committed) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                                           "Completion record does not carry a committed attempt");
                }
                if (view.type == RecordType::Rollback && record.phase != AttemptPhase::RolledBack) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                                           "Rollback record does not carry a rolled-back attempt");
                }
                if (!(record.create_seq <= view.seq)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::SequenceRegression,
                                           "attempt record precedes its creation sequence");
                }
                upsert_attempt(record);
                evict_attempts();
                return Status::success();
            }
            case RecordType::Fence: {
                FenceRecord record;
                if (!decode_fence_payload(view.payload, view.payload_size(), record)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                           "fence record payload is malformed");
                }
                if (record.seq.value() != view.seq.value()) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::CorruptRecord,
                                           "fence record sequence does not match its lineage position");
                }
                state.fences.push_back(record);
                evict_fences();
                return Status::success();
            }
            case RecordType::DecisionCommit: {
                FailoverDecision decision;
                if (!decode_decision_payload(view.payload, view.payload_size(), decision)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                           "decision record payload is malformed");
                }
                // Audit-only: a decision's durable consequences are carried by
                // the attempt records and the idempotency index.
                return Status::success();
            }
            case RecordType::IdempotencyIndex: {
                IdempotencyEntry entry;
                if (!decode_idempotency_payload(view.payload, view.payload_size(), entry)) {
                    return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                           "idempotency record payload is malformed");
                }
                state.idempotency.push_back(std::move(entry));
                evict_idempotency();
                return Status::success();
            }
            case RecordType::Checkpoint:
            case RecordType::Shutdown:
            default:
                // Bookkeeping records carry no lineage state of their own.
                return Status::success();
        }
    }

    Status serialise(DurableState& out_state, std::vector<std::uint8_t>& out) const {
        Writer sink;
        sink.u16(kIdentityFormatVersion);
        sink.str(out_state.fabric.value());
        out_state.policy.encode(sink);
        out_state.topology.encode(sink);
        sink.u64(out_state.epoch.value());
        sink.incarnation(out_state.coordinator);
        sink.u64(out_state.last_seq.value());
        sink.u64(out_state.boot_count);
        sink.u64(out_state.record_count);
        sink.u64(out_state.epoch_advances);
        // All three element counts precede their arrays so that a reader can
        // bound every element before it materialises any of them.
        sink.u32(static_cast<std::uint32_t>(out_state.attempts.size()));
        sink.u32(static_cast<std::uint32_t>(out_state.fences.size()));
        sink.u32(static_cast<std::uint32_t>(out_state.idempotency.size()));
        for (const AttemptRecord& record : out_state.attempts) {
            record.encode(sink);
        }
        for (const FenceRecord& record : out_state.fences) {
            record.encode(sink);
        }
        for (const IdempotencyEntry& entry : out_state.idempotency) {
            sink.u16(1);
            const std::vector<std::uint8_t> payload = encode_idempotency_payload(entry);
            if (payload.size() > kMaxIdempotencyDecisionBytes) {
                return Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                                       "idempotency entry exceeds the snapshot bound");
            }
            sink.blob(payload, static_cast<std::uint32_t>(kMaxIdempotencyDecisionBytes));
        }
        if (!sink.ok()) {
            return Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                                   "snapshot exceeds the supported bound");
        }
        out = sink.take();
        return Status::success();
    }

    Status deserialise(const std::vector<std::uint8_t>& payload, DurableState& out_state) const {
        Reader reader(payload.data(), payload.size());
        const std::uint16_t format_version = reader.u16();
        DurableState value;
        const std::string fabric = reader.str();
        FailoverPolicy policy;
        TopologySnapshot topology;
        if (!FailoverPolicy::decode(reader, policy)) {
            return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                   "snapshot policy is malformed");
        }
        if (!TopologySnapshot::decode(reader, topology)) {
            return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                   "snapshot topology is malformed");
        }
        value.epoch = Epoch::from(reader.u64());
        value.coordinator = reader.incarnation();
        value.last_seq = LineageSeq::from(reader.u64());
        value.boot_count = reader.u64();
        value.record_count = reader.u64();
        value.epoch_advances = reader.u64();
        const std::uint32_t attempt_count = reader.u32();
        const std::uint32_t fence_count = reader.u32();
        const std::uint32_t idempotency_count = reader.u32();
        if (!reader.ok()) {
            return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                   "snapshot header fields are malformed");
        }
        if (format_version != kIdentityFormatVersion) {
            return Status::failure(Outcome::Unsupported, ReasonCode::UnsupportedFormatVersion,
                                   "unsupported snapshot identity format version");
        }
        if (attempt_count > 1000000u || fence_count > 1000000u || idempotency_count > 1000000u) {
            return Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                                   "snapshot declares an impossible element count");
        }
        const Result<FabricName> fabric_name = FabricName::parse(fabric);
        if (!fabric_name.ok()) {
            return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                   "snapshot fabric name is malformed");
        }
        value.fabric = fabric_name.value();
        value.policy = policy;
        value.topology = std::move(topology);
        value.attempts.reserve(attempt_count);
        for (std::uint32_t i = 0; i < attempt_count; ++i) {
            AttemptRecord record;
            if (!AttemptRecord::decode(reader, record)) {
                return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                       "snapshot attempt record is malformed");
            }
            value.attempts.push_back(std::move(record));
        }
        value.fences.reserve(fence_count);
        for (std::uint32_t i = 0; i < fence_count; ++i) {
            FenceRecord record;
            if (!FenceRecord::decode(reader, record)) {
                return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                       "snapshot fence record is malformed");
            }
            value.fences.push_back(record);
        }
        value.idempotency.reserve(idempotency_count);
        for (std::uint32_t i = 0; i < idempotency_count; ++i) {
            const std::uint16_t entry_version = reader.u16();
            const std::span<const std::uint8_t> blob =
                reader.blob(static_cast<std::uint32_t>(kMaxIdempotencyDecisionBytes));
            if (!reader.ok() || entry_version != 1) {
                return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                       "snapshot idempotency entry is malformed");
            }
            IdempotencyEntry entry;
            if (!decode_idempotency_payload(blob.data(), blob.size(), entry)) {
                return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding,
                                       "snapshot idempotency payload is malformed");
            }
            value.idempotency.push_back(std::move(entry));
        }
        if (!reader.at_end()) {
            return Status::failure(Outcome::Corrupt, ReasonCode::TrailingBytes,
                                   "snapshot contains trailing bytes");
        }
        out_state = std::move(value);
        return Status::success();
    }
};

LineageStore::~LineageStore() {
    close();
}

LineageStore::LineageStore(LineageStore&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

LineageStore& LineageStore::operator=(LineageStore&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

Result<LineageStore> LineageStore::open(const StoreOptions& options) {
    if (options.directory.empty()) {
        return Result<LineageStore>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "state directory must not be empty"));
    }
    const Status policy_status = options.policy.validate();
    if (!policy_status.ok()) {
        return Result<LineageStore>::failure(policy_status);
    }
    const Status topology_status = options.topology.validate();
    if (!topology_status.ok()) {
        return Result<LineageStore>::failure(topology_status);
    }
    const Status directory = detail::ensure_directory(options.directory);
    if (!directory.ok()) {
        return Result<LineageStore>::failure(directory);
    }

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->snapshot_path = options.directory / kSnapshotFileName;
    impl->journal_path = options.directory / kJournalFileName;
    impl->staging_path = options.directory / kStagingFileName;
    impl->state.fabric = options.fabric;
    impl->state.policy = options.policy;
    impl->state.topology = options.topology;

    // A staging journal left behind by a crash between the snapshot write and
    // the rotation is never a source of truth.
    if (detail::path_exists(impl->staging_path)) {
        const Status removed = detail::remove_file(impl->staging_path);
        if (!removed.ok()) {
            return Result<LineageStore>::failure(removed);
        }
    }

    if (detail::path_exists(impl->snapshot_path)) {
        const Result<std::vector<std::uint8_t>> framed =
            detail::read_file(impl->snapshot_path, kMaxSnapshotBytes);
        if (!framed.ok()) {
            return Result<LineageStore>::failure(framed.status());
        }
        std::vector<std::uint8_t> payload;
        const Status unframed = unframe_snapshot(framed.value(), payload);
        if (!unframed.ok()) {
            return Result<LineageStore>::failure(unframed);
        }
        DurableState restored;
        const Status decoded = impl->deserialise(payload, restored);
        if (!decoded.ok()) {
            return Result<LineageStore>::failure(decoded);
        }
        if (!(restored.fabric == options.fabric)) {
            Status status = Status::failure(Outcome::Conflict, ReasonCode::UnknownFabric,
                                            "durable lineage belongs to a different fabric");
            status.add(ReasonCode::InvalidArgument, restored.fabric.value());
            return Result<LineageStore>::failure(status);
        }
        const Status restored_policy = restored.policy.validate();
        if (!restored_policy.ok()) {
            return Result<LineageStore>::failure(restored_policy);
        }
        const Status restored_topology = restored.topology.validate();
        if (!restored_topology.ok()) {
            return Result<LineageStore>::failure(restored_topology);
        }
        impl->state = std::move(restored);
        impl->store_status.snapshot_loaded = true;
        impl->store_status.snapshot_bytes = static_cast<std::uint64_t>(framed.value().size());
    }

    const LineageSeq covered = impl->state.last_seq;
    JournalReplayHooks hooks;
    hooks.on_record = [&impl, covered](const RecordView& view) -> Status {
        // Records at or below the snapshot watermark were already folded into
        // the snapshot and are skipped rather than re-applied.
        if (view.seq.value() <= covered.value()) {
            return Status::success();
        }
        return impl->apply_record(view);
    };

    const Result<JournalOpenResult> replayed = JournalReader::replay(
        impl->journal_path, options.repair_torn_tail, hooks);

    LineageSeq journal_last = covered;
    LineageSeq journal_base = covered;
    bool create_journal = false;

    if (replayed.ok()) {
        impl->store_status.journal_loaded = true;
        impl->store_status.torn_tail_recovered = replayed.value().torn_tail_recovered;
        impl->store_status.truncated_to_empty = replayed.value().truncated_to_empty;
        impl->store_status.records_replayed = replayed.value().records_replayed;
        impl->store_status.bytes_discarded = replayed.value().bytes_discarded;
        journal_last = replayed.value().last_seq;
        journal_base = replayed.value().header.base_seq;
        if (journal_last < covered) {
            journal_last = covered;
        }
        if (journal_base < covered) {
            journal_base = covered;
        }
        create_journal = replayed.value().truncated_to_empty;
    } else if (replayed.status().any_of(Outcome::NotFound)) {
        create_journal = true;
    } else {
        return Result<LineageStore>::failure(replayed.status());
    }

    impl->state.last_seq = journal_last;

    Result<JournalWriter> writer = JournalWriter::open(impl->journal_path, journal_base,
                                                       impl->state.last_seq, create_journal);
    if (!writer.ok()) {
        return Result<LineageStore>::failure(writer.status());
    }
    impl->writer = std::move(writer.value());
    impl->records_since_snapshot = 0;

    LineageStore store;
    store.impl_ = impl.release();
    return Result<LineageStore>::success(std::move(store));
}

const DurableState& LineageStore::state() const noexcept {
    static const DurableState kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->state;
}

DurableState& LineageStore::mutable_state() noexcept {
    return impl_->state;
}

const StoreStatus& LineageStore::status() const noexcept {
    static const StoreStatus kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->store_status;
}

std::uint64_t LineageStore::records_since_snapshot() const noexcept {
    return impl_ == nullptr ? 0 : impl_->records_since_snapshot;
}

bool LineageStore::failed() const noexcept {
    return impl_ == nullptr || impl_->failed;
}

std::filesystem::path LineageStore::directory() const {
    return impl_ == nullptr ? std::filesystem::path() : impl_->options.directory;
}

Status LineageStore::commit(RecordType type, const std::vector<std::uint8_t>& payload,
                            const std::function<void(LineageSeq)>& apply) {
    if (impl_ == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed,
                               "lineage store is not open");
    }
    if (impl_->failed) {
        return Status::failure(Outcome::IoError, ReasonCode::StickyDecodeFailure,
                               "lineage store has already failed");
    }
    LineageSeq next;
    if (!impl_->state.last_seq.next(next)) {
        impl_->failed = true;
        return Status::failure(Outcome::Exhausted, ReasonCode::CheckedArithmeticOverflow,
                               "lineage sequence space is exhausted");
    }
    Status appended = impl_->writer.append(type, next, payload);
    if (!appended.ok()) {
        impl_->failed = true;
        return appended;
    }
    if (apply) {
        apply(next);
    }
    impl_->state.last_seq = next;
    ++impl_->state.record_count;
    ++impl_->records_since_snapshot;
    return Status::success();
}

Result<LineageSeq> LineageStore::checkpoint() {
    if (impl_ == nullptr) {
        return Result<LineageSeq>::failure(Status::failure(Outcome::Closed,
                                                           ReasonCode::ConnectionClosed,
                                                           "lineage store is not open"));
    }
    if (impl_->failed) {
        return Result<LineageSeq>::failure(
            Status::failure(Outcome::IoError, ReasonCode::StickyDecodeFailure,
                            "lineage store has already failed"));
    }
    std::vector<std::uint8_t> payload;
    const Status serialised = impl_->serialise(impl_->state, payload);
    if (!serialised.ok()) {
        return Result<LineageSeq>::failure(serialised);
    }
    const std::vector<std::uint8_t> framed = frame_snapshot(payload);
    const Status written = detail::write_file_atomic(impl_->snapshot_path, framed);
    if (!written.ok()) {
        return Result<LineageSeq>::failure(written);
    }
    impl_->store_status.snapshot_loaded = true;
    impl_->store_status.snapshot_bytes = static_cast<std::uint64_t>(framed.size());
    ++impl_->store_status.snapshots_written;

    // A crash here leaves the previous journal segment plus a snapshot that
    // covers a prefix of it; replay skips the covered prefix.
    detail::hit_crash_point(detail::CrashPoint::DuringCheckpoint);

    const LineageSeq covers = impl_->state.last_seq;
    CheckpointPayload checkpoint_payload;
    checkpoint_payload.covers_seq = covers;
    checkpoint_payload.record_count = impl_->state.record_count;
    checkpoint_payload.snapshot_bytes = static_cast<std::uint64_t>(framed.size());

    impl_->writer.close();
    Result<JournalWriter> staging = JournalWriter::open(impl_->staging_path, covers, covers, true);
    if (!staging.ok()) {
        impl_->failed = true;
        return Result<LineageSeq>::failure(staging.status());
    }
    staging.value().close();
    std::error_code error;
    std::filesystem::rename(impl_->staging_path, impl_->journal_path, error);
    if (error) {
        impl_->failed = true;
        return Result<LineageSeq>::failure(
            Status::failure(Outcome::IoError, ReasonCode::InternalError,
                            "journal rotation failed"));
    }
    Result<JournalWriter> reopened = JournalWriter::open(impl_->journal_path, covers, covers, false);
    if (!reopened.ok()) {
        impl_->failed = true;
        return Result<LineageSeq>::failure(reopened.status());
    }
    impl_->writer = std::move(reopened.value());
    impl_->records_since_snapshot = 0;

    // Record the compaction in the fresh segment so an auditor can see where the
    // snapshot boundary sits.
    Status appended = commit(RecordType::Checkpoint, encode_checkpoint_payload(checkpoint_payload),
                                   [](LineageSeq) {});
    if (!appended.ok()) {
        return Result<LineageSeq>::failure(appended);
    }
    return Result<LineageSeq>::success(covers);
}

Result<std::size_t> LineageStore::fence_restart_authority(Epoch new_epoch,
                                                          Incarnation new_incarnation) {
    if (impl_ == nullptr) {
        return Result<std::size_t>::failure(Status::failure(Outcome::Closed,
                                                            ReasonCode::ConnectionClosed,
                                                            "lineage store is not open"));
    }
    std::size_t interrupted = 0;
    // Copy first: commit() mutates the attempt vector.
    const std::vector<AttemptRecord> snapshot = impl_->state.attempts;
    for (const AttemptRecord& record : snapshot) {
        if (attempt_phase_is_terminal(record.phase)) {
            continue;
        }
        const bool held_authority = attempt_phase_holds_authority(record.phase);
        AttemptRecord updated = record;
        updated.phase = AttemptPhase::Interrupted;
        updated.reasons.emplace_back(interruption_reason(record.phase),
                                     "process terminated while the attempt was in flight");
        if (updated.reasons.size() > kMaxAttemptReasons) {
            updated.reasons.erase(updated.reasons.begin());
        }
        const Status applied = commit(RecordType::AttemptUpdate, encode_attempt_payload(updated),
                                      [impl = impl_, updated](LineageSeq seq) {
                                          AttemptRecord stored = updated;
                                          stored.update_seq = seq;
                                          impl->upsert_attempt(stored);
                                      });
        if (!applied.ok()) {
            return Result<std::size_t>::failure(applied);
        }
        ++interrupted;

        FenceRecord fence;
        fence.kind = FenceKind::PreRestartAuthority;
        fence.subject = record.subject;
        fence.subject_generation = record.subject_generation;
        fence.attempt_seq = record.attempt_seq;
        fence.epoch = new_epoch;
        fence.coordinator = new_incarnation;
        fence.detail = held_authority ? "held failover authority across a restart"
                                      : "was in flight across a restart";
        const Status fenced = commit(RecordType::Fence, encode_fence_payload(fence),
                                     [impl = impl_, &fence](LineageSeq seq) {
                                         FenceRecord stored = fence;
                                         stored.seq = seq;
                                         impl->state.fences.push_back(stored);
                                         impl->evict_fences();
                                     });
        if (!fenced.ok()) {
            return Result<std::size_t>::failure(fenced);
        }
    }
    return Result<std::size_t>::success(interrupted);
}

void LineageStore::close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->writer.close();
    delete impl_;
    impl_ = nullptr;
}

void LineageStore::note_attempt(const AttemptRecord& record) {
    if (impl_ == nullptr) {
        return;
    }
    impl_->upsert_attempt(record);
    impl_->evict_attempts();
}

void LineageStore::note_fence(FenceRecord record) {
    if (impl_ == nullptr) {
        return;
    }
    impl_->state.fences.push_back(std::move(record));
    impl_->evict_fences();
}

void LineageStore::note_idempotency(IdempotencyEntry entry) {
    if (impl_ == nullptr) {
        return;
    }
    impl_->state.idempotency.push_back(std::move(entry));
    impl_->evict_idempotency();
}

void LineageStore::note_epoch(Epoch epoch, Incarnation coordinator, std::uint64_t boot_count,
                              std::uint64_t epoch_advances) {
    if (impl_ == nullptr) {
        return;
    }
    impl_->state.epoch = epoch;
    impl_->state.coordinator = coordinator;
    impl_->state.boot_count = boot_count;
    impl_->state.epoch_advances = epoch_advances;
}

const AttemptRecord* LineageStore::latest_attempt(const LinkKey& subject,
                                                  LinkGeneration generation) const {
    if (impl_ == nullptr) {
        return nullptr;
    }
    const AttemptRecord* found = nullptr;
    for (const AttemptRecord& record : impl_->state.attempts) {
        if (record.subject != subject || record.subject_generation != generation) {
            continue;
        }
        if (found == nullptr || found->attempt_seq < record.attempt_seq) {
            found = &record;
        }
    }
    return found;
}

std::size_t LineageStore::count_attempts(const LinkKey& subject,
                                         LinkGeneration generation) const {
    if (impl_ == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const AttemptRecord& record : impl_->state.attempts) {
        if (record.subject == subject && record.subject_generation == generation) {
            ++count;
        }
    }
    return count;
}

const AttemptRecord* LineageStore::find_attempt(const LinkKey& subject, AttemptSeq seq) const {
    if (impl_ == nullptr) {
        return nullptr;
    }
    for (const AttemptRecord& record : impl_->state.attempts) {
        if (record.subject == subject && record.attempt_seq == seq) {
            return &record;
        }
    }
    return nullptr;
}

const AttemptRecord* LineageStore::active_authority(const LinkKey& subject,
                                                    LinkGeneration generation) const {
    if (impl_ == nullptr) {
        return nullptr;
    }
    const AttemptRecord* found = nullptr;
    for (const AttemptRecord& record : impl_->state.attempts) {
        if (record.subject != subject || record.subject_generation != generation) {
            continue;
        }
        if (!attempt_phase_holds_authority(record.phase)) {
            continue;
        }
        if (record.authority.epoch.value() != impl_->state.epoch.value() ||
            record.authority.coordinator != impl_->state.coordinator) {
            // Authority minted by a previous incarnation never counts as current.
            continue;
        }
        if (found == nullptr || found->attempt_seq < record.attempt_seq) {
            found = &record;
        }
    }
    return found;
}

const IdempotencyEntry* LineageStore::find_idempotency(const Digest& key) const {
    if (impl_ == nullptr || key.is_zero()) {
        return nullptr;
    }
    for (const IdempotencyEntry& entry : impl_->state.idempotency) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace lff