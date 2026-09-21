// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Model-based property suite. A seeded driver issues a long random sequence of
// legitimate operations and the invariants below are re-checked after *every*
// operation, not only at the end. The seed is printed by the framework's case
// name and can be changed from the environment for reproduction.
#include "framework.hpp"
#include "support.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

const std::vector<std::string>& link_names() {
    static const std::vector<std::string> names = {"alpha", "beta", "gamma", "delta", "epsilon"};
    return names;
}

FabricFixture property_fixture() {
    return make_fabric("property-fabric", 1,
                       {{"alpha", 1, 100}, {"beta", 2, 2000}, {"gamma", 3, 3000},
                        {"delta", 4, 4000}, {"epsilon", 5, 5000}});
}

struct InvariantChecker {
    using Key = std::pair<std::string, std::uint64_t>;

    static void check(Engine& engine, const FabricFixture& fixture,
                      const std::map<Key, Digest>& live_authority, std::size_t step) {
        const EngineView view = engine.inspect(64);
        LFF_CHECK_MSG(!view.store_failed, format_string("step %zu: store failed", step));
        LFF_CHECK_MSG(view.active_authorities <= live_authority.size(),
                      format_string("step %zu: more active authorities than grants", step));
        LFF_CHECK_MSG(view.retained_decisions <= 4096,
                      format_string("step %zu: decision log unbounded", step));
        LFF_CHECK_MSG(view.tracked_publishers <= 512,
                      format_string("step %zu: publisher registry unbounded", step));

        std::set<Digest> attempt_ids;
        std::map<Key, std::size_t> authority_counts;
        for (const AttemptRecord& record : view.recent_attempts) {
            LFF_CHECK_MSG(attempt_ids.insert(record.attempt_id).second,
                          format_string("step %zu: duplicate attempt identity", step));
            LFF_CHECK_MSG(record.create_seq.value() <= record.update_seq.value(),
                          format_string("step %zu: attempt lineage regressed", step));
            LFF_CHECK_MSG(record.subject.fabric == fixture.fabric,
                          format_string("step %zu: attempt from a foreign fabric", step));
            // Authority minted by a previous incarnation never counts as live.
            if (attempt_phase_holds_authority(record.phase) &&
                record.authority.coordinator == view.coordinator &&
                record.authority.epoch.value() == view.epoch.value()) {
                ++authority_counts[{record.subject.to_string(),
                                     record.subject_generation.value()}];
            }
        }
        for (const auto& entry : authority_counts) {
            LFF_CHECK_MSG(entry.second <= 1,
                          format_string("step %zu: two authorities for one subject generation", step));
        }
        for (const auto& entry : live_authority) {
            const auto found = authority_counts.find(entry.first);
            LFF_CHECK_MSG(found != authority_counts.end(),
                          format_string("step %zu: a granted subject lost its authority", step));
        }
        for (const FenceRecord& fence : view.recent_fences) {
            LFF_CHECK_MSG(fence.seq.value() <= view.last_seq.value(),
                          format_string("step %zu: fence beyond the lineage head", step));
        }
    }

    static void bind_authority(std::map<Key, Digest>& live, const FailoverDecision& decision) {
        if (decision.kind == DecisionKind::Grant && decision.outcome == Outcome::Ok) {
            live[{decision.subject.to_string(), decision.subject_generation.value()}] =
                decision.decision_id;
        }
        if (decision.kind == DecisionKind::Rollback && decision.outcome == Outcome::Ok) {
            live.erase({decision.subject.to_string(), decision.subject_generation.value()});
        }
    }
};

// The seed can be overridden for reproduction: set LFF_PROPERTY_SEED to the
// failing value reported by a previous run.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4996)  // getenv is used read-only, on a test-only path
#endif
std::uint64_t seed_from_environment(std::uint64_t fallback) {
    const char* raw = std::getenv("LFF_PROPERTY_SEED");
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    std::uint64_t value = 0;
    for (const char* cursor = raw; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return fallback;
        }
        value = value * 10 + static_cast<std::uint64_t>(*cursor - '0');
    }
    return value == 0 ? fallback : value;
}
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

