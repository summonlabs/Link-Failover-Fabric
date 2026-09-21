// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Scale suite. It measures *completed* work at several sizes and asserts the
// properties that matter for a long-running runtime: retained state stays
// bounded, per-operation cost does not grow with history, and the ratio between
// scales stays close to linear.
#include "framework.hpp"
#include "support.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

FabricFixture scale_fixture() {
    return make_fabric("scale-fabric", 1, {{"alpha", 1, 1000}, {"beta", 2, 4000}});
}

struct Measurement {
    std::size_t operations{0};
    double milliseconds{0.0};
    double per_operation_micros{0.0};
    std::size_t retained_attempts{0};
    std::size_t retained_decisions{0};
    std::uint64_t journal_bytes{0};
};

Measurement measure(const std::string& tag, std::size_t operations, bool checkpoint_every_round) {
    TempDir dir("scale-" + tag);
    FabricFixture fixture = scale_fixture();
    EngineOptions options = engine_options(fixture, dir.path());
    options.snapshot_every_records = 1024;
    options.limits.max_retained_decisions = 4096;
    Result<Engine> engine = Engine::open(options);
    if (!engine.ok()) {
        LFF_FAIL("engine open failed: " + engine.status().to_string());
    }
    const Incarnation publisher = Incarnation::generate();
    Measurement measurement;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < operations; ++i) {
        // Each operation is a complete, committed decision cycle.
        const std::uint64_t sequence = static_cast<std::uint64_t>(i) + 1;
        expect_ok(engine.value().publish_failure_report(make_failure(
                      fixture, "alpha", 1, ObservationState::Down, 1000, "monitor", publisher, 1,
                      sequence)),
                  "scale failure report");
        expect_ok(engine.value().publish_replacement_report(make_replacement(
                      fixture, "beta", 2, ObservationState::Up, 1000, 4000, 1, true, "monitor",
                      publisher, 1, sequence)),
                  "scale replacement report");
        Result<FailoverDecision> decision = engine.value().request_failover(
            failover_request(fixture, "alpha", 1, {"beta"}));
        if (!decision.ok()) {
            LFF_FAIL("scale failover failed: " + decision.status().to_string());
        }
        if (decision.value().kind == DecisionKind::Grant) {
            expect_ok(engine.value()
                          .record_application(application_for(decision.value(), publisher,
                                                              engine.value().epoch()))
                          .status(),
                      "scale acknowledge");
            expect_ok(engine.value()
                          .record_verification(verification_for(decision.value(), publisher,
                                                                engine.value().epoch()))
                          .status(),
                      "scale verify");
        }
        if (checkpoint_every_round && (i % 512) == 511) {
            expect_ok(engine.value().checkpoint(), "scale checkpoint");
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    measurement.operations = operations;
    measurement.milliseconds =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurement.per_operation_micros = measurement.milliseconds * 1000.0 /
                                       static_cast<double>(operations);
    const EngineView view = engine.value().inspect(0);
    measurement.retained_attempts = view.retained_attempts;
    measurement.retained_decisions = view.retained_decisions;
    measurement.journal_bytes = byte_size(dir.file("lineage.jrnl"));
    expect_ok(engine.value().close(), "scale close");
    return measurement;
}

}  // namespace

LFF_TEST(scale, completed_work_scales_close_to_linearly_and_state_stays_bounded) {
    const std::size_t small = 2000;
    const std::size_t large = 20000;
    const Measurement first = measure("small", small, true);
    const Measurement second = measure("large", large, true);

    std::printf("scale small ops=%zu ms=%.1f us_per_op=%.3f attempts=%zu decisions=%zu journal=%llu\n",
                first.operations, first.milliseconds, first.per_operation_micros,
                first.retained_attempts, first.retained_decisions,
                static_cast<unsigned long long>(first.journal_bytes));
    std::printf("scale large ops=%zu ms=%.1f us_per_op=%.3f attempts=%zu decisions=%zu journal=%llu\n",
                second.operations, second.milliseconds, second.per_operation_micros,
                second.retained_attempts, second.retained_decisions,
                static_cast<unsigned long long>(second.journal_bytes));

    // Retained state must not grow with the number of completed operations.
    LFF_CHECK_MSG(second.retained_decisions <= 4096, "decision log grew past its bound");
    LFF_CHECK_MSG(second.retained_attempts <= 4096, "attempt table grew past its bound");
    LFF_CHECK_MSG(second.journal_bytes < 8u * 1024u * 1024u,
                  "journal grew past the compaction bound");

    // A ten-fold increase in work must not cost far more than ten-fold per
    // operation. The bound is deliberately loose: it detects accidental
    // O(N^2)/history-dependent behaviour, not micro-variance.
    const double ratio = second.per_operation_micros / first.per_operation_micros;
    std::printf("scale per_operation_ratio=%.3f\n", ratio);
    LFF_CHECK_MSG(ratio < 4.0,
                  format_string("per-operation cost grew %.2fx between scales", ratio));
}

