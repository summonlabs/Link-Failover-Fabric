// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/policy.hpp"

#include "lff/bytes.hpp"
#include "lff/version.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace lff {
namespace {

struct AgreementName {
    AgreementRule value;
    std::string_view name;
};

constexpr AgreementName kAgreementNames[] = {
    {AgreementRule::Unanimous, "unanimous"},
    {AgreementRule::MajorityOfFreshQualified, "majority"},
};

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    std::uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        std::uint64_t next = 0;
        if (!checked_mul_u64(value, 10, next) ||
            !checked_add_u64(next, static_cast<std::uint64_t>(ch - '0'), value)) {
            return false;
        }
    }
    out = value;
    return true;
}

bool parse_bool(std::string_view text, bool& out) noexcept {
    if (text == "true" || text == "1") {
        out = true;
        return true;
    }
    if (text == "false" || text == "0") {
        out = false;
        return true;
    }
    return false;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

std::string_view agreement_rule_name(AgreementRule value) noexcept {
    for (const AgreementName& entry : kAgreementNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedAgreementRule";
}

bool agreement_rule_parse(std::string_view text, AgreementRule& out) noexcept {
    for (const AgreementName& entry : kAgreementNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

FailoverPolicy FailoverPolicy::defaults() {
    return FailoverPolicy{};
}

Status FailoverPolicy::validate() const {
    if (min_failure_confidence.value() > kMaxConfidence) {
        return Status::failure(Outcome::Invalid, ReasonCode::PolicyRejected,
                               "min_failure_confidence out of range");
    }
    if (min_replacement_confidence.value() > kMaxConfidence) {
        return Status::failure(Outcome::Invalid, ReasonCode::PolicyRejected,
                               "min_replacement_confidence out of range");
    }
    if (evidence_max_age_ticks == 0 || evidence_max_age_ticks > kMaxEvidenceAgeTicks) {
        return Status::failure(Outcome::Invalid, ReasonCode::PolicyRejected,
                               "evidence_max_age_ticks out of range");
    }
    if (max_candidates == 0 || max_candidates > kMaxPolicyCandidates) {
        return Status::failure(Outcome::Invalid, ReasonCode::PolicyRejected,
                               "max_candidates out of range");
    }
    if (max_attempts_per_generation == 0 || max_attempts_per_generation > 1024) {
        return Status::failure(Outcome::Invalid, ReasonCode::PolicyRejected,
                               "max_attempts_per_generation out of range");
    }
    if (max_retained_attempts == 0 || max_retained_attempts > kMaxRetainedAttemptsPerLink) {
        return Status::failure(Outcome::Invalid, ReasonCode::PolicyRejected,
                               "max_retained_attempts out of range");
    }
    if (generation.is_zero()) {
        return Status::failure(Outcome::Invalid, ReasonCode::PolicyRejected,
                               "policy generation must be non-zero");
    }
    return Status::success();
}

void FailoverPolicy::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.u64(generation.value());
    writer.u32(min_failure_confidence.value());
    writer.u32(min_replacement_confidence.value());
    writer.u64(evidence_max_age_ticks);
    writer.u32(min_replacement_capacity);
    writer.bool8(require_subject_capacity);
    writer.bool8(require_adjacency_authorization);
    writer.bool8(allow_degraded_replacement);
    writer.bool8(treat_degraded_as_failed);
    writer.u16(static_cast<std::uint16_t>(agreement));
    writer.u32(max_attempts_per_generation);
    writer.u32(max_candidates);
    writer.u32(max_retained_attempts);
    writer.bool8(require_verified_effect);
    writer.bool8(failover_enabled);
}

bool FailoverPolicy::decode(Reader& reader, FailoverPolicy& out) {
    const std::uint16_t format_version = reader.u16();
    const std::uint64_t generation = reader.u64();
    const std::uint32_t min_failure = reader.u32();
    const std::uint32_t min_replacement = reader.u32();
    const std::uint64_t max_age = reader.u64();
    const std::uint32_t min_capacity = reader.u32();
    const bool subject_capacity = reader.bool8();
    const bool adjacency = reader.bool8();
    const bool degraded_replacement = reader.bool8();
    const bool degraded_failure = reader.bool8();
    const std::uint16_t agreement = reader.u16();
    const std::uint32_t max_attempts = reader.u32();
    const std::uint32_t max_candidates = reader.u32();
    const std::uint32_t max_retained = reader.u32();
    const bool verified_effect = reader.bool8();
    const bool enabled = reader.bool8();
    if (!reader.ok()) {
        return false;
    }
    if (format_version != kIdentityFormatVersion) {
        return false;
    }
    if (agreement >= kAgreementRuleCount) {
        return false;
    }
    FailoverPolicy value;
    value.generation = PolicyGeneration::from(generation);
    value.min_failure_confidence = Confidence::from(min_failure);
    value.min_replacement_confidence = Confidence::from(min_replacement);
    value.evidence_max_age_ticks = max_age;
    value.min_replacement_capacity = min_capacity;
    value.require_subject_capacity = subject_capacity;
    value.require_adjacency_authorization = adjacency;
    value.allow_degraded_replacement = degraded_replacement;
    value.treat_degraded_as_failed = degraded_failure;
    value.agreement = static_cast<AgreementRule>(agreement);
    value.max_attempts_per_generation = max_attempts;
    value.max_candidates = max_candidates;
    value.max_retained_attempts = max_retained;
    value.require_verified_effect = verified_effect;
    value.failover_enabled = enabled;
    out = value;
    return true;
}

Digest FailoverPolicy::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

std::string failover_policy_to_string(const FailoverPolicy& policy) {
    std::ostringstream out;
    out << "generation=" << policy.generation.value() << "\n";
    out << "min_failure_confidence=" << policy.min_failure_confidence.value() << "\n";
    out << "min_replacement_confidence=" << policy.min_replacement_confidence.value() << "\n";
    out << "evidence_max_age_ticks=" << policy.evidence_max_age_ticks << "\n";
    out << "min_replacement_capacity=" << policy.min_replacement_capacity << "\n";
    out << "require_subject_capacity=" << (policy.require_subject_capacity ? "true" : "false")
        << "\n";
    out << "require_adjacency_authorization="
        << (policy.require_adjacency_authorization ? "true" : "false") << "\n";
    out << "allow_degraded_replacement=" << (policy.allow_degraded_replacement ? "true" : "false")
        << "\n";
    out << "treat_degraded_as_failed=" << (policy.treat_degraded_as_failed ? "true" : "false")
        << "\n";
    out << "agreement=" << agreement_rule_name(policy.agreement) << "\n";
    out << "max_attempts_per_generation=" << policy.max_attempts_per_generation << "\n";
    out << "max_candidates=" << policy.max_candidates << "\n";
    out << "max_retained_attempts=" << policy.max_retained_attempts << "\n";
    out << "require_verified_effect=" << (policy.require_verified_effect ? "true" : "false") << "\n";
    out << "failover_enabled=" << (policy.failover_enabled ? "true" : "false") << "\n";
    return out.str();
}

bool failover_policy_parse(std::string_view text, FailoverPolicy& out) {
    FailoverPolicy policy = out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        std::uint64_t number = 0;
        bool flag = false;
        if (key == "generation") {
            if (!parse_u64(value, number)) return false;
            policy.generation = PolicyGeneration::from(number);
        } else if (key == "min_failure_confidence") {
            if (!parse_u64(value, number) || number > kMaxConfidence) return false;
            policy.min_failure_confidence = Confidence::from(static_cast<std::uint32_t>(number));
        } else if (key == "min_replacement_confidence") {
            if (!parse_u64(value, number) || number > kMaxConfidence) return false;
            policy.min_replacement_confidence = Confidence::from(static_cast<std::uint32_t>(number));
        } else if (key == "evidence_max_age_ticks") {
            if (!parse_u64(value, number)) return false;
            policy.evidence_max_age_ticks = number;
        } else if (key == "min_replacement_capacity") {
            if (!parse_u64(value, number) || number > 0xFFFFFFFFull) return false;
            policy.min_replacement_capacity = static_cast<std::uint32_t>(number);
        } else if (key == "require_subject_capacity") {
            if (!parse_bool(value, flag)) return false;
            policy.require_subject_capacity = flag;
        } else if (key == "require_adjacency_authorization") {
            if (!parse_bool(value, flag)) return false;
            policy.require_adjacency_authorization = flag;
        } else if (key == "allow_degraded_replacement") {
            if (!parse_bool(value, flag)) return false;
            policy.allow_degraded_replacement = flag;
        } else if (key == "treat_degraded_as_failed") {
            if (!parse_bool(value, flag)) return false;
            policy.treat_degraded_as_failed = flag;
        } else if (key == "agreement") {
            AgreementRule rule{};
            if (!agreement_rule_parse(value, rule)) return false;
            policy.agreement = rule;
        } else if (key == "max_attempts_per_generation") {
            if (!parse_u64(value, number) || number > 0xFFFFFFFFull) return false;
            policy.max_attempts_per_generation = static_cast<std::uint32_t>(number);
        } else if (key == "max_candidates") {
            if (!parse_u64(value, number) || number > 0xFFFFFFFFull) return false;
            policy.max_candidates = static_cast<std::uint32_t>(number);
        } else if (key == "max_retained_attempts") {
            if (!parse_u64(value, number) || number > 0xFFFFFFFFull) return false;
            policy.max_retained_attempts = static_cast<std::uint32_t>(number);
        } else if (key == "require_verified_effect") {
            if (!parse_bool(value, flag)) return false;
            policy.require_verified_effect = flag;
        } else if (key == "failover_enabled") {
            if (!parse_bool(value, flag)) return false;
            policy.failover_enabled = flag;
        } else {
            return false;
        }
    }
    out = policy;
    return true;
}

// ---------------------------------------------------------------------------
// LinkDefinition
// ---------------------------------------------------------------------------
void LinkDefinition::encode(Writer& writer) const {
    writer.str(link.fabric.value());
    writer.str(link.link.value());
    writer.u64(generation.value());
    writer.u32(capacity_units);
}

bool LinkDefinition::decode(Reader& reader, LinkDefinition& out) {
    LinkDefinition value;
    const std::string fabric = reader.str();
    const std::string link = reader.str();
    const std::uint64_t generation = reader.u64();
    const std::uint32_t capacity = reader.u32();
    if (!reader.ok()) {
        return false;
    }
    if (!decode_name(fabric, value.link.fabric) || !decode_name(link, value.link.link)) {
        return false;
    }
    value.generation = LinkGeneration::from(generation);
    value.capacity_units = capacity;
    out = value;
    return true;
}

bool link_definition_less(const LinkDefinition& a, const LinkDefinition& b) noexcept {
    if (a.link < b.link) {
        return true;
    }
    if (b.link < a.link) {
        return false;
    }
    return a.generation > b.generation;
}

Status TopologySnapshot::validate() const {
    if (links.size() > kMaxTopologyLinks) {
        return Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                               "topology link count exceeds the supported bound");
    }
    for (const LinkDefinition& definition : links) {
        if (definition.link.empty()) {
            return Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                   "topology contains an empty link key");
        }
        if (definition.generation.is_zero()) {
            return Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                   "topology link generation must be non-zero");
        }
    }
    for (std::size_t i = 1; i < links.size(); ++i) {
        if (!(links[i - 1].link < links[i].link)) {
            return Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                   "topology links are not strictly ordered and unique");
        }
    }
    return Status::success();
}

