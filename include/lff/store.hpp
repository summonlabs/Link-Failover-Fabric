// Link Failover Fabric — durable lineage store.
//
// The store owns the only state that survives restart:
//   * fabric definition, policy and topology (durable definitions);
//   * committed lineage: boot records, attempt records, fences, completions,
//     rollbacks and the idempotency index.
//
// It never restores dynamic liveness, observation freshness, active authority,
// in-flight process state or backend effects. A record that says "this
// coordinator incarnation held authority" is history; that incarnation no
// longer exists and its authority is fenced on the next boot.
//
// Commit ordering: a record is appended, flushed to the medium, and only then
// applied to the in-memory lineage. A failed append leaves in-memory state
// untouched and marks the store permanently failed: the runtime refuses further
// mutation rather than continuing with state that disagrees with the medium.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/attempt.hpp"
#include "lff/authority.hpp"
#include "lff/decision.hpp"
#include "lff/export.hpp"
#include "lff/journal.hpp"
#include "lff/policy.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lff {

inline constexpr std::size_t kMaxIdempotencyEntries = 8192;
inline constexpr std::size_t kMaxFenceRecords = 8192;
inline constexpr std::size_t kMaxEvidenceEntries = 8192;
inline constexpr std::size_t kMaxSnapshotBytes = 32u * 1024u * 1024u;

// In-memory observation of one (subject, publisher) pair. This lives for the
// lifetime of a coordinator incarnation and is never written to disk.
struct EvidenceEntry {
    RetainedFailureEvidence failure;
    std::map<LinkKey, RetainedReplacementEvidence> replacement;
    std::map<LinkKey, ObservationSeq> last_observation_seq;
};

struct IdempotencyEntry {
    Digest key;
    Digest decision_id;
    DecisionKind kind{DecisionKind::Indeterminate};
    Assertion assertion{Assertion::None};
    Outcome outcome{Outcome::Indeterminate};
    LinkKey subject;
    LinkGeneration subject_generation;
    bool has_replacement{false};
    LinkKey replacement;
    LinkGeneration replacement_generation;
    AttemptSeq attempt_seq;
    LineageSeq seq;
    std::vector<Reason> reasons;
};

inline constexpr std::size_t kMaxIdempotencyDecisionBytes = 4096;

struct DurableState {
    FabricName fabric;
    FailoverPolicy policy;
    TopologySnapshot topology;
    Epoch epoch;
    Incarnation coordinator;
    LineageSeq last_seq;
    std::uint64_t boot_count{0};
    std::uint64_t record_count{0};
    std::uint64_t epoch_advances{0};

    std::vector<AttemptRecord> attempts;
    std::vector<FenceRecord> fences;
    std::vector<IdempotencyEntry> idempotency;
};

struct StoreOptions {
    std::filesystem::path directory;
    FabricName fabric;
    FailoverPolicy policy;
    TopologySnapshot topology;
    std::size_t max_attempts{4096};
    std::size_t max_fences{kMaxFenceRecords};
    std::size_t max_idempotency{kMaxIdempotencyEntries};
    std::uint64_t snapshot_every_records{1024};
    bool enable_snapshot{true};
    bool repair_torn_tail{true};
};

struct StoreStatus {
    bool snapshot_loaded{false};
    bool journal_loaded{false};
    bool torn_tail_recovered{false};
    bool truncated_to_empty{false};
    std::uint64_t records_replayed{0};
    std::uint64_t bytes_discarded{0};
    std::uint64_t attempts_evicted{0};
    std::uint64_t fences_evicted{0};
    std::uint64_t idempotency_evicted{0};
    std::uint64_t snapshots_written{0};
    std::uint64_t snapshot_bytes{0};
};

class LFF_API LineageStore {
public:
    LineageStore() = default;
    ~LineageStore();
    LineageStore(const LineageStore&) = delete;
    LineageStore& operator=(const LineageStore&) = delete;
    LineageStore(LineageStore&&) noexcept;
    LineageStore& operator=(LineageStore&&) noexcept;

    static Result<LineageStore> open(const StoreOptions& options);