// Reconstructs the decision identity of a retained attempt so that the
// application and verification helpers can address it.
FailoverDecision decision_for(const AttemptRecord& record) {
    FailoverDecision decision;
    decision.fabric = record.subject.fabric;
    decision.subject = record.subject;
    decision.subject_generation = record.subject_generation;
    decision.attempt_seq = record.attempt_seq;
    return decision;
}

}  // namespace

LFF_TEST(property, engine_invariants_hold_after_every_operation) {
    const std::uint64_t seed = seed_from_environment(0x1BADB002ull);
    Random random(seed);
    TempDir dir("property-invariants");
    FabricFixture fixture = property_fixture();
    fixture.policy.max_attempts_per_generation = 8;
    fixture.policy.max_candidates = 8;
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());

    std::map<std::pair<std::string, std::uint64_t>, Digest> live;
    std::vector<Incarnation> publishers = {Incarnation::generate(), Incarnation::generate()};
    std::uint64_t sequence = 1;

    for (std::size_t step = 0; step < 400; ++step) {
        const std::uint32_t choice = random.below32(10);
        const std::size_t link_index = static_cast<std::size_t>(random.below(link_names().size()));
        const std::string& link = link_names()[link_index];
        const std::uint64_t generation = static_cast<std::uint64_t>(link_index) + 1;
        const std::size_t publisher_index = static_cast<std::size_t>(random.below(2));
        const Incarnation& publisher = publishers[publisher_index];
        const std::string publisher_name = "monitor-" + std::to_string(publisher_index);

        if (choice < 3) {
            const ObservationState state =
                static_cast<ObservationState>(random.below32(kObservationStateCount));
            expect_ok(engine.value().publish_failure_report(
                          make_failure(fixture, link, generation, state, random.below32(1001),
                                       publisher_name, publisher, 1, sequence)),
                      "property failure report");
            ++sequence;  // one monotonic space per publisher and subject
        } else if (choice < 6) {
            const ObservationState state =
                static_cast<ObservationState>(random.below32(kObservationStateCount));
            expect_ok(engine.value().publish_replacement_report(
                          make_replacement(fixture, link, generation, state, random.below32(1001),
                                           random.below32(6000), random.below32(10),
                                           random.chance(3, 4), publisher_name, publisher, 1,
                                           sequence)),
                      "property replacement report");
            ++sequence;
        } else if (choice < 8) {
            FailoverRequest request = failover_request(fixture, "alpha", 1, {});
            const std::uint32_t count = 1 + random.below32(3);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::string& name = link_names()[1 + random.below(4)];
                bool duplicate = false;
                for (const ReplacementCandidate& existing : request.candidates) {
                    duplicate = duplicate || existing.candidate.link.value() == name;
                }
                if (!duplicate) {
                    request.candidates.push_back(candidate(fixture, name));
                }
            }
            Result<FailoverDecision> decision = engine.value().request_failover(request);
            if (decision.ok()) {
                InvariantChecker::bind_authority(live, decision.value());
            }
        } else if (choice < 9) {
            const EngineView view = engine.value().inspect(64);
            for (const AttemptRecord& record : view.recent_attempts) {
                if (record.subject.link.value() != "alpha") {
                    continue;
                }
                if (attempt_phase_is_terminal(record.phase)) {
                    continue;
                }
                if (random.chance(1, 2)) {
                    ApplicationReport report =
                        application_for(decision_for(record), publisher, engine.value().epoch(),
                                        random.chance(4, 5));
                    (void)engine.value().record_application(report);
                } else {
                    VerificationReport report = verification_for(
                        decision_for(record), publisher, engine.value().epoch(), random.chance(4, 5));
                    (void)engine.value().record_verification(report);
                }
                break;
            }
        } else {
            if (random.chance(1, 4)) {
                (void)engine.value().checkpoint();
            }
        }
        InvariantChecker::check(engine.value(), fixture, live, step);
    }
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(property, identical_operation_sequences_produce_identical_decisions) {
    const auto run = [](const std::filesystem::path& directory) {
        FabricFixture fixture = property_fixture();
        Result<Engine> engine = Engine::open(engine_options(fixture, directory));
        if (!engine.ok()) {
            LFF_FAIL("engine open failed: " + engine.status().to_string());
        }
        Random random(seed_from_environment(0xDEADBEEFull));
        std::vector<Incarnation> publishers = {Incarnation::generate(), Incarnation::generate()};
        std::vector<std::string> keys;
        std::uint64_t sequence = 1;
        for (std::size_t step = 0; step < 200; ++step) {
            const std::size_t link_index =
                static_cast<std::size_t>(random.below(link_names().size()));
            const std::string& link = link_names()[link_index];
            // Link generations come from the fixture, not from the position in
            // the name table, so the request always binds a real generation.
            const std::uint64_t generation = static_cast<std::uint64_t>(link_index) + 1;
            const std::size_t index = static_cast<std::size_t>(random.below(2));
            const ObservationState state =
                static_cast<ObservationState>(random.below32(kObservationStateCount));
            // Failure and replacement observations share one monotonic sequence
            // space per (publisher, subject), so the counter never repeats.
            expect_ok(engine.value().publish_failure_report(
                          make_failure(fixture, link, generation, state, random.below32(1001),
                                       "monitor-" + std::to_string(index), publishers[index], 1,
                                       sequence)),
                      "determinism failure report");
            ++sequence;
            expect_ok(engine.value().publish_replacement_report(
                          make_replacement(fixture, link, generation, state, random.below32(1001),
                                           random.below32(6000), random.below32(10),
                                           random.chance(3, 4), "monitor-" + std::to_string(index),
                                           publishers[index], 1, sequence)),
                      "determinism replacement report");
            ++sequence;
            FailoverRequest request = failover_request(fixture, "alpha", 1, {"beta", "gamma"});
            Result<FailoverDecision> decision = engine.value().request_failover(request);
            if (decision.ok()) {
                keys.push_back(decision.value().canonical_key());
                if (decision.value().kind == DecisionKind::Grant) {
                    expect_ok(engine.value()
                                  .record_application(application_for(decision.value(), publishers[0],
                                                                      engine.value().epoch()))
                                  .status(),
                              "determinism acknowledge");
                    expect_ok(engine.value()
                                  .record_verification(verification_for(decision.value(),
                                                                        publishers[0],
                                                                        engine.value().epoch()))
                                  .status(),
                              "determinism verify");
                }
            }
        }
        const std::string view = engine_view_to_string(engine.value().inspect(16));
        expect_ok(engine.value().close(), "close");
        return std::make_pair(keys, view);
    };

    TempDir first_dir("property-determinism-a");
    TempDir second_dir("property-determinism-b");
    const auto first = run(first_dir.path());
    const auto second = run(second_dir.path());
    LFF_CHECK_EQ(first.first.size(), second.first.size());
    for (std::size_t i = 0; i < first.first.size(); ++i) {
        if (first.first[i] != second.first[i]) {
            LFF_FAIL(format_string("decision %zu differs between identical runs", i));
        }
    }
}

