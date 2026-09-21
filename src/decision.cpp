// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/decision.hpp"

#include "lff/bytes.hpp"
#include "lff/version.hpp"

#include <string>

namespace lff {
namespace {

struct KindName {
    DecisionKind value;
    std::string_view name;
};

constexpr KindName kKindNames[] = {
    {DecisionKind::Observe, "Observe"},   {DecisionKind::Grant, "Grant"},
    {DecisionKind::Refuse, "Refuse"},     {DecisionKind::Rollback, "Rollback"},
    {DecisionKind::Revalidate, "Revalidate"},
    {DecisionKind::Indeterminate, "Indeterminate"},
    {DecisionKind::Acknowledge, "Acknowledge"},
    {DecisionKind::Verify, "Verify"},     {DecisionKind::Fence, "Fence"},
};

struct AssertionName {
    Assertion value;
    std::string_view name;
};

constexpr AssertionName kAssertionNames[] = {
    {Assertion::None, "None"},
    {Assertion::Observation, "Observation"},
    {Assertion::Eligibility, "Eligibility"},
    {Assertion::Recommendation, "Recommendation"},
    {Assertion::Authorization, "Authorization"},
    {Assertion::Acknowledgement, "Acknowledgement"},
    {Assertion::VerifiedEffect, "VerifiedEffect"},
};

void encode_reasons(Writer& writer, const std::vector<Reason>& reasons) {
    writer.u32(static_cast<std::uint32_t>(reasons.size()));
    for (const Reason& reason : reasons) {
        writer.u16(static_cast<std::uint16_t>(reason.code));
        writer.str(reason.detail, static_cast<std::uint32_t>(kMaxReasonDetail));
    }
}

bool decode_reasons(Reader& reader, std::vector<Reason>& out, std::size_t limit) {
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > limit) {
        return false;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t code = reader.u16();
        const std::string detail = reader.str(static_cast<std::uint32_t>(kMaxReasonDetail));
        if (!reader.ok() || code >= kReasonCodeCount) {
            return false;
        }
        out.emplace_back(static_cast<ReasonCode>(code), detail);
    }
    return true;
}

}  // namespace

std::string_view decision_kind_name(DecisionKind value) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedDecisionKind";
}

bool decision_kind_parse(std::string_view text, DecisionKind& out) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

