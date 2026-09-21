// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Restart, fencing and durable-lineage semantics, proven in process by opening
// the same lineage directory repeatedly.
#include "framework.hpp"
#include "support.hpp"

#include <string>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

FabricFixture restart_fixture() {
    return make_fabric("restart-fabric", 1,
                       {{"alpha", 1, 1000}, {"beta", 2, 4000}, {"gamma", 3, 3000}});
}

struct RestartHarness {
    TempDir dir{"restart"};
    FabricFixture fixture{restart_fixture()};
    Incarnation publisher{Incarnation::generate()};

    explicit RestartHarness(const std::string& tag) : dir("restart-" + tag) {}

    Result<Engine> open(const EngineOptions* override_options = nullptr) {
        EngineOptions options = engine_options(fixture, dir.path());
        if (override_options != nullptr) {
            options = *override_options;
        }
        Result<Engine> engine = Engine::open(options);
        if (!engine.ok()) {
            LFF_FAIL("engine open failed: " + engine.status().to_string());
        }
        return engine;
    }

    void publish(Engine& engine, const std::string& link, std::uint64_t generation,
                 ObservationState state, std::uint64_t sequence) {
        expect_ok(engine.publish_failure_report(make_failure(fixture, link, generation, state, 1000,
                                                             "monitor", publisher, 1, sequence)),
                  "publish failure");
    }
    void publish_alternate(Engine& engine, const std::string& link, std::uint64_t generation,
                           std::uint64_t sequence) {
        expect_ok(engine.publish_replacement_report(
                      make_replacement(fixture, link, generation, ObservationState::Up, 1000, 4000,
                                       1, true, "monitor", publisher, 1, sequence)),
                  "publish replacement");
    }
};

}  // namespace

LFF_TEST(restart, restart_preserves_lineage_but_restores_no_authority) {
    RestartHarness harness("lineage");
    FailoverDecision grant;
    {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        harness.publish(engine.value(), "alpha", 1, ObservationState::Down, 1);
        harness.publish_alternate(engine.value(), "beta", 2, 2);
        grant = expect_decision(engine.value().request_failover(
                                    failover_request(harness.fixture, "alpha", 1, {"beta"})),
                                "grant");
        expect_ok(engine.value().record_application(
                      application_for(grant, harness.publisher, engine.value().epoch()))
                      .status(),
                  "acknowledge");
        LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{1});
        expect_ok(engine.value().close(), "close");
    }
    {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        const EngineView view = engine.value().inspect(8);
        LFF_CHECK_EQ(view.active_authorities, std::size_t{0});
        LFF_CHECK_EQ(view.interrupted_attempts, std::size_t{1});
        LFF_CHECK_EQ(view.boot_count, 2ull);
        LFF_CHECK_EQ(view.recent_attempts.size(), std::size_t{1});
        LFF_CHECK(view.recent_attempts[0].phase == AttemptPhase::Interrupted);
        LFF_CHECK_EQ(view.recent_attempts[0].attempt_id.hex(), grant.attempt_id.hex());
        // The pre-restart lineage is preserved exactly, including the epoch that
        // minted it.
        LFF_CHECK_EQ(view.recent_attempts[0].authority.epoch.value(),
                     grant.authority.epoch.value());
        LFF_CHECK(view.recent_attempts[0].authority.coordinator != view.coordinator);
        LFF_CHECK(!view.recent_fences.empty());
        LFF_CHECK(view.recent_fences[0].kind == FenceKind::PreRestartAuthority);

        // Dynamic observation state was never durable: the request is now
        // indeterminate rather than authorised.
        const Result<FailoverDecision> decision = engine.value().request_failover(
            failover_request(harness.fixture, "alpha", 1, {"beta"}));
        const FailoverDecision& value = expect_decision(decision, "post restart failover");
        LFF_CHECK(value.kind == DecisionKind::Indeterminate);
        LFF_CHECK(value.outcome == Outcome::Indeterminate);
        bool saw = false;
        for (const Reason& reason : value.reasons) {
            saw = saw || reason.code == ReasonCode::RevalidationRequired;
        }
        LFF_CHECK(saw);
        expect_ok(engine.value().close(), "close");
    }
}

