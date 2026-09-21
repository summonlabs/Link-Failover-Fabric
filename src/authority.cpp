// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/authority.hpp"

#include "lff/bytes.hpp"
#include "lff/version.hpp"

#include <array>
#include <string>

namespace lff {
namespace {

struct FenceName {
    FenceKind value;
    std::string_view name;
};

constexpr FenceName kFenceNames[] = {
    {FenceKind::AttemptSuperseded, "AttemptSuperseded"},
    {FenceKind::PreRestartAuthority, "PreRestartAuthority"},
    {FenceKind::SessionRevoked, "SessionRevoked"},
    {FenceKind::PolicyChanged, "PolicyChanged"},
    {FenceKind::TopologyChanged, "TopologyChanged"},
    {FenceKind::EpochAdvanced, "EpochAdvanced"},
    {FenceKind::RollbackRevoked, "RollbackRevoked"},
};

std::string u64_text(std::uint64_t value) {
    return std::to_string(value);
}

}  // namespace

std::string_view fence_kind_name(FenceKind value) noexcept {
    for (const FenceName& entry : kFenceNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedFenceKind";
}

bool fence_kind_parse(std::string_view text, FenceKind& out) noexcept {
    for (const FenceName& entry : kFenceNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// AuthorityVector
// ---------------------------------------------------------------------------
void AuthorityVector::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(fabric.value());
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.str(replacement.fabric.value());
    writer.str(replacement.link.value());
    writer.u64(replacement_generation.value());
    writer.u64(topology_generation.value());
    writer.digest(topology_digest);
    writer.u64(policy_generation.value());
    writer.digest(policy_digest);
    writer.u64(epoch.value());
    writer.incarnation(coordinator);
    writer.str(evidence_publisher.value());
    writer.incarnation(publisher_incarnation);
    writer.u64(publisher_seq.value());
    writer.incarnation(applier_incarnation);
    writer.u64(attempt_seq.value());
    writer.digest(evidence_digest);
}

bool AuthorityVector::decode(Reader& reader, AuthorityVector& out) {
    const std::uint16_t format_version = reader.u16();
    const std::string fabric = reader.str();
    const std::string subject_fabric = reader.str();
    const std::string subject_link = reader.str();
    const std::uint64_t subject_generation = reader.u64();
    const std::string replacement_fabric = reader.str();
    const std::string replacement_link = reader.str();
    const std::uint64_t replacement_generation = reader.u64();
    const std::uint64_t topology_generation = reader.u64();
    const Digest topology_digest = reader.digest();
    const std::uint64_t policy_generation = reader.u64();
    const Digest policy_digest = reader.digest();
    const std::uint64_t epoch = reader.u64();
    const Incarnation coordinator = reader.incarnation();
    const std::string evidence_publisher = reader.str();
    const Incarnation publisher_incarnation = reader.incarnation();
    const std::uint64_t publisher_seq = reader.u64();
    const Incarnation applier_incarnation = reader.incarnation();
    const std::uint64_t attempt_seq = reader.u64();
    const Digest evidence_digest = reader.digest();
    if (!reader.ok()) {
        return false;
    }
    if (format_version != kIdentityFormatVersion) {
        return false;
    }
    AuthorityVector value;
    if (!decode_name(fabric, value.fabric) || !decode_name(subject_fabric, value.subject.fabric) ||
        !decode_name(subject_link, value.subject.link) ||
        !decode_name(replacement_fabric, value.replacement.fabric) ||
        !decode_name(replacement_link, value.replacement.link) ||
        !decode_name(evidence_publisher, value.evidence_publisher)) {
        return false;
    }
    value.subject_generation = LinkGeneration::from(subject_generation);
    value.replacement_generation = LinkGeneration::from(replacement_generation);
    value.topology_generation = TopologyGeneration::from(topology_generation);
    value.topology_digest = topology_digest;
    value.policy_generation = PolicyGeneration::from(policy_generation);
    value.policy_digest = policy_digest;
    value.epoch = Epoch::from(epoch);
    value.coordinator = coordinator;
    value.publisher_incarnation = publisher_incarnation;
    value.publisher_seq = ObservationSeq::from(publisher_seq);
    value.applier_incarnation = applier_incarnation;
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.evidence_digest = evidence_digest;
    out = value;
    return true;
}

Digest AuthorityVector::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

std::string AuthorityVector::to_string() const {
    std::string out;
    out.append(fabric.value());
    out.append(" subject=");
    out.append(subject.to_string());
    out.append("@");
    out.append(u64_text(subject_generation.value()));
    out.append(" replacement=");
    out.append(replacement.to_string());
    out.append("@");
    out.append(u64_text(replacement_generation.value()));
    out.append(" topology=");
    out.append(u64_text(topology_generation.value()));
    out.append(" policy=");
    out.append(u64_text(policy_generation.value()));
    out.append(" epoch=");
    out.append(u64_text(epoch.value()));
    out.append(" coordinator=");
    out.append(coordinator.hex());
    out.append(" attempt=");
    out.append(u64_text(attempt_seq.value()));
    return out;
}

// ---------------------------------------------------------------------------
// AuthorityExpectation
// ---------------------------------------------------------------------------
void AuthorityExpectation::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.bool8(epoch.has_value());
    if (epoch.has_value()) {
        writer.u64(epoch->value());
    }
    writer.bool8(coordinator.has_value());
    if (coordinator.has_value()) {
        writer.incarnation(*coordinator);
    }
    writer.bool8(topology_generation.has_value());
    if (topology_generation.has_value()) {
        writer.u64(topology_generation->value());
    }
    writer.bool8(policy_generation.has_value());
    if (policy_generation.has_value()) {
        writer.u64(policy_generation->value());
    }
    writer.bool8(subject_generation.has_value());
    if (subject_generation.has_value()) {
        writer.u64(subject_generation->value());
    }
    writer.bool8(policy_digest.has_value());
    if (policy_digest.has_value()) {
        writer.digest(*policy_digest);
    }
    writer.bool8(topology_digest.has_value());
    if (topology_digest.has_value()) {
        writer.digest(*topology_digest);
    }
}

bool AuthorityExpectation::decode(Reader& reader, AuthorityExpectation& out) {
    const std::uint16_t format_version = reader.u16();
    AuthorityExpectation value;
    if (reader.bool8()) {
        value.epoch = Epoch::from(reader.u64());
    }
    if (reader.bool8()) {
        value.coordinator = reader.incarnation();
    }
    if (reader.bool8()) {
        value.topology_generation = TopologyGeneration::from(reader.u64());
    }
    if (reader.bool8()) {
        value.policy_generation = PolicyGeneration::from(reader.u64());
    }
    if (reader.bool8()) {
        value.subject_generation = LinkGeneration::from(reader.u64());
    }
    if (reader.bool8()) {
        value.policy_digest = reader.digest();
    }
    if (reader.bool8()) {
        value.topology_digest = reader.digest();
    }
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    out = value;
    return true;
}

std::vector<AuthorityMismatch> compare_authority(const AuthorityExpectation& expectation,
                                                 const AuthorityVector& current) {
    std::vector<AuthorityMismatch> mismatches;
    if (expectation.epoch.has_value() && expectation.epoch->value() != current.epoch.value()) {
        mismatches.push_back(AuthorityMismatch{ReasonCode::EpochMismatch, "epoch",
                                               u64_text(expectation.epoch->value()),
                                               u64_text(current.epoch.value())});
    }
    if (expectation.coordinator.has_value() && *expectation.coordinator != current.coordinator) {
        mismatches.push_back(AuthorityMismatch{ReasonCode::CoordinatorIncarnationMismatch,
                                               "coordinator", expectation.coordinator->hex(),
                                               current.coordinator.hex()});
    }
    if (expectation.topology_generation.has_value() &&
        expectation.topology_generation->value() != current.topology_generation.value()) {
        mismatches.push_back(AuthorityMismatch{ReasonCode::TopologyGenerationMismatch,
                                               "topology_generation",
                                               u64_text(expectation.topology_generation->value()),
                                               u64_text(current.topology_generation.value())});
    }
    if (expectation.policy_generation.has_value() &&
        expectation.policy_generation->value() != current.policy_generation.value()) {
        mismatches.push_back(AuthorityMismatch{ReasonCode::PolicyGenerationMismatch,
                                               "policy_generation",
                                               u64_text(expectation.policy_generation->value()),
                                               u64_text(current.policy_generation.value())});
    }
    if (expectation.subject_generation.has_value() &&
        expectation.subject_generation->value() != current.subject_generation.value()) {
        mismatches.push_back(AuthorityMismatch{ReasonCode::SubjectGenerationMismatch,
                                               "subject_generation",
                                               u64_text(expectation.subject_generation->value()),
                                               u64_text(current.subject_generation.value())});
    }
    if (expectation.policy_digest.has_value() && *expectation.policy_digest != current.policy_digest) {
        mismatches.push_back(AuthorityMismatch{ReasonCode::PolicyGenerationMismatch, "policy_digest",
                                               expectation.policy_digest->hex(),
                                               current.policy_digest.hex()});
    }
    if (expectation.topology_digest.has_value() &&
        *expectation.topology_digest != current.topology_digest) {
        mismatches.push_back(AuthorityMismatch{ReasonCode::TopologyGenerationMismatch,
                                               "topology_digest", expectation.topology_digest->hex(),
                                               current.topology_digest.hex()});
    }
    return mismatches;
}

// ---------------------------------------------------------------------------
// FenceRecord
// ---------------------------------------------------------------------------
void FenceRecord::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.u64(attempt_seq.value());
    writer.u64(epoch.value());
    writer.incarnation(coordinator);
    writer.u64(seq.value());
    writer.str(detail, 512);
}

bool FenceRecord::decode(Reader& reader, FenceRecord& out) {
    const std::uint16_t format_version = reader.u16();
    const std::uint16_t kind = reader.u16();
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t attempt_seq = reader.u64();
    const std::uint64_t epoch = reader.u64();
    const Incarnation coordinator = reader.incarnation();
    const std::uint64_t seq = reader.u64();
    const std::string detail = reader.str(512);
    if (!reader.ok() || format_version != kIdentityFormatVersion) {
        return false;
    }
    if (kind >= kFenceKindCount) {
        return false;
    }
    FenceRecord value;
    if (!decode_name(fabric, value.subject.fabric) || !decode_name(link, value.subject.link)) {
        return false;
    }
    value.kind = static_cast<FenceKind>(kind);
    value.subject_generation = LinkGeneration::from(generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.epoch = Epoch::from(epoch);
    value.coordinator = coordinator;
    value.seq = LineageSeq::from(seq);
    value.detail = detail;
    out = value;
    return true;
}

Digest FenceRecord::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

}  // namespace lff
