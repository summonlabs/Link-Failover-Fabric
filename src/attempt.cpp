// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/attempt.hpp"

#include "lff/bytes.hpp"
#include "lff/version.hpp"

#include <string>

namespace lff {
namespace {

struct PhaseName {
    AttemptPhase value;
    std::string_view name;
};

constexpr PhaseName kPhaseNames[] = {
    {AttemptPhase::Evaluated, "Evaluated"},
    {AttemptPhase::Authorized, "Authorized"},
    {AttemptPhase::Acknowledged, "Acknowledged"},
    {AttemptPhase::Verified, "Verified"},
    {AttemptPhase::Refused, "Refused"},
    {AttemptPhase::Indeterminate, "Indeterminate"},
    {AttemptPhase::Committed, "Committed"},
    {AttemptPhase::RolledBack, "RolledBack"},
    {AttemptPhase::Interrupted, "Interrupted"},
    {AttemptPhase::Superseded, "Superseded"},
    {AttemptPhase::Aborted, "Aborted"},
};

struct RevalidationName {
    RevalidationVerdict value;
    std::string_view name;
};

constexpr RevalidationName kRevalidationNames[] = {
    {RevalidationVerdict::NotApplied, "NotApplied"},
    {RevalidationVerdict::Applied, "Applied"},
    {RevalidationVerdict::Unknown, "Unknown"},
};

}  // namespace

std::string_view revalidation_verdict_name(RevalidationVerdict value) noexcept {
    for (const RevalidationName& entry : kRevalidationNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedRevalidationVerdict";
}

bool revalidation_verdict_parse(std::string_view text, RevalidationVerdict& out) noexcept {
    for (const RevalidationName& entry : kRevalidationNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

std::string_view attempt_phase_name(AttemptPhase value) noexcept {
    for (const PhaseName& entry : kPhaseNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedAttemptPhase";
}

bool attempt_phase_parse(std::string_view text, AttemptPhase& out) noexcept {
    for (const PhaseName& entry : kPhaseNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

bool attempt_phase_is_terminal(AttemptPhase value) noexcept {
    switch (value) {
        case AttemptPhase::Refused:
        case AttemptPhase::Indeterminate:
        case AttemptPhase::Committed:
        case AttemptPhase::RolledBack:
        case AttemptPhase::Interrupted:
        case AttemptPhase::Superseded:
        case AttemptPhase::Aborted:
            return true;
        case AttemptPhase::Evaluated:
        case AttemptPhase::Authorized:
        case AttemptPhase::Acknowledged:
        case AttemptPhase::Verified:
        default:
            return false;
    }
}

bool attempt_phase_holds_authority(AttemptPhase value) noexcept {
    switch (value) {
        case AttemptPhase::Authorized:
        case AttemptPhase::Acknowledged:
        case AttemptPhase::Verified:
        case AttemptPhase::Committed:
            return true;
        default:
            return false;
    }
}

ReasonCode interruption_reason(AttemptPhase value) noexcept {
    switch (value) {
        case AttemptPhase::Evaluated:
            return ReasonCode::InterruptedBeforeCommit;
        case AttemptPhase::Authorized:
            return ReasonCode::InterruptedAfterCommit;
        case AttemptPhase::Acknowledged:
        case AttemptPhase::Verified:
            return ReasonCode::InterruptedAfterApply;
        default:
            return ReasonCode::AttemptUnknown;
    }
}

void AttemptRecord::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u64(attempt_seq.value());
    writer.digest(attempt_id);
    writer.digest(idempotency_key);
    writer.u16(static_cast<std::uint16_t>(phase));
    writer.str(replacement.fabric.value());
    writer.str(replacement.link.value());
    writer.u64(replacement_generation.value());
    authority.encode(writer);
    writer.digest(evidence_digest);
    writer.u64(created_tick);
    writer.u64(created_epoch.value());
    writer.incarnation(coordinator);
    writer.u64(create_seq.value());
    writer.u64(update_seq.value());
    writer.bool8(predecessor.has_value());
    if (predecessor.has_value()) {
        writer.digest(*predecessor);
    }
    writer.incarnation(applier_incarnation);
    writer.bool8(revalidated);
    writer.u16(static_cast<std::uint16_t>(revalidation));
    writer.u32(static_cast<std::uint32_t>(reasons.size()));
    for (const Reason& reason : reasons) {
        writer.u16(static_cast<std::uint16_t>(reason.code));
        writer.str(reason.detail, static_cast<std::uint32_t>(kMaxReasonDetail));
    }
}

bool AttemptRecord::decode(Reader& reader, AttemptRecord& out) {
    const std::uint16_t format_version = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t attempt_seq = reader.u64();
    const Digest attempt_id = reader.digest();
    const Digest idempotency_key = reader.digest();
    const std::uint16_t phase = reader.u16();
    const std::string replacement_fabric = reader.str();
    const std::string replacement_link = reader.str();
    const std::uint64_t replacement_generation = reader.u64();
    AuthorityVector authority;
    const bool authority_ok = AuthorityVector::decode(reader, authority);
    const Digest evidence_digest = reader.digest();
    const std::uint64_t created_tick = reader.u64();
    const std::uint64_t created_epoch = reader.u64();
    const Incarnation coordinator = reader.incarnation();
    const std::uint64_t create_seq = reader.u64();
    const std::uint64_t update_seq = reader.u64();
    const bool has_predecessor = reader.bool8();
    Digest predecessor;
    if (has_predecessor) {
        predecessor = reader.digest();
    }
    const Incarnation applier = reader.incarnation();
    const bool revalidated = reader.bool8();
    const std::uint16_t revalidation = reader.u16();
    const std::uint32_t reason_count = reader.u32();
    if (!reader.ok() || !authority_ok) {
        return false;
    }
    if (format_version != kIdentityFormatVersion) {
        return false;
    }
    if (phase >= kAttemptPhaseCount || reason_count > kMaxAttemptReasons ||
        revalidation >= kRevalidationVerdictCount) {
        return false;
    }
    AttemptRecord value;
    if (!decode_name(fabric, value.subject.fabric) || !decode_name(link, value.subject.link) ||
        !decode_name(replacement_fabric, value.replacement.fabric) ||
        !decode_name(replacement_link, value.replacement.link)) {
        return false;
    }
    value.subject_generation = LinkGeneration::from(generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.attempt_id = attempt_id;
    value.idempotency_key = idempotency_key;
    value.phase = static_cast<AttemptPhase>(phase);
    value.replacement_generation = LinkGeneration::from(replacement_generation);
    value.authority = authority;
    value.evidence_digest = evidence_digest;
    value.created_tick = created_tick;
    value.created_epoch = Epoch::from(created_epoch);
    value.coordinator = coordinator;
    value.create_seq = LineageSeq::from(create_seq);
    value.update_seq = LineageSeq::from(update_seq);
    if (has_predecessor) {
        value.predecessor = predecessor;
    }
    value.applier_incarnation = applier;
    value.revalidated = revalidated;
    value.revalidation = static_cast<RevalidationVerdict>(revalidation);
    for (std::uint32_t i = 0; i < reason_count; ++i) {
        const std::uint16_t code = reader.u16();
        const std::string detail = reader.str(static_cast<std::uint32_t>(kMaxReasonDetail));
        if (!reader.ok()) {
            return false;
        }
        if (code >= kReasonCodeCount) {
            return false;
        }
        value.reasons.emplace_back(static_cast<ReasonCode>(code), detail);
    }
    if (!reader.ok()) {
        return false;
    }
    out = std::move(value);
    return true;
}

Digest AttemptRecord::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

std::string AttemptRecord::to_string() const {
    std::string out;
    out.append(subject.to_string());
    out.append("@");
    out.append(std::to_string(subject_generation.value()));
    out.append("#");
    out.append(std::to_string(attempt_seq.value()));
    out.append(" phase=");
    out.append(attempt_phase_name(phase));
    if (!replacement.empty()) {
        out.append(" replacement=");
        out.append(replacement.to_string());
        out.append("@");
        out.append(std::to_string(replacement_generation.value()));
    }
    return out;
}

Digest compute_attempt_id(const FabricName& fabric, const LinkKey& subject,
                          LinkGeneration generation, AttemptSeq seq) {
    Writer writer;
    writer.u16(kIdentityFormatVersion);
    writer.str("lff.attempt");
    writer.str(fabric.value());
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(generation.value());
    writer.u64(seq.value());
    return sha256(writer.data().data(), writer.size());
}

}  // namespace lff