LFF_TEST(restart, revalidation_releases_or_reestablishes_authority) {
    RestartHarness harness("revalidate");
    FailoverDecision grant;
    {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        harness.publish(engine.value(), "alpha", 1, ObservationState::Down, 1);
        harness.publish_alternate(engine.value(), "beta", 2, 2);
        grant = expect_decision(engine.value().request_failover(
                                    failover_request(harness.fixture, "alpha", 1, {"beta"})),
                                "grant");
        expect_ok(engine.value().close(), "close");
    }
    {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        RevalidationRequest request;
        request.subject = grant.subject;
        request.subject_generation = grant.subject_generation;
        request.attempt_seq = grant.attempt_seq;
        request.verdict = RevalidationVerdict::NotApplied;
        request.observer_incarnation = Incarnation::generate();
        request.observer_epoch = engine.value().epoch();
        const FailoverDecision& value =
            expect_decision(engine.value().resolve_interrupted(request), "revalidation");
        LFF_CHECK(value.kind == DecisionKind::Revalidate);
        LFF_CHECK(value.outcome == Outcome::Ok);
        LFF_CHECK_EQ(engine.value().inspect(0).interrupted_attempts, std::size_t{0});
        LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{0});

        // Idempotent repeat of the same revalidation.
        const FailoverDecision again =
            expect_decision(engine.value().resolve_interrupted(request), "repeated revalidation");
        LFF_CHECK(again.idempotent_replay);
        LFF_CHECK(again.outcome == Outcome::Ok);
        LFF_CHECK(!again.durable);

        // A stale observer epoch is refused.
        RevalidationRequest stale = request;
        stale.observer_epoch = Epoch::from(engine.value().epoch().value() + 1);
        LFF_CHECK(!engine.value().resolve_interrupted(stale).ok());
        expect_ok(engine.value().close(), "close");
    }
    {
        // With NotApplied, the generation is free again once evidence returns.
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        harness.publisher = Incarnation::generate();
        harness.publish(engine.value(), "alpha", 1, ObservationState::Down, 10);
        harness.publish_alternate(engine.value(), "beta", 2, 11);
        const FailoverDecision& value = expect_decision(
            engine.value().request_failover(failover_request(harness.fixture, "alpha", 1,
                                                             {"beta"})),
            "second grant");
        LFF_CHECK(value.kind == DecisionKind::Grant);
        LFF_CHECK_EQ(value.attempt_seq.value(), 2ull);
        expect_ok(engine.value().close(), "close");
    }
}

