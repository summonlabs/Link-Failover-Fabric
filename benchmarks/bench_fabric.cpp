// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Completed-work benchmark. Every number reported here is work that finished:
// decisions that were made and committed, observations that were accepted,
// records that reached the medium, and requests that were answered.
#include "lff/client.hpp"
#include "lff/engine.hpp"
#include "lff/journal.hpp"
#include "lff/server.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace lff;

std::filesystem::path fresh_directory(const std::string& tag) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("lff-bench-" + tag);
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path, ignored);
    return path;
}

struct Fixture {
    FabricName fabric;
    FailoverPolicy policy;
    TopologySnapshot topology;
};

Fixture make_fixture() {
    Fixture fixture;
    fixture.fabric = FabricName::parse("bench-fabric").value();
    fixture.policy = FailoverPolicy::defaults();
    fixture.policy.generation = PolicyGeneration::from(1);
    fixture.topology.generation = TopologyGeneration::from(1);
    fixture.topology.links.push_back(LinkDefinition{
        LinkKey{fixture.fabric, LinkName::parse("alpha").value()}, LinkGeneration::from(1), 1000});
    fixture.topology.links.push_back(LinkDefinition{
        LinkKey{fixture.fabric, LinkName::parse("beta").value()}, LinkGeneration::from(2), 4000});
    fixture.topology.canonicalise();
    return fixture;
}

EngineOptions options_for(const Fixture& fixture, const std::filesystem::path& directory) {
    EngineOptions options;
    options.fabric = fixture.fabric;
    options.policy = fixture.policy;
    options.topology = fixture.topology;
    options.state_dir = directory;
    options.snapshot_every_records = 2048;
    return options;
}