    const DurableState& state() const noexcept;
    DurableState& mutable_state() noexcept;
    const StoreStatus& status() const noexcept;
    // Records appended since the snapshot that is currently on the medium. This
    // is what compaction policy must be driven by: the absolute lineage
    // sequence keeps growing forever, so using it would checkpoint on every
    // single record once the first snapshot exists.
    std::uint64_t records_since_snapshot() const noexcept;
    bool failed() const noexcept;
    std::filesystem::path directory() const;

    // Appends a record, flushes it, and only then invokes `apply` with the
    // assigned lineage sequence.
    Status commit(RecordType type, const std::vector<std::uint8_t>& payload,
                  const std::function<void(LineageSeq)>& apply);

    // Serialises the durable state, atomically replaces the previous snapshot
    // and starts a fresh journal segment. Returns the sequence the snapshot
    // covers.
    Result<LineageSeq> checkpoint();

    // Marks every non-terminal attempt as interrupted and writes a fence for the
    // authority it held. Returns how many attempts were interrupted.
    Result<std::size_t> fence_restart_authority(Epoch new_epoch, Incarnation new_incarnation);

    void close() noexcept;

    // In-memory lineage mutations. Callers must have appended (or loaded) the
    // matching durable record first; these helpers never touch the medium.
    void note_attempt(const AttemptRecord& record);
    void note_fence(FenceRecord record);
    void note_idempotency(IdempotencyEntry entry);
    void note_epoch(Epoch epoch, Incarnation coordinator, std::uint64_t boot_count,
                    std::uint64_t epoch_advances);

    const AttemptRecord* find_attempt(const LinkKey& subject, AttemptSeq seq) const;
    const AttemptRecord* latest_attempt(const LinkKey& subject, LinkGeneration generation) const;
    std::size_t count_attempts(const LinkKey& subject, LinkGeneration generation) const;
    const AttemptRecord* active_authority(const LinkKey& subject,
                                          LinkGeneration generation) const;
    const IdempotencyEntry* find_idempotency(const Digest& key) const;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

// Canonical payload encoders. Every payload starts with the identity format
// version so a future format is refused explicitly.
LFF_API std::vector<std::uint8_t> encode_attempt_payload(const AttemptRecord& record);
LFF_API bool decode_attempt_payload(const std::uint8_t* data, std::size_t size, AttemptRecord& out);

LFF_API std::vector<std::uint8_t> encode_policy_payload(const FailoverPolicy& policy);
LFF_API bool decode_policy_payload(const std::uint8_t* data, std::size_t size, FailoverPolicy& out);

LFF_API std::vector<std::uint8_t> encode_topology_payload(const TopologySnapshot& topology);
LFF_API bool decode_topology_payload(const std::uint8_t* data, std::size_t size,
                                     TopologySnapshot& out);

LFF_API std::vector<std::uint8_t> encode_fence_payload(const FenceRecord& record);
LFF_API bool decode_fence_payload(const std::uint8_t* data, std::size_t size, FenceRecord& out);

LFF_API std::vector<std::uint8_t> encode_decision_payload(const FailoverDecision& decision);
LFF_API bool decode_decision_payload(const std::uint8_t* data, std::size_t size,
                                     FailoverDecision& out);

LFF_API std::vector<std::uint8_t> encode_idempotency_payload(const IdempotencyEntry& entry);
LFF_API bool decode_idempotency_payload(const std::uint8_t* data, std::size_t size,
                                        IdempotencyEntry& out);

struct BootPayload {
    Epoch epoch;
    Incarnation coordinator;
    std::uint64_t boot_count{0};
    std::uint64_t epoch_advances{0};
    std::string toolchain;
    std::string architecture;
};
LFF_API std::vector<std::uint8_t> encode_boot_payload(const BootPayload& payload);
LFF_API bool decode_boot_payload(const std::uint8_t* data, std::size_t size, BootPayload& out);

struct CheckpointPayload {
    LineageSeq covers_seq;
    std::uint64_t record_count{0};
    std::uint64_t snapshot_bytes{0};
};
LFF_API std::vector<std::uint8_t> encode_checkpoint_payload(const CheckpointPayload& payload);
LFF_API bool decode_checkpoint_payload(const std::uint8_t* data, std::size_t size,
                                       CheckpointPayload& out);

}  // namespace lff