void TopologySnapshot::canonicalise() {
    std::sort(links.begin(), links.end(), link_definition_less);
}

Digest TopologySnapshot::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

const LinkDefinition* TopologySnapshot::find(const LinkKey& key) const {
    for (const LinkDefinition& definition : links) {
        if (definition.link == key) {
            return &definition;
        }
    }
    return nullptr;
}

void TopologySnapshot::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.u64(generation.value());
    writer.u32(static_cast<std::uint32_t>(links.size()));
    for (const LinkDefinition& definition : links) {
        definition.encode(writer);
    }
}

bool TopologySnapshot::decode(Reader& reader, TopologySnapshot& out) {
    const std::uint16_t format_version = reader.u16();
    const std::uint64_t generation = reader.u64();
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) {
        return false;
    }
    if (format_version != kIdentityFormatVersion) {
        return false;
    }
    if (count > kMaxTopologyLinks) {
        return false;
    }
    TopologySnapshot value;
    value.generation = TopologyGeneration::from(generation);
    value.links.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        LinkDefinition definition;
        if (!LinkDefinition::decode(reader, definition)) {
            return false;
        }
        value.links.push_back(std::move(definition));
    }
    if (!reader.ok()) {
        return false;
    }
    out = std::move(value);
    return true;
}

}  // namespace lff