void bench_decisions(const Fixture& fixture, std::size_t operations) {
    const std::filesystem::path directory = fresh_directory("decisions");
    Result<Engine> engine = Engine::open(options_for(fixture, directory));
    if (!engine.ok()) {
        std::printf("bench decisions: open failed: %s\n", engine.status().to_string().c_str());
        return;
    }
    const Incarnation publisher = Incarnation::generate();
    std::size_t grants = 0;
    std::size_t refusals = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < operations; ++i) {
        const std::uint64_t sequence = static_cast<std::uint64_t>(i) + 1;
        FailureReport failure;
        failure.subject.fabric = fixture.fabric;
        failure.subject.link = LinkName::parse("alpha").value();
        failure.link_generation = LinkGeneration::from(1);
        failure.topology_generation = fixture.topology.generation;
        failure.state = ObservationState::Down;
        failure.confidence = make_confidence(1000);
        failure.stamp.publisher = PublisherName::parse("monitor").value();
        failure.stamp.publisher_incarnation = publisher;
        failure.stamp.publisher_epoch = Epoch::from(1);
        failure.stamp.observation_seq = ObservationSeq::from(sequence);
        (void)engine.value().publish_failure_report(failure);

        ReplacementReport replacement;
        replacement.candidate.fabric = fixture.fabric;
        replacement.candidate.link = LinkName::parse("beta").value();
        replacement.link_generation = LinkGeneration::from(2);
        replacement.topology_generation = fixture.topology.generation;
        replacement.state = ObservationState::Up;
        replacement.confidence = make_confidence(1000);
        replacement.capacity_units = 4000;
        replacement.adjacency_authorized = true;
        replacement.stamp.publisher = PublisherName::parse("monitor").value();
        replacement.stamp.publisher_incarnation = publisher;
        replacement.stamp.publisher_epoch = Epoch::from(1);
        replacement.stamp.observation_seq = ObservationSeq::from(sequence);
        (void)engine.value().publish_replacement_report(replacement);

        FailoverRequest request;
        request.subject = failure.subject;
        request.subject_generation = failure.link_generation;
        ReplacementCandidate entry;
        entry.candidate = replacement.candidate;
        request.candidates.push_back(entry);
        Result<FailoverDecision> decision = engine.value().request_failover(request);
        if (decision.ok()) {
            if (decision.value().kind == DecisionKind::Grant) {
                ++grants;
            } else {
                ++refusals;
            }
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(finished - started).count();
    std::printf(
        "bench decisions operations=%zu grants=%zu non_grants=%zu seconds=%.3f per_second=%.0f\n",
        operations, grants, refusals, seconds, static_cast<double>(operations) / seconds);
    (void)engine.value().close();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void bench_evidence(std::size_t operations) {
    const std::filesystem::path directory = fresh_directory("evidence");
    Fixture fixture = make_fixture();
    Result<Engine> engine = Engine::open(options_for(fixture, directory));
    if (!engine.ok()) {
        std::printf("bench evidence: open failed: %s\n", engine.status().to_string().c_str());
        return;
    }
    const Incarnation publisher = Incarnation::generate();
    std::size_t accepted = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < operations; ++i) {
        FailureReport failure;
        failure.subject.fabric = fixture.fabric;
        failure.subject.link = LinkName::parse("alpha").value();
        failure.link_generation = LinkGeneration::from(1);
        failure.topology_generation = fixture.topology.generation;
        failure.state = ObservationState::Down;
        failure.confidence = make_confidence(1000);
        failure.stamp.publisher = PublisherName::parse("monitor").value();
        failure.stamp.publisher_incarnation = publisher;
        failure.stamp.publisher_epoch = Epoch::from(1);
        failure.stamp.observation_seq = ObservationSeq::from(static_cast<std::uint64_t>(i) + 1);
        if (engine.value().publish_failure_report(failure).ok()) {
            ++accepted;
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(finished - started).count();
    std::printf("bench evidence accepted=%zu seconds=%.3f per_second=%.0f\n", accepted, seconds,
                static_cast<double>(operations) / seconds);
    (void)engine.value().close();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void bench_journal(std::size_t records) {
    const std::filesystem::path directory = fresh_directory("journal");
    Result<JournalWriter> writer = JournalWriter::open(directory / "bench.jrnl", LineageSeq::from(0),
                                                       LineageSeq::from(0), true);
    if (!writer.ok()) {
        std::printf("bench journal: open failed\n");
        return;
    }
    std::vector<std::uint8_t> payload(512, 0x33);
    const auto started = std::chrono::steady_clock::now();
    std::size_t written = 0;
    for (std::size_t i = 0; i < records; ++i) {
        if (writer.value()
                .append(RecordType::AttemptUpdate,
                        LineageSeq::from(static_cast<std::uint64_t>(i) + 1), payload)
                .ok()) {
            ++written;
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(finished - started).count();
    std::printf("bench journal records=%zu written=%zu bytes=%llu seconds=%.3f per_second=%.0f\n",
                records, written,
                static_cast<unsigned long long>(writer.value().bytes_written()), seconds,
                static_cast<double>(written) / seconds);
    writer.value().close();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void bench_service(std::size_t requests, std::size_t threads) {
    const std::filesystem::path directory = fresh_directory("service");
    Fixture fixture = make_fixture();
    Result<Engine> engine = Engine::open(options_for(fixture, directory));
    if (!engine.ok()) {
        std::printf("bench service: open failed\n");
        return;
    }
    ServerOptions server_options;
    server_options.endpoint = Endpoint::parse("127.0.0.1:0").value();
    server_options.engine = &engine.value();
    server_options.max_sessions = threads + 4;
    Result<std::unique_ptr<Server>> server = Server::start(server_options);
    if (!server.ok()) {
        std::printf("bench service: listen failed\n");
        return;
    }
    const Endpoint endpoint = server.value()->local_endpoint();
    std::vector<std::unique_ptr<Client>> clients;
    for (std::size_t i = 0; i < threads; ++i) {
        Result<std::unique_ptr<Client>> client = Client::connect(endpoint, "bench");
        if (!client.ok()) {
            std::printf("bench service: connect failed\n");
            (void)server.value()->shutdown();
            (void)engine.value().close();
            return;
        }
        clients.push_back(std::move(client.value()));
    }

    const std::size_t per_thread = requests / threads;
    std::vector<std::thread> workers;
    std::vector<std::size_t> completed(threads, 0);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back([&, i]() {
            for (std::size_t step = 0; step < per_thread; ++step) {
                if (clients[i]->ping().ok()) {
                    ++completed[i];
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto finished = std::chrono::steady_clock::now();
    std::size_t total = 0;
    for (std::size_t count : completed) {
        total += count;
    }
    const double seconds = std::chrono::duration<double>(finished - started).count();
    std::printf("bench service sessions=%zu requests=%zu seconds=%.3f per_second=%.0f\n", threads,
                total, seconds, static_cast<double>(total) / seconds);
    for (std::unique_ptr<Client>& client : clients) {
        client->close();
    }
    (void)server.value()->shutdown();
    (void)engine.value().close();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t scale = 1;
    if (argc > 1) {
        std::size_t parsed = 0;
        for (const char* cursor = argv[1]; *cursor != '\0'; ++cursor) {
            if (*cursor < '0' || *cursor > '9' || parsed > 100000) {
                parsed = 1;
                break;
            }
            parsed = parsed * 10 + static_cast<std::size_t>(*cursor - '0');
        }
        scale = parsed == 0 ? 1 : parsed;
    }
    std::printf("Link Failover Fabric %s completed-work benchmark (scale=%zu)\n",
                std::string(kVersionString).c_str(), scale);
    const Fixture fixture = make_fixture();
    bench_journal(2000 * scale);
    bench_evidence(20000 * scale);
    bench_decisions(fixture, 20000 * scale);
    bench_service(4000 * scale, 4);
    std::printf("bench complete\n");
    return 0;
}
