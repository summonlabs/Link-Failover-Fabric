// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "detail/tool_common.hpp"

#include "lff/bytes.hpp"

#include <cstdio>
#include <fstream>
#include <ostream>
#include <sstream>

namespace lff::tool {
namespace {

bool parse_u64_text(const std::string& text, std::uint64_t& out) {
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

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

}  // namespace

bool Arguments::has(const std::string& name) const {
    return options.find(name) != options.end();
}

std::string Arguments::get(const std::string& name, const std::string& fallback) const {
    const auto found = options.find(name);
    return found == options.end() ? fallback : found->second;
}

Result<std::uint64_t> Arguments::get_u64(const std::string& name, std::uint64_t fallback) const {
    const auto found = options.find(name);
    if (found == options.end()) {
        return Result<std::uint64_t>::success(fallback);
    }
    std::uint64_t value = 0;
    if (!parse_u64_text(found->second, value)) {
        Status status = Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                        "option is not a decimal integer");
        status.add(ReasonCode::InvalidArgument, name);
        return Result<std::uint64_t>::failure(status);
    }
    return Result<std::uint64_t>::success(value);
}

Result<std::uint32_t> Arguments::get_u32(const std::string& name, std::uint32_t fallback) const {
    const Result<std::uint64_t> value = get_u64(name, fallback);
    if (!value.ok()) {
        return Result<std::uint32_t>::failure(value.status());
    }
    if (value.value() > 0xFFFFFFFFull) {
        Status status = Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                        "option exceeds the 32-bit range");
        status.add(ReasonCode::InvalidArgument, name);
        return Result<std::uint32_t>::failure(status);
    }
    return Result<std::uint32_t>::success(static_cast<std::uint32_t>(value.value()));
}

Result<bool> Arguments::get_bool(const std::string& name, bool fallback) const {
    const auto found = options.find(name);
    if (found == options.end()) {
        return Result<bool>::success(fallback);
    }
    if (found->second == "true" || found->second == "1" || found->second == "yes") {
        return Result<bool>::success(true);
    }
    if (found->second == "false" || found->second == "0" || found->second == "no") {
        return Result<bool>::success(false);
    }
    Status status = Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                    "option is not a boolean");
    status.add(ReasonCode::InvalidArgument, name);
    return Result<bool>::failure(status);
}

Result<Arguments> Arguments::parse(const std::vector<std::string>& argv) {
    // Options are always written as --name=value. A bare --name sets the value
    // to "true", which is how boolean flags are expressed. Anything else is a
    // positional argument. The separator is mandatory so that a positional
    // command name can never be swallowed as an option value.
    Arguments arguments;
    for (const std::string& token : argv) {
        if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
            const std::string body = token.substr(2);
            const std::size_t separator = body.find('=');
            const std::string name = separator == std::string::npos ? body : body.substr(0, separator);
            const std::string value =
                separator == std::string::npos ? std::string("true") : body.substr(separator + 1);
            if (name.empty()) {
                return Result<Arguments>::failure(
                    Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                    "empty option name"));
            }
            if (arguments.options.find(name) != arguments.options.end()) {
                Status status = Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                                "duplicate option");
                status.add(ReasonCode::InvalidArgument, name);
                return Result<Arguments>::failure(status);
            }
            arguments.options.emplace(name, value);
            continue;
        }
        arguments.positional.push_back(token);
    }
    return Result<Arguments>::success(std::move(arguments));
}

Result<std::string> read_text_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        Status status = Status::failure(Outcome::NotFound, ReasonCode::InvalidArgument,
                                        "cannot open file");
        status.add(ReasonCode::InvalidArgument, path);
        return Result<std::string>::failure(status);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    if (text.size() > (1u << 20)) {
        return Result<std::string>::failure(Status::failure(Outcome::LimitExceeded,
                                                            ReasonCode::LimitExceeded,
                                                            "file exceeds the supported bound"));
    }
    return Result<std::string>::success(text);
}

Result<TopologySnapshot> parse_topology(const std::string& text, const FabricName& fabric) {
    TopologySnapshot topology;
    topology.generation = TopologyGeneration::from(1);
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string_view line = trim(std::string_view(text).substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator != std::string_view::npos && line.substr(0, separator) == "generation") {
            std::uint64_t generation = 0;
            if (!parse_u64_text(std::string(trim(line.substr(separator + 1))), generation)) {
                return Result<TopologySnapshot>::failure(
                    Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                    "topology generation is not a decimal integer"));
            }
            topology.generation = TopologyGeneration::from(generation);
            continue;
        }
        std::istringstream fields{std::string(line)};
        std::string keyword;
        std::string name;
        std::uint64_t generation = 0;
        std::uint64_t capacity = 0;
        fields >> keyword >> name >> generation >> capacity;
        if (!fields || keyword != "link") {
            return Result<TopologySnapshot>::failure(
                Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                "topology line must be 'link <name> <generation> <capacity>'"));
        }
        const Result<LinkName> link = LinkName::parse(name);
        if (!link.ok()) {
            return Result<TopologySnapshot>::failure(link.status());
        }
        LinkDefinition definition;
        definition.link.fabric = fabric;
        definition.link.link = link.value();
        definition.generation = LinkGeneration::from(generation);
        definition.capacity_units = static_cast<std::uint32_t>(capacity);
        topology.links.push_back(definition);
    }
    topology.canonicalise();
    const Status valid = topology.validate();
    if (!valid.ok()) {
        return Result<TopologySnapshot>::failure(valid);
    }
    return Result<TopologySnapshot>::success(std::move(topology));
}

