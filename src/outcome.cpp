// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/outcome.hpp"

#include <algorithm>

namespace lff {
namespace {

struct OutcomeName {
    Outcome value;
    std::string_view name;
};

constexpr OutcomeName kOutcomeNames[] = {
    {Outcome::Ok, "Ok"},
    {Outcome::Refused, "Refused"},
    {Outcome::Unknown, "Unknown"},
    {Outcome::Stale, "Stale"},
    {Outcome::Conflict, "Conflict"},
    {Outcome::Invalid, "Invalid"},
    {Outcome::Unsupported, "Unsupported"},
    {Outcome::NotFound, "NotFound"},
    {Outcome::LimitExceeded, "LimitExceeded"},
    {Outcome::Corrupt, "Corrupt"},
    {Outcome::Indeterminate, "Indeterminate"},
    {Outcome::Interrupted, "Interrupted"},
    {Outcome::Exhausted, "Exhausted"},
    {Outcome::IoError, "IoError"},
    {Outcome::Closed, "Closed"},
    {Outcome::Unauthenticated, "Unauthenticated"},
};

struct ReasonName {
    ReasonCode value;
    std::string_view name;
};

constexpr ReasonName kReasonNames[] = {
    {ReasonCode::None, "None"},
    {ReasonCode::InvalidArgument, "InvalidArgument"},
    {ReasonCode::MalformedEncoding, "MalformedEncoding"},
    {ReasonCode::UnsupportedValue, "UnsupportedValue"},
    {ReasonCode::LimitExceeded, "LimitExceeded"},
    {ReasonCode::TrailingBytes, "TrailingBytes"},
    {ReasonCode::ChecksumMismatch, "ChecksumMismatch"},
    {ReasonCode::UnsupportedFormatVersion, "UnsupportedFormatVersion"},
    {ReasonCode::CorruptRecord, "CorruptRecord"},
    {ReasonCode::SequenceRegression, "SequenceRegression"},
    {ReasonCode::TornTailRecovered, "TornTailRecovered"},
    {ReasonCode::NullField, "NullField"},
    {ReasonCode::UnknownFabric, "UnknownFabric"},
    {ReasonCode::UnknownLink, "UnknownLink"},
    {ReasonCode::UnknownGeneration, "UnknownGeneration"},
    {ReasonCode::GenerationMismatch, "GenerationMismatch"},
    {ReasonCode::PolicyGenerationMismatch, "PolicyGenerationMismatch"},
    {ReasonCode::TopologyGenerationMismatch, "TopologyGenerationMismatch"},
    {ReasonCode::SubjectGenerationMismatch, "SubjectGenerationMismatch"},
    {ReasonCode::ReplacementGenerationMismatch, "ReplacementGenerationMismatch"},
    {ReasonCode::EpochMismatch, "EpochMismatch"},
    {ReasonCode::IncarnationMismatch, "IncarnationMismatch"},
    {ReasonCode::StaleAuthority, "StaleAuthority"},
    {ReasonCode::SessionMismatch, "SessionMismatch"},
    {ReasonCode::SessionSequenceRegressed, "SessionSequenceRegressed"},
    {ReasonCode::SessionSequenceReplayed, "SessionSequenceReplayed"},
    {ReasonCode::CoordinatorIncarnationMismatch, "CoordinatorIncarnationMismatch"},
    {ReasonCode::PublisherIncarnationMismatch, "PublisherIncarnationMismatch"},
    {ReasonCode::WorkerIncarnationMismatch, "WorkerIncarnationMismatch"},
    {ReasonCode::PreRestartAuthority, "PreRestartAuthority"},
    {ReasonCode::EvidenceMissing, "EvidenceMissing"},
    {ReasonCode::EvidenceUnknown, "EvidenceUnknown"},
    {ReasonCode::EvidenceStale, "EvidenceStale"},
    {ReasonCode::EvidenceConflicted, "EvidenceConflicted"},
    {ReasonCode::EvidenceLowConfidence, "EvidenceLowConfidence"},
    {ReasonCode::EvidenceDigestMismatch, "EvidenceDigestMismatch"},
    {ReasonCode::EvidenceFromPriorIncarnation, "EvidenceFromPriorIncarnation"},
    {ReasonCode::SubjectNotFailed, "SubjectNotFailed"},
    {ReasonCode::SubjectUnknown, "SubjectUnknown"},
    {ReasonCode::ReplacementNotEligible, "ReplacementNotEligible"},
    {ReasonCode::ReplacementInsufficientCapacity, "ReplacementInsufficientCapacity"},
    {ReasonCode::ReplacementNotAdjacencyAuthorized, "ReplacementNotAdjacencyAuthorized"},
    {ReasonCode::ReplacementStateUnusable, "ReplacementStateUnusable"},
    {ReasonCode::ReplacementIsSubject, "ReplacementIsSubject"},
    {ReasonCode::NoEligibleReplacement, "NoEligibleReplacement"},
    {ReasonCode::SelectionIndeterminate, "SelectionIndeterminate"},
    {ReasonCode::SelectedHighestObjective, "SelectedHighestObjective"},
    {ReasonCode::CertifiedInfeasible, "CertifiedInfeasible"},
    {ReasonCode::CandidateSetEmpty, "CandidateSetEmpty"},
    {ReasonCode::AlreadyAuthoritative, "AlreadyAuthoritative"},
    {ReasonCode::AttemptInFlight, "AttemptInFlight"},
    {ReasonCode::AttemptCompleted, "AttemptCompleted"},
    {ReasonCode::AttemptUnknown, "AttemptUnknown"},
    {ReasonCode::DuplicateCompletion, "DuplicateCompletion"},
    {ReasonCode::LateAcknowledgement, "LateAcknowledgement"},
    {ReasonCode::ReplayDetected, "ReplayDetected"},
    {ReasonCode::IdempotentReplay, "IdempotentReplay"},
    {ReasonCode::RevalidationRequired, "RevalidationRequired"},
    {ReasonCode::FenceRequired, "FenceRequired"},
    {ReasonCode::Fenced, "Fenced"},
    {ReasonCode::AttemptsExhausted, "AttemptsExhausted"},
    {ReasonCode::BackendRejected, "BackendRejected"},
    {ReasonCode::BackendUnknown, "BackendUnknown"},
    {ReasonCode::EffectUnverified, "EffectUnverified"},
    {ReasonCode::EffectVerified, "EffectVerified"},
    {ReasonCode::AcknowledgementNotEffect, "AcknowledgementNotEffect"},
    {ReasonCode::RollbackRequested, "RollbackRequested"},
    {ReasonCode::RollbackNotAuthorized, "RollbackNotAuthorized"},
    {ReasonCode::RollbackCompleted, "RollbackCompleted"},
    {ReasonCode::RestoreRequiresNewGeneration, "RestoreRequiresNewGeneration"},
    {ReasonCode::OriginalLinkRestored, "OriginalLinkRestored"},
    {ReasonCode::InterruptedBeforeCommit, "InterruptedBeforeCommit"},
    {ReasonCode::InterruptedAfterCommit, "InterruptedAfterCommit"},
    {ReasonCode::InterruptedAfterApply, "InterruptedAfterApply"},
    {ReasonCode::EpochAdvancedOnBoot, "EpochAdvancedOnBoot"},
    {ReasonCode::DurableLineagePreserved, "DurableLineagePreserved"},
    {ReasonCode::DynamicStateNotRestored, "DynamicStateNotRestored"},
    {ReasonCode::ProtocolVersionMismatch, "ProtocolVersionMismatch"},
    {ReasonCode::FrameTooLarge, "FrameTooLarge"},
    {ReasonCode::UnknownMessageType, "UnknownMessageType"},
    {ReasonCode::StickyDecodeFailure, "StickyDecodeFailure"},
    {ReasonCode::ConnectionClosed, "ConnectionClosed"},
    {ReasonCode::SocketError, "SocketError"},
    {ReasonCode::NotListening, "NotListening"},
    {ReasonCode::Shutdown, "Shutdown"},
    {ReasonCode::TruncatedFrame, "TruncatedFrame"},
    {ReasonCode::ReservedBitsSet, "ReservedBitsSet"},
    {ReasonCode::PolicyRejected, "PolicyRejected"},
    {ReasonCode::PolicyConfidenceUnreachable, "PolicyConfidenceUnreachable"},
    {ReasonCode::PolicyDisabled, "PolicyDisabled"},
    {ReasonCode::InternalError, "InternalError"},
    {ReasonCode::CheckedArithmeticOverflow, "CheckedArithmeticOverflow"},
    {ReasonCode::FeatureUnsupported, "FeatureUnsupported"},
    {ReasonCode::SyntheticFixture, "SyntheticFixture"},
};

}  // namespace

std::string_view outcome_name(Outcome value) noexcept {
    for (const OutcomeName& entry : kOutcomeNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedOutcome";
}

bool outcome_parse(std::string_view text, Outcome& out) noexcept {
    for (const OutcomeName& entry : kOutcomeNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

bool outcome_is_ok(Outcome value) noexcept {
    return value == Outcome::Ok;
}

std::string_view reason_code_name(ReasonCode value) noexcept {
    for (const ReasonName& entry : kReasonNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedReasonCode";
}

bool reason_code_parse(std::string_view text, ReasonCode& out) noexcept {
    for (const ReasonName& entry : kReasonNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

bool operator==(const Reason& a, const Reason& b) noexcept {
    return a.code == b.code && a.detail == b.detail;
}

bool operator<(const Reason& a, const Reason& b) noexcept {
    if (a.code != b.code) {
        return a.code < b.code;
    }
    return a.detail < b.detail;
}

bool Status::has(ReasonCode code) const noexcept {
    return std::any_of(reasons_.begin(), reasons_.end(),
                       [code](const Reason& reason) { return reason.code == code; });
}

std::string Status::to_string() const {
    std::string out(outcome_name(outcome_));
    if (!reasons_.empty()) {
        out.push_back('(');
        for (std::size_t i = 0; i < reasons_.size(); ++i) {
            if (i != 0) {
                out.append(", ");
            }
            out.append(reason_code_name(reasons_[i].code));
            if (!reasons_[i].detail.empty()) {
                out.push_back('=');
                out.append(reasons_[i].detail);
            }
        }
        out.push_back(')');
    }
    return out;
}

std::string Status::canonical_key() const {
    std::string out(outcome_name(outcome_));
    out.push_back('|');
    for (const Reason& reason : reasons_) {
        out.append(reason_code_name(reason.code));
        out.push_back(':');
        out.append(reason.detail);
        out.push_back(';');
    }
    return out;
}

std::string to_string(const Status& status) {
    return status.to_string();
}

}  // namespace lff
