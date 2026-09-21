// Link Failover Fabric — failover policy and fabric topology definitions.
//
// Policy is durable definition, not dynamic state: it survives restart exactly
// as written and carries a generation that every decision binds. A policy or
// topology change invalidates cached eligibility reasoning, never the durable
// lineage of decisions already taken.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"
#include "lff/evidence.hpp"
#include "lff/hash.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lff {

// How multiple fresh, qualified failure observations are combined.
enum class AgreementRule : std::uint16_t {
    Unanimous = 0,                  // every qualified report must state failure
    MajorityOfFreshQualified = 1,   // strict majority must state failure; ties conflict
};
inline constexpr std::uint16_t kAgreementRuleCount = 2;

LFF_API std::string_view agreement_rule_name(AgreementRule value) noexcept;
LFF_API bool agreement_rule_parse(std::string_view text, AgreementRule& out) noexcept;

inline constexpr std::uint32_t kMaxPolicyCandidates = 256;
inline constexpr std::uint32_t kMaxRetainedAttemptsPerLink = 256;
inline constexpr std::uint64_t kMaxEvidenceAgeTicks = 1ull << 40;

struct FailoverPolicy {
    PolicyGeneration generation;

    // Minimum confidence (parts per thousand) for an observation to count.
    Confidence min_failure_confidence{Confidence::from(900)};
    Confidence min_replacement_confidence{Confidence::from(900)};

    // Maximum age, in coordinator ticks, of a usable observation.
    std::uint64_t evidence_max_age_ticks{4096};

    // A candidate must declare at least this much usable capacity.
    std::uint32_t min_replacement_capacity{1};
    // When true a candidate must additionally declare at least as much capacity
    // as the failed link's current topology definition. This is the service
    // obligation the replacement has to satisfy.
    bool require_subject_capacity{true};

    // When true a candidate without adjacent-authority authorization is not eligible.
    bool require_adjacency_authorization{true};
    // When true a Degraded candidate may replace a failed link.
    bool allow_degraded_replacement{false};
    // When true a Degraded subject observation counts as a failure.
    bool treat_degraded_as_failed{false};

    AgreementRule agreement{AgreementRule::Unanimous};

    // Bounded attempt budget per governed link generation.
    std::uint32_t max_attempts_per_generation{8};
    // Bounded candidate set accepted from one request.
    std::uint32_t max_candidates{kMaxPolicyCandidates};
    // Bounded retained attempt history per governed link.
    std::uint32_t max_retained_attempts{kMaxRetainedAttemptsPerLink};

    // When true, an acknowledged but unverified application is not treated as a
    // completed effect; the attempt stops at Acknowledgement with
    // Outcome::Indeterminate until verification arrives.
    bool require_verified_effect{true};

    // Master switch. When false every failover request is refused with
    // ReasonCode::PolicyDisabled and no authority is created.
    bool failover_enabled{true};

    Status validate() const;
    Digest digest() const;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, FailoverPolicy& out);

    static FailoverPolicy defaults();
};

LFF_API std::string failover_policy_to_string(const FailoverPolicy& policy);
LFF_API bool failover_policy_parse(std::string_view text, FailoverPolicy& out);

// ---------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------
struct LinkDefinition {
    LinkKey link;
    LinkGeneration generation;
    std::uint32_t capacity_units{0};

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, LinkDefinition& out);
};

LFF_API bool link_definition_less(const LinkDefinition& a, const LinkDefinition& b) noexcept;

// A topology snapshot is an explicitly supplied set of link definitions. The
// runtime never discovers topology; it only consumes what it is given, and it
// stores the definitions canonically ordered so that the digest is stable.
struct TopologySnapshot {
    TopologyGeneration generation;
    std::vector<LinkDefinition> links;

    Status validate() const;
    void canonicalise();
    Digest digest() const;
    const LinkDefinition* find(const LinkKey& key) const;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, TopologySnapshot& out);
};

inline constexpr std::size_t kMaxTopologyLinks = 4096;

}  // namespace lff
