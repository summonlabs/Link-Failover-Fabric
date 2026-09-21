// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Selection is the only algorithmic component of the runtime, so it is tested
// three ways:
//   1. directly, against the declared eligibility trichotomy;
//   2. differentially, against a slow reference solver that enumerates every
//      ordering of the candidate set;
//   3. adversarially, with instances built specifically to defeat a greedy
//      "take the biggest capacity" or "take the first usable" heuristic.
#include "framework.hpp"
#include "lff/selector.hpp"
#include "support.hpp"

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

SelectionInput make_input(const FabricFixture& fixture, const std::string& subject,
                          std::uint64_t subject_generation) {
    SelectionInput input;
    input.fabric = fixture.fabric;
    input.subject.fabric = fixture.fabric;
    input.subject.link = LinkName::parse(subject).value();
    input.subject_generation = LinkGeneration::from(subject_generation);
    input.topology_generation = fixture.topology.generation;
    input.policy_generation = fixture.policy.generation;
    input.policy = fixture.policy;
    input.min_replacement_capacity = fixture.policy.min_replacement_capacity;
    input.epoch = Epoch::from(1);
    input.coordinator = Incarnation::generate();
    return input;
}

ResolvedCandidate resolved(const FabricFixture& fixture, const std::string& link,
                           std::uint64_t generation, ObservationState state,
                           std::uint32_t confidence, std::uint32_t capacity, std::uint32_t cost,
                           bool adjacency, FreshnessClass freshness, std::uint64_t age) {
    ResolvedCandidate entry;
    entry.candidate.fabric = fixture.fabric;
    entry.candidate.link = LinkName::parse(link).value();
    entry.link_generation = LinkGeneration::from(generation);
    entry.evidence_present = true;
    entry.report.candidate = entry.candidate;
    entry.report.link_generation = entry.link_generation;
    entry.report.topology_generation = fixture.topology.generation;
    entry.report.state = state;
    entry.report.confidence = make_confidence(confidence);
    entry.report.capacity_units = capacity;
    entry.report.cost_units = cost;
    entry.report.adjacency_authorized = adjacency;
    entry.evidence_digest = sha256(link + std::to_string(generation));
    entry.freshness = freshness;
    entry.age_ticks = age;
    return entry;
}

// ---------------------------------------------------------------------------
// Slow reference solver
//
// Enumerates every permutation of the candidate set, scores the first candidate
// of each permutation with an independently written eligibility oracle, and
// keeps the best. It shares no code with select_replacement.
// ---------------------------------------------------------------------------
struct ReferenceResult {
    bool has_selection{false};
    std::size_t selected{0};
    std::size_t eligible{0};
    std::size_t disproven{0};
    std::size_t indeterminate{0};
};

bool reference_eligible(const SelectionInput& input, const ResolvedCandidate& entry,
                        bool& conclusive) {
    conclusive = false;
    if (entry.candidate == input.subject) {
        conclusive = true;
        return false;
    }
    if (entry.conflicted) {
        return false;
    }
    if (!entry.evidence_present) {
        return false;
    }
    if (entry.generation_asserted && !entry.generation_matched) {
        return false;
    }
    if (entry.freshness != FreshnessClass::Current) {
        return false;
    }
    if (entry.report.confidence.value() < input.policy.min_replacement_confidence.value()) {
        return false;
    }
    if (entry.report.state == ObservationState::Unknown) {
        return false;
    }
    if (entry.report.state == ObservationState::Down ||
        entry.report.state == ObservationState::Unusable) {
        conclusive = true;
        return false;
    }
    if (entry.report.state == ObservationState::Degraded &&
        !input.policy.allow_degraded_replacement) {
        conclusive = true;
        return false;
    }
    if (entry.report.capacity_units < input.min_replacement_capacity) {
        conclusive = true;
        return false;
    }
    if (input.policy.require_adjacency_authorization && !entry.report.adjacency_authorized) {
        conclusive = true;
        return false;
    }
    return true;
}

// Compares two candidates directly, without using objective_better.
bool reference_better(const ResolvedCandidate& a, const ResolvedCandidate& b) {
    if (a.report.adjacency_authorized != b.report.adjacency_authorized) {
        return a.report.adjacency_authorized;
    }
    if (a.report.confidence.value() != b.report.confidence.value()) {
        return a.report.confidence.value() > b.report.confidence.value();
    }
    if (a.age_ticks != b.age_ticks) {
        return a.age_ticks < b.age_ticks;
    }
    if (a.report.capacity_units != b.report.capacity_units) {
        return a.report.capacity_units > b.report.capacity_units;
    }
    if (a.report.cost_units != b.report.cost_units) {
        return a.report.cost_units < b.report.cost_units;
    }
    if (a.link_generation.value() != b.link_generation.value()) {
        return a.link_generation.value() > b.link_generation.value();
    }
    return a.candidate.link.value() < b.candidate.link.value();
}

ReferenceResult reference_select(const SelectionInput& input) {
    ReferenceResult result;
    std::vector<std::size_t> best_eligible;
    for (std::size_t i = 0; i < input.candidates.size(); ++i) {
        bool conclusive = false;
        if (reference_eligible(input, input.candidates[i], conclusive)) {
            ++result.eligible;
            best_eligible.push_back(i);
        } else if (conclusive) {
            ++result.disproven;
        } else {
            ++result.indeterminate;
        }
    }
    if (best_eligible.empty()) {
        return result;
    }
    // Exhaustive: try every ordering of the eligible set and keep the maximal
    // element under the declared objective.
    std::vector<std::size_t> order = best_eligible;
    std::sort(order.begin(), order.end());
    std::size_t champion = order.front();
    do {
        const std::size_t candidate = order.front();
        if (reference_better(input.candidates[candidate], input.candidates[champion])) {
            champion = candidate;
        }
    } while (std::next_permutation(order.begin(), order.end()));
    result.has_selection = true;
    result.selected = champion;
    return result;
}

}  // namespace

LFF_TEST(unit, selector_requires_a_non_empty_candidate_set) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    SelectionInput input = make_input(fixture, "alpha", 1);
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::EmptyCandidateSet);
    LFF_CHECK(outcome.summary == ReasonCode::CandidateSetEmpty);
    expect_ok(validate_selection(input, outcome), "empty selection");
}

LFF_TEST(unit, selector_refuses_its_own_subject) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    SelectionInput input = make_input(fixture, "alpha", 1);
    input.candidates.push_back(resolved(fixture, "alpha", 1, ObservationState::Up, 1000, 1000, 0,
                                        true, FreshnessClass::Current, 0));
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::ProvenInfeasible);
    LFF_CHECK_EQ(outcome.assessments.size(), std::size_t{1});
    LFF_CHECK(outcome.assessments[0].reason == ReasonCode::ReplacementIsSubject);
    expect_ok(validate_selection(input, outcome), "self-substitution selection");
}

LFF_TEST(unit, selector_trichotomy_is_exact) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}, {"beta", 2, 500}});
    SelectionInput input = make_input(fixture, "alpha", 1);
    input.candidates.push_back(resolved(fixture, "beta", 2, ObservationState::Down, 1000, 500, 0,
                                        true, FreshnessClass::Current, 0));
    input.candidates.push_back(resolved(fixture, "gamma", 3, ObservationState::Up, 1000, 500, 0,
                                        true, FreshnessClass::Current, 0));
    input.candidates.push_back(resolved(fixture, "delta", 4, ObservationState::Up, 1000, 0, 0, true,
                                        FreshnessClass::Current, 0));
    const SelectionOutcome outcome = select_replacement(input);
    // beta is disproven, delta is disproven (capacity), gamma is eligible.
    LFF_CHECK(outcome.status == SelectionStatus::Selected);
    LFF_CHECK_EQ(outcome.disproven_count, std::size_t{2});
    LFF_CHECK_EQ(outcome.eligible_count, std::size_t{1});
    LFF_CHECK_EQ(outcome.assessments[*outcome.selected_index].candidate.link.value(),
                 std::string("gamma"));
    expect_ok(validate_selection(input, outcome), "mixed selection");
}

LFF_TEST(unit, indeterminate_evidence_never_selects) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    const ObservationState states[] = {ObservationState::Unknown};
    const FreshnessClass freshnesses[] = {FreshnessClass::Current, FreshnessClass::Expired,
                                          FreshnessClass::PriorIncarnation, FreshnessClass::Future};
    for (ObservationState state : states) {
        for (FreshnessClass freshness : freshnesses) {
            SelectionInput input = make_input(fixture, "alpha", 1);
            input.candidates.push_back(resolved(fixture, "beta", 2, state, 1000, 500, 0, true,
                                                freshness, 10));
            const SelectionOutcome outcome = select_replacement(input);
            LFF_CHECK(outcome.status == SelectionStatus::Indeterminate);
            LFF_CHECK_EQ(outcome.eligible_count, std::size_t{0});
            LFF_CHECK(!outcome.selected_index.has_value());
            expect_ok(validate_selection(input, outcome), "indeterminate selection");
        }
    }
    // Low confidence is undecided, not disproven.
    SelectionInput input = make_input(fixture, "alpha", 1);
    input.candidates.push_back(resolved(fixture, "beta", 2, ObservationState::Up, 10, 500, 0, true,
                                        FreshnessClass::Current, 0));
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::Indeterminate);
    LFF_CHECK(outcome.assessments[0].reason == ReasonCode::EvidenceLowConfidence);
}

LFF_TEST(unit, missing_evidence_is_indeterminate_not_refusal) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    SelectionInput input = make_input(fixture, "alpha", 1);
    ResolvedCandidate entry;
    entry.candidate.fabric = fixture.fabric;
    entry.candidate.link = LinkName::parse("beta").value();
    entry.link_generation = LinkGeneration::from(2);
    entry.evidence_present = false;
    input.candidates.push_back(entry);
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::Indeterminate);
    LFF_CHECK(outcome.assessments[0].reason == ReasonCode::EvidenceMissing);
}

LFF_TEST(unit, stale_replacement_generation_is_indeterminate) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    SelectionInput input = make_input(fixture, "alpha", 1);
    ResolvedCandidate entry = resolved(fixture, "beta", 7, ObservationState::Up, 1000, 500, 0, true,
                                       FreshnessClass::Current, 0);
    entry.generation_asserted = true;
    entry.generation_matched = false;
    input.candidates.push_back(entry);
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::Indeterminate);
    LFF_CHECK(outcome.assessments[0].reason == ReasonCode::ReplacementGenerationMismatch);
}

LFF_TEST(unit, conflicted_candidate_is_never_eligible) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    SelectionInput input = make_input(fixture, "alpha", 1);
    ResolvedCandidate entry = resolved(fixture, "beta", 2, ObservationState::Up, 1000, 900, 0, true,
                                       FreshnessClass::Current, 0);
    entry.conflicted = true;
    input.candidates.push_back(entry);
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::Indeterminate);
    LFF_CHECK(outcome.assessments[0].reason == ReasonCode::EvidenceConflicted);
}

LFF_TEST(unit, objective_is_total_and_antisymmetric) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    CandidateAssessment a;
    a.candidate = LinkKey{fixture.fabric, LinkName::parse("beta").value()};
    a.link_generation = LinkGeneration::from(2);
    a.confidence = make_confidence(900);
    a.age_ticks = 5;
    a.capacity_units = 100;
    a.cost_units = 10;
    a.adjacency_authorized = true;
    CandidateAssessment b = a;
    b.candidate = LinkKey{fixture.fabric, LinkName::parse("gamma").value()};
    const ObjectiveKey ka = ObjectiveKey::from(a);
    const ObjectiveKey kb = ObjectiveKey::from(b);
    LFF_CHECK(objective_better(ka, kb));
    LFF_CHECK(!objective_better(kb, ka));
    LFF_CHECK(!objective_better(ka, ka));
    CandidateAssessment same = a;
    LFF_CHECK(!objective_better(ObjectiveKey::from(same), ka));
    LFF_CHECK(!objective_better(ka, ObjectiveKey::from(same)));
}

LFF_TEST(unit, selection_is_independent_of_candidate_order) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    SelectionInput base = make_input(fixture, "alpha", 1);
    base.candidates.push_back(resolved(fixture, "beta", 2, ObservationState::Up, 900, 900, 5, true,
                                       FreshnessClass::Current, 1));
    base.candidates.push_back(resolved(fixture, "gamma", 3, ObservationState::Up, 950, 100, 5, true,
                                       FreshnessClass::Current, 2));
    base.candidates.push_back(resolved(fixture, "delta", 4, ObservationState::Down, 999, 9000, 0,
                                       true, FreshnessClass::Current, 0));
    base.candidates.push_back(resolved(fixture, "epsilon", 5, ObservationState::Up, 950, 100, 5,
                                       true, FreshnessClass::Current, 2));
    const SelectionOutcome reference = select_replacement(base);
    LFF_CHECK(reference.status == SelectionStatus::Selected);
    const std::string expected =
        reference.assessments[*reference.selected_index].candidate.link.value();

    Random random(0xC0FFEEu);
    for (int iteration = 0; iteration < 200; ++iteration) {
        SelectionInput shuffled = base;
        for (std::size_t i = shuffled.candidates.size(); i > 1; --i) {
            const std::size_t j = static_cast<std::size_t>(random.below(i));
            std::swap(shuffled.candidates[i - 1], shuffled.candidates[j]);
        }
        const SelectionOutcome outcome = select_replacement(shuffled);
        LFF_CHECK(outcome.status == SelectionStatus::Selected);
        const std::string actual =
            outcome.assessments[*outcome.selected_index].candidate.link.value();
        if (actual != expected) {
            LFF_FAIL("order-dependent selection: " + actual + " != " + expected);
        }
        // The assessment list is canonically ordered regardless of input order.
        for (std::size_t i = 1; i < outcome.assessments.size(); ++i) {
            LFF_CHECK(candidate_assessment_less(outcome.assessments[i - 1],
                                                outcome.assessments[i]) ||
                      !candidate_assessment_less(outcome.assessments[i],
                                                 outcome.assessments[i - 1]));
        }
    }
}

LFF_TEST(unit, differential_against_reference_solver) {
    Random random(0x5EED1234u);
    std::size_t selections = 0;
    std::size_t infeasible = 0;
    std::size_t indeterminate = 0;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        FabricFixture fixture = make_fabric("difffabric", 1, {{"alpha", 1, 400}});
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.policy.min_replacement_capacity = static_cast<std::uint32_t>(random.below(3));
        input.policy.min_replacement_confidence = make_confidence(random.below32(1000));
        input.policy.require_adjacency_authorization = random.chance(1, 2);
        input.policy.allow_degraded_replacement = random.chance(1, 3);
        input.min_replacement_capacity = input.policy.min_replacement_capacity;

        const std::uint32_t count = 1 + random.below32(6);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::string name = "link-" + std::to_string(i);
            ResolvedCandidate entry = resolved(
                fixture, name, 1 + random.below(4),
                static_cast<ObservationState>(random.below32(kObservationStateCount)),
                random.below32(1001), random.below32(1000), random.below32(50),
                random.chance(1, 2),
                static_cast<FreshnessClass>(random.below32(kFreshnessClassCount)),
                random.below(100));
            if (random.chance(1, 6)) {
                entry.evidence_present = false;
            }
            if (random.chance(1, 8)) {
                entry.generation_asserted = true;
                entry.generation_matched = random.chance(1, 2);
            }
            if (random.chance(1, 12)) {
                entry.conflicted = true;
            }
            input.candidates.push_back(entry);
        }
        std::sort(input.candidates.begin(), input.candidates.end(),
                  [](const ResolvedCandidate& a, const ResolvedCandidate& b) {
                      return a.candidate < b.candidate;
                  });

        const SelectionOutcome outcome = select_replacement(input);
        const ReferenceResult reference = reference_select(input);
        expect_ok(validate_selection(input, outcome), "differential selection");

        switch (outcome.status) {
            case SelectionStatus::Selected: {
                ++selections;
                if (!reference.has_selection) {
                    LFF_FAIL(format_string("seed-case %d: selector chose but reference found none",
                                           iteration));
                }
                if (outcome.assessments[*outcome.selected_index].candidate !=
                    input.candidates[reference.selected].candidate) {
                    LFF_FAIL(format_string("seed-case %d: selector disagrees with reference", iteration));
                }
                LFF_CHECK_EQ(outcome.eligible_count, reference.eligible);
                break;
            }
            case SelectionStatus::ProvenInfeasible:
                ++infeasible;
                LFF_CHECK(!reference.has_selection);
                LFF_CHECK_EQ(reference.indeterminate, std::size_t{0});
                LFF_CHECK_EQ(reference.disproven, input.candidates.size());
                break;
            case SelectionStatus::Indeterminate:
                ++indeterminate;
                LFF_CHECK(!reference.has_selection);
                LFF_CHECK(reference.indeterminate > 0);
                break;
            case SelectionStatus::EmptyCandidateSet:
            default:
                LFF_FAIL("unexpected empty candidate set");
        }
    }
    // The corpus must actually exercise every branch of the trichotomy.
    LFF_CHECK(selections > 100);
    LFF_CHECK(infeasible > 10);
    LFF_CHECK(indeterminate > 10);
}

// Instances built to defeat obvious heuristics.
LFF_TEST(unit, adversarial_instances_defeat_greedy_heuristics) {
    FabricFixture fixture = make_fabric("adv-fabric", 1, {{"alpha", 1, 100}});

    // "Take the largest capacity" loses: the largest alternate has stale
    // evidence, so a smaller but current alternate must win.
    {
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.candidates.push_back(resolved(fixture, "big", 2, ObservationState::Up, 1000, 100000, 0,
                                            true, FreshnessClass::Expired, 999));
        input.candidates.push_back(resolved(fixture, "small", 3, ObservationState::Up, 1000, 200, 0,
                                            true, FreshnessClass::Current, 0));
        const SelectionOutcome outcome = select_replacement(input);
        LFF_CHECK(outcome.status == SelectionStatus::Selected);
        LFF_CHECK_EQ(outcome.assessments[*outcome.selected_index].candidate.link.value(),
                     std::string("small"));
    }
    // "Take the first usable" loses: ordering must not matter.
    {
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.candidates.push_back(resolved(fixture, "first", 2, ObservationState::Up, 800, 900, 0,
                                            true, FreshnessClass::Current, 4));
        input.candidates.push_back(resolved(fixture, "best", 3, ObservationState::Up, 1000, 900, 0,
                                            true, FreshnessClass::Current, 4));
        const SelectionOutcome outcome = select_replacement(input);
        LFF_CHECK_EQ(outcome.assessments[*outcome.selected_index].candidate.link.value(),
                     std::string("best"));
    }
    // "Take the cheapest" loses to capacity, which precedes cost.
    {
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.candidates.push_back(resolved(fixture, "cheap", 2, ObservationState::Up, 1000, 100, 1,
                                            true, FreshnessClass::Current, 0));
        input.candidates.push_back(resolved(fixture, "roomy", 3, ObservationState::Up, 1000, 500, 9,
                                            true, FreshnessClass::Current, 0));
        const SelectionOutcome outcome = select_replacement(input);
        LFF_CHECK_EQ(outcome.assessments[*outcome.selected_index].candidate.link.value(),
                     std::string("roomy"));
    }
    // "Take the freshest" loses to confidence, which precedes age.
    {
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.candidates.push_back(resolved(fixture, "fresh", 2, ObservationState::Up, 900, 500, 0,
                                            true, FreshnessClass::Current, 0));
        input.candidates.push_back(resolved(fixture, "certain", 3, ObservationState::Up, 1000, 500, 0,
                                            true, FreshnessClass::Current, 3));
        const SelectionOutcome outcome = select_replacement(input);
        LFF_CHECK_EQ(outcome.assessments[*outcome.selected_index].candidate.link.value(),
                     std::string("certain"));
    }
    // Canonical tie-break: everything equal but the link name.
    {
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.candidates.push_back(resolved(fixture, "zeta", 2, ObservationState::Up, 1000, 500, 0,
                                            true, FreshnessClass::Current, 0));
        input.candidates.push_back(resolved(fixture, "eta", 2, ObservationState::Up, 1000, 500, 0,
                                            true, FreshnessClass::Current, 0));
        const SelectionOutcome outcome = select_replacement(input);
        LFF_CHECK_EQ(outcome.assessments[*outcome.selected_index].candidate.link.value(),
                     std::string("eta"));
    }
    // Degraded alternates are refused by policy, not silently accepted.
    {
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.candidates.push_back(resolved(fixture, "degraded", 2, ObservationState::Degraded, 1000,
                                            500, 0, true, FreshnessClass::Current, 0));
        SelectionOutcome outcome = select_replacement(input);
        LFF_CHECK(outcome.status == SelectionStatus::ProvenInfeasible);
        input.policy.allow_degraded_replacement = true;
        outcome = select_replacement(input);
        LFF_CHECK(outcome.status == SelectionStatus::Selected);
    }
    // Missing adjacency authorization is a conclusive disqualifier only while
    // the observation is current and confident.
    {
        SelectionInput input = make_input(fixture, "alpha", 1);
        input.policy.require_adjacency_authorization = true;
        input.candidates.push_back(resolved(fixture, "unauthorized", 2, ObservationState::Up, 1000,
                                            500, 0, false, FreshnessClass::Current, 0));
        const SelectionOutcome outcome = select_replacement(input);
        LFF_CHECK(outcome.status == SelectionStatus::ProvenInfeasible);
        LFF_CHECK(outcome.assessments[0].reason == ReasonCode::ReplacementNotAdjacencyAuthorized);
    }
}

LFF_TEST(unit, validate_selection_rejects_inconsistent_outcomes) {
    FabricFixture fixture = make_fabric("selector-fabric", 1, {{"alpha", 1, 100}});
    SelectionInput input = make_input(fixture, "alpha", 1);
    input.candidates.push_back(resolved(fixture, "beta", 2, ObservationState::Up, 1000, 500, 0, true,
                                        FreshnessClass::Current, 0));
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::Selected);

    SelectionOutcome tampered = outcome;
    tampered.selected_index = 5;
    LFF_CHECK(!validate_selection(input, tampered).ok());
    tampered = outcome;
    tampered.assessments[0].verdict = Verdict::Indeterminate;
    LFF_CHECK(!validate_selection(input, tampered).ok());
    tampered = outcome;
    tampered.assessments.clear();
    LFF_CHECK(!validate_selection(input, tampered).ok());
    tampered = outcome;
    tampered.status = SelectionStatus::ProvenInfeasible;
    LFF_CHECK(!validate_selection(input, tampered).ok());

    // A candidate set with a duplicated link makes the tie-break non-total.
    SelectionInput duplicated = input;
    duplicated.candidates.push_back(input.candidates.front());
    LFF_CHECK(!validate_selection(duplicated, outcome).ok());

    // A disproven assessment with an inconclusive reason is impossible.
    SelectionOutcome fabricated;
    fabricated.status = SelectionStatus::ProvenInfeasible;
    fabricated.summary = ReasonCode::CertifiedInfeasible;
    fabricated.disproven_count = 1;
    CandidateAssessment assessment = outcome.assessments[0];
    assessment.verdict = Verdict::Disproven;
    assessment.reason = ReasonCode::EvidenceMissing;
    fabricated.assessments.push_back(assessment);
    LFF_CHECK(!validate_selection(input, fabricated).ok());
}