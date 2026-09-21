// Link Failover Fabric — failover attempt lifecycle.
//
// An attempt is the unit of single-winner exclusion. At most one attempt per
// (fabric, subject, subject generation) holds authority at a time, and the
// authority it holds is bound to the generations recorded in its
// AuthorityVector. Attempts are retained in bounded history: an attempt that has
// been evicted is refused by identity, never silently re-created.
//
// Durable attempt records are written in two steps so that every crash boundary
// the runtime claims to survive is an observable state:
//
//   1. AttemptCreate at phase Evaluated  — the intent, carrying no authority
//   2. AttemptUpdate to phase Authorized — the grant, carrying authority
//
// A process killed between (1) and (2) leaves an Evaluated attempt that the next
// boot fences as Interrupted with ReasonCode::InterruptedBeforeCommit. A process
// killed after (2) leaves Authorized (InterruptedAfterCommit), and one killed
// after an applier reported acceptance leaves Acknowledged or Verified
// (InterruptedAfterApply).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/authority.hpp"
#include "lff/export.hpp"
#include "lff/hash.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lff {

// How an interrupted attempt was resolved after a restart.
enum class RevalidationVerdict : std::uint16_t {
    NotApplied = 0,  // the replacement provably never became effective
    Applied = 1,     // the replacement provably became effective
    Unknown = 2,     // neither can be proven; the subject stays indeterminate
};
inline constexpr std::uint16_t kRevalidationVerdictCount = 3;

LFF_API std::string_view revalidation_verdict_name(RevalidationVerdict value) noexcept;
LFF_API bool revalidation_verdict_parse(std::string_view text, RevalidationVerdict& out) noexcept;

enum class AttemptPhase : std::uint16_t {
    // Non-terminal phases. Authority exists from Authorized onwards.
    Evaluated = 0,     // intent recorded; no authority
    Authorized = 1,    // single-winner authority granted and durably committed
    Acknowledged = 2,  // an applier reported acceptance (NOT effect)
    Verified = 3,      // an independent observer reported the effect

    // Terminal phases.
    Refused = 4,       // eligibility conclusively disproven; no authority
    Indeterminate = 5, // no total determination was possible
    Committed = 6,     // verified effect committed
    RolledBack = 7,    // reverted to the original link as a new transition
    Interrupted = 8,   // in flight across a restart, effect unknown
    Superseded = 9,    // fenced by a newer attempt for a newer generation
    Aborted = 10,      // fenced or revalidated as never applied
};
inline constexpr std::uint16_t kAttemptPhaseCount = 11;

LFF_API std::string_view attempt_phase_name(AttemptPhase value) noexcept;
LFF_API bool attempt_phase_parse(std::string_view text, AttemptPhase& out) noexcept;
LFF_API bool attempt_phase_is_terminal(AttemptPhase value) noexcept;

// True while the attempt still owns exclusive authority over its subject
// generation. Verified is still authoritative until it is committed or fenced:
// an uncommitted but verified application must keep excluding competitors.
LFF_API bool attempt_phase_holds_authority(AttemptPhase value) noexcept;

// Reason code describing where a non-terminal attempt was cut off by a restart.
LFF_API ReasonCode interruption_reason(AttemptPhase value) noexcept;

inline constexpr std::size_t kMaxAttemptReasons = 16;
inline constexpr std::size_t kMaxRetainedAttempts = 8192;

struct AttemptRecord {
    LinkKey subject;
    LinkGeneration subject_generation;
    AttemptSeq attempt_seq;
    Digest attempt_id;
    Digest idempotency_key;

    AttemptPhase phase{AttemptPhase::Evaluated};

    LinkKey replacement;
    LinkGeneration replacement_generation;

    AuthorityVector authority;
    Digest evidence_digest;

    std::uint64_t created_tick{0};
    Epoch created_epoch;
    Incarnation coordinator;
    LineageSeq create_seq;
    LineageSeq update_seq;

    std::optional<Digest> predecessor;
    // Applier bound to the actual effect, once one exists.
    Incarnation applier_incarnation;

    // Set when an interrupted attempt has been revalidated after a restart.
    bool revalidated{false};
    RevalidationVerdict revalidation{RevalidationVerdict::Unknown};

    std::vector<Reason> reasons;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, AttemptRecord& out);
    Digest digest() const;
    std::string to_string() const;
};

// Deterministic attempt identity. attempt_id depends on the subject, its
// generation and the coordinator-assigned sequence, so replaying the same
// client request cannot mint a second identity for the same sequence.
LFF_API Digest compute_attempt_id(
    const FabricName& fabric, const LinkKey& subject, LinkGeneration generation, AttemptSeq seq);

}  // namespace lff