std::string_view assertion_name(Assertion value) noexcept {
    for (const AssertionName& entry : kAssertionNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedAssertion";
}

bool assertion_parse(std::string_view text, Assertion& out) noexcept {
    for (const AssertionName& entry : kAssertionNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

std::uint16_t assertion_rank(Assertion value) noexcept {
    return static_cast<std::uint16_t>(value);
}

// ---------------------------------------------------------------------------
// FailoverDecision
// ---------------------------------------------------------------------------
Digest FailoverDecision::compute_id() const {
    Writer writer;
    writer.u16(kIdentityFormatVersion);
    writer.str("lff.decision");
    writer.str(fabric.value());
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u64(attempt_seq.value());
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.u16(static_cast<std::uint16_t>(assertion));
    writer.u64(lineage_seq.value());
    writer.bool8(has_replacement);
    if (has_replacement) {
        writer.str(replacement.fabric.value());
        writer.str(replacement.link.value());
        writer.u64(replacement_generation.value());
    }
    return sha256(writer.data().data(), writer.size());
}

std::string FailoverDecision::to_string() const {
    std::string out;
    out.append(decision_kind_name(kind));
    out.append("/");
    out.append(outcome_name(outcome));
    out.append(" assertion=");
    out.append(assertion_name(assertion));
    out.append(" subject=");
    out.append(subject.to_string());
    out.append("@");
    out.append(std::to_string(subject_generation.value()));
    out.append(" attempt=");
    out.append(std::to_string(attempt_seq.value()));
    if (has_replacement) {
        out.append(" replacement=");
        out.append(replacement.to_string());
        out.append("@");
        out.append(std::to_string(replacement_generation.value()));
    }
    out.append(" epoch=");
    out.append(std::to_string(epoch.value()));
    if (durable) {
        out.append(" lineage=");
        out.append(std::to_string(lineage_seq.value()));
    } else {
        out.append(" lineage=none");
    }
    if (!reasons.empty()) {
        out.append(" [");
        for (std::size_t i = 0; i < reasons.size(); ++i) {
            if (i != 0) {
                out.append(",");
            }
            out.append(reason_code_name(reasons[i].code));
        }
        out.append("]");
    }
    return out;
}

std::string FailoverDecision::canonical_key() const {
    std::string out;
    out.append(decision_kind_name(kind));
    out.push_back('|');
    out.append(outcome_name(outcome));
    out.push_back('|');
    out.append(assertion_name(assertion));
    out.push_back('|');
    out.append(subject.to_string());
    out.push_back('@');
    out.append(std::to_string(subject_generation.value()));
    out.push_back('|');
    out.append(std::to_string(attempt_seq.value()));
    out.push_back('|');
    if (has_replacement) {
        out.append(replacement.to_string());
        out.push_back('@');
        out.append(std::to_string(replacement_generation.value()));
    }
    out.push_back('|');
    out.append(std::to_string(epoch.value()));
    out.push_back('|');
    out.append(std::to_string(lineage_seq.value()));
    for (const Reason& reason : reasons) {
        out.push_back('|');
        out.append(reason_code_name(reason.code));
        out.push_back(':');
        out.append(reason.detail);
    }
    return out;
}

void FailoverDecision::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.digest(decision_id);
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.u16(static_cast<std::uint16_t>(assertion));
    writer.u16(static_cast<std::uint16_t>(outcome));
    writer.str(fabric.value());
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.bool8(has_replacement);
    if (has_replacement) {
        writer.str(replacement.fabric.value());
        writer.str(replacement.link.value());
        writer.u64(replacement_generation.value());
    }
    writer.u64(attempt_seq.value());
    writer.digest(attempt_id);
    writer.u64(epoch.value());
    writer.incarnation(coordinator);
    authority.encode(writer);
    writer.digest(evidence_digest);
    writer.u64(lineage_seq.value());
    writer.bool8(durable);
    writer.bool8(idempotent_replay);
    encode_reasons(writer, reasons);
    writer.u32(static_cast<std::uint32_t>(candidates.size()));
    for (const CandidateAssessment& assessment : candidates) {
        writer.str(assessment.candidate.fabric.value());
        writer.str(assessment.candidate.link.value());
        writer.u64(assessment.link_generation.value());
        writer.u16(static_cast<std::uint16_t>(assessment.verdict));
        writer.u16(static_cast<std::uint16_t>(assessment.reason));
        writer.str(assessment.detail, 256);
        writer.u16(static_cast<std::uint16_t>(assessment.state));
        writer.u32(assessment.confidence.value());
        writer.u16(static_cast<std::uint16_t>(assessment.freshness));
        writer.u64(assessment.age_ticks);
        writer.u32(assessment.capacity_units);
        writer.u32(assessment.cost_units);
        writer.bool8(assessment.adjacency_authorized);
        writer.digest(assessment.evidence_digest);
    }
}

bool FailoverDecision::decode(Reader& reader, FailoverDecision& out) {
    const std::uint16_t format_version = reader.u16();
    const Digest decision_id = reader.digest();
    const std::uint16_t kind = reader.u16();
    const std::uint16_t assertion = reader.u16();
    const std::uint16_t outcome = reader.u16();
    const std::string fabric = reader.str();
    const std::string subject_fabric = reader.str();
    const std::string subject_link = reader.str();
    const std::uint64_t subject_generation = reader.u64();
    const bool has_replacement = reader.bool8();
    std::string replacement_fabric;
    std::string replacement_link;
    std::uint64_t replacement_generation = 0;
    if (has_replacement) {
        replacement_fabric = reader.str();
        replacement_link = reader.str();
        replacement_generation = reader.u64();
    }
    const std::uint64_t attempt_seq = reader.u64();
    const Digest attempt_id = reader.digest();
    const std::uint64_t epoch = reader.u64();
    const Incarnation coordinator = reader.incarnation();
    AuthorityVector authority;
    const bool authority_ok = AuthorityVector::decode(reader, authority);
    const Digest evidence_digest = reader.digest();
    const std::uint64_t lineage_seq = reader.u64();
    const bool durable = reader.bool8();
    const bool idempotent_replay = reader.bool8();
    std::vector<Reason> reasons;
    const bool reasons_ok = decode_reasons(reader, reasons, kMaxDecisionReasons);
    const std::uint32_t candidate_count = reader.u32();
    if (!reader.ok() || !authority_ok || !reasons_ok) {
        return false;
    }
    if (format_version != kIdentityFormatVersion) {
        return false;
    }
    if (kind >= kDecisionKindCount || assertion >= kAssertionCount || outcome >= kOutcomeCount ||
        candidate_count > kMaxDecisionCandidates) {
        return false;
    }
    FailoverDecision value;
    if (!decode_name(fabric, value.fabric) ||
        !decode_name(subject_fabric, value.subject.fabric) ||
        !decode_name(subject_link, value.subject.link)) {
        return false;
    }
    value.decision_id = decision_id;
    value.kind = static_cast<DecisionKind>(kind);
    value.assertion = static_cast<Assertion>(assertion);
    value.outcome = static_cast<Outcome>(outcome);
    value.subject_generation = LinkGeneration::from(subject_generation);
    value.has_replacement = has_replacement;
    if (has_replacement) {
        if (!decode_name(replacement_fabric, value.replacement.fabric) ||
            !decode_name(replacement_link, value.replacement.link)) {
            return false;
        }
        value.replacement_generation = LinkGeneration::from(replacement_generation);
    }
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.attempt_id = attempt_id;
    value.epoch = Epoch::from(epoch);
    value.coordinator = coordinator;
    value.authority = authority;
    value.evidence_digest = evidence_digest;
    value.lineage_seq = LineageSeq::from(lineage_seq);
    value.durable = durable;
    value.idempotent_replay = idempotent_replay;
    value.reasons = std::move(reasons);
    value.candidates.reserve(candidate_count);
    for (std::uint32_t i = 0; i < candidate_count; ++i) {
        CandidateAssessment assessment;
        const std::string candidate_fabric = reader.str();
        const std::string candidate_link = reader.str();
        const std::uint64_t candidate_generation = reader.u64();
        const std::uint16_t verdict = reader.u16();
        const std::uint16_t reason = reader.u16();
        const std::string detail = reader.str(256);
        const std::uint16_t state = reader.u16();
        const std::uint32_t confidence = reader.u32();
        const std::uint16_t freshness = reader.u16();
        const std::uint64_t age = reader.u64();
        const std::uint32_t capacity = reader.u32();
        const std::uint32_t cost = reader.u32();
        const bool adjacency = reader.bool8();
        const Digest evidence = reader.digest();
        if (!reader.ok()) {
            return false;
        }
        if (verdict >= kVerdictCount || reason >= kReasonCodeCount || state >= kObservationStateCount ||
            freshness >= kFreshnessClassCount || confidence > kMaxConfidence) {
            return false;
        }
        if (!decode_name(candidate_fabric, assessment.candidate.fabric) ||
            !decode_name(candidate_link, assessment.candidate.link)) {
            return false;
        }
        assessment.link_generation = LinkGeneration::from(candidate_generation);
        assessment.verdict = static_cast<Verdict>(verdict);
        assessment.reason = static_cast<ReasonCode>(reason);
        assessment.detail = detail;
        assessment.state = static_cast<ObservationState>(state);
        assessment.confidence = Confidence::from(confidence);
        assessment.freshness = static_cast<FreshnessClass>(freshness);
        assessment.age_ticks = age;
        assessment.capacity_units = capacity;
        assessment.cost_units = cost;
        assessment.adjacency_authorized = adjacency;
        assessment.evidence_digest = evidence;
        value.candidates.push_back(std::move(assessment));
    }
    out = std::move(value);
    return true;
}

// ---------------------------------------------------------------------------
// Requests and reports
// ---------------------------------------------------------------------------
void FailoverRequest::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u32(static_cast<std::uint32_t>(candidates.size()));
    for (const ReplacementCandidate& candidate : candidates) {
        candidate.encode(writer);
    }
    expected.encode(writer);
    writer.digest(idempotency_key);
}

bool FailoverRequest::decode(Reader& reader, FailoverRequest& out) {
    const std::uint16_t format_version = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    if (count > kMaxPolicyCandidates) {
        return false;
    }
    const Result<FabricName> fabric_name = FabricName::parse(fabric);
    const Result<LinkName> link_name = LinkName::parse(link);
    if (!fabric_name.ok() || !link_name.ok()) {
        return false;
    }
    FailoverRequest value;
    value.subject.fabric = fabric_name.value();
    value.subject.link = link_name.value();
    value.subject_generation = LinkGeneration::from(generation);
    value.candidates.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ReplacementCandidate candidate;
        if (!ReplacementCandidate::decode(reader, candidate)) {
            return false;
        }
        value.candidates.push_back(std::move(candidate));
    }
    if (!AuthorityExpectation::decode(reader, value.expected)) {
        return false;
    }
    value.idempotency_key = reader.digest();
    if (!reader.ok()) {
        return false;
    }
    out = std::move(value);
    return true;
}

