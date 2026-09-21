// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Downstream consumer of the installed Link Failover Fabric package. It links
// only `lff::lff` and exercises the public surface: identity, policy, engine
// construction, a failover decision and a durable restart.
#include "lff/engine.hpp"
#include "lff/hash.hpp"
#include "lff/version.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    const std::filesystem::path state_dir =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::temp_directory_path() / "lff-consumer-state";
    std::error_code ignored;
    std::filesystem::remove_all(state_dir, ignored);

    std::printf("version=%s\n", std::string(lff::kVersionString).c_str());
    std::printf("toolchain=%s arch=%s\n", std::string(lff::build_toolchain()).c_str(),
                std::string(lff::build_architecture()).c_str());

    const lff::Result<lff::FabricName> fabric = lff::FabricName::parse("consumer-fabric");
    if (!fabric.ok()) {
        std::printf("fabric_name=rejected\n");
        return 1;
    }

    lff::FailoverPolicy policy = lff::FailoverPolicy::defaults();
    policy.generation = lff::PolicyGeneration::from(1);

    lff::TopologySnapshot topology;
    topology.generation = lff::TopologyGeneration::from(1);
    topology.links.push_back(lff::LinkDefinition{
        lff::LinkKey{fabric.value(), lff::LinkName::parse("primary").value()},
        lff::LinkGeneration::from(1), 1000});
    topology.links.push_back(lff::LinkDefinition{
        lff::LinkKey{fabric.value(), lff::LinkName::parse("standby").value()},
        lff::LinkGeneration::from(2), 2000});
    topology.canonicalise();

    lff::EngineOptions options;
    options.fabric = fabric.value();
    options.policy = policy;
    options.topology = topology;
    options.state_dir = state_dir;

    lff::Result<lff::Engine> engine = lff::Engine::open(options);
    if (!engine.ok()) {
        std::printf("open=failed:%s\n", engine.status().to_string().c_str());
        return 1;
    }
    std::printf("epoch=%llu\n", static_cast<unsigned long long>(engine.value().epoch().value()));

    const lff::Incarnation publisher = lff::Incarnation::generate();
    lff::FailureReport failure;
    failure.subject.fabric = fabric.value();
    failure.subject.link = lff::LinkName::parse("primary").value();
    failure.link_generation = lff::LinkGeneration::from(1);
    failure.topology_generation = lff::TopologyGeneration::from(1);
    failure.state = lff::ObservationState::Down;
    failure.confidence = lff::make_confidence(990);
    failure.stamp.publisher = lff::PublisherName::parse("consumer-monitor").value();
    failure.stamp.publisher_incarnation = publisher;
    failure.stamp.publisher_epoch = lff::Epoch::from(1);
    failure.stamp.observation_seq = lff::ObservationSeq::from(1);
    if (!engine.value().publish_failure_report(failure).ok()) {
        std::printf("publish_failure=failed\n");
        return 1;
    }

    lff::ReplacementReport replacement;
    replacement.candidate.fabric = fabric.value();
    replacement.candidate.link = lff::LinkName::parse("standby").value();
    replacement.link_generation = lff::LinkGeneration::from(2);
    replacement.topology_generation = lff::TopologyGeneration::from(1);
    replacement.state = lff::ObservationState::Up;
    replacement.confidence = lff::make_confidence(990);
    replacement.capacity_units = 2000;
    replacement.adjacency_authorized = true;
    replacement.stamp.publisher = lff::PublisherName::parse("consumer-monitor").value();
    replacement.stamp.publisher_incarnation = publisher;
    replacement.stamp.publisher_epoch = lff::Epoch::from(1);
    replacement.stamp.observation_seq = lff::ObservationSeq::from(2);
    if (!engine.value().publish_replacement_report(replacement).ok()) {
        std::printf("publish_replacement=failed\n");
        return 1;
    }

    lff::FailoverRequest request;
    request.subject = failure.subject;
    request.subject_generation = failure.link_generation;
    lff::ReplacementCandidate candidate_entry;
    candidate_entry.candidate = replacement.candidate;
    request.candidates.push_back(candidate_entry);

    lff::Result<lff::FailoverDecision> decision = engine.value().request_failover(request);
    if (!decision.ok()) {
        std::printf("failover=failed:%s\n", decision.status().to_string().c_str());
        return 1;
    }
    std::printf("decision=%s assertion=%s durable=%s\n",
                std::string(lff::decision_kind_name(decision.value().kind)).c_str(),
                std::string(lff::assertion_name(decision.value().assertion)).c_str(),
                decision.value().durable ? "true" : "false");
    std::printf("decision_id=%s\n", decision.value().decision_id.hex().c_str());
    std::printf("policy_digest=%s\n", engine.value().policy().digest().hex().c_str());

    if (decision.value().kind != lff::DecisionKind::Grant) {
        std::printf("expected=grant\n");
        return 1;
    }
    const lff::Status closed = engine.value().close();
    std::filesystem::remove_all(state_dir, ignored);
    std::printf("closed=%s\n", closed.ok() ? "ok" : "failed");
    return closed.ok() ? 0 : 1;
}