LFF_TEST(scale, decision_cost_is_independent_of_candidate_count_within_the_bound) {
    const std::vector<std::size_t> sizes = {1, 8, 64, 256};
    double previous = 0.0;
    for (std::size_t candidates : sizes) {
        TempDir dir("scale-candidates");
        FabricFixture fixture = scale_fixture();
        for (std::size_t i = 0; i < 400; ++i) {
            const std::string name = "alt-" + std::to_string(i);
            LinkDefinition definition;
            definition.link.fabric = fixture.fabric;
            definition.link.link = LinkName::parse(name).value();
            definition.generation = LinkGeneration::from(10 + i);
            definition.capacity_units = 4000;
            fixture.topology.links.push_back(definition);
        }
        fixture.topology.canonicalise();
        fixture.policy.max_candidates = kMaxPolicyCandidates;
        EngineOptions options = engine_options(fixture, dir.path());
        options.limits.max_candidates_per_request = kMaxPolicyCandidates;
        Result<Engine> engine = Engine::open(options);
        LFF_CHECK(engine.ok());
        const Incarnation publisher = Incarnation::generate();
        expect_ok(engine.value().publish_failure_report(make_failure(
                      fixture, "alpha", 1, ObservationState::Down, 1000, "monitor", publisher, 1, 1)),
                  "failure");
        for (std::size_t i = 0; i < candidates; ++i) {
            expect_ok(engine.value().publish_replacement_report(make_replacement(
                          fixture, "alt-" + std::to_string(i), 10 + i, ObservationState::Up, 1000,
                          4000, 1, true, "monitor", publisher, 1, 2 + i)),
                      "replacement");
        }
        FailoverRequest request = failover_request(fixture, "alpha", 1, {});
        for (std::size_t i = 0; i < candidates; ++i) {
            request.candidates.push_back(candidate(fixture, "alt-" + std::to_string(i)));
        }
        constexpr std::size_t kIterations = 400;
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < kIterations; ++i) {
            Result<FailoverDecision> decision = engine.value().request_failover(request);
            if (!decision.ok()) {
                LFF_FAIL("candidate scale request failed: " + decision.status().to_string());
            }
        }
        const auto finished = std::chrono::steady_clock::now();
        const double micros =
            std::chrono::duration<double, std::micro>(finished - started).count() /
            static_cast<double>(kIterations);
        std::printf("scale candidates=%zu us_per_decision=%.2f\n", candidates, micros);
        if (previous > 0.0) {
            LFF_CHECK_MSG(micros < previous * 64.0 + 200.0,
                          "decision cost exploded with the candidate count");
        }
        previous = micros;
        expect_ok(engine.value().close(), "close");
    }
}

LFF_TEST(scale, journal_append_throughput_is_stable) {
    TempDir dir("scale-journal");
    Result<JournalWriter> writer = JournalWriter::open(dir.file("bulk.jrnl"), LineageSeq::from(0),
                                                       LineageSeq::from(0), true);
    LFF_CHECK(writer.ok());
    std::vector<std::uint8_t> payload(256, 0x5A);
    constexpr std::size_t kRecords = 4000;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kRecords; ++i) {
        expect_ok(writer.value().append(RecordType::AttemptUpdate,
                                        LineageSeq::from(static_cast<std::uint64_t>(i) + 1), payload),
                  "bulk append");
    }
    const auto finished = std::chrono::steady_clock::now();
    const double micros =
        std::chrono::duration<double, std::micro>(finished - started).count() /
        static_cast<double>(kRecords);
    std::printf("scale journal_records=%zu us_per_append=%.2f bytes=%llu\n", kRecords, micros,
                static_cast<unsigned long long>(writer.value().bytes_written()));
    writer.value().close();
    LFF_CHECK_MSG(micros < 5000.0, "durable append latency is implausible");
    {
        std::size_t replayed = 0;
        JournalReplayHooks hooks;
        hooks.on_record = [&replayed](const RecordView&) {
            ++replayed;
            return Status::success();
        };
        const Result<JournalOpenResult> result =
            JournalReader::replay(dir.file("bulk.jrnl"), false, hooks);
        expect_ok(result.status(), "bulk replay");
        LFF_CHECK_EQ(replayed, kRecords);
    }
}