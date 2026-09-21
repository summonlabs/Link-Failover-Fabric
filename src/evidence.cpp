// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/evidence.hpp"

#include "lff/bytes.hpp"
#include "lff/version.hpp"

#include <array>

namespace lff {
namespace {

struct ObservationName {
    ObservationState value;
    std::string_view name;
};

constexpr ObservationName kObservationNames[] = {
    {ObservationState::Unknown, "Unknown"},
    {ObservationState::Up, "Up"},
    {ObservationState::Degraded, "Degraded"},
    {ObservationState::Down, "Down"},
    {ObservationState::Unusable, "Unusable"},
};

struct FreshnessName {
    FreshnessClass value;
    std::string_view name;
};

constexpr FreshnessName kFreshnessNames[] = {
    {FreshnessClass::Current, "Current"},
    {FreshnessClass::Expired, "Expired"},
    {FreshnessClass::PriorIncarnation, "PriorIncarnation"},
    {FreshnessClass::Future, "Future"},
};

struct VerdictName {
    Verdict value;
    std::string_view name;
};

constexpr VerdictName kVerdictNames[] = {
    {Verdict::Eligible, "Eligible"},
    {Verdict::Disproven, "Disproven"},
    {Verdict::Indeterminate, "Indeterminate"},
};

}  // namespace

std::string_view observation_state_name(ObservationState value) noexcept {
    for (const ObservationName& entry : kObservationNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedObservationState";
}

bool observation_state_parse(std::string_view text, ObservationState& out) noexcept {
    for (const ObservationName& entry : kObservationNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

bool observation_state_is_failure(ObservationState value, bool treat_degraded_as_failure) noexcept {
    switch (value) {
        case ObservationState::Down:
        case ObservationState::Unusable:
            return true;
        case ObservationState::Degraded:
            return treat_degraded_as_failure;
        case ObservationState::Up:
        case ObservationState::Unknown:
        default:
            return false;
    }
}

std::string_view freshness_class_name(FreshnessClass value) noexcept {
    for (const FreshnessName& entry : kFreshnessNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedFreshnessClass";
}

std::string_view verdict_name(Verdict value) noexcept {
    for (const VerdictName& entry : kVerdictNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedVerdict";
}

// ---------------------------------------------------------------------------
// EvidenceStamp
// ---------------------------------------------------------------------------
void EvidenceStamp::encode(Writer& writer) const {
    writer.str(publisher.value());
    writer.incarnation(publisher_incarnation);
    writer.u64(publisher_epoch.value());
    writer.u64(observation_seq.value());
    writer.i64(wall_clock_ms);
}

bool EvidenceStamp::decode(Reader& reader, EvidenceStamp& out) {
    EvidenceStamp value;
    const std::string publisher = reader.str();
    const Incarnation incarnation = reader.incarnation();
    const std::uint64_t epoch = reader.u64();
    const std::uint64_t seq = reader.u64();
    const std::int64_t wall = reader.i64();
    if (!reader.ok()) {
        return false;
    }
    if (!decode_name(publisher, value.publisher)) {
        return false;
    }
    value.publisher_incarnation = incarnation;
    value.publisher_epoch = Epoch::from(epoch);
    value.observation_seq = ObservationSeq::from(seq);
    value.wall_clock_ms = wall;
    out = value;
    return true;
}

Digest EvidenceStamp::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

// ---------------------------------------------------------------------------
// FailureReport
// ---------------------------------------------------------------------------
void FailureReport::encode(Writer& writer) const {
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(link_generation.value());
    writer.u64(topology_generation.value());
    writer.u16(static_cast<std::uint16_t>(state));
    writer.u32(confidence.value());
    stamp.encode(writer);
}

bool FailureReport::decode(Reader& reader, FailureReport& out) {
    FailureReport value;
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t topology = reader.u64();
    const std::uint16_t state = reader.u16();
    const std::uint32_t confidence = reader.u32();
    if (!reader.ok()) {
        return false;
    }
    if (state >= kObservationStateCount || confidence > kMaxConfidence) {
        return false;
    }
    EvidenceStamp stamp;
    if (!EvidenceStamp::decode(reader, stamp)) {
        return false;
    }
    if (!decode_name(fabric, value.subject.fabric) || !decode_name(link, value.subject.link)) {
        return false;
    }
    value.link_generation = LinkGeneration::from(generation);
    value.topology_generation = TopologyGeneration::from(topology);
    value.state = static_cast<ObservationState>(state);
    value.confidence = Confidence::from(confidence);
    value.stamp = stamp;
    out = value;
    return true;
}

Digest FailureReport::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

// ---------------------------------------------------------------------------
// ReplacementReport
// ---------------------------------------------------------------------------
void ReplacementReport::encode(Writer& writer) const {
    writer.str(candidate.fabric.value());
    writer.str(candidate.link.value());
    writer.u64(link_generation.value());
    writer.u64(topology_generation.value());
    writer.u16(static_cast<std::uint16_t>(state));
    writer.u32(confidence.value());
    writer.u32(capacity_units);
    writer.u32(cost_units);
    writer.bool8(adjacency_authorized);
    writer.u64(adjacency_policy_generation.value());
    stamp.encode(writer);
}

bool ReplacementReport::decode(Reader& reader, ReplacementReport& out) {
    ReplacementReport value;
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint64_t topology = reader.u64();
    const std::uint16_t state = reader.u16();
    const std::uint32_t confidence = reader.u32();
    const std::uint32_t capacity = reader.u32();
    const std::uint32_t cost = reader.u32();
    const bool authorized = reader.bool8();
    const std::uint64_t adjacency_policy = reader.u64();
    if (!reader.ok()) {
        return false;
    }
    if (state >= kObservationStateCount || confidence > kMaxConfidence) {
        return false;
    }
    EvidenceStamp stamp;
    if (!EvidenceStamp::decode(reader, stamp)) {
        return false;
    }
    if (!decode_name(fabric, value.candidate.fabric) || !decode_name(link, value.candidate.link)) {
        return false;
    }
    value.link_generation = LinkGeneration::from(generation);
    value.topology_generation = TopologyGeneration::from(topology);
    value.state = static_cast<ObservationState>(state);
    value.confidence = Confidence::from(confidence);
    value.capacity_units = capacity;
    value.cost_units = cost;
    value.adjacency_authorized = authorized;
    value.adjacency_policy_generation = PolicyGeneration::from(adjacency_policy);
    value.stamp = stamp;
    out = value;
    return true;
}

Digest ReplacementReport::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

// ---------------------------------------------------------------------------
// ReplacementCandidate
// ---------------------------------------------------------------------------
void ReplacementCandidate::encode(Writer& writer) const {
    writer.str(candidate.fabric.value());
    writer.str(candidate.link.value());
    writer.u64(expected_generation.value());
}

bool ReplacementCandidate::decode(Reader& reader, ReplacementCandidate& out) {
    ReplacementCandidate value;
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    if (!reader.ok()) {
        return false;
    }
    if (!decode_name(fabric, value.candidate.fabric) || !decode_name(link, value.candidate.link)) {
        return false;
    }
    value.expected_generation = LinkGeneration::from(generation);
    out = value;
    return true;
}

// ---------------------------------------------------------------------------
// CandidateAssessment
// ---------------------------------------------------------------------------
bool candidate_assessment_less(const CandidateAssessment& a, const CandidateAssessment& b) noexcept {
    if (a.candidate < b.candidate) {
        return true;
    }
    if (b.candidate < a.candidate) {
        return false;
    }
    return a.link_generation > b.link_generation;
}

std::string candidate_assessment_to_string(const CandidateAssessment& value) {
    std::string out;
    out.append(value.candidate.to_string());
    out.append("@");
    out.append(std::to_string(value.link_generation.value()));
    out.append(" ");
    out.append(verdict_name(value.verdict));
    out.append(" ");
    out.append(reason_code_name(value.reason));
    if (!value.detail.empty()) {
        out.append(" (");
        out.append(value.detail);
        out.append(")");
    }
    return out;
}

}  // namespace lff