Digest FailoverRequest::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

void RollbackRequest::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u64(attempt_seq.value());
    expected.encode(writer);
    writer.digest(idempotency_key);
    writer.str(rationale, 256);
}

bool RollbackRequest::decode(Reader& reader, RollbackRequest& out) {
    const std::uint16_t format_version = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t attempt_seq = reader.u64();
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    const Result<FabricName> fabric_name = FabricName::parse(fabric);
    const Result<LinkName> link_name = LinkName::parse(link);
    if (!fabric_name.ok() || !link_name.ok()) {
        return false;
    }
    RollbackRequest value;
    value.subject.fabric = fabric_name.value();
    value.subject.link = link_name.value();
    value.subject_generation = LinkGeneration::from(generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    if (!AuthorityExpectation::decode(reader, value.expected)) {
        return false;
    }
    value.idempotency_key = reader.digest();
    value.rationale = reader.str(256);
    if (!reader.ok()) {
        return false;
    }
    out = std::move(value);
    return true;
}

Digest RollbackRequest::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

void RevalidationRequest::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u64(attempt_seq.value());
    writer.u16(static_cast<std::uint16_t>(verdict));
    writer.incarnation(observer_incarnation);
    writer.u64(observer_epoch.value());
    writer.str(rationale, 256);
}

