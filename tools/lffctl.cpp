// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// lffctl — the operator and publisher client for the Link Failover Fabric.
//
// Every command is one framed session request. The tool never decides anything:
// it publishes observations, asks for a failover decision, reports an
// application or a verification, and prints exactly what the coordinator
// answered, including its outcome, assertion level and reasons.
#include "detail/tool_common.hpp"
#include "lff/client.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using lff::tool::kExitNegative;
using lff::tool::kExitOk;
using lff::tool::kExitTransport;
using lff::tool::kExitUsage;

int usage() {
    std::cerr <<
        "usage: lffctl --endpoint=HOST:PORT <command> [--name=value ...]\n"
        "\n"
        "commands:\n"
        "  ping\n"
        "  inspect [--history=N]\n"
        "  shutdown\n"
        "  set-policy --file=PATH | --generation=N [other policy keys]\n"
        "  set-topology --file=PATH\n"
        "  publish-failure --link=L --generation=G --topology-generation=T --state=ST\n"
        "                  --confidence=C --publisher=P [--publisher-epoch=N]\n"
        "                  [--publisher-incarnation=HEX] [--observation-seq=N]\n"
        "  publish-replacement --link=L --generation=G --topology-generation=T --state=ST\n"
        "                      --confidence=C --capacity=N [--cost=N]\n"
        "                      [--adjacency-authorized=BOOL] --publisher=P [...]\n"
        "  failover --link=L --generation=G [--candidates=a,b:7] [--idempotency=HEX]\n"
        "           [--expect-epoch=N] [--expect-coordinator=HEX]\n"
        "           [--expect-topology-generation=N] [--expect-policy-generation=N]\n"
        "  rollback --link=L --attempt-seq=N [--rationale=TEXT]\n"
        "  apply --link=L --generation=G --attempt-seq=N --attempt-id=HEX [--reject]\n"
        "  verify --link=L --generation=G --attempt-seq=N --attempt-id=HEX [--not-observed]\n"
        "  revalidate --link=L --generation=G --attempt-seq=N --verdict=V\n"
        "  claim [--max=N]\n"
        "\n"
        "states: Unknown Up Degraded Down Unusable\n"
        "verdicts: NotApplied Applied Unknown\n";
    return kExitUsage;
}

int fail(const std::string& message) {
    std::cerr << message << "\n";
    return kExitUsage;
}

bool parse_digest(const std::string& text, lff::Digest& out) {
    if (text.empty()) {
        out = lff::Digest{};
        return true;
    }
    return lff::Digest::parse(text, out);
}

int report_status(const lff::Status& status) {
    lff::tool::print_status(std::cout, status);
    std::cout.flush();
    return status.ok() ? kExitOk : kExitNegative;
}

int report_decision(const lff::Result<lff::FailoverDecision>& decision) {
    if (!decision.ok()) {
        return report_status(decision.status());
    }
    lff::tool::print_decision(std::cout, decision.value());
    std::cout.flush();
    return decision.value().outcome == lff::Outcome::Ok ? kExitOk : kExitNegative;
}

