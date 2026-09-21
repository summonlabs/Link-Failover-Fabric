// Link Failover Fabric — externally visible decisions.
//
// Every decision states the exact subject it applies to, the authority that made
// it legal, the strongest assertion it actually establishes, and a deterministic
// explanation. Observation, eligibility, recommendation, authorization,
// acknowledgement and verified effect are separate assertion levels and are
// never conflated: ReasonCode::AcknowledgementNotEffect exists precisely so a
// caller can see that an acknowledgement is not a proven effect.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/attempt.hpp"
#include "lff/authority.hpp"
#include "lff/evidence.hpp"
#include "lff/export.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lff {

enum class DecisionKind : std::uint16_t {
    Observe = 0,       // a fabric observation was recorded
    Grant = 1,         // failover authority was granted
    Refuse = 2,        // failover was conclusively refused
    Rollback = 3,      // a grant was reverted
    Revalidate = 4,    // an interrupted attempt was resolved
    Indeterminate = 5, // no determination was possible
    Acknowledge = 6,   // an applier accepted the directive
    Verify = 7,        // an effect was independently verified
    Fence = 8,         // pre-existing authority was fenced
};
inline constexpr std::uint16_t kDecisionKindCount = 9;

LFF_API std::string_view decision_kind_name(DecisionKind value) noexcept;
LFF_API bool decision_kind_parse(std::string_view text, DecisionKind& out) noexcept;

// The ladder of assertions. A decision reports the highest rung it actually
// reached. Refusal and indeterminate decisions report Observation or Eligibility.
enum class Assertion : std::uint16_t {
    None = 0,
    Observation = 1,
    Eligibility = 2,
    Recommendation = 3,
    Authorization = 4,
    Acknowledgement = 5,
    VerifiedEffect = 6,
};
inline constexpr std::uint16_t kAssertionCount = 7;

LFF_API std::string_view assertion_name(Assertion value) noexcept;
LFF_API bool assertion_parse(std::string_view text, Assertion& out) noexcept;
LFF_API std::uint16_t assertion_rank(Assertion value) noexcept;

inline constexpr std::size_t kMaxDecisionReasons = kMaxReasons;
inline constexpr std::size_t kMaxDecisionCandidates = 256;
inline constexpr std::size_t kMaxRetainedDecisions = 8192;

struct FailoverDecision {
    Digest decision_id;
    DecisionKind kind{DecisionKind::Indeterminate};
    Assertion assertion{Assertion::None};
    Outcome outcome{Outcome::Indeterminate};

    FabricName fabric;
    LinkKey subject;
    LinkGeneration subject_generation;

    bool has_replacement{false};
    LinkKey replacement;
    LinkGeneration replacement_generation;

    AttemptSeq attempt_seq;
    // Deterministic identity of the attempt this decision governs. Zero when the
    // decision does not concern a recorded attempt.
    Digest attempt_id;
    Epoch epoch;
    Incarnation coordinator;
    AuthorityVector authority;
    Digest evidence_digest;

    // Durable position of the record that carries this decision. Zero when the
    // decision was not (and will not be) committed; Durable() distinguishes
    // "not yet committed" from "committed at sequence 0", which cannot happen.
    LineageSeq lineage_seq;
    bool durable{false};

    // True when this decision is a replay of an earlier identical request and
    // establishes no new authority.
    bool idempotent_replay{false};

    std::vector<Reason> reasons;
    std::vector<CandidateAssessment> candidates;

    Digest compute_id() const;
    std::string to_string() const;
    // Stable canonical form used in digests and golden tests.
    std::string canonical_key() const;
    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, FailoverDecision& out);
};

// A request to make a replacement authoritative for one failed link generation.
struct FailoverRequest {
    LinkKey subject;
    LinkGeneration subject_generation;
    std::vector<ReplacementCandidate> candidates;
    AuthorityExpectation expected;
    // Optional caller-supplied identity for idempotent retry. Two requests with
    // the same key never produce two authorities.
    Digest idempotency_key;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, FailoverRequest& out);
    Digest digest() const;
};

// A request to revert an existing grant. Restoring the original link is a new
// generation-bound transition, never a silent resurrection of the old one.
struct RollbackRequest {
    LinkKey subject;
    LinkGeneration subject_generation;
    AttemptSeq attempt_seq;
    AuthorityExpectation expected;
    Digest idempotency_key;
    std::string rationale;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, RollbackRequest& out);
    Digest digest() const;
};

struct RevalidationRequest {
    LinkKey subject;
    LinkGeneration subject_generation;
    AttemptSeq attempt_seq;
    RevalidationVerdict verdict{RevalidationVerdict::Unknown};
    Incarnation observer_incarnation;
    Epoch observer_epoch;
    std::string rationale;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, RevalidationRequest& out);
};

// An application report from a backend/applier. An acknowledgement is not a
// verified effect and is recorded as its own assertion level.
struct ApplicationReport {
    LinkKey subject;
    LinkGeneration subject_generation;
    AttemptSeq attempt_seq;
    Digest attempt_id;
    Incarnation applier_incarnation;
    Epoch applier_epoch;
    bool accepted{false};
    std::string detail;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, ApplicationReport& out);
};

struct VerificationReport {
    LinkKey subject;
    LinkGeneration subject_generation;
    AttemptSeq attempt_seq;
    Digest attempt_id;
    Incarnation verifier_incarnation;
    Epoch verifier_epoch;
    bool effect_observed{false};
    std::string detail;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, VerificationReport& out);
};

}  // namespace lff