FailureReport synthetic_failure_report(const FabricName& fabric, const std::string& link,
                                       std::uint64_t link_generation,
                                       std::uint64_t topology_generation,
                                       ObservationState state, std::uint32_t confidence,
                                       const std::string& publisher,
                                       Incarnation publisher_incarnation,
                                       std::uint64_t publisher_epoch,
                                       std::uint64_t observation_seq) {
    FailureReport report;
    report.subject.fabric = fabric;
    const Result<LinkName> link_name = LinkName::parse(link);
    if (link_name.ok()) {
        report.subject.link = link_name.value();
    }
    report.link_generation = LinkGeneration::from(link_generation);
    report.topology_generation = TopologyGeneration::from(topology_generation);
    report.state = state;
    report.confidence = make_confidence(confidence);
    const Result<PublisherName> publisher_name = PublisherName::parse(publisher);
    if (publisher_name.ok()) {
        report.stamp.publisher = publisher_name.value();
    }
    report.stamp.publisher_incarnation = publisher_incarnation;
    report.stamp.publisher_epoch = Epoch::from(publisher_epoch);
    report.stamp.observation_seq = ObservationSeq::from(observation_seq);
    return report;
}

ReplacementReport synthetic_replacement_report(
    const FabricName& fabric, const std::string& link, std::uint64_t link_generation,
    std::uint64_t topology_generation, ObservationState state, std::uint32_t confidence,
    std::uint32_t capacity_units, std::uint32_t cost_units, bool adjacency_authorized,
    const std::string& publisher, Incarnation publisher_incarnation, std::uint64_t publisher_epoch,
    std::uint64_t observation_seq) {
    ReplacementReport report;
    report.candidate.fabric = fabric;
    const Result<LinkName> link_name = LinkName::parse(link);
    if (link_name.ok()) {
        report.candidate.link = link_name.value();
    }
    report.link_generation = LinkGeneration::from(link_generation);
    report.topology_generation = TopologyGeneration::from(topology_generation);
    report.state = state;
    report.confidence = make_confidence(confidence);
    report.capacity_units = capacity_units;
    report.cost_units = cost_units;
    report.adjacency_authorized = adjacency_authorized;
    const Result<PublisherName> publisher_name = PublisherName::parse(publisher);
    if (publisher_name.ok()) {
        report.stamp.publisher = publisher_name.value();
    }
    report.stamp.publisher_incarnation = publisher_incarnation;
    report.stamp.publisher_epoch = Epoch::from(publisher_epoch);
    report.stamp.observation_seq = ObservationSeq::from(observation_seq);
    return report;
}

Incarnation deterministic_incarnation(const std::string& name, std::uint64_t epoch) {
    const Digest digest = DigestBuilder()
                              .add("lff.publisher", name)
                              .add_u64("epoch", epoch)
                              .finish();
    Incarnation incarnation;
    for (std::size_t i = 0; i < incarnation.bytes.size(); ++i) {
        incarnation.bytes[i] = digest.bytes[i];
    }
    return incarnation;
}

void print_status(std::ostream& out, const Status& status) {
    out << "outcome=" << outcome_name(status.outcome()) << "\n";
    for (const Reason& reason : status.reasons()) {
        out << "reason=" << reason_code_name(reason.code);
        if (!reason.detail.empty()) {
            out << ":" << reason.detail;
        }
        out << "\n";
    }
}

void print_decision(std::ostream& out, const FailoverDecision& decision) {
    out << "outcome=" << outcome_name(decision.outcome) << "\n";
    out << "kind=" << decision_kind_name(decision.kind) << "\n";
    out << "assertion=" << assertion_name(decision.assertion) << "\n";
    out << "subject=" << decision.subject.to_string() << "\n";
    out << "subject_generation=" << decision.subject_generation.value() << "\n";
    out << "attempt_seq=" << decision.attempt_seq.value() << "\n";
    out << "attempt_id=" << compute_attempt_id(decision.fabric, decision.subject,
                                               decision.subject_generation, decision.attempt_seq)
                                .hex()
                         << "\n";
    out << "decision_id=" << decision.decision_id.hex() << "\n";
    out << "epoch=" << decision.epoch.value() << "\n";
    out << "coordinator=" << decision.coordinator.hex() << "\n";
    out << "durable=" << (decision.durable ? "true" : "false") << "\n";
    out << "idempotent_replay=" << (decision.idempotent_replay ? "true" : "false") << "\n";
    out << "lineage_seq=" << decision.lineage_seq.value() << "\n";
    if (decision.has_replacement) {
        out << "replacement=" << decision.replacement.to_string() << "\n";
        out << "replacement_generation=" << decision.replacement_generation.value() << "\n";
    }
    out << "authority=" << decision.authority.digest().hex() << "\n";
    out << "evidence_digest=" << decision.evidence_digest.hex() << "\n";
    for (const Reason& reason : decision.reasons) {
        out << "reason=" << reason_code_name(reason.code);
        if (!reason.detail.empty()) {
            out << ":" << reason.detail;
        }
        out << "\n";
    }
    for (const CandidateAssessment& assessment : decision.candidates) {
        out << "candidate=" << assessment.candidate.to_string() << "@"
            << assessment.link_generation.value() << ":" << verdict_name(assessment.verdict) << ":"
            << reason_code_name(assessment.reason) << "\n";
    }
}

void print_limits(std::ostream& out, const char* label) {
    out << "limits=" << label << "\n";
}

}  // namespace lff::tool
