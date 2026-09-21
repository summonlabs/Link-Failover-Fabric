// Link Failover Fabric — the failover authority coordinator.
//
// The engine answers exactly one question, deterministically and truthfully:
//
//   Given an authoritative link failure, the current topology, alternate
//   connectivity, policy, service obligations and exact generations, which
//   replacement link or dependency may become authoritative now, what must be
//   fenced first, and when must failover be refused, rolled back, revalidated,
//   or declared indeterminate?
//
// What it owns
//   * whether a failed link generation is no longer eligible for service;
//   * which explicitly supplied alternate may replace it;
//   * the lifecycle of that failover authority, including rollback and
//     post-restart revalidation.
//
// What it does not own (and will not absorb)
//   * topology discovery, end-to-end route computation, bandwidth reservation;
//   * switch/NIC programming, traffic pacing, congestion synthesis, hardware
//     repair. Those live behind the explicitly supplied applier boundary and are
//     addressed by typed directives, not by the engine.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/attempt.hpp"
#include "lff/decision.hpp"
#include "lff/export.hpp"
#include "lff/selector.hpp"
#include "lff/store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lff {

struct EngineLimits {
    std::size_t max_tracked_publishers{512};
    std::size_t max_evidence_entries{kMaxEvidenceEntries};
    std::size_t max_attempts{4096};
    std::size_t max_fences{kMaxFenceRecords};
    std::size_t max_idempotency{kMaxIdempotencyEntries};
    std::size_t max_retained_decisions{4096};
    std::size_t max_pending_directives{256};
    std::size_t max_candidates_per_request{kMaxPolicyCandidates};
};

struct EngineOptions {
    FabricName fabric;
    FailoverPolicy policy;
    TopologySnapshot topology;
    std::filesystem::path state_dir;
    EngineLimits limits;
    std::uint64_t snapshot_every_records{1024};
    bool enable_snapshot{true};
    bool repair_torn_tail{true};
};

// A directive handed to an external applier. It is not authority: the authority
// is the durable grant the directive was derived from, and the applier must
// present the same authority vector for its report to be accepted.
struct ApplyDirective {
    LinkKey subject;
    LinkGeneration subject_generation;
    LinkKey replacement;
    LinkGeneration replacement_generation;
    AttemptSeq attempt_seq;
    Digest attempt_id;
    AuthorityVector authority;
    std::uint64_t issued_tick{0};

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, ApplyDirective& out);
    Digest digest() const;
};

struct EngineView {
    FabricName fabric;
    Epoch epoch;
    Incarnation coordinator;
    PolicyGeneration policy_generation;
    TopologyGeneration topology_generation;
    Digest policy_digest;
    Digest topology_digest;
    std::uint64_t boot_count{0};
    LineageSeq last_seq;
    std::size_t tracked_publishers{0};
    std::size_t evidence_entries{0};
    std::size_t retained_attempts{0};
    std::size_t retained_fences{0};
    std::size_t retained_decisions{0};
    std::size_t pending_directives{0};
    std::size_t active_authorities{0};
    std::size_t interrupted_attempts{0};
    bool store_failed{false};
    std::vector<AttemptRecord> recent_attempts;
    std::vector<FenceRecord> recent_fences;

    std::string to_string() const;
};

class LFF_API Engine {
public:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    // Opens the durable lineage, advances the coordinator epoch, writes a boot
    // record and fences all pre-restart authority. No failover authority from a
    // previous boot is ever restored as live.
    static Result<Engine> open(const EngineOptions& options);

    // --- adjacent fabric inputs (observations, never authority) -------------
    Status publish_failure_report(const FailureReport& report);
    Status publish_replacement_report(const ReplacementReport& report);
    Status set_policy(const FailoverPolicy& policy);
    Status set_topology(const TopologySnapshot& topology);

    // --- the decision -------------------------------------------------------
    Result<FailoverDecision> request_failover(const FailoverRequest& request);
    Result<FailoverDecision> record_application(const ApplicationReport& report);
    Result<FailoverDecision> record_verification(const VerificationReport& report);
    Result<FailoverDecision> request_rollback(const RollbackRequest& request);
    Result<FailoverDecision> resolve_interrupted(const RevalidationRequest& request);

    // --- applier boundary ---------------------------------------------------
    // Directives are produced by record_application() being *absent*: the engine
    // never invents an applier. A granted attempt exposes its directive until an
    // application report arrives for it.
    std::vector<ApplyDirective> pending_directives(std::size_t max_count) const;
    bool take_directive(const LinkKey& subject, LinkGeneration generation, AttemptSeq seq,
                        ApplyDirective& out) const;

    // --- inspection ---------------------------------------------------------
    EngineView inspect(std::size_t history_limit = 16) const;
    Epoch epoch() const noexcept;
    Incarnation incarnation() const noexcept;
    const FabricName& fabric() const noexcept;
    FailoverPolicy policy() const;
    TopologySnapshot topology() const;
    Status checkpoint();
    Status close();

private:
    struct Impl;
    static Status fence_active_authorities(Impl& impl, FenceKind kind, const std::string& detail);
    std::unique_ptr<Impl> impl_;
};

LFF_API std::string engine_view_to_string(const EngineView& view);

}  // namespace lff
