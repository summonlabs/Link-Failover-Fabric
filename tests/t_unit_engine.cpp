// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Engine semantics: the product-defining behaviour of the failover authority.
#include "framework.hpp"
#include "support.hpp"

#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

FabricFixture standard_fixture() {
    return make_fabric("engine-fabric", 1,
                       {{"alpha", 4, 1000}, {"beta", 5, 4000}, {"gamma", 6, 2000}});
}

struct Harness {
    TempDir dir{"engine"};
    FabricFixture fixture{standard_fixture()};
    Result<Engine> engine;
    Incarnation publisher{Incarnation::generate()};

    explicit Harness(const std::string& tag) : dir("engine-" + tag) {
        engine = Engine::open(engine_options(fixture, dir.path()));
        if (!engine.ok()) {
            LFF_FAIL("engine open failed: " + engine.status().to_string());
        }
    }
    ~Harness() {
        if (engine.ok()) {
            (void)engine.value().close();
        }
    }
    Engine& get() { return engine.value(); }

    void publish_failure(const std::string& link, std::uint64_t generation, ObservationState state,
                         std::uint32_t confidence, std::uint64_t sequence,
                         const std::string& name = "monitor") {
        expect_ok(get().publish_failure_report(make_failure(fixture, link, generation, state,
                                                            confidence, name, publisher, 1, sequence)),
                  "publish failure");
    }
    void publish_replacement(const std::string& link, std::uint64_t generation,
                             ObservationState state, std::uint32_t confidence,
                             std::uint32_t capacity, bool adjacency, std::uint64_t sequence,
                             const std::string& name = "monitor") {
        expect_ok(get().publish_replacement_report(
                      make_replacement(fixture, link, generation, state, confidence, capacity, 1,
                                       adjacency, name, publisher, 1, sequence)),
                  "publish replacement");
    }
};

}  // namespace

LFF_TEST(unit, open_advances_epoch_and_writes_a_boot_record) {
    Harness harness("boot");
    const EngineView view = harness.get().inspect(4);
    LFF_CHECK_EQ(view.epoch.value(), 1ull);
    LFF_CHECK_EQ(view.boot_count, 1ull);
    LFF_CHECK(!view.coordinator.is_zero());
    LFF_CHECK_EQ(view.retained_attempts, std::size_t{0});
}

LFF_TEST(unit, failover_requires_failure_evidence) {
    Harness harness("no-evidence");
    const Result<FailoverDecision> decision =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"}));
    const FailoverDecision& value = expect_decision(decision, "no evidence");
    LFF_CHECK(value.kind == DecisionKind::Indeterminate);
    LFF_CHECK(value.outcome == Outcome::Unknown);
    LFF_CHECK(value.assertion == Assertion::Observation);
    LFF_CHECK(value.durable);
    bool saw_missing = false;
    for (const Reason& reason : value.reasons) {
        saw_missing = saw_missing || reason.code == ReasonCode::EvidenceMissing;
    }
    LFF_CHECK(saw_missing);
}

