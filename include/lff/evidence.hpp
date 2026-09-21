// Link Failover Fabric — fabric observations and candidate replacement evidence.
//
// Evidence is an observation, never authority. It is always attributed to a
// named publisher and to the exact incarnation of that publisher's process, and
// it is only ever "fresh" relative to the coordinator tick at which the
// coordinator actually received it. Publisher wall clocks are advisory metadata
// and are never used to order or to age a decision.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"
#include "lff/hash.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"

#include <cstdint>
#include <string>

namespace lff {

using PublisherName = StrongName<struct PublisherNameTag>;

// ---------------------------------------------------------------------------
// Observation state
// ---------------------------------------------------------------------------
enum class ObservationState : std::uint16_t {
    Unknown = 0,  // the publisher explicitly cannot say (never treated as health)
    Up = 1,
    Degraded = 2, // carrying traffic, but below nominal capability
    Down = 3,
    Unusable = 4, // present but explicitly not usable for service
};
inline constexpr std::uint16_t kObservationStateCount = 5;

LFF_API std::string_view observation_state_name(ObservationState value) noexcept;
LFF_API bool observation_state_parse(std::string_view text, ObservationState& out) noexcept;
LFF_API bool observation_state_is_failure(ObservationState value, bool treat_degraded_as_failure) noexcept;

// ---------------------------------------------------------------------------
// Evidence stamp — who observed, under which process incarnation
// ---------------------------------------------------------------------------
struct EvidenceStamp {
    PublisherName publisher;
    Incarnation publisher_incarnation;
    Epoch publisher_epoch;
    ObservationSeq observation_seq;
    // Advisory only. Never used for ordering, ageing or authority.
    std::int64_t wall_clock_ms{0};

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, EvidenceStamp& out);
    Digest digest() const;
};

// ---------------------------------------------------------------------------
// Failure report — an observation about the subject link
// ---------------------------------------------------------------------------
struct FailureReport {
    LinkKey subject;
    LinkGeneration link_generation;
    TopologyGeneration topology_generation;
    ObservationState state{ObservationState::Unknown};
    Confidence confidence;
    EvidenceStamp stamp;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, FailureReport& out);
    Digest digest() const;
};

// ---------------------------------------------------------------------------
// Replacement report — an observation about a candidate alternate link
// ---------------------------------------------------------------------------
struct ReplacementReport {
    LinkKey candidate;
    LinkGeneration link_generation;
    TopologyGeneration topology_generation;
    ObservationState state{ObservationState::Unknown};
    Confidence confidence;
    std::uint32_t capacity_units{0};
    std::uint32_t cost_units{0};
    bool adjacency_authorized{false};
    PolicyGeneration adjacency_policy_generation;
    EvidenceStamp stamp;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, ReplacementReport& out);
    Digest digest() const;
};

// ---------------------------------------------------------------------------
// Freshness — derived exclusively from coordinator-local receipt state
// ---------------------------------------------------------------------------
enum class FreshnessClass : std::uint16_t {
    Current = 0,          // received by this coordinator incarnation, inside the window
    Expired = 1,          // received by this coordinator incarnation, outside the window
    PriorIncarnation = 2, // recorded by a previous coordinator boot, or a retired publisher boot
    Future = 3,           // receipt tick is ahead of the coordinator clock: invalid
};
inline constexpr std::uint16_t kFreshnessClassCount = 4;

LFF_API std::string_view freshness_class_name(FreshnessClass value) noexcept;

// Retained evidence couples the publisher's observation to the coordinator's
// own receipt state. Only the receipt state decides freshness.
struct RetainedFailureEvidence {
    FailureReport report;
    Digest digest;
    LineageSeq receipt_seq;
    std::uint64_t receipt_tick{0};
    Epoch receipt_epoch;
    Incarnation receipt_coordinator;
    bool durable{false};
};

struct RetainedReplacementEvidence {
    ReplacementReport report;
    Digest digest;
    LineageSeq receipt_seq;
    std::uint64_t receipt_tick{0};
    Epoch receipt_epoch;
    Incarnation receipt_coordinator;
    bool durable{false};
};

// ---------------------------------------------------------------------------
// Resolution results
//
// Verdict is the trichotomy the whole runtime is built on: a candidate is
// either proven usable, proven unusable, or undecided. Undecided never becomes
// authority.
// ---------------------------------------------------------------------------
enum class Verdict : std::uint16_t {
    Eligible = 0,
    Disproven = 1,
    Indeterminate = 2,
};
inline constexpr std::uint16_t kVerdictCount = 3;

LFF_API std::string_view verdict_name(Verdict value) noexcept;

struct CandidateAssessment {
    LinkKey candidate;
    LinkGeneration link_generation;
    Verdict verdict{Verdict::Indeterminate};
    ReasonCode reason{ReasonCode::None};
    std::string detail;

    // Objective inputs, retained so that the ordering can be replayed exactly.
    ObservationState state{ObservationState::Unknown};
    Confidence confidence;
    FreshnessClass freshness{FreshnessClass::PriorIncarnation};
    std::uint64_t age_ticks{0};
    std::uint32_t capacity_units{0};
    std::uint32_t cost_units{0};
    bool adjacency_authorized{false};
    Digest evidence_digest;
};

LFF_API bool candidate_assessment_less(const CandidateAssessment& a, const CandidateAssessment& b) noexcept;
LFF_API std::string candidate_assessment_to_string(const CandidateAssessment& value);

// ---------------------------------------------------------------------------
// Replacement candidate
//
// A caller explicitly supplies the alternates it wants considered. The runtime
// never discovers topology and never invents a candidate. `expected_generation`
// is what the caller believes is current for that alternate; a zero value means
// "the generation the runtime currently holds". A non-zero value that no longer
// matches the retained evidence is a stale binding and makes the candidate
// undecided, never eligible.
// ---------------------------------------------------------------------------
struct ReplacementCandidate {
    LinkKey candidate;
    LinkGeneration expected_generation;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, ReplacementCandidate& out);
};

}  // namespace lff