bool RevalidationRequest::decode(Reader& reader, RevalidationRequest& out) {
    const std::uint16_t format_version = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t attempt_seq = reader.u64();
    const std::uint16_t verdict = reader.u16();
    const Incarnation observer = reader.incarnation();
    const std::uint64_t observer_epoch = reader.u64();
    const std::string rationale = reader.str(256);
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    if (verdict >= kRevalidationVerdictCount) {
        return false;
    }
    const Result<FabricName> fabric_name = FabricName::parse(fabric);
    const Result<LinkName> link_name = LinkName::parse(link);
    if (!fabric_name.ok() || !link_name.ok()) {
        return false;
    }
    RevalidationRequest value;
    value.subject.fabric = fabric_name.value();
    value.subject.link = link_name.value();
    value.subject_generation = LinkGeneration::from(generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.verdict = static_cast<RevalidationVerdict>(verdict);
    value.observer_incarnation = observer;
    value.observer_epoch = Epoch::from(observer_epoch);
    value.rationale = rationale;
    out = std::move(value);
    return true;
}

void ApplicationReport::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u64(attempt_seq.value());
    writer.digest(attempt_id);
    writer.incarnation(applier_incarnation);
    writer.u64(applier_epoch.value());
    writer.bool8(accepted);
    writer.str(detail, 256);
}

bool ApplicationReport::decode(Reader& reader, ApplicationReport& out) {
    const std::uint16_t format_version = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t attempt_seq = reader.u64();
    const Digest attempt_id = reader.digest();
    const Incarnation applier = reader.incarnation();
    const std::uint64_t applier_epoch = reader.u64();
    const bool accepted = reader.bool8();
    const std::string detail = reader.str(256);
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    const Result<FabricName> fabric_name = FabricName::parse(fabric);
    const Result<LinkName> link_name = LinkName::parse(link);
    if (!fabric_name.ok() || !link_name.ok()) {
        return false;
    }
    ApplicationReport value;
    value.subject.fabric = fabric_name.value();
    value.subject.link = link_name.value();
    value.subject_generation = LinkGeneration::from(generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.attempt_id = attempt_id;
    value.applier_incarnation = applier;
    value.applier_epoch = Epoch::from(applier_epoch);
    value.accepted = accepted;
    value.detail = detail;
    out = std::move(value);
    return true;
}

void VerificationReport::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u64(attempt_seq.value());
    writer.digest(attempt_id);
    writer.incarnation(verifier_incarnation);
    writer.u64(verifier_epoch.value());
    writer.bool8(effect_observed);
    writer.str(detail, 256);
}

bool VerificationReport::decode(Reader& reader, VerificationReport& out) {
    const std::uint16_t format_version = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t attempt_seq = reader.u64();
    const Digest attempt_id = reader.digest();
    const Incarnation verifier = reader.incarnation();
    const std::uint64_t verifier_epoch = reader.u64();
    const bool observed = reader.bool8();
    const std::string detail = reader.str(256);
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    const Result<FabricName> fabric_name = FabricName::parse(fabric);
    const Result<LinkName> link_name = LinkName::parse(link);
    if (!fabric_name.ok() || !link_name.ok()) {
        return false;
    }
    VerificationReport value;
    value.subject.fabric = fabric_name.value();
    value.subject.link = link_name.value();
    value.subject_generation = LinkGeneration::from(generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.attempt_id = attempt_id;
    value.verifier_incarnation = verifier;
    value.verifier_epoch = Epoch::from(verifier_epoch);
    value.effect_observed = observed;
    value.detail = detail;
    out = std::move(value);
    return true;
}

}  // namespace lff