LFF_TEST(property, bounded_history_never_grows_without_limit) {
    TempDir dir("property-bounds");
    FabricFixture fixture = property_fixture();
    EngineOptions options = engine_options(fixture, dir.path());
    options.limits.max_retained_decisions = 32;
    options.limits.max_attempts = 24;
    options.limits.max_idempotency = 16;
    Result<Engine> engine = Engine::open(options);
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    Random random(0x5A5A5A5Aull);
    for (std::size_t step = 0; step < 300; ++step) {
        const std::string& link = link_names()[random.below(link_names().size())];
        const std::uint64_t generation =
            1 + static_cast<std::uint64_t>(random.below(5));
        const ObservationState state =
            static_cast<ObservationState>(random.below32(kObservationStateCount));
        (void)engine.value().publish_failure_report(
            make_failure(fixture, link, generation, state, random.below32(1001), "monitor",
                         publisher, 1, step + 1));
        (void)engine.value().publish_replacement_report(
            make_replacement(fixture, link, generation, state, random.below32(1001),
                             random.below32(6000), 1, true, "monitor", publisher, 1, step + 1));
        FailoverRequest request = failover_request(fixture, "alpha", 1, {"beta"});
        request.idempotency_key = sha256("idem-" + std::to_string(step));
        (void)engine.value().request_failover(request);
        const EngineView view = engine.value().inspect(8);
        if (view.retained_decisions > 32) {
            LFF_FAIL(format_string("retained decisions exceeded the bound at step %zu", step));
        }
        if (view.retained_attempts > 24) {
            LFF_FAIL(format_string("retained attempts exceeded the bound at step %zu", step));
        }
    }
    expect_ok(engine.value().close(), "close");
}