// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A complete, self-contained failover: observe a link failure, ask the runtime
// which alternate may replace it, apply the replacement, and verify the effect.
//
// Everything in this example is SYNTHETIC: the fabric, the observations and the
// applier are local fixtures, and no switch, NIC or RDMA device is involved.
#include "lff/engine.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

lff::FailureReport failure_report(const lff::FabricName& fabric, const lff::Incarnation& publisher,
                                  std::uint64_t sequence) {
    lff::FailureReport report;
    report.subject.fabric = fabric;
    report.subject.link = lff::LinkName::parse("uplink-a").value();
    report.link_generation = lff::LinkGeneration::from(2);
    report.topology_generation = lff::TopologyGeneration::from(1);
    report.state = lff::ObservationState::Down;
    report.confidence = lff::make_confidence(990);
    report.stamp.publisher = lff::PublisherName::parse("synthetic-monitor").value();
    report.stamp.publisher_incarnation = publisher;
    report.stamp.publisher_epoch = lff::Epoch::from(1);
    report.stamp.observation_seq = lff::ObservationSeq::from(sequence);
    return report;
}

lff::ReplacementReport replacement_report(const lff::FabricName& fabric,
                                          const lff::Incarnation& publisher, std::uint64_t sequence) {
    lff::ReplacementReport report;
    report.candidate.fabric = fabric;
    report.candidate.link = lff::LinkName::parse("uplink-b").value();
    report.link_generation = lff::LinkGeneration::from(5);
    report.topology_generation = lff::TopologyGeneration::from(1);
    report.state = lff::ObservationState::Up;
    report.confidence = lff::make_confidence(980);
    report.capacity_units = 40000;
    report.cost_units = 7;
    report.adjacency_authorized = true;
    report.stamp.publisher = lff::PublisherName::parse("synthetic-monitor").value();
    report.stamp.publisher_incarnation = publisher;
    report.stamp.publisher_epoch = lff::Epoch::from(1);
    report.stamp.observation_seq = lff::ObservationSeq::from(sequence);
    return report;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path state_dir =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::temp_directory_path() / "lff-example-minimal";
    std::error_code ignored;
    std::filesystem::remove_all(state_dir, ignored);

    const lff::Result<lff::FabricName> fabric = lff::FabricName::parse("synthetic-fabric");
    if (!fabric.ok()) {
        std::cerr << "fabric name rejected\n";
        return 1;
    }

    lff::FailoverPolicy policy = lff::FailoverPolicy::defaults();
    policy.generation = lff::PolicyGeneration::from(1);
    policy.min_replacement_capacity = 1000;

    lff::TopologySnapshot topology;
    topology.generation = lff::TopologyGeneration::from(1);
    {
        lff::LinkDefinition primary;
        primary.link.fabric = fabric.value();
        primary.link.link = lff::LinkName::parse("uplink-a").value();
        primary.generation = lff::LinkGeneration::from(2);
        primary.capacity_units = 10000;
        topology.links.push_back(primary);

        lff::LinkDefinition alternate;
        alternate.link.fabric = fabric.value();
        alternate.link.link = lff::LinkName::parse("uplink-b").value();
        alternate.generation = lff::LinkGeneration::from(5);
        alternate.capacity_units = 40000;
        topology.links.push_back(alternate);
        topology.canonicalise();
    }

    lff::EngineOptions options;
    options.fabric = fabric.value();
    options.policy = policy;
    options.topology = topology;
    options.state_dir = state_dir;

    lff::Result<lff::Engine> engine = lff::Engine::open(options);
    if (!engine.ok()) {
        std::cerr << "open failed: " << engine.status().to_string() << "\n";
        return 1;
    }
    std::cout << "SYNTHETIC fixture; state dir " << state_dir.string() << "\n";

    const lff::Incarnation publisher = lff::Incarnation::generate();
    const lff::Status observed =
        engine.value().publish_failure_report(failure_report(fabric.value(), publisher, 1));
    const lff::Status alternate =
        engine.value().publish_replacement_report(replacement_report(fabric.value(), publisher, 2));
    std::cout << "publish-failure    " << observed.to_string() << "\n";
    std::cout << "publish-replacement " << alternate.to_string() << "\n";

    lff::FailoverRequest request;
    request.subject.fabric = fabric.value();
    request.subject.link = lff::LinkName::parse("uplink-a").value();
    request.subject_generation = lff::LinkGeneration::from(2);
    lff::ReplacementCandidate candidate;
    candidate.candidate.fabric = fabric.value();
    candidate.candidate.link = lff::LinkName::parse("uplink-b").value();
    request.candidates.push_back(candidate);

    lff::Result<lff::FailoverDecision> granted = engine.value().request_failover(request);
    if (!granted.ok()) {
        std::cerr << "failover failed: " << granted.status().to_string() << "\n";
        return 1;
    }
    std::cout << "failover           " << granted.value().to_string() << "\n";
    if (granted.value().kind != lff::DecisionKind::Grant) {
        std::cerr << "expected a grant\n";
        return 1;
    }

    lff::ApplicationReport application;
    application.subject = request.subject;
    application.subject_generation = request.subject_generation;
    application.attempt_seq = granted.value().attempt_seq;
    application.attempt_id = lff::compute_attempt_id(fabric.value(), request.subject,
                                                     request.subject_generation,
                                                     granted.value().attempt_seq);
    application.applier_incarnation = publisher;
    application.applier_epoch = engine.value().epoch();
    application.accepted = true;
    application.detail = "synthetic applier";
    lff::Result<lff::FailoverDecision> acknowledged =
        engine.value().record_application(application);
    std::cout << "acknowledge        "
              << (acknowledged.ok() ? acknowledged.value().to_string()
                                    : acknowledged.status().to_string())
              << "\n";

    lff::VerificationReport verification;
    verification.subject = request.subject;
    verification.subject_generation = request.subject_generation;
    verification.attempt_seq = granted.value().attempt_seq;
    verification.attempt_id = application.attempt_id;
    verification.verifier_incarnation = publisher;
    verification.verifier_epoch = engine.value().epoch();
    verification.effect_observed = true;
    verification.detail = "synthetic verification";
    lff::Result<lff::FailoverDecision> verified = engine.value().record_verification(verification);
    std::cout << "verify             "
              << (verified.ok() ? verified.value().to_string()
                                : verified.status().to_string())
              << "\n";

    std::cout << engine_view_to_string(engine.value().inspect(4));
    const lff::Status closed = engine.value().close();
    std::filesystem::remove_all(state_dir, ignored);
    return closed.ok() ? 0 : 1;
}
