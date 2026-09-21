// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Concurrency and lifecycle suite. Racing threads are released from a barrier
// rather than a sleep, so the outcome is deterministic in kind (exactly one
// winner) even though the interleaving is not.
#include "framework.hpp"
#include "support.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

struct Barrier {
    explicit Barrier(std::size_t participants) : threshold(participants) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex);
        ++arrived;
        if (arrived == threshold) {
            released = true;
            condition.notify_all();
            return;
        }
        condition.wait(lock, [this]() { return released; });
    }
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t threshold;
    std::size_t arrived{0};
    bool released{false};
};

FabricFixture race_fixture() {
    return make_fabric("race-fabric", 1, {{"alpha", 1, 1000}, {"beta", 2, 4000}, {"gamma", 3, 3000}});
}

}  // namespace

LFF_TEST(concurrency, racing_failover_requests_produce_exactly_one_grant) {
    TempDir dir("race-grant");
    FabricFixture fixture = race_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(fixture, "alpha", 1, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 2, ObservationState::Up, 1000, 4000, 1, true,
                                   "monitor", publisher, 1, 2)),
              "replacement beta");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "gamma", 3, ObservationState::Up, 1000, 3000, 1, true,
                                   "monitor", publisher, 1, 3)),
              "replacement gamma");

    constexpr std::size_t kThreads = 8;
    Barrier barrier(kThreads);
    std::vector<std::thread> threads;
    std::vector<int> outcomes(kThreads, -1);
    std::vector<std::uint64_t> sequences(kThreads, 0);
    std::atomic<std::size_t> grants{0};
    std::atomic<std::size_t> conflicts{0};

    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            barrier.arrive_and_wait();
            FailoverRequest request = failover_request(fixture, "alpha", 1, {"beta", "gamma"});
            Result<FailoverDecision> decision = engine.value().request_failover(request);
            if (!decision.ok()) {
                outcomes[i] = -2;
                return;
            }
            outcomes[i] = static_cast<int>(decision.value().kind);
            sequences[i] = decision.value().attempt_seq.value();
            if (decision.value().kind == DecisionKind::Grant) {
                grants.fetch_add(1);
            } else if (decision.value().outcome == Outcome::Conflict) {
                conflicts.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    LFF_CHECK_EQ(grants.load(), std::size_t{1});
    LFF_CHECK_EQ(conflicts.load(), kThreads - 1);
    const EngineView view = engine.value().inspect(16);
    LFF_CHECK_EQ(view.active_authorities, std::size_t{1});
    (void)view;
    // Every contender answers for the same governed generation, so every
    // response must name the single authoritative attempt sequence.
    std::set<std::uint64_t> distinct;
    for (std::uint64_t sequence : sequences) {
        distinct.insert(sequence);
    }
    LFF_CHECK_EQ(distinct.size(), std::size_t{1});
    LFF_CHECK_EQ(*distinct.begin(), 1ull);
    const EngineView final_view = engine.value().inspect(8);
    LFF_CHECK_EQ(final_view.recent_attempts.size(), std::size_t{1});
    LFF_CHECK(final_view.recent_attempts[0].attempt_seq.value() == 1);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(concurrency, concurrent_observations_are_serialised_safely) {
    TempDir dir("race-observe");
    FabricFixture fixture = race_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());

    constexpr std::size_t kThreads = 6;
    constexpr std::size_t kPerThread = 200;
    Barrier barrier(kThreads);
    std::vector<std::thread> threads;
    std::atomic<std::size_t> accepted{0};
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            barrier.arrive_and_wait();
            const Incarnation publisher = Incarnation::generate();
            const std::string name = "monitor-" + std::to_string(i);
            for (std::size_t step = 0; step < kPerThread; ++step) {
                const Status status = engine.value().publish_failure_report(make_failure(
                    fixture, "alpha", 1, ObservationState::Down, 1000, name, publisher, 1,
                    step + 1));
                if (status.ok()) {
                    accepted.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    LFF_CHECK_EQ(accepted.load(), kThreads * kPerThread);
    const EngineView view = engine.value().inspect(4);
    LFF_CHECK_EQ(view.tracked_publishers, kThreads);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(concurrency, concurrent_apply_and_verify_complete_once) {
    TempDir dir("race-apply");
    FabricFixture fixture = race_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(fixture, "alpha", 1, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 2, ObservationState::Up, 1000, 4000, 1, true,
                                   "monitor", publisher, 1, 2)),
              "replacement");
    const FailoverDecision grant =
        expect_decision(engine.value().request_failover(failover_request(fixture, "alpha", 1,
                                                                        {"beta"})),
                        "grant");

    constexpr std::size_t kThreads = 6;
    Barrier barrier(kThreads);
    std::vector<std::thread> threads;
    std::atomic<std::size_t> ok_count{0};
    std::atomic<std::size_t> conflict_count{0};
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            barrier.arrive_and_wait();
            Result<FailoverDecision> decision = engine.value().record_application(
                application_for(grant, publisher, engine.value().epoch()));
            if (!decision.ok()) {
                return;
            }
            if (decision.value().outcome == Outcome::Ok) {
                ok_count.fetch_add(1);
            } else if (decision.value().outcome == Outcome::Conflict) {
                conflict_count.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    LFF_CHECK_EQ(ok_count.load(), std::size_t{1});
    LFF_CHECK_EQ(conflict_count.load(), kThreads - 1);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(concurrency, concurrent_checkpoint_and_mutation_are_consistent) {
    TempDir dir("race-checkpoint");
    FabricFixture fixture = race_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();

    Barrier barrier(2);
    std::atomic<bool> stop{false};
    std::thread mutator([&]() {
        barrier.arrive_and_wait();
        for (std::size_t step = 0; step < 300 && !stop.load(); ++step) {
            (void)engine.value().publish_failure_report(make_failure(
                fixture, "alpha", 1, ObservationState::Down, 1000, "monitor", publisher, 1,
                step + 1));
        }
    });
    std::thread checkpointer([&]() {
        barrier.arrive_and_wait();
        for (std::size_t step = 0; step < 40; ++step) {
            (void)engine.value().checkpoint();
        }
        stop.store(true);
    });
    mutator.join();
    checkpointer.join();
    const EngineView view = engine.value().inspect(4);
    LFF_CHECK(!view.store_failed);
    LFF_CHECK_EQ(view.boot_count, 1ull);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(concurrency, engine_is_usable_from_many_threads_until_close) {
    TempDir dir("race-close");
    FabricFixture fixture = race_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(fixture, "alpha", 1, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 2, ObservationState::Up, 1000, 4000, 1, true,
                                   "monitor", publisher, 1, 2)),
              "replacement");
    expect_ok(engine.value().close(), "close");

    constexpr std::size_t kThreads = 4;
    Barrier barrier(kThreads);
    std::vector<std::thread> threads;
    std::atomic<std::size_t> closed{0};
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            barrier.arrive_and_wait();
            for (std::size_t step = 0; step < 50; ++step) {
                Result<FailoverDecision> decision = engine.value().request_failover(
                    failover_request(fixture, "alpha", 1, {"beta"}));
                if (!decision.ok() && decision.status().any_of(Outcome::Closed)) {
                    closed.fetch_add(1);
                    return;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    LFF_CHECK_EQ(closed.load(), kThreads);
}