bool build_expectation(const lff::tool::Arguments& args, lff::AuthorityExpectation& out) {
    if (args.has("expect-epoch")) {
        const lff::Result<std::uint64_t> value = args.get_u64("expect-epoch", 0);
        if (!value.ok()) {
            return false;
        }
        out.epoch = lff::Epoch::from(value.value());
    }
    if (args.has("expect-coordinator")) {
        lff::Incarnation incarnation;
        if (!lff::Incarnation::parse(args.get("expect-coordinator"), incarnation)) {
            return false;
        }
        out.coordinator = incarnation;
    }
    if (args.has("expect-topology-generation")) {
        const lff::Result<std::uint64_t> value = args.get_u64("expect-topology-generation", 0);
        if (!value.ok()) {
            return false;
        }
        out.topology_generation = lff::TopologyGeneration::from(value.value());
    }
    if (args.has("expect-policy-generation")) {
        const lff::Result<std::uint64_t> value = args.get_u64("expect-policy-generation", 0);
        if (!value.ok()) {
            return false;
        }
        out.policy_generation = lff::PolicyGeneration::from(value.value());
    }
    return true;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == separator) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    const lff::Result<lff::tool::Arguments> parsed = lff::tool::Arguments::parse(arguments);
    if (!parsed.ok()) {
        std::cerr << "argument error: " << parsed.status().to_string() << "\n";
        return kExitUsage;
    }
    const lff::tool::Arguments& args = parsed.value();
    if (args.positional.empty()) {
        return usage();
    }
    const std::string command = args.positional.front();
    const std::string endpoint_text = args.get("endpoint");
    if (endpoint_text.empty()) {
        return fail("--endpoint is required");
    }
    const lff::Result<lff::Endpoint> endpoint = lff::Endpoint::parse(endpoint_text);
    if (!endpoint.ok()) {
        return fail("invalid endpoint");
    }

    lff::Result<std::unique_ptr<lff::Client>> client =
        lff::Client::connect(endpoint.value(), "ctl", lff::kDefaultFramePayload);
    if (!client.ok()) {
        std::cerr << "connect failed: " << client.status().to_string() << "\n";
        return kExitTransport;
    }

    if (command == "ping") {
        return report_status(client.value()->ping());
    }
    if (command == "shutdown") {
        const int code = report_status(client.value()->shutdown_server());
        return code;
    }
    if (command == "inspect") {
        const lff::Result<std::uint64_t> history = args.get_u64("history", 16);
        if (!history.ok()) {
            return fail("--history must be a decimal integer");
        }
        lff::Result<lff::EngineView> view =
            client.value()->inspect(static_cast<std::size_t>(history.value()));
        if (!view.ok()) {
            return report_status(view.status());
        }
        std::cout << lff::engine_view_to_string(view.value());
        std::cout.flush();
        return kExitOk;
    }
    if (command == "set-policy") {
        lff::FailoverPolicy policy = lff::FailoverPolicy::defaults();
        policy.generation = lff::PolicyGeneration::from(1);
        if (args.has("file")) {
            const lff::Result<std::string> text = lff::tool::read_text_file(args.get("file"));
            if (!text.ok()) {
                return report_status(text.status());
            }
            if (!lff::failover_policy_parse(text.value(), policy)) {
                return fail("policy file is malformed");
            }
        } else {
            const lff::Result<std::uint64_t> generation = args.get_u64("generation", 1);
            if (!generation.ok()) {
                return fail("--generation must be a decimal integer");
            }
            policy.generation = lff::PolicyGeneration::from(generation.value());
            const lff::Result<std::uint64_t> min_failure =
                args.get_u64("min-failure-confidence", policy.min_failure_confidence.value());
            const lff::Result<std::uint64_t> min_replacement =
                args.get_u64("min-replacement-confidence", policy.min_replacement_confidence.value());
            const lff::Result<std::uint64_t> max_age =
                args.get_u64("evidence-max-age-ticks", policy.evidence_max_age_ticks);
            const lff::Result<std::uint64_t> max_candidates =
                args.get_u64("max-candidates", policy.max_candidates);
            const lff::Result<std::uint64_t> max_attempts =
                args.get_u64("max-attempts-per-generation", policy.max_attempts_per_generation);
            const lff::Result<bool> adjacency = args.get_bool("require-adjacency-authorization",
                                                              policy.require_adjacency_authorization);
            const lff::Result<bool> degraded =
                args.get_bool("allow-degraded-replacement", policy.allow_degraded_replacement);
            const lff::Result<bool> enabled = args.get_bool("failover-enabled", policy.failover_enabled);
            const lff::Result<bool> verified =
                args.get_bool("require-verified-effect", policy.require_verified_effect);
            if (!min_failure.ok() || !min_replacement.ok() || !max_age.ok() || !max_candidates.ok() ||
                !max_attempts.ok() || !adjacency.ok() || !degraded.ok() || !enabled.ok() ||
                !verified.ok()) {
                return fail("policy option is not a decimal integer or boolean");
            }
            if (min_failure.value() > lff::kMaxConfidence ||
                min_replacement.value() > lff::kMaxConfidence ||
                max_candidates.value() > 0xFFFFFFFFull || max_attempts.value() > 0xFFFFFFFFull) {
                return fail("policy option is out of range");
            }
            policy.min_failure_confidence = lff::make_confidence(
                static_cast<std::uint32_t>(min_failure.value()));
            policy.min_replacement_confidence = lff::make_confidence(
                static_cast<std::uint32_t>(min_replacement.value()));
            policy.evidence_max_age_ticks = max_age.value();
            policy.max_candidates = static_cast<std::uint32_t>(max_candidates.value());
            policy.max_attempts_per_generation = static_cast<std::uint32_t>(max_attempts.value());
            policy.require_adjacency_authorization = adjacency.value();
            policy.allow_degraded_replacement = degraded.value();
            policy.failover_enabled = enabled.value();
            policy.require_verified_effect = verified.value();
            if (args.has("agreement")) {
                lff::AgreementRule rule{};
                if (!lff::agreement_rule_parse(args.get("agreement"), rule)) {
                    return fail("--agreement must be unanimous or majority");
                }
                policy.agreement = rule;
            }
        }
        return report_status(client.value()->set_policy(policy));
    }
    if (command == "set-topology") {
        if (!args.has("file")) {
            return fail("set-topology requires --file");
        }
        const lff::Result<std::string> text = lff::tool::read_text_file(args.get("file"));
        if (!text.ok()) {
            return report_status(text.status());
        }
        const lff::Result<lff::EngineView> view = client.value()->inspect(0);
        if (!view.ok()) {
            return report_status(view.status());
        }
        lff::Result<lff::TopologySnapshot> topology =
            lff::tool::parse_topology(text.value(), view.value().fabric);
        if (!topology.ok()) {
            return report_status(topology.status());
        }
        return report_status(client.value()->set_topology(topology.value()));
    }
    if (command == "publish-failure" || command == "publish-replacement") {
        const lff::Result<lff::EngineView> view = client.value()->inspect(0);
        if (!view.ok()) {
            return report_status(view.status());
        }
        const lff::FabricName fabric = view.value().fabric;
        const std::string link = args.get("link");
        if (link.empty()) {
            return fail("--link is required");
        }
        const lff::Result<std::uint64_t> generation = args.get_u64("generation", 0);
        const lff::Result<std::uint64_t> topology_generation =
            args.get_u64("topology-generation", view.value().topology_generation.value());
        const lff::Result<std::uint64_t> confidence = args.get_u64("confidence", 1000);
        const lff::Result<std::uint64_t> publisher_epoch = args.get_u64("publisher-epoch", 1);
        const lff::Result<std::uint64_t> observation_seq = args.get_u64("observation-seq", 1);
        if (!generation.ok() || !topology_generation.ok() || !confidence.ok() ||
            !publisher_epoch.ok() || !observation_seq.ok()) {
            return fail("numeric option is not a decimal integer");
        }
        if (confidence.value() > lff::kMaxConfidence) {
            return fail("--confidence must be at most 1000");
        }
        lff::ObservationState state{};
        if (!lff::observation_state_parse(args.get("state", "Unknown"), state)) {
            return fail("--state must be Unknown, Up, Degraded, Down or Unusable");
        }
        const std::string publisher = args.get("publisher", "synthetic-publisher");
        lff::Incarnation incarnation;
        if (args.has("publisher-incarnation")) {
            if (!lff::Incarnation::parse(args.get("publisher-incarnation"), incarnation)) {
                return fail("--publisher-incarnation must be 32 hexadecimal characters");
            }
        } else {
            incarnation = lff::tool::deterministic_incarnation(publisher, publisher_epoch.value());
        }

        if (command == "publish-failure") {
            const lff::FailureReport report = lff::tool::synthetic_failure_report(
                fabric, link, generation.value(), topology_generation.value(), state,
                static_cast<std::uint32_t>(confidence.value()), publisher, incarnation,
                publisher_epoch.value(), observation_seq.value());
            return report_status(client.value()->publish_failure_report(report));
        }
        const lff::Result<std::uint64_t> capacity = args.get_u64("capacity", 0);
        const lff::Result<std::uint64_t> cost = args.get_u64("cost", 0);
        const lff::Result<bool> adjacency = args.get_bool("adjacency-authorized", true);
        if (!capacity.ok() || !cost.ok() || !adjacency.ok()) {
            return fail("--capacity, --cost and --adjacency-authorized are malformed");
        }
        if (capacity.value() > 0xFFFFFFFFull || cost.value() > 0xFFFFFFFFull) {
            return fail("--capacity and --cost must fit in 32 bits");
        }
        const lff::ReplacementReport report = lff::tool::synthetic_replacement_report(
            fabric, link, generation.value(), topology_generation.value(), state,
            static_cast<std::uint32_t>(confidence.value()),
            static_cast<std::uint32_t>(capacity.value()), static_cast<std::uint32_t>(cost.value()),
            adjacency.value(), publisher, incarnation, publisher_epoch.value(),
            observation_seq.value());
        return report_status(client.value()->publish_replacement_report(report));
    }
    if (command == "failover") {
        const std::string link = args.get("link");
        if (link.empty()) {
            return fail("--link is required");
        }
        const lff::Result<lff::EngineView> view = client.value()->inspect(0);
        if (!view.ok()) {
            return report_status(view.status());
        }
        const lff::Result<lff::FabricName> fabric =
            lff::FabricName::parse(view.value().fabric.value());
        const lff::Result<lff::LinkName> link_name = lff::LinkName::parse(link);
        if (!fabric.ok() || !link_name.ok()) {
            return fail("invalid fabric or link name");
        }
        const lff::Result<std::uint64_t> generation = args.get_u64("generation", 0);
        if (!generation.ok() || generation.value() == 0) {
            return fail("--generation is required and must be non-zero");
        }
        lff::FailoverRequest request;
        request.subject.fabric = fabric.value();
        request.subject.link = link_name.value();
        request.subject_generation = lff::LinkGeneration::from(generation.value());
        for (const std::string& spec : split(args.get("candidates"), ',')) {
            if (spec.empty()) {
                continue;
            }
            const std::vector<std::string> fields = split(spec, ':');
            const lff::Result<lff::LinkName> candidate = lff::LinkName::parse(fields[0]);
            if (!candidate.ok()) {
                return fail("invalid candidate link name");
            }
            lff::ReplacementCandidate entry;
            entry.candidate.fabric = fabric.value();
            entry.candidate.link = candidate.value();
            if (fields.size() > 1 && !fields[1].empty()) {
                lff::Result<std::uint64_t> expected =
                    args.get_u64("unused", 0);
                (void)expected;
                std::uint64_t value = 0;
                for (char ch : fields[1]) {
                    if (ch < '0' || ch > '9') {
                        return fail("candidate generation must be decimal");
                    }
                    value = value * 10 + static_cast<std::uint64_t>(ch - '0');
                }
                entry.expected_generation = lff::LinkGeneration::from(value);
            }
            request.candidates.push_back(entry);
        }
        if (!build_expectation(args, request.expected)) {
            return fail("authority expectation is malformed");
        }
        if (!parse_digest(args.get("idempotency"), request.idempotency_key)) {
            return fail("--idempotency must be 64 hexadecimal characters");
        }
        return report_decision(client.value()->request_failover(request));
    }
    if (command == "rollback") {
        lff::RollbackRequest request;
        const lff::Result<lff::EngineView> view = client.value()->inspect(0);
        if (!view.ok()) {
            return report_status(view.status());
        }
        const lff::Result<lff::LinkName> link = lff::LinkName::parse(args.get("link"));
        const lff::Result<std::uint64_t> attempt_seq = args.get_u64("attempt-seq", 0);
        if (!link.ok() || !attempt_seq.ok() || attempt_seq.value() == 0) {
            return fail("--link and --attempt-seq are required");
        }
        request.subject.fabric = view.value().fabric;
        request.subject.link = link.value();
        request.attempt_seq = lff::AttemptSeq::from(attempt_seq.value());
        request.rationale = args.get("rationale", "operator rollback");
        if (!build_expectation(args, request.expected)) {
            return fail("authority expectation is malformed");
        }
        if (!parse_digest(args.get("idempotency"), request.idempotency_key)) {
            return fail("--idempotency must be 64 hexadecimal characters");
        }
        return report_decision(client.value()->request_rollback(request));
    }
    if (command == "apply" || command == "verify") {
        lff::ApplicationReport application;
        lff::VerificationReport verification;
        const lff::Result<lff::EngineView> view = client.value()->inspect(0);
        if (!view.ok()) {
            return report_status(view.status());
        }
        const lff::Result<lff::LinkName> link = lff::LinkName::parse(args.get("link"));
        const lff::Result<std::uint64_t> generation = args.get_u64("generation", 0);
        const lff::Result<std::uint64_t> attempt_seq = args.get_u64("attempt-seq", 0);
        if (!link.ok() || !generation.ok() || !attempt_seq.ok()) {
            return fail("--link, --generation and --attempt-seq are required");
        }
        lff::Digest attempt_id;
        if (!lff::Digest::parse(args.get("attempt-id"), attempt_id)) {
            return fail("--attempt-id must be 64 hexadecimal characters");
        }
        if (command == "apply") {
            const lff::Result<bool> accepted = args.get_bool("reject", false);
            if (!accepted.ok()) {
                return fail("--reject must be a boolean");
            }
            application.subject.fabric = view.value().fabric;
            application.subject.link = link.value();
            application.subject_generation = lff::LinkGeneration::from(generation.value());
            application.attempt_seq = lff::AttemptSeq::from(attempt_seq.value());
            application.attempt_id = attempt_id;
            application.applier_incarnation = client.value()->incarnation();
            application.applier_epoch = view.value().epoch;
            application.accepted = !accepted.value();
            application.detail = "ctl apply";
            return report_decision(client.value()->record_application(application));
        }
        const lff::Result<bool> observed = args.get_bool("not-observed", false);
        if (!observed.ok()) {
            return fail("--not-observed must be a boolean");
        }
        verification.subject.fabric = view.value().fabric;
        verification.subject.link = link.value();
        verification.subject_generation = lff::LinkGeneration::from(generation.value());
        verification.attempt_seq = lff::AttemptSeq::from(attempt_seq.value());
        verification.attempt_id = attempt_id;
        verification.verifier_incarnation = client.value()->incarnation();
        verification.verifier_epoch = view.value().epoch;
        verification.effect_observed = !observed.value();
        verification.detail = "ctl verify";
        return report_decision(client.value()->record_verification(verification));
    }
    if (command == "revalidate") {
        lff::RevalidationRequest request;
        const lff::Result<lff::EngineView> view = client.value()->inspect(0);
        if (!view.ok()) {
            return report_status(view.status());
        }
        const lff::Result<lff::LinkName> link = lff::LinkName::parse(args.get("link"));
        const lff::Result<std::uint64_t> generation = args.get_u64("generation", 0);
        const lff::Result<std::uint64_t> attempt_seq = args.get_u64("attempt-seq", 0);
        if (!link.ok() || !generation.ok() || !attempt_seq.ok()) {
            return fail("--link, --generation and --attempt-seq are required");
        }
        lff::RevalidationVerdict verdict{};
        if (!lff::revalidation_verdict_parse(args.get("verdict", "Unknown"), verdict)) {
            return fail("--verdict must be NotApplied, Applied or Unknown");
        }
        request.subject.fabric = view.value().fabric;
        request.subject.link = link.value();
        request.subject_generation = lff::LinkGeneration::from(generation.value());
        request.attempt_seq = lff::AttemptSeq::from(attempt_seq.value());
        request.verdict = verdict;
        request.observer_incarnation = client.value()->incarnation();
        request.observer_epoch = view.value().epoch;
        request.rationale = args.get("rationale", "ctl revalidate");
        return report_decision(client.value()->resolve_interrupted(request));
    }
    if (command == "claim") {
        const lff::Result<std::uint64_t> max_count = args.get_u64("max", 8);
        if (!max_count.ok() || max_count.value() == 0 || max_count.value() > 64) {
            return fail("--max must be between 1 and 64");
        }
        lff::Result<lff::ClaimResponse> response =
            client.value()->claim_directives(static_cast<std::size_t>(max_count.value()));
        if (!response.ok()) {
            return report_status(response.status());
        }
        std::cout << "directives=" << response.value().directives.size() << "\n";
        for (const lff::ApplyDirective& directive : response.value().directives) {
            std::cout << "directive subject=" << directive.subject.to_string()
                      << " generation=" << directive.subject_generation.value()
                      << " replacement=" << directive.replacement.to_string()
                      << " replacement_generation=" << directive.replacement_generation.value()
                      << " attempt_seq=" << directive.attempt_seq.value()
                      << " attempt_id=" << directive.attempt_id.hex()
                      << " authority=" << directive.authority.digest().hex() << "\n";
        }
        std::cout.flush();
        return kExitOk;
    }
    return usage();
}
