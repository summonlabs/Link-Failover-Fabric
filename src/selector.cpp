// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/selector.hpp"

#include "lff/bytes.hpp"

#include <algorithm>
#include <string>

namespace lff {
namespace {

struct SelectionStatusName {
    SelectionStatus value;
    std::string_view name;
};

constexpr SelectionStatusName kSelectionStatusNames[] = {
    {SelectionStatus::Selected, "Selected"},
    {SelectionStatus::ProvenInfeasible, "ProvenInfeasible"},
    {SelectionStatus::Indeterminate, "Indeterminate"},
    {SelectionStatus::EmptyCandidateSet, "EmptyCandidateSet"},
};

// A disqualifying property is conclusive only when it was observed under
// current evidence. Everything else leaves the candidate undecided.
bool reason_is_conclusive(ReasonCode code) noexcept {
    switch (code) {
        case ReasonCode::ReplacementIsSubject:
        case ReasonCode::ReplacementStateUnusable:
        case ReasonCode::ReplacementNotEligible:
        case ReasonCode::ReplacementInsufficientCapacity:
        case ReasonCode::ReplacementNotAdjacencyAuthorized:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::string_view selection_status_name(SelectionStatus value) noexcept {
    for (const SelectionStatusName& entry : kSelectionStatusNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedSelectionStatus";
}

ObjectiveKey ObjectiveKey::from(const CandidateAssessment& assessment) {
    ObjectiveKey key;
    key.adjacency_authorized = assessment.adjacency_authorized;
    key.confidence = assessment.confidence.value();
    key.age_ticks = assessment.age_ticks;
    key.capacity_units = assessment.capacity_units;
    key.cost_units = assessment.cost_units;
    key.link_generation = assessment.link_generation.value();
    key.link_name = assessment.candidate.link.value();
    return key;
}

bool objective_better(const ObjectiveKey& a, const ObjectiveKey& b) noexcept {
    if (a.adjacency_authorized != b.adjacency_authorized) {
        return a.adjacency_authorized;
    }
    if (a.confidence != b.confidence) {
        return a.confidence > b.confidence;
    }
    if (a.age_ticks != b.age_ticks) {
        return a.age_ticks < b.age_ticks;
    }
    if (a.capacity_units != b.capacity_units) {
        return a.capacity_units > b.capacity_units;
    }
    if (a.cost_units != b.cost_units) {
        return a.cost_units < b.cost_units;
    }
    if (a.link_generation != b.link_generation) {
        return a.link_generation > b.link_generation;
    }
    // Total: candidate link identity is unique inside the accepted set, so this
    // final component is never a tie.
    return a.link_name < b.link_name;
}

SelectionOutcome select_replacement(const SelectionInput& input) {
    SelectionOutcome outcome;
    outcome.assessments.reserve(input.candidates.size());

    if (input.candidates.empty()) {
        outcome.status = SelectionStatus::EmptyCandidateSet;
        outcome.summary = ReasonCode::CandidateSetEmpty;
        return outcome;
    }

    for (const ResolvedCandidate& resolved : input.candidates) {
        CandidateAssessment assessment;
        assessment.candidate = resolved.candidate;
        assessment.link_generation = resolved.link_generation;
        assessment.state = resolved.report.state;
        assessment.confidence = resolved.report.confidence;
        assessment.freshness = resolved.freshness;
        assessment.age_ticks = resolved.age_ticks;
        assessment.capacity_units = resolved.report.capacity_units;
        assessment.cost_units = resolved.report.cost_units;
        assessment.adjacency_authorized = resolved.report.adjacency_authorized;
        assessment.evidence_digest = resolved.evidence_digest;
        assessment.verdict = Verdict::Indeterminate;

        if (resolved.candidate == input.subject) {
            assessment.verdict = Verdict::Disproven;
            assessment.reason = ReasonCode::ReplacementIsSubject;
            assessment.detail = "a link cannot replace itself";
        } else if (resolved.conflicted) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::EvidenceConflicted;
            assessment.detail = "current qualified observations contradict each other";
        } else if (!resolved.evidence_present) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::EvidenceMissing;
            assessment.detail = "no current evidence for this candidate";
        } else if (resolved.generation_asserted && !resolved.generation_matched) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::ReplacementGenerationMismatch;
            assessment.detail = "asserted replacement generation does not match retained evidence";
        } else if (resolved.freshness == FreshnessClass::PriorIncarnation) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::EvidenceFromPriorIncarnation;
            assessment.detail = "evidence was received by a previous coordinator incarnation";
        } else if (resolved.freshness == FreshnessClass::Expired) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::EvidenceStale;
            assessment.detail = "evidence is older than the policy window";
        } else if (resolved.freshness == FreshnessClass::Future) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::EvidenceStale;
            assessment.detail = "evidence receipt tick is ahead of the coordinator clock";
        } else if (resolved.report.confidence.value() < input.policy.min_replacement_confidence.value()) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::EvidenceLowConfidence;
            assessment.detail = "evidence confidence is below the policy threshold";
        } else if (resolved.report.state == ObservationState::Unknown) {
            assessment.verdict = Verdict::Indeterminate;
            assessment.reason = ReasonCode::EvidenceUnknown;
            assessment.detail = "publisher reported an unknown state";
        } else if (resolved.report.state == ObservationState::Down ||
                   resolved.report.state == ObservationState::Unusable) {
            assessment.verdict = Verdict::Disproven;
            assessment.reason = ReasonCode::ReplacementStateUnusable;
            assessment.detail = "current evidence shows the candidate is not usable";
        } else if (resolved.report.state == ObservationState::Degraded &&
                   !input.policy.allow_degraded_replacement) {
            assessment.verdict = Verdict::Disproven;
            assessment.reason = ReasonCode::ReplacementNotEligible;
            assessment.detail = "degraded candidates are not accepted by policy";
        } else if (resolved.report.capacity_units < input.min_replacement_capacity) {
            assessment.verdict = Verdict::Disproven;
            assessment.reason = ReasonCode::ReplacementInsufficientCapacity;
            assessment.detail = "declared capacity is below the required minimum";
        } else if (input.policy.require_adjacency_authorization &&
                   !resolved.report.adjacency_authorized) {
            assessment.verdict = Verdict::Disproven;
            assessment.reason = ReasonCode::ReplacementNotAdjacencyAuthorized;
            assessment.detail = "adjacent authority has not authorized this link";
        } else {
            assessment.verdict = Verdict::Eligible;
            assessment.reason = ReasonCode::SelectedHighestObjective;
            assessment.detail.clear();
        }

        outcome.assessments.push_back(std::move(assessment));
    }

    // Canonical order: independent of the order the caller supplied.
    std::sort(outcome.assessments.begin(), outcome.assessments.end(), candidate_assessment_less);

    for (const CandidateAssessment& assessment : outcome.assessments) {
        switch (assessment.verdict) {
            case Verdict::Eligible:
                ++outcome.eligible_count;
                break;
            case Verdict::Disproven:
                ++outcome.disproven_count;
                break;
            case Verdict::Indeterminate:
            default:
                ++outcome.indeterminate_count;
                break;
        }
    }

    if (outcome.eligible_count == 0) {
        if (outcome.indeterminate_count == 0) {
            outcome.status = SelectionStatus::ProvenInfeasible;
            outcome.summary = ReasonCode::CertifiedInfeasible;
        } else {
            outcome.status = SelectionStatus::Indeterminate;
            outcome.summary = ReasonCode::SelectionIndeterminate;
        }
        return outcome;
    }

    std::size_t best = outcome.assessments.size();
    ObjectiveKey best_key;
    for (std::size_t i = 0; i < outcome.assessments.size(); ++i) {
        if (outcome.assessments[i].verdict != Verdict::Eligible) {
            continue;
        }
        const ObjectiveKey key = ObjectiveKey::from(outcome.assessments[i]);
        if (best == outcome.assessments.size() || objective_better(key, best_key)) {
            best = i;
            best_key = key;
        }
    }

    if (best == outcome.assessments.size()) {
        outcome.status = SelectionStatus::Indeterminate;
        outcome.summary = ReasonCode::SelectionIndeterminate;
        return outcome;
    }

    outcome.status = SelectionStatus::Selected;
    outcome.summary = ReasonCode::SelectedHighestObjective;
    outcome.selected_index = best;
    return outcome;
}

