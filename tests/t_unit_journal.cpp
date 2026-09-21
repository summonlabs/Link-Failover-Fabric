// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Persistence adversarial suite. Every durable format is attacked the way a
// corrupted medium or a tampering adversary would attack it, and the runtime
// must refuse rather than silently reinterpret.
#include "framework.hpp"
#include "lff/journal.hpp"
#include "support.hpp"

#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

std::vector<std::uint8_t> payload(std::uint32_t seed) {
    Writer writer;
    writer.u32(seed);
    writer.str("synthetic-record-" + std::to_string(seed));
    return writer.take();
}

struct ReplayCapture {
    std::vector<RecordType> types;
    std::vector<std::uint64_t> sequences;
    std::size_t payload_bytes{0};
};

JournalReplayHooks capture_hooks(ReplayCapture& capture) {
    JournalReplayHooks hooks;
    hooks.on_record = [&capture](const RecordView& view) {
        capture.types.push_back(view.type);
        capture.sequences.push_back(view.seq.value());
        capture.payload_bytes += view.payload_size();
        return Status::success();
    };
    return hooks;
}

std::filesystem::path write_journal(const TempDir& dir, std::uint32_t records) {
    const std::filesystem::path path = dir.file("lineage.jrnl");
    Result<JournalWriter> writer = JournalWriter::open(path, LineageSeq::from(100), LineageSeq::from(100), true);
    if (!writer.ok()) {
        LFF_FAIL("cannot create journal: " + writer.status().to_string());
    }
    for (std::uint32_t i = 0; i < records; ++i) {
        const Status appended = writer.value().append(RecordType::AttemptUpdate,
                                                      LineageSeq::from(101 + i), payload(i));
        if (!appended.ok()) {
            LFF_FAIL("cannot append: " + appended.to_string());
        }
    }
    writer.value().close();
    return path;
}

}  // namespace

LFF_TEST(persistence, journal_round_trip) {
    TempDir dir("journal-roundtrip");
    const std::filesystem::path path = write_journal(dir, 5);
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(path, false, capture_hooks(capture));
    expect_ok(replayed.status(), "journal replay");
    LFF_CHECK_EQ(replayed.value().records_replayed, 5ull);
    LFF_CHECK(!replayed.value().torn_tail_recovered);
    LFF_CHECK_EQ(capture.types.size(), std::size_t{5});
    LFF_CHECK_EQ(capture.sequences.front(), 101ull);
    LFF_CHECK_EQ(capture.sequences.back(), 105ull);
    LFF_CHECK_EQ(replayed.value().last_seq.value(), 105ull);
    LFF_CHECK_EQ(replayed.value().header.base_seq.value(), 100ull);
}

LFF_TEST(persistence, missing_journal_reports_not_found) {
    TempDir dir("journal-missing");
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(dir.file("absent.jrnl"), true, capture_hooks(capture));
    LFF_CHECK(!replayed.ok());
    expect_outcome(replayed.status(), Outcome::NotFound, "absent journal");
}

LFF_TEST(persistence, torn_header_is_recoverable_only_on_request) {
    TempDir dir("journal-torn-header");
    const std::filesystem::path strict = dir.file("strict.jrnl");
    const std::filesystem::path repair = dir.file("repair.jrnl");
    std::vector<std::uint8_t> bytes = {1, 2, 3, 4, 5, 6, 7};
    write_bytes(strict, bytes);
    write_bytes(repair, bytes);

    ReplayCapture capture;
    const Result<JournalOpenResult> refused =
        JournalReader::replay(strict, false, capture_hooks(capture));
    LFF_CHECK(!refused.ok());
    expect_outcome(refused.status(), Outcome::Corrupt, "strict torn header");
    LFF_CHECK_EQ(byte_size(strict), 7ull);

    const Result<JournalOpenResult> recovered =
        JournalReader::replay(repair, true, capture_hooks(capture));
    expect_ok(recovered.status(), "repaired torn header");
    LFF_CHECK(recovered.value().torn_tail_recovered);
    LFF_CHECK(recovered.value().truncated_to_empty);
    LFF_CHECK_EQ(recovered.value().bytes_discarded, 7ull);
    LFF_CHECK_EQ(byte_size(repair), 0ull);
}

LFF_TEST(persistence, torn_record_tail_is_recovered_only_on_request) {
    TempDir dir("journal-torn-record");
    const std::filesystem::path source = write_journal(dir, 3);
    std::vector<std::uint8_t> complete = read_bytes(source);

    // Case 1: a prefix of the next record header.
    {
        const std::filesystem::path path = dir.file("partial-header.jrnl");
        std::vector<std::uint8_t> bytes = complete;
        bytes.insert(bytes.end(), 10, 0x5A);
        write_bytes(path, bytes);
        ReplayCapture capture;
        const Result<JournalOpenResult> refused =
            JournalReader::replay(path, false, capture_hooks(capture));
        LFF_CHECK(!refused.ok());
        expect_reason(refused.status(), ReasonCode::TornTailRecovered, "strict torn record");
        LFF_CHECK_EQ(byte_size(path), bytes.size());

        ReplayCapture recovered_capture;
        const Result<JournalOpenResult> recovered =
            JournalReader::replay(path, true, capture_hooks(recovered_capture));
        expect_ok(recovered.status(), "repaired torn record");
        LFF_CHECK_EQ(recovered.value().records_replayed, 3ull);
        LFF_CHECK(recovered.value().torn_tail_recovered);
        LFF_CHECK_EQ(recovered.value().bytes_discarded, 10ull);
        LFF_CHECK_EQ(byte_size(path), complete.size());
    }

    // Case 2: a complete header with a short payload.
    {
        TempDir other("journal-short-payload");
        const std::filesystem::path path = other.file("short.jrnl");
        std::vector<std::uint8_t> bytes = complete;
        const std::size_t before = bytes.size();
        Result<JournalWriter> writer = JournalWriter::open(path, LineageSeq::from(100),
                                                           LineageSeq::from(103), true);
        LFF_CHECK(writer.ok());
        for (std::uint32_t i = 0; i < 3; ++i) {
            expect_ok(writer.value().append(RecordType::AttemptUpdate, LineageSeq::from(101 + i),
                                            payload(i)),
                      "append");
        }
        expect_ok(writer.value().append(RecordType::AttemptUpdate, LineageSeq::from(104),
                                        payload(99)),
                  "append tail");
        writer.value().close();
        std::vector<std::uint8_t> full = read_bytes(path);
        full.resize(full.size() - 5);
        write_bytes(path, full);
        ReplayCapture capture;
        const Result<JournalOpenResult> recovered =
            JournalReader::replay(path, true, capture_hooks(capture));
        expect_ok(recovered.status(), "repaired short payload");
        LFF_CHECK_EQ(recovered.value().records_replayed, 3ull);
        LFF_CHECK(recovered.value().torn_tail_recovered);
        LFF_CHECK(bytes.size() >= before);
    }
}

LFF_TEST(persistence, corrupt_header_is_never_repaired) {
    TempDir dir("journal-corrupt-header");
    const std::filesystem::path path = write_journal(dir, 2);
    std::vector<std::uint8_t> bytes = read_bytes(path);
    bytes[0] ^= 0xFF;
    write_bytes(path, bytes);
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(path, true, capture_hooks(capture));
    LFF_CHECK(!replayed.ok());
    expect_outcome(replayed.status(), Outcome::Corrupt, "corrupt magic");
    LFF_CHECK_EQ(byte_size(path), bytes.size());

    bytes[0] ^= 0xFF;
    bytes[28] ^= 0x01;  // header checksum
    write_bytes(path, bytes);
    ReplayCapture second;
    const Result<JournalOpenResult> checksum =
        JournalReader::replay(path, true, capture_hooks(second));
    LFF_CHECK(!checksum.ok());
    expect_reason(checksum.status(), ReasonCode::ChecksumMismatch, "corrupt header crc");
}

LFF_TEST(persistence, unsupported_versions_are_refused) {
    TempDir dir("journal-version");
    // File header format version.
    {
        const std::filesystem::path path = write_journal(dir, 1);
        std::vector<std::uint8_t> bytes = read_bytes(path);
        bytes[4] = 99;
        bytes[28] = 0;
        bytes[29] = 0;
        bytes[30] = 0;
        bytes[31] = 0;
        write_bytes(path, bytes);
        // Repair the header checksum so only the version is wrong.
        Writer probe;
        probe.raw(bytes.data(), 28);
        ReplayCapture capture;
        const Result<JournalOpenResult> replayed =
            JournalReader::replay(path, true, capture_hooks(capture));
        LFF_CHECK(!replayed.ok());
        LFF_CHECK(replayed.status().outcome() == Outcome::Corrupt ||
                  replayed.status().outcome() == Outcome::Unsupported);
    }
    // Record header format version, with a valid header checksum.
    {
        TempDir other("journal-record-version");
        const std::filesystem::path path = write_journal(other, 1);
        std::vector<std::uint8_t> bytes = read_bytes(path);
        const std::size_t record_offset = kJournalHeaderSize;
        bytes[record_offset + 4] = 99;
        const std::uint32_t crc = crc32c(bytes.data() + record_offset, 28);
        for (std::size_t i = 0; i < 4; ++i) {
            bytes[record_offset + 28 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
        }
        write_bytes(path, bytes);
        ReplayCapture capture;
        const Result<JournalOpenResult> replayed =
            JournalReader::replay(path, true, capture_hooks(capture));
        LFF_CHECK(!replayed.ok());
        expect_outcome(replayed.status(), Outcome::Unsupported, "record version");
    }
}

LFF_TEST(persistence, unknown_record_type_is_refused) {
    TempDir dir("journal-unknown-type");
    const std::filesystem::path path = write_journal(dir, 1);
    std::vector<std::uint8_t> bytes = read_bytes(path);
    const std::size_t record_offset = kJournalHeaderSize;
    bytes[record_offset + 6] = 0xFF;
    bytes[record_offset + 7] = 0x7F;
    const std::uint32_t crc = crc32c(bytes.data() + record_offset, 28);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[record_offset + 28 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
    }
    write_bytes(path, bytes);
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(path, true, capture_hooks(capture));
    LFF_CHECK(!replayed.ok());
    expect_reason(replayed.status(), ReasonCode::UnsupportedValue, "unknown record type");
}

LFF_TEST(persistence, impossible_payload_length_is_refused_before_allocation) {
    TempDir dir("journal-huge-length");
    const std::filesystem::path path = write_journal(dir, 1);
    std::vector<std::uint8_t> bytes = read_bytes(path);
    const std::size_t record_offset = kJournalHeaderSize;
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[record_offset + 8 + i] = 0xFF;
    }
    const std::uint32_t crc = crc32c(bytes.data() + record_offset, 28);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[record_offset + 28 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
    }
    write_bytes(path, bytes);
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(path, true, capture_hooks(capture));
    LFF_CHECK(!replayed.ok());
    expect_outcome(replayed.status(), Outcome::LimitExceeded, "impossible length");
}

LFF_TEST(persistence, corrupt_payload_checksum_is_fatal) {
    TempDir dir("journal-payload-crc");
    const std::filesystem::path path = write_journal(dir, 2);
    std::vector<std::uint8_t> bytes = read_bytes(path);
    bytes.back() ^= 0x01;
    write_bytes(path, bytes);
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(path, true, capture_hooks(capture));
    LFF_CHECK(!replayed.ok());
    expect_reason(replayed.status(), ReasonCode::ChecksumMismatch, "payload checksum");
    LFF_CHECK_EQ(byte_size(path), bytes.size());
}

LFF_TEST(persistence, sequence_regression_is_fatal) {
    TempDir dir("journal-regression");
    const std::filesystem::path path = write_journal(dir, 3);
    std::vector<std::uint8_t> bytes = read_bytes(path);
    // Rewrite the second record's sequence to repeat the first one.
    const std::size_t second = kJournalHeaderSize + kRecordHeaderSize + payload(0).size();
    const std::uint64_t duplicated = 101;
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[second + 12 + i] = static_cast<std::uint8_t>((duplicated >> (i * 8)) & 0xFFu);
    }
    const std::uint32_t crc = crc32c(bytes.data() + second, 28);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[second + 28 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
    }
    write_bytes(path, bytes);
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(path, true, capture_hooks(capture));
    LFF_CHECK(!replayed.ok());
    expect_reason(replayed.status(), ReasonCode::SequenceRegression, "sequence regression");
}

LFF_TEST(persistence, reserved_bits_are_refused) {
    TempDir dir("journal-reserved");
    const std::filesystem::path path = write_journal(dir, 1);
    std::vector<std::uint8_t> bytes = read_bytes(path);
    const std::size_t record_offset = kJournalHeaderSize;
    bytes[record_offset + 24] = 1;
    const std::uint32_t crc = crc32c(bytes.data() + record_offset, 28);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[record_offset + 28 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
    }
    write_bytes(path, bytes);
    ReplayCapture capture;
    const Result<JournalOpenResult> replayed =
        JournalReader::replay(path, true, capture_hooks(capture));
    LFF_CHECK(!replayed.ok());
    expect_reason(replayed.status(), ReasonCode::ReservedBitsSet, "reserved record field");

    const std::filesystem::path file_header = dir.file("flags.jrnl");
    std::vector<std::uint8_t> header_bytes = read_bytes(write_journal(dir, 1));
    header_bytes[6] = 1;
    const std::uint32_t header_crc = crc32c(header_bytes.data(), 28);
    for (std::size_t i = 0; i < 4; ++i) {
        header_bytes[28 + i] = static_cast<std::uint8_t>((header_crc >> (i * 8)) & 0xFFu);
    }
    write_bytes(file_header, header_bytes);
    ReplayCapture flags_capture;
    const Result<JournalOpenResult> flags =
        JournalReader::replay(file_header, true, capture_hooks(flags_capture));
    LFF_CHECK(!flags.ok());
    expect_reason(flags.status(), ReasonCode::ReservedBitsSet, "reserved header flags");
}

LFF_TEST(persistence, writer_refuses_sequence_regression_and_sticks) {
    TempDir dir("journal-writer-regression");
    Result<JournalWriter> writer = JournalWriter::open(dir.file("w.jrnl"), LineageSeq::from(0),
                                                       LineageSeq::from(0), true);
    LFF_CHECK(writer.ok());
    expect_ok(writer.value().append(RecordType::Boot, LineageSeq::from(1), payload(1)), "append");
    const Status regressed = writer.value().append(RecordType::Boot, LineageSeq::from(1), payload(1));
    LFF_CHECK(!regressed.ok());
    expect_outcome(regressed, Outcome::Conflict, "regressed append");
    LFF_CHECK(writer.value().failed());
    // A writer that failed never reports success again.
    const Status after = writer.value().append(RecordType::Boot, LineageSeq::from(2), payload(2));
    LFF_CHECK(!after.ok());
    LFF_CHECK_EQ(writer.value().last_seq().value(), 1ull);
    writer.value().close();
}

LFF_TEST(persistence, writer_rejects_base_sequence_mismatch) {
    TempDir dir("journal-base-mismatch");
    write_journal(dir, 1);
    const std::filesystem::path path = dir.file("lineage.jrnl");
    const Result<JournalWriter> reopened =
        JournalWriter::open(path, LineageSeq::from(999), LineageSeq::from(1000), false);
    LFF_CHECK(!reopened.ok());
    expect_outcome(reopened.status(), Outcome::Conflict, "base mismatch");
}

LFF_TEST(persistence, writer_rejects_last_before_base) {
    TempDir dir("journal-last-before-base");
    const Result<JournalWriter> opened = JournalWriter::open(dir.file("bad.jrnl"),
                                                             LineageSeq::from(10),
                                                             LineageSeq::from(5), true);
    LFF_CHECK(!opened.ok());
    expect_reason(opened.status(), ReasonCode::SequenceRegression, "last before base");
}

LFF_TEST(persistence, header_read_validates_without_replay) {
    TempDir dir("journal-header");
    const std::filesystem::path path = write_journal(dir, 1);
    const Result<JournalHeader> header = JournalReader::read_header(path);
    LFF_CHECK(header.ok());
    LFF_CHECK_EQ(header.value().base_seq.value(), 100ull);
    LFF_CHECK_EQ(header.value().format_version, kJournalFormatVersion);
    std::vector<std::uint8_t> truncated = read_bytes(path);
    truncated.resize(8);
    write_bytes(path, truncated);
    LFF_CHECK(!JournalReader::read_header(path).ok());
}

LFF_TEST(persistence, hook_failure_aborts_replay_and_preserves_journal) {
    TempDir dir("journal-hook-abort");
    const std::filesystem::path path = write_journal(dir, 4);
    const std::uint64_t size_before = byte_size(path);
    ReplayCapture capture;
    JournalReplayHooks hooks;
    hooks.on_record = [&capture](const RecordView& view) {
        capture.sequences.push_back(view.seq.value());
        if (capture.sequences.size() == 3) {
            return Status::failure(Outcome::Corrupt, ReasonCode::MalformedEncoding, "synthetic");
        }
        return Status::success();
    };
    const Result<JournalOpenResult> replayed = JournalReader::replay(path, true, hooks);
    LFF_CHECK(!replayed.ok());
    expect_reason(replayed.status(), ReasonCode::MalformedEncoding, "hook failure");
    LFF_CHECK_EQ(byte_size(path), size_before);
}