// Link Failover Fabric — replacement eligibility and selection.
//
// Supported problem class
// -----------------------
// Given (a) one failed link generation and its retained failure evidence,
// (b) a finite, explicitly supplied set of at most policy.max_candidates
// candidate alternates with retained replacement evidence, and (c) the current
// topology, policy and coordinator authority, decide which single alternate —
// if any — may become authoritative now.
//
// The problem is single-hop replacement selection. It is NOT route computation,
// NOT bandwidth reservation, NOT topology discovery and NOT congestion control.
//
// Correctness, validity, completeness, optimality
// -----------------------------------------------
// * VALID:   an outcome is valid when every field it reports is derived from
//            retained evidence that is current under the bound authority vector,
//            and when `validate_selection` accepts it.
// * CORRECT: a Selected outcome names a candidate that is (i) present in the
//            input, (ii) not the subject, and (iii) proven eligible under the
//            bound policy generation.
// * COMPLETE: when no candidate is eligible the outcome is either
//            ProvenInfeasible — which carries a certificate naming, for every
//            candidate, the disqualifying property that was observed under
//            current evidence — or Indeterminate. Failure to decide is never
//            reported as proof that no solution exists.
// * OPTIMAL: the selected candidate is the maximum of a declared total
//            lexicographic objective over the eligible set, with a canonical
//            final tie-break on (link name ascending, generation descending).
//            The objective is total because candidate link identity is unique
//            within the accepted set.
// * DETERMINISTIC: the outcome is independent of insertion, container or
//            discovery order; candidates are canonically ordered first.
//
// When the outcome is Indeterminate the caller receives SEARCH_INDETERMINATE
// semantics (ReasonCode::SelectionIndeterminate) rather than a weaker
// affirmative answer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/evidence.hpp"
#include "lff/export.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"
#include "lff/policy.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lff {

enum class SelectionStatus : std::uint16_t {
    Selected = 0,           // an eligible candidate was selected
    ProvenInfeasible = 1,   // every candidate was conclusively disproven
    Indeterminate = 2,      // at least one candidate could not be decided
    EmptyCandidateSet = 3,  // the caller supplied no candidates
};
inline constexpr std::uint16_t kSelectionStatusCount = 4;

LFF_API std::string_view selection_status_name(SelectionStatus value) noexcept;

// One candidate resolved against the runtime's retained evidence.
struct ResolvedCandidate {
    LinkKey candidate;
    LinkGeneration link_generation;
    bool evidence_present{false};
    ReplacementReport report;
    Digest evidence_digest;
    FreshnessClass freshness{FreshnessClass::PriorIncarnation};
    std::uint64_t age_ticks{0};
    bool generation_asserted{false};
    bool generation_matched{false};
    // Set when current, qualified observations of this candidate contradict each
    // other. A conflicted candidate is undecided, never eligible, and never
    // disproven: contradiction is not evidence of absence.
    bool conflicted{false};
};

struct SelectionInput {
    FabricName fabric;
    LinkKey subject;
    LinkGeneration subject_generation;
    TopologyGeneration topology_generation;
    PolicyGeneration policy_generation;
    FailoverPolicy policy;
    std::uint32_t min_replacement_capacity{0};
    std::vector<ResolvedCandidate> candidates;  // canonical order, unique by link
    Epoch epoch;
    Incarnation coordinator;
};

// The lexicographic objective, exposed so that it can be tested directly and
// replayed from a recorded assessment.
struct ObjectiveKey {
    bool adjacency_authorized{false};
    std::uint32_t confidence{0};
    std::uint64_t age_ticks{0};       // lower is fresher
    std::uint32_t capacity_units{0};
    std::uint32_t cost_units{0};
    std::uint64_t link_generation{0};
    std::string link_name;            // final canonical tie-break, ascending

    static ObjectiveKey from(const CandidateAssessment& assessment);
};

// Returns true when a is strictly better than b under the total objective.
LFF_API bool objective_better(const ObjectiveKey& a, const ObjectiveKey& b) noexcept;

struct SelectionOutcome {
    SelectionStatus status{SelectionStatus::Indeterminate};
    ReasonCode summary{ReasonCode::SelectionIndeterminate};
    std::optional<std::size_t> selected_index;  // index into the canonical candidate list
    std::vector<CandidateAssessment> assessments;
    std::size_t eligible_count{0};
    std::size_t disproven_count{0};
    std::size_t indeterminate_count{0};
};

LFF_API SelectionOutcome select_replacement(const SelectionInput& input);

// Independent re-validation of a produced outcome. The engine calls this before
// it commits any grant; a failure here means no authority is created.
LFF_API Status validate_selection(const SelectionInput& input, const SelectionOutcome& outcome);

LFF_API std::string selection_outcome_to_string(const SelectionOutcome& outcome);

}  // namespace lff