LFF_TEST(restart, revalidation_applied_restores_authority_under_the_new_epoch) {
    RestartHarness harness("applied");
    FailoverDecision grant;
    {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        harness.publish(engine.value(), "alpha", 1, ObservationState::Down, 1);
        harness.publish_alternate(engine.value(), "beta", 2, 2);
        grant = expect_decision(engine.value().request_failover(
                                    failover_request(harness.fixture, "alpha", 1, {"beta"})),
                                "grant");
        expect_ok(engine.value().close(), "close");
    }
    Result<Engine> engine = harness.open();
    LFF_CHECK(engine.ok());
    RevalidationRequest request;
    request.subject = grant.subject;
    request.subject_generation = grant.subject_generation;
    request.attempt_seq = grant.attempt_seq;
    request.verdict = RevalidationVerdict::Applied;
    request.observer_incarnation = Incarnation::generate();
    request.observer_epoch = engine.value().epoch();
    const FailoverDecision& value =
        expect_decision(engine.value().resolve_interrupted(request), "applied revalidation");
    LFF_CHECK(value.assertion == Assertion::Authorization);
    LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{1});
    const EngineView view = engine.value().inspect(4);
    LFF_CHECK(view.recent_attempts[0].phase == AttemptPhase::Committed);
    // The re-established authority is bound to the *current* epoch and
    // incarnation, so it excludes competitors exactly like a fresh grant.
    LFF_CHECK_EQ(view.recent_attempts[0].authority.epoch.value(), engine.value().epoch().value());
    LFF_CHECK(view.recent_attempts[0].authority.coordinator == engine.value().incarnation());

    // Fresh evidence cannot buy a second authority for the same generation.
    harness.publisher = Incarnation::generate();
    harness.publish(engine.value(), "alpha", 1, ObservationState::Down, 50);
    harness.publish_alternate(engine.value(), "beta", 2, 51);
    const FailoverDecision blocked = expect_decision(
        engine.value().request_failover(failover_request(harness.fixture, "alpha", 1, {"beta"})),
        "competing request");
    LFF_CHECK(blocked.kind == DecisionKind::Refuse);
    LFF_CHECK(blocked.outcome == Outcome::Conflict);
    LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{1});
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(restart, interrupted_before_commit_is_distinguished_from_after_commit) {
    // A process that dies between the intent record and the grant record leaves
    // an Evaluated attempt. Its interruption must be reported distinctly.
    RestartHarness harness("before-commit");
    {
        StoreOptions options;
        options.directory = harness.dir.path();
        options.fabric = harness.fixture.fabric;
        options.policy = harness.fixture.policy;
        options.topology = harness.fixture.topology;
        Result<LineageStore> store = LineageStore::open(options);
        LFF_CHECK(store.ok());
        AttemptRecord record;
        record.subject.fabric = harness.fixture.fabric;
        record.subject.link = LinkName::parse("alpha").value();
        record.subject_generation = LinkGeneration::from(1);
        record.attempt_seq = AttemptSeq::from(1);
        record.attempt_id = compute_attempt_id(harness.fixture.fabric, record.subject,
                                               record.subject_generation, record.attempt_seq);
        record.phase = AttemptPhase::Evaluated;
        record.created_tick = 1;
        record.created_epoch = Epoch::from(1);
        record.coordinator = Incarnation::generate();
        const Status committed = store.value().commit(
            RecordType::AttemptCreate, encode_attempt_payload(record),
            [&store, &record](LineageSeq seq) {
                AttemptRecord stored = record;
                stored.create_seq = seq;
                stored.update_seq = seq;
                store.value().note_attempt(stored);
            });
        expect_ok(committed, "intent record");
        store.value().close();
    }
    Result<Engine> engine = harness.open();
    LFF_CHECK(engine.ok());
    const EngineView view = engine.value().inspect(8);
    LFF_CHECK_EQ(view.interrupted_attempts, std::size_t{1});
    LFF_CHECK_EQ(view.active_authorities, std::size_t{0});
    bool saw = false;
    for (const Reason& reason : view.recent_attempts[0].reasons) {
        saw = saw || reason.code == ReasonCode::InterruptedBeforeCommit;
    }
    LFF_CHECK(saw);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(restart, snapshot_compaction_keeps_lineage_and_bounds_the_journal) {
    RestartHarness harness("snapshot");
    {
        EngineOptions options = engine_options(harness.fixture, harness.dir.path());
        options.snapshot_every_records = 4;
        Result<Engine> engine = harness.open(&options);
        LFF_CHECK(engine.ok());
        for (std::uint64_t i = 0; i < 40; ++i) {
            harness.publish(engine.value(), "alpha", 1, ObservationState::Down, i + 1);
            harness.publish_alternate(engine.value(), "beta", 2, i + 100);
            (void)engine.value().request_failover(
                failover_request(harness.fixture, "alpha", 1, {"beta"}));
        }
        LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{1});
        expect_ok(engine.value().close(), "close");
    }
    LFF_CHECK(file_exists(harness.dir.file("lineage.snap")));
    Result<Engine> engine = harness.open();
    LFF_CHECK(engine.ok());
    const EngineView view = engine.value().inspect(8);
    LFF_CHECK_EQ(view.boot_count, 2ull);
    LFF_CHECK_EQ(view.active_authorities, std::size_t{0});
    LFF_CHECK_EQ(view.interrupted_attempts, std::size_t{1});
    LFF_CHECK(view.retained_attempts >= 1);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(restart, epoch_and_incarnation_advance_on_every_open) {
    RestartHarness harness("epoch");
    std::vector<std::string> coordinators;
    std::vector<std::uint64_t> epochs;
    for (int round = 0; round < 5; ++round) {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        epochs.push_back(engine.value().epoch().value());
        coordinators.push_back(engine.value().incarnation().hex());
        expect_ok(engine.value().close(), "close");
    }
    for (std::size_t i = 0; i < epochs.size(); ++i) {
        LFF_CHECK_EQ(epochs[i], static_cast<std::uint64_t>(i) + 1);
    }
    for (std::size_t i = 1; i < coordinators.size(); ++i) {
        LFF_CHECK(coordinators[i] != coordinators[i - 1]);
    }
    Result<Engine> engine = harness.open();
    LFF_CHECK(engine.ok());
    LFF_CHECK_EQ(engine.value().inspect(0).boot_count, 6ull);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(restart, idempotency_index_survives_restart_without_restoring_authority) {
    RestartHarness harness("idempotency");
    FailoverRequest request = failover_request(harness.fixture, "alpha", 1, {"beta"});
    request.idempotency_key = sha256("restart-idempotency");
    FailoverDecision grant;
    {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        harness.publish(engine.value(), "alpha", 1, ObservationState::Down, 1);
        harness.publish_alternate(engine.value(), "beta", 2, 2);
        grant = expect_decision(engine.value().request_failover(request), "grant");
        LFF_CHECK(grant.kind == DecisionKind::Grant);
        expect_ok(engine.value().close(), "close");
    }
    Result<Engine> engine = harness.open();
    LFF_CHECK(engine.ok());
    const FailoverDecision& replayed =
        expect_decision(engine.value().request_failover(request), "replayed after restart");
    LFF_CHECK(replayed.idempotent_replay);
    LFF_CHECK(replayed.kind == DecisionKind::Grant);
    LFF_CHECK_EQ(replayed.decision_id, grant.decision_id);
    // The replay restates the historical verdict; it does not restore authority.
    LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{0});
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(restart, policy_and_topology_generations_survive_restart) {
    RestartHarness harness("definitions");
    {
        Result<Engine> engine = harness.open();
        LFF_CHECK(engine.ok());
        FailoverPolicy updated = engine.value().policy();
        updated.generation = PolicyGeneration::from(5);
        updated.min_replacement_capacity = 42;
        expect_ok(engine.value().set_policy(updated), "set policy");
        TopologySnapshot topology = engine.value().topology();
        topology.generation = TopologyGeneration::from(7);
        topology.links.push_back(LinkDefinition{LinkKey{harness.fixture.fabric,
                                                        LinkName::parse("delta").value()},
                                                LinkGeneration::from(8), 800});
        topology.canonicalise();
        expect_ok(engine.value().set_topology(topology), "set topology");
        expect_ok(engine.value().close(), "close");
    }
    Result<Engine> engine = harness.open();
    LFF_CHECK(engine.ok());
    LFF_CHECK_EQ(engine.value().policy().generation.value(), 5ull);
    LFF_CHECK_EQ(engine.value().policy().min_replacement_capacity, 42u);
    LFF_CHECK_EQ(engine.value().topology().generation.value(), 7ull);
    LFF_CHECK_EQ(engine.value().topology().links.size(), std::size_t{4});
    expect_ok(engine.value().close(), "close");
}