LFF_TEST(unit, healthy_subject_is_refused_not_indeterminate) {
    Harness harness("healthy");
    harness.publish_failure("alpha", 4, ObservationState::Up, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const Result<FailoverDecision> decision =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"}));
    const FailoverDecision& value = expect_decision(decision, "healthy subject");
    LFF_CHECK(value.kind == DecisionKind::Refuse);
    LFF_CHECK(value.outcome == Outcome::Refused);
    bool saw = false;
    for (const Reason& reason : value.reasons) {
        saw = saw || reason.code == ReasonCode::SubjectNotFailed;
    }
    LFF_CHECK(saw);
}

LFF_TEST(unit, low_confidence_evidence_is_unknown_not_authority) {
    Harness harness("low-confidence");
    harness.fixture.policy.min_failure_confidence = make_confidence(990);
    std::vector<std::uint8_t> ignored;
    harness.publish_failure("alpha", 4, ObservationState::Down, 500, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const Result<FailoverDecision> decision =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"}));
    const FailoverDecision& value = expect_decision(decision, "low confidence");
    LFF_CHECK(value.outcome == Outcome::Unknown);
    LFF_CHECK(value.kind == DecisionKind::Indeterminate);
}

LFF_TEST(unit, grant_binds_full_authority_and_is_single_winner) {
    Harness harness("grant");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);

    const Result<FailoverDecision> first =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"}));
    const FailoverDecision& grant = expect_decision(first, "grant");
    LFF_CHECK(grant.kind == DecisionKind::Grant);
    LFF_CHECK(grant.outcome == Outcome::Ok);
    LFF_CHECK(grant.assertion == Assertion::Authorization);
    LFF_CHECK(grant.durable);
    LFF_CHECK(grant.has_replacement);
    LFF_CHECK_EQ(grant.replacement.link.value(), std::string("beta"));
    LFF_CHECK_EQ(grant.replacement_generation.value(), 5ull);
    LFF_CHECK_EQ(grant.attempt_seq.value(), 1ull);
    LFF_CHECK_EQ(grant.authority.topology_generation.value(), 1ull);
    LFF_CHECK_EQ(grant.authority.policy_generation.value(), 1ull);
    LFF_CHECK_EQ(grant.authority.epoch.value(), grant.epoch.value());
    LFF_CHECK(grant.authority.coordinator == grant.coordinator);
    LFF_CHECK(!grant.authority.evidence_digest.is_zero());

    // A second, competing request for the same generation cannot also win.
    const Result<FailoverDecision> second =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"gamma"}));
    const FailoverDecision& refused = expect_decision(second, "second attempt");
    LFF_CHECK(refused.kind == DecisionKind::Refuse);
    LFF_CHECK(refused.outcome == Outcome::Conflict);
    bool saw = false;
    for (const Reason& reason : refused.reasons) {
        saw = saw || reason.code == ReasonCode::AlreadyAuthoritative;
    }
    LFF_CHECK(saw);
    const EngineView view = harness.get().inspect(4);
    LFF_CHECK_EQ(view.active_authorities, std::size_t{1});
}

LFF_TEST(unit, indistinguishable_attempts_are_never_two_authorities) {
    Harness harness("distinct-attempts");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    FailoverRequest request = failover_request(harness.fixture, "alpha", 4, {"beta"});
    const Result<FailoverDecision> first = harness.get().request_failover(request);
    LFF_CHECK(expect_decision(first, "grant").kind == DecisionKind::Grant);
    // A different link generation is a different governed subject and may be
    // granted independently.
    harness.publish_failure("gamma", 6, ObservationState::Down, 1000, 3);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 4);
    FailoverRequest other = failover_request(harness.fixture, "gamma", 6, {"beta"});
    const Result<FailoverDecision> second = harness.get().request_failover(other);
    LFF_CHECK(expect_decision(second, "second subject").kind == DecisionKind::Grant);
    const EngineView view = harness.get().inspect(4);
    LFF_CHECK_EQ(view.active_authorities, std::size_t{2});
}

LFF_TEST(unit, stale_expectation_is_refused_with_the_exact_fields) {
    Harness harness("stale-expectation");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    FailoverRequest request = failover_request(harness.fixture, "alpha", 4, {"beta"});
    request.expected.epoch = Epoch::from(99);
    request.expected.topology_generation = TopologyGeneration::from(77);
    const Result<FailoverDecision> decision = harness.get().request_failover(request);
    const FailoverDecision& value = expect_decision(decision, "stale expectation");
    LFF_CHECK(value.outcome == Outcome::Stale);
    LFF_CHECK(value.kind == DecisionKind::Indeterminate);
    LFF_CHECK(!value.durable);
    bool saw_epoch = false;
    bool saw_topology = false;
    for (const Reason& reason : value.reasons) {
        saw_epoch = saw_epoch || reason.code == ReasonCode::EpochMismatch;
        saw_topology = saw_topology || reason.code == ReasonCode::TopologyGenerationMismatch;
    }
    LFF_CHECK(saw_epoch);
    LFF_CHECK(saw_topology);
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{0});
}

LFF_TEST(unit, subject_generation_must_match_the_topology) {
    Harness harness("subject-generation");
    harness.publish_failure("alpha", 3, ObservationState::Down, 1000, 1);
    const Result<FailoverDecision> decision =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 3, {"beta"}));
    LFF_CHECK(!decision.ok());
    expect_reason(decision.status(), ReasonCode::SubjectGenerationMismatch, "subject generation");
    const Result<FailoverDecision> unknown =
        harness.get().request_failover(failover_request(harness.fixture, "nowhere", 1, {"beta"}));
    LFF_CHECK(!unknown.ok());
    expect_outcome(unknown.status(), Outcome::NotFound, "unknown subject");
}

LFF_TEST(unit, idempotent_replay_returns_the_original_without_new_authority) {
    Harness harness("idempotency");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    FailoverRequest request = failover_request(harness.fixture, "alpha", 4, {"beta"});
    request.idempotency_key = sha256("idem-key-1");
    const Result<FailoverDecision> first = harness.get().request_failover(request);
    const FailoverDecision& grant = expect_decision(first, "first");
    LFF_CHECK(grant.kind == DecisionKind::Grant);
    LFF_CHECK(!grant.idempotent_replay);

    const Result<FailoverDecision> replay = harness.get().request_failover(request);
    const FailoverDecision& replayed = expect_decision(replay, "replay");
    LFF_CHECK(replayed.idempotent_replay);
    LFF_CHECK(replayed.kind == DecisionKind::Grant);
    LFF_CHECK_EQ(replayed.decision_id, grant.decision_id);
    LFF_CHECK_EQ(replayed.attempt_seq.value(), grant.attempt_seq.value());
    bool saw = false;
    for (const Reason& reason : replayed.reasons) {
        saw = saw || reason.code == ReasonCode::IdempotentReplay;
    }
    LFF_CHECK(saw);
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{1});
}

LFF_TEST(unit, application_lifecycle_and_assertion_ladder) {
    Harness harness("lifecycle");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const FailoverDecision grant = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");

    const std::vector<ApplyDirective> directives = harness.get().pending_directives(4);
    LFF_CHECK_EQ(directives.size(), std::size_t{1});
    LFF_CHECK_EQ(directives[0].attempt_id.hex(), grant.attempt_id.hex());

    const Result<FailoverDecision> acknowledged = harness.get().record_application(
        application_for(grant, harness.publisher, harness.get().epoch()));
    const FailoverDecision& ack = expect_decision(acknowledged, "acknowledge");
    LFF_CHECK(ack.kind == DecisionKind::Acknowledge);
    LFF_CHECK(ack.assertion == Assertion::Acknowledgement);
    LFF_CHECK(ack.outcome == Outcome::Ok);
    bool saw_not_effect = false;
    for (const Reason& reason : ack.reasons) {
        saw_not_effect = saw_not_effect || reason.code == ReasonCode::AcknowledgementNotEffect;
    }
    LFF_CHECK(saw_not_effect);
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{1});

    const Result<FailoverDecision> verified = harness.get().record_verification(
        verification_for(grant, harness.publisher, harness.get().epoch()));
    const FailoverDecision& verify = expect_decision(verified, "verify");
    LFF_CHECK(verify.kind == DecisionKind::Verify);
    LFF_CHECK(verify.assertion == Assertion::VerifiedEffect);
    LFF_CHECK(verify.outcome == Outcome::Ok);
    const EngineView view = harness.get().inspect(4);
    LFF_CHECK_EQ(view.recent_attempts.size(), std::size_t{1});
    LFF_CHECK(view.recent_attempts[0].phase == AttemptPhase::Committed);
}

LFF_TEST(unit, unverified_effect_is_indeterminate_not_success) {
    Harness harness("unverified");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const FailoverDecision grant = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");
    expect_ok(harness.get().record_application(
                  application_for(grant, harness.publisher, harness.get().epoch()))
                  .status(),
              "acknowledge");
    const Result<FailoverDecision> verified = harness.get().record_verification(
        verification_for(grant, harness.publisher, harness.get().epoch(), false));
    const FailoverDecision& value = expect_decision(verified, "unverified");
    LFF_CHECK(value.outcome == Outcome::Indeterminate);
    LFF_CHECK(value.assertion == Assertion::Acknowledgement);
    LFF_CHECK(harness.get().inspect(4).recent_attempts[0].phase == AttemptPhase::Indeterminate);
}

LFF_TEST(unit, rollback_revokes_authority_and_requires_a_new_generation) {
    Harness harness("rollback");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const FailoverDecision grant = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");
    expect_ok(harness.get().record_application(
                  application_for(grant, harness.publisher, harness.get().epoch()))
                  .status(),
              "acknowledge");

    RollbackRequest rollback;
    rollback.subject = grant.subject;
    rollback.subject_generation = grant.subject_generation;
    rollback.attempt_seq = grant.attempt_seq;
    rollback.rationale = "synthetic rollback";
    const Result<FailoverDecision> result = harness.get().request_rollback(rollback);
    const FailoverDecision& value = expect_decision(result, "rollback");
    LFF_CHECK(value.kind == DecisionKind::Rollback);
    LFF_CHECK(value.outcome == Outcome::Ok);
    bool saw_restore = false;
    for (const Reason& reason : value.reasons) {
        saw_restore = saw_restore || reason.code == ReasonCode::RestoreRequiresNewGeneration;
    }
    LFF_CHECK(saw_restore);
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{0});

    // A repeated rollback is idempotent.
    const Result<FailoverDecision> again = harness.get().request_rollback(rollback);
    LFF_CHECK(expect_decision(again, "repeat rollback").idempotent_replay);
}

LFF_TEST(unit, contradictory_failure_reports_conflict) {
    Harness harness("contradiction");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1, "monitor-a");
    const Incarnation second = Incarnation::generate();
    expect_ok(harness.get().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Up, 1000, "monitor-b",
                               second, 1, 1)),
              "second publisher");
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const Result<FailoverDecision> decision =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"}));
    const FailoverDecision& value = expect_decision(decision, "contradiction");
    LFF_CHECK(value.outcome == Outcome::Indeterminate);
    bool saw = false;
    for (const Reason& reason : value.reasons) {
        saw = saw || reason.code == ReasonCode::EvidenceConflicted;
    }
    LFF_CHECK(saw);
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{0});
}

LFF_TEST(unit, majority_agreement_resolves_contradiction_but_ties_do_not) {
    Harness harness("majority");
    harness.fixture.policy.agreement = AgreementRule::MajorityOfFreshQualified;
    TempDir dir("engine-majority-2");
    Result<Engine> engine = Engine::open(engine_options(harness.fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher_a = Incarnation::generate();
    const Incarnation publisher_b = Incarnation::generate();
    const Incarnation publisher_c = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "p-a",
                               publisher_a, 1, 1)),
              "a");
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "p-b",
                               publisher_b, 1, 1)),
              "b");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(harness.fixture, "beta", 5, ObservationState::Up, 1000, 4000, 1,
                                   true, "p-a", publisher_a, 1, 2)),
              "replacement");
    FailoverRequest request = failover_request(harness.fixture, "alpha", 4, {"beta"});
    LFF_CHECK(expect_decision(engine.value().request_failover(request), "majority")
                  .kind == DecisionKind::Grant);

    // Now add a third publisher that disagrees: 2 of 3 still carries.
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "gamma", 6, ObservationState::Down, 1000, "p-a",
                               publisher_a, 1, 3)),
              "gamma a");
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "gamma", 6, ObservationState::Up, 1000, "p-c",
                               publisher_c, 1, 1)),
              "gamma c");
    FailoverRequest second = failover_request(harness.fixture, "gamma", 6, {"beta"});
    LFF_CHECK(expect_decision(engine.value().request_failover(second), "tie")
                  .outcome == Outcome::Indeterminate);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(unit, policy_and_topology_changes_fence_live_authority) {
    Harness harness("fencing");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const FailoverDecision grant = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{1});

    FailoverPolicy updated = harness.get().policy();
    updated.generation = PolicyGeneration::from(2);
    updated.min_failure_confidence = make_confidence(800);
    expect_ok(harness.get().set_policy(updated), "set policy");
    const EngineView view = harness.get().inspect(8);
    LFF_CHECK_EQ(view.active_authorities, std::size_t{0});
    LFF_CHECK(view.retained_fences > 0);
    bool saw_policy_fence = false;
    for (const FenceRecord& fence : view.recent_fences) {
        saw_policy_fence = saw_policy_fence || fence.kind == FenceKind::PolicyChanged;
    }
    LFF_CHECK(saw_policy_fence);

    // The fenced attempt can no longer be acknowledged.
    const Result<FailoverDecision> late = harness.get().record_application(
        application_for(grant, harness.publisher, harness.get().epoch()));
    LFF_CHECK(expect_decision(late, "late acknowledgement").outcome == Outcome::Conflict);

    // A regressing policy generation is refused outright.
    FailoverPolicy stale = updated;
    stale.generation = PolicyGeneration::from(1);
    const Status refused = harness.get().set_policy(stale);
    LFF_CHECK(!refused.ok());
    expect_outcome(refused, Outcome::Stale, "policy regression");
    // Same generation with different content is a conflict, not a silent update.
    FailoverPolicy conflicting = updated;
    conflicting.min_failure_confidence = make_confidence(700);
    const Status conflict = harness.get().set_policy(conflicting);
    LFF_CHECK(!conflict.ok());
    expect_outcome(conflict, Outcome::Conflict, "policy conflict");
    // Identical content is an idempotent no-op.
    expect_ok(harness.get().set_policy(updated), "idempotent policy");
}

LFF_TEST(unit, publisher_restart_retires_its_previous_observations) {
    Harness harness("publisher-restart");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1, "monitor");
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2, "monitor");

    // The same publisher restarts: new incarnation, new epoch.
    const Incarnation restarted = Incarnation::generate();
    expect_ok(harness.get().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               restarted, 2, 1)),
              "restarted publisher");
    // Its retired incarnation can no longer publish.
    const Status retired = harness.get().publish_failure_report(
        make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                     harness.publisher, 1, 2));
    LFF_CHECK(!retired.ok());
    expect_outcome(retired, Outcome::Stale, "retired publisher");
    // Two incarnations may not claim the same publisher epoch.
    const Status duplicate_epoch = harness.get().publish_failure_report(
        make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                     Incarnation::generate(), 2, 5));
    LFF_CHECK(!duplicate_epoch.ok());
    expect_outcome(duplicate_epoch, Outcome::Conflict, "duplicate publisher epoch");
}

LFF_TEST(unit, observation_sequence_must_advance) {
    Harness harness("observation-seq");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 5);
    const Status replayed = harness.get().publish_failure_report(
        make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                     harness.publisher, 1, 5));
    LFF_CHECK(!replayed.ok());
    expect_reason(replayed, ReasonCode::SessionSequenceReplayed, "observation replay");
    const Status regressed = harness.get().publish_failure_report(
        make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                     harness.publisher, 1, 4));
    LFF_CHECK(!regressed.ok());
    expect_ok(harness.get().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               harness.publisher, 1, 6)),
              "advancing sequence");
}

LFF_TEST(unit, replacement_evidence_must_match_the_topology_generation) {
    Harness harness("replacement-generation");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    // Evidence published against a generation the topology does not carry.
    ReplacementReport report = make_replacement(harness.fixture, "beta", 5, ObservationState::Up,
                                                1000, 4000, 1, true, "monitor", harness.publisher, 1, 2);
    report.topology_generation = TopologyGeneration::from(9);
    expect_ok(harness.get().publish_replacement_report(report), "publish stale topology evidence");
    const Result<FailoverDecision> decision =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"}));
    const FailoverDecision& value = expect_decision(decision, "stale topology generation");
    LFF_CHECK(value.outcome == Outcome::Indeterminate);
    LFF_CHECK(value.kind == DecisionKind::Indeterminate);
}

LFF_TEST(unit, failover_is_proven_infeasible_only_with_a_certificate) {
    Harness harness("infeasible");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Down, 1000, 4000, true, 2);
    harness.publish_replacement("gamma", 6, ObservationState::Up, 1000, 1, false, 3);
    const Result<FailoverDecision> decision = harness.get().request_failover(
        failover_request(harness.fixture, "alpha", 4, {"beta", "gamma"}));
    const FailoverDecision& value = expect_decision(decision, "infeasible");
    LFF_CHECK(value.kind == DecisionKind::Refuse);
    LFF_CHECK(value.outcome == Outcome::Refused);
    LFF_CHECK_EQ(value.candidates.size(), std::size_t{2});
    bool saw_certificate = false;
    for (const Reason& reason : value.reasons) {
        saw_certificate = saw_certificate || reason.code == ReasonCode::CertifiedInfeasible;
    }
    LFF_CHECK(saw_certificate);
    for (const CandidateAssessment& assessment : value.candidates) {
        LFF_CHECK(assessment.verdict == Verdict::Disproven);
    }
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{0});
}

LFF_TEST(unit, one_undecided_candidate_prevents_an_infeasibility_claim) {
    Harness harness("partial-infeasible");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Down, 1000, 4000, true, 2);
    const Result<FailoverDecision> decision = harness.get().request_failover(
        failover_request(harness.fixture, "alpha", 4, {"beta", "gamma"}));
    const FailoverDecision& value = expect_decision(decision, "partial");
    LFF_CHECK(value.kind == DecisionKind::Indeterminate);
    LFF_CHECK(value.outcome == Outcome::Indeterminate);
}

LFF_TEST(unit, service_obligation_capacity_is_enforced) {
    Harness harness("obligation");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    // alpha carries 1000 units, so a 500-unit alternate does not satisfy it.
    harness.publish_replacement("gamma", 6, ObservationState::Up, 1000, 500, true, 2);
    const Result<FailoverDecision> decision =
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"gamma"}));
    LFF_CHECK(expect_decision(decision, "obligation").kind == DecisionKind::Refuse);

    harness.fixture.policy.require_subject_capacity = false;
    TempDir dir("engine-obligation-2");
    Result<Engine> engine = Engine::open(engine_options(harness.fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(harness.fixture, "gamma", 6, ObservationState::Up, 1000, 500, 1,
                                   true, "monitor", publisher, 1, 2)),
              "replacement");
    LFF_CHECK(expect_decision(
                  engine.value().request_failover(
                      failover_request(harness.fixture, "alpha", 4, {"gamma"})),
                  "no obligation")
                  .kind == DecisionKind::Grant);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(unit, attempt_budget_is_bounded_and_explicit) {
    Harness harness("budget");
    harness.fixture.policy.max_attempts_per_generation = 2;
    TempDir dir("engine-budget-2");
    Result<Engine> engine = Engine::open(engine_options(harness.fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Up, 1000, "monitor",
                               publisher, 1, 1)),
              "healthy failure report");
    FailoverRequest request = failover_request(harness.fixture, "alpha", 4, {"beta"});
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(harness.fixture, "beta", 5, ObservationState::Up, 1000, 4000, 1,
                                   true, "monitor", publisher, 1, 2)),
              "replacement");
    for (int i = 0; i < 2; ++i) {
        const FailoverDecision& refused =
            expect_decision(engine.value().request_failover(request), "budgeted refusal");
        LFF_CHECK(refused.kind == DecisionKind::Refuse);
        LFF_CHECK(refused.durable);
    }
    const FailoverDecision& exhausted =
        expect_decision(engine.value().request_failover(request), "exhausted");
    LFF_CHECK(!exhausted.durable);
    bool saw = false;
    for (const Reason& reason : exhausted.reasons) {
        saw = saw || reason.code == ReasonCode::AttemptsExhausted;
    }
    LFF_CHECK(saw);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(unit, candidate_set_validation_is_explicit) {
    Harness harness("candidates");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    // Supplying no alternates is a well-formed request that the runtime refuses
    // with an explicit certificate rather than a transport-level error.
    const FailoverDecision empty = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {})),
        "empty candidate set");
    LFF_CHECK(empty.kind == DecisionKind::Refuse);
    LFF_CHECK(empty.outcome == Outcome::Refused);
    bool saw_empty = false;
    for (const Reason& reason : empty.reasons) {
        saw_empty = saw_empty || reason.code == ReasonCode::CandidateSetEmpty;
    }
    LFF_CHECK(saw_empty);

    FailoverRequest duplicated = failover_request(harness.fixture, "alpha", 4, {"beta", "beta"});
    const Result<FailoverDecision> duplicate = harness.get().request_failover(duplicated);
    LFF_CHECK(!duplicate.ok());
    expect_reason(duplicate.status(), ReasonCode::InvalidArgument, "duplicate candidates");

    FailoverRequest self = failover_request(harness.fixture, "alpha", 4, {"alpha"});
    const Result<FailoverDecision> subject = harness.get().request_failover(self);
    LFF_CHECK(!subject.ok());
    expect_reason(subject.status(), ReasonCode::ReplacementIsSubject, "self substitution");

    FailoverRequest unknown_fabric = failover_request(harness.fixture, "alpha", 4, {"beta"});
    unknown_fabric.subject.fabric = FabricName::parse("other-fabric").value();
    const Result<FailoverDecision> fabric = harness.get().request_failover(unknown_fabric);
    LFF_CHECK(!fabric.ok());
    expect_reason(fabric.status(), ReasonCode::UnknownFabric, "unknown fabric");
}

LFF_TEST(unit, applier_reports_must_carry_the_bound_identity) {
    Harness harness("applier-identity");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const FailoverDecision grant = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");

    ApplicationReport wrong_id = application_for(grant, harness.publisher, harness.get().epoch());
    wrong_id.attempt_id = sha256("not-the-attempt");
    LFF_CHECK(!harness.get().record_application(wrong_id).ok());
    expect_reason(harness.get().record_application(wrong_id).status(),
                  ReasonCode::AttemptUnknown, "wrong attempt identity");

    ApplicationReport wrong_generation = application_for(grant, harness.publisher,
                                                         harness.get().epoch());
    wrong_generation.subject_generation = LinkGeneration::from(99);
    LFF_CHECK(!harness.get().record_application(wrong_generation).ok());

    ApplicationReport unknown = application_for(grant, harness.publisher, harness.get().epoch());
    unknown.attempt_seq = AttemptSeq::from(9);
    LFF_CHECK(!harness.get().record_application(unknown).ok());
    expect_outcome(harness.get().record_application(unknown).status(), Outcome::NotFound,
                   "unknown attempt");

    ApplicationReport no_applier = application_for(grant, Incarnation{}, harness.get().epoch());
    LFF_CHECK(!harness.get().record_application(no_applier).ok());
    expect_reason(harness.get().record_application(no_applier).status(),
                  ReasonCode::WorkerIncarnationMismatch, "missing applier");
}

LFF_TEST(unit, rejected_application_aborts_the_attempt) {
    Harness harness("rejected");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const FailoverDecision grant = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");
    const Result<FailoverDecision> rejected = harness.get().record_application(
        application_for(grant, harness.publisher, harness.get().epoch(), false));
    const FailoverDecision& value = expect_decision(rejected, "rejected application");
    LFF_CHECK(value.outcome == Outcome::Refused);
    LFF_CHECK(harness.get().inspect(4).recent_attempts[0].phase == AttemptPhase::Aborted);
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{0});
}

LFF_TEST(unit, verified_effect_can_be_disabled_by_policy) {
    Harness harness("no-verify");
    harness.fixture.policy.require_verified_effect = false;
    TempDir dir("engine-no-verify-2");
    Result<Engine> engine = Engine::open(engine_options(harness.fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(harness.fixture, "beta", 5, ObservationState::Up, 1000, 4000, 1,
                                   true, "monitor", publisher, 1, 2)),
              "replacement");
    const FailoverDecision grant = expect_decision(
        engine.value().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");
    expect_ok(engine.value().record_application(application_for(grant, publisher,
                                                                engine.value().epoch()))
                  .status(),
              "acknowledge");
    LFF_CHECK(engine.value().inspect(4).recent_attempts[0].phase == AttemptPhase::Committed);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(unit, disabled_policy_refuses_every_failover) {
    Harness harness("disabled");
    TempDir dir("engine-disabled");
    harness.fixture.policy.failover_enabled = false;
    Result<Engine> engine = Engine::open(engine_options(harness.fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    const FailoverDecision& value =
        expect_decision(engine.value().request_failover(
                            failover_request(harness.fixture, "alpha", 4, {"beta"})),
                        "disabled");
    LFF_CHECK(value.kind == DecisionKind::Refuse);
    bool saw = false;
    for (const Reason& reason : value.reasons) {
        saw = saw || reason.code == ReasonCode::PolicyDisabled;
    }
    LFF_CHECK(saw);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(unit, observation_of_an_unknown_fabric_is_refused) {
    Harness harness("unknown-fabric");
    FailureReport report = make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000,
                                        "monitor", harness.publisher, 1, 1);
    report.subject.fabric = FabricName::parse("elsewhere").value();
    const Status status = harness.get().publish_failure_report(report);
    LFF_CHECK(!status.ok());
    expect_outcome(status, Outcome::NotFound, "foreign fabric observation");
    FailureReport zero = make_failure(harness.fixture, "alpha", 4, ObservationState::Down, 1000,
                                      "monitor", Incarnation{}, 1, 1);
    LFF_CHECK(!harness.get().publish_failure_report(zero).ok());
}

LFF_TEST(unit, take_directive_is_identity_bound) {
    Harness harness("directive");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    const FailoverDecision grant = expect_decision(
        harness.get().request_failover(failover_request(harness.fixture, "alpha", 4, {"beta"})),
        "grant");
    ApplyDirective directive;
    LFF_CHECK(harness.get().take_directive(grant.subject, grant.subject_generation,
                                           grant.attempt_seq, directive));
    LFF_CHECK_EQ(directive.attempt_id.hex(), grant.attempt_id.hex());
    LFF_CHECK_EQ(directive.replacement.link.value(), std::string("beta"));
    ApplyDirective wrong;
    LFF_CHECK(!harness.get().take_directive(grant.subject, LinkGeneration::from(99),
                                            grant.attempt_seq, wrong));
    LFF_CHECK(!harness.get().take_directive(grant.subject, grant.subject_generation,
                                            AttemptSeq::from(99), wrong));
    expect_ok(harness.get().record_application(
                  application_for(grant, harness.publisher, harness.get().epoch()))
                  .status(),
              "acknowledge");
    LFF_CHECK(harness.get().pending_directives(4).empty());
}

LFF_TEST(unit, inspect_reports_bounded_and_canonical_history) {
    Harness harness("inspect");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    expect_decision(harness.get().request_failover(failover_request(harness.fixture, "alpha", 4,
                                                                    {"beta"})),
                    "grant");
    const EngineView view = harness.get().inspect(1);
    LFF_CHECK_EQ(view.recent_attempts.size(), std::size_t{1});
    LFF_CHECK_EQ(view.retained_decisions, std::size_t{1});
    const std::string text = engine_view_to_string(view);
    LFF_CHECK(text.find("fabric=engine-fabric") != std::string::npos);
    LFF_CHECK(text.find("active_authorities=1") != std::string::npos);
    const EngineView empty = harness.get().inspect(0);
    LFF_CHECK(empty.recent_attempts.empty());
}

LFF_TEST(unit, checkpoint_preserves_lineage) {
    Harness harness("checkpoint");
    harness.publish_failure("alpha", 4, ObservationState::Down, 1000, 1);
    harness.publish_replacement("beta", 5, ObservationState::Up, 1000, 4000, true, 2);
    expect_decision(harness.get().request_failover(failover_request(harness.fixture, "alpha", 4,
                                                                    {"beta"})),
                    "grant");
    const LineageSeq before = harness.get().inspect(0).last_seq;
    expect_ok(harness.get().checkpoint(), "checkpoint");
    const LineageSeq after = harness.get().inspect(0).last_seq;
    LFF_CHECK(after.value() > before.value());
    LFF_CHECK_EQ(harness.get().inspect(0).active_authorities, std::size_t{1});
}

LFF_TEST(unit, engine_state_is_consistent_after_close) {
    FabricFixture fixture = standard_fixture();
    TempDir dir("engine-closed");
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    expect_ok(engine.value().close(), "close");
    expect_ok(engine.value().close(), "double close");
    const Status after = engine.value().publish_failure_report(
        make_failure(fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                     Incarnation::generate(), 1, 1));
    LFF_CHECK(!after.ok());
    expect_outcome(after, Outcome::Closed, "publish after close");
}