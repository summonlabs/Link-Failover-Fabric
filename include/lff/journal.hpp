// Link Failover Fabric — versioned, integrity-checked durable lineage.
//
// Durable format (journal file)
// -----------------------------
//   32-byte file header
//      0  u32  magic 'LIN1'
//      4  u16  format version
//      6  u16  flags (must be zero)
//      8  u64  base sequence (the lineage sequence this segment starts after)
//     16  u64  reserved (must be zero)
//     24  u32  reserved (must be zero)
//     28  u32  CRC-32C over header bytes [0, 28)
//
//   32-byte record header, then payload
//      0  u32  magic 'LRC1'
//      4  u16  format version
//      6  u16  record type
//      8  u32  payload length
//     12  u64  record sequence
//     20  u32  CRC-32C over the payload
//     24  u32  reserved (must be zero)
//     28  u32  CRC-32C over record header bytes [0, 28)
//
// Records are appended, flushed to the medium and only then reported as
// committed. A record that is only partially present at end-of-file is a torn
// tail and is recovered by truncating back to the last complete record, and
// only when the caller asked for repair. A record that is fully present but
// fails its checksum, declares an unsupported version, names an unknown record
// type, sets a reserved bit or regresses its sequence is corruption: opening
// fails with Outcome::Corrupt and nothing is truncated or repaired.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"
#include "lff/version.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lff {

// Durable record kinds. Evidence is deliberately absent: an observation is
// dynamic state that belongs to one coordinator incarnation and is never
// restored from disk.
enum class RecordType : std::uint16_t {
    Boot = 0,
    PolicyCommit = 1,
    TopologyCommit = 2,
    AttemptCreate = 3,
    AttemptUpdate = 4,
    DecisionCommit = 5,
    Fence = 6,
    Completion = 7,
    Rollback = 8,
    IdempotencyIndex = 9,
    Checkpoint = 10,
    Shutdown = 11,
};
inline constexpr std::uint16_t kRecordTypeCount = 12;

LFF_API std::string_view record_type_name(RecordType value) noexcept;
LFF_API bool record_type_parse(std::string_view text, RecordType& out) noexcept;

inline constexpr std::size_t kJournalHeaderSize = 32;
inline constexpr std::size_t kRecordHeaderSize = 32;
inline constexpr std::size_t kMaxRecordPayload = 1u << 20;  // 1 MiB
inline constexpr std::uint32_t kJournalMagic = 0x314E494Cu;  // "LIN1"
inline constexpr std::uint32_t kRecordMagic = 0x3143524Cu;   // "LRC1"
inline constexpr std::uint64_t kMaxJournalBytes = 256ull * 1024ull * 1024ull;

struct RecordView {
    RecordType type{RecordType::Boot};
    LineageSeq seq;
    std::uint32_t payload_length{0};
    const std::uint8_t* payload{nullptr};

    std::size_t payload_size() const noexcept { return payload_length; }
};

struct JournalHeader {
    std::uint16_t format_version{kJournalFormatVersion};
    std::uint16_t flags{0};
    LineageSeq base_seq;
};

struct JournalOpenResult {
    JournalHeader header;
    LineageSeq last_seq;
    bool torn_tail_recovered{false};
    bool truncated_to_empty{false};
    std::uint64_t records_replayed{0};
    std::uint64_t bytes_discarded{0};
};

// Append-only writer. Every append is flushed and forced to the medium before
// the call returns.
class LFF_API JournalWriter {
public:
    JournalWriter() = default;
    ~JournalWriter();
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    JournalWriter(JournalWriter&& other) noexcept;
    JournalWriter& operator=(JournalWriter&& other) noexcept;

    // `create_new` truncates any existing file and writes a fresh header with
    // `base_seq`. Otherwise the existing header is validated against
    // `base_seq` and appends continue from `last_seq`.
    static Result<JournalWriter> open(const std::filesystem::path& path, LineageSeq base_seq,
                                      LineageSeq last_seq, bool create_new);

    // The sequence must be strictly greater than the previously written one.
    Status append(RecordType type, LineageSeq seq, const std::vector<std::uint8_t>& payload);
    Status flush();

    bool is_open() const noexcept;
    bool failed() const noexcept;
    LineageSeq last_seq() const noexcept;
    const std::filesystem::path& path() const noexcept;
    std::uint64_t bytes_written() const noexcept;

    void close() noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

struct JournalReplayHooks {
    std::function<Status(const RecordView&)> on_record;
};

// Replays an existing journal, validating every header, checksum, version, enum
// and sequence, and invoking the hook for each complete record in order.
class LFF_API JournalReader {
public:
    // Returns Outcome::NotFound when the file does not exist.
    static Result<JournalOpenResult> replay(const std::filesystem::path& path, bool repair_torn_tail,
                                            const JournalReplayHooks& hooks);

    static Result<JournalHeader> read_header(const std::filesystem::path& path);
};

}  // namespace lff