Status validate_selection(const SelectionInput& input, const SelectionOutcome& outcome) {
    if (outcome.assessments.size() != input.candidates.size()) {
        return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                               "assessment count does not match candidate count");
    }
    // Inputs must name each candidate link at most once; otherwise the final
    // objective tie-break is not total. The order of the input does not matter:
    // the selector canonicalises it before deciding.
    for (std::size_t i = 0; i < input.candidates.size(); ++i) {
        for (std::size_t j = i + 1; j < input.candidates.size(); ++j) {
            if (input.candidates[i].candidate == input.candidates[j].candidate) {
                return Status::failure(Outcome::Indeterminate, ReasonCode::InvalidArgument,
                                       "candidate set contains a duplicate link");
            }
        }
    }
    for (const CandidateAssessment& assessment : outcome.assessments) {
        bool found = false;
        for (const ResolvedCandidate& resolved : input.candidates) {
            if (resolved.candidate == assessment.candidate &&
                resolved.link_generation == assessment.link_generation) {
                found = true;
                break;
            }
        }
        if (!found) {
            return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                                   "assessment names a candidate that was not in the input");
        }
        if (assessment.verdict == Verdict::Disproven && !reason_is_conclusive(assessment.reason)) {
            return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                                   "a candidate was disproven for a non-conclusive reason");
        }
    }

    switch (outcome.status) {
        case SelectionStatus::EmptyCandidateSet:
            if (!input.candidates.empty()) {
                return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                                       "empty-candidate outcome with a non-empty candidate set");
            }
            return Status::success();

        case SelectionStatus::ProvenInfeasible:
            if (outcome.eligible_count != 0 || outcome.indeterminate_count != 0) {
                return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                                       "infeasibility claimed while candidates remain undecided");
            }
            return Status::success();

        case SelectionStatus::Indeterminate:
            if (outcome.eligible_count != 0) {
                return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                                       "indeterminate outcome with an eligible candidate present");
            }
            return Status::success();

        case SelectionStatus::Selected:
        default:
            break;
    }

    if (!outcome.selected_index.has_value() || *outcome.selected_index >= outcome.assessments.size()) {
        return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                               "selected outcome without a valid selected index");
    }
    const CandidateAssessment& chosen = outcome.assessments[*outcome.selected_index];
    if (chosen.verdict != Verdict::Eligible) {
        return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                               "selected candidate is not eligible");
    }
    if (chosen.candidate == input.subject) {
        return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                               "selected candidate is the failed subject");
    }
    // Independent re-derivation of the maximum, without reusing the selector's
    // own bookkeeping.
    const ObjectiveKey chosen_key = ObjectiveKey::from(chosen);
    for (const CandidateAssessment& assessment : outcome.assessments) {
        if (assessment.verdict != Verdict::Eligible) {
            continue;
        }
        const ObjectiveKey key = ObjectiveKey::from(assessment);
        if (objective_better(key, chosen_key)) {
            return Status::failure(Outcome::Indeterminate, ReasonCode::InternalError,
                                   "selected candidate is not the maximum of the objective");
        }
    }
    return Status::success();
}

std::string selection_outcome_to_string(const SelectionOutcome& outcome) {
    std::string out(selection_status_name(outcome.status));
    out.append(" eligible=");
    out.append(std::to_string(outcome.eligible_count));
    out.append(" disproven=");
    out.append(std::to_string(outcome.disproven_count));
    out.append(" indeterminate=");
    out.append(std::to_string(outcome.indeterminate_count));
    if (outcome.selected_index.has_value() && *outcome.selected_index < outcome.assessments.size()) {
        out.append(" selected=");
        out.append(outcome.assessments[*outcome.selected_index].candidate.to_string());
    }
    return out;
}

}  // namespace lff
