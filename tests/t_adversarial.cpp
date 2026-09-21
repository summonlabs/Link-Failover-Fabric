// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Break-it suite: hostile inputs, resource-exhaustion attempts, duplicate and
// late completions, contradictory alternate-link evidence, and replayed state.
#include "framework.hpp"
#include "support.hpp"
#include "lff/store.hpp"

#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

FabricFixture adversarial_fixture() {
    return make_fabric("adversarial-fabric", 1,
                       {{"alpha", 4, 1000}, {"beta", 5, 4000}, {"gamma", 6, 3000},
                        {"delta", 7, 3500}});
}

}  // namespace

LFF_TEST(adversarial, contradictory_alternate_evidence_yields_no_authority) {
    TempDir dir("adv-alternate");
    FabricFixture fixture = adversarial_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation monitoring_a = Incarnation::generate();
    const Incarnation monitoring_b = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(fixture, "alpha", 4, ObservationState::Down, 1000, "mon-a",
                               monitoring_a, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 5, ObservationState::Up, 1000, 4000, 1, true,
                                   "mon-a", monitoring_a, 1, 2)),
              "replacement up");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 5, ObservationState::Down, 1000, 4000, 1, true,
                                   "mon-b", monitoring_b, 1, 1)),
              "replacement down");
    const FailoverDecision& decision =
        expect_decision(engine.value().request_failover(
                            failover_request(fixture, "alpha", 4, {"beta"})),
                        "contradictory alternate");
    LFF_CHECK(decision.outcome == Outcome::Indeterminate);
    LFF_CHECK(decision.kind == DecisionKind::Indeterminate);
    bool saw_conflict = false;
    for (const CandidateAssessment& assessment : decision.candidates) {
        saw_conflict = saw_conflict || assessment.reason == ReasonCode::EvidenceConflicted;
    }
    LFF_CHECK(saw_conflict);
    LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{0});

    // A low-confidence contradiction is not qualified and does not block.
    const Incarnation monitoring_c = Incarnation::generate();
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 5, ObservationState::Down, 100, 4000, 1, true,
                                   "mon-c", monitoring_c, 1, 1)),
              "weak contradiction");
    // The strong contradiction still stands, so the candidate stays undecided.
    LFF_CHECK(expect_decision(engine.value().request_failover(
                                  failover_request(fixture, "alpha", 4, {"beta"})),
                              "unresolved contradiction")
                  .kind == DecisionKind::Indeterminate);
    // Retiring the contradicting publisher (a real restart) removes its voice,
    // and the remaining current observation decides the question.
    const Incarnation restarted = Incarnation::generate();
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 5, ObservationState::Up, 1000, 4000, 1, true,
                                   "mon-b", restarted, 2, 1)),
              "publisher restart");
    LFF_CHECK(expect_decision(engine.value().request_failover(
                                  failover_request(fixture, "alpha", 4, {"beta"})),
                              "restarted publisher")
                  .kind == DecisionKind::Grant);
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(adversarial, duplicate_and_late_completions_change_nothing) {
    TempDir dir("adv-duplicate");
    FabricFixture fixture = adversarial_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 5, ObservationState::Up, 1000, 4000, 1, true,
                                   "monitor", publisher, 1, 2)),
              "replacement");
    const FailoverDecision grant =
        expect_decision(engine.value().request_failover(failover_request(fixture, "alpha", 4,
                                                                        {"beta"})),
                        "grant");
    const ApplicationReport first = application_for(grant, publisher, engine.value().epoch());
    expect_ok(engine.value().record_application(first).status(), "first application");
    const FailoverDecision& duplicate =
        expect_decision(engine.value().record_application(first), "duplicate application");
    LFF_CHECK(duplicate.outcome == Outcome::Conflict);
    bool saw_duplicate = false;
    for (const Reason& reason : duplicate.reasons) {
        saw_duplicate = saw_duplicate || reason.code == ReasonCode::DuplicateCompletion;
    }
    LFF_CHECK(saw_duplicate);
    LFF_CHECK(!duplicate.durable);

    const VerificationReport verification = verification_for(grant, publisher,
                                                             engine.value().epoch());
    expect_ok(engine.value().record_verification(verification).status(), "verification");
    const FailoverDecision& repeated =
        expect_decision(engine.value().record_verification(verification), "repeated verification");
    LFF_CHECK(repeated.outcome == Outcome::Conflict);
    const FailoverDecision& late =
        expect_decision(engine.value().record_application(first), "late application");
    LFF_CHECK(late.outcome == Outcome::Conflict);
    bool saw_late = false;
    for (const Reason& reason : late.reasons) {
        saw_late = saw_late || reason.code == ReasonCode::LateAcknowledgement;
    }
    LFF_CHECK(saw_late);
    LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{1});
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(adversarial, stale_and_replayed_attempts_are_rejected) {
    TempDir dir("adv-replay");
    FabricFixture fixture = adversarial_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "beta", 5, ObservationState::Up, 1000, 4000, 1, true,
                                   "monitor", publisher, 1, 2)),
              "replacement");
    const FailoverDecision grant =
        expect_decision(engine.value().request_failover(failover_request(fixture, "alpha", 4,
                                                                        {"beta"})),
                        "grant");

    // Rollback, then try to acknowledge the attempt that no longer holds authority.
    RollbackRequest rollback;
    rollback.subject = grant.subject;
    rollback.subject_generation = grant.subject_generation;
    rollback.attempt_seq = grant.attempt_seq;
    expect_ok(engine.value().request_rollback(rollback).status(), "rollback");
    const FailoverDecision& late =
        expect_decision(engine.value().record_application(
                            application_for(grant, publisher, engine.value().epoch())),
                        "late apply after rollback");
    LFF_CHECK(late.outcome == Outcome::Conflict);
    LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{0});
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(adversarial, evidence_table_exhaustion_is_a_deterministic_refusal) {
    TempDir dir("adv-exhaustion");
    FabricFixture fixture = adversarial_fixture();
    EngineOptions options = engine_options(fixture, dir.path());
    options.limits.max_evidence_entries = 4;
    Result<Engine> engine = Engine::open(options);
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    std::size_t accepted = 0;
    bool refused = false;
    for (std::uint64_t i = 0; i < 32; ++i) {
        const std::string name = "link-" + std::to_string(i);
        const Status status = engine.value().publish_replacement_report(
            make_replacement(fixture, name, 1, ObservationState::Up, 1000, 100, 1, true, "monitor",
                             publisher, 1, 1));
        if (status.ok()) {
            ++accepted;
        } else {
            expect_outcome(status, Outcome::Exhausted, "evidence exhaustion");
            refused = true;
            break;
        }
    }
    LFF_CHECK(refused);
    LFF_CHECK_EQ(accepted, 4u);
    // Replacement in place never grows the table.
    expect_ok(engine.value().publish_replacement_report(
                  make_replacement(fixture, "link-0", 1, ObservationState::Up, 1000, 100, 1, true,
                                   "monitor", publisher, 1, 2)),
              "replace in place");
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(adversarial, publisher_registry_exhaustion_is_a_deterministic_refusal) {
    TempDir dir("adv-publishers");
    FabricFixture fixture = adversarial_fixture();
    EngineOptions options = engine_options(fixture, dir.path());
    options.limits.max_tracked_publishers = 3;
    Result<Engine> engine = Engine::open(options);
    LFF_CHECK(engine.ok());
    for (std::uint64_t i = 0; i < 3; ++i) {
        expect_ok(engine.value().publish_failure_report(make_failure(
                      fixture, "alpha", 4, ObservationState::Down, 1000, "monitor-" + std::to_string(i),
                      Incarnation::generate(), 1, 1)),
                  "publisher");
    }
    const Status overflow = engine.value().publish_failure_report(
        make_failure(fixture, "alpha", 4, ObservationState::Down, 1000, "monitor-overflow",
                     Incarnation::generate(), 1, 1));
    LFF_CHECK(!overflow.ok());
    expect_outcome(overflow, Outcome::Exhausted, "publisher exhaustion");
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(adversarial, candidate_flood_is_refused_before_evaluation) {
    TempDir dir("adv-candidates");
    FabricFixture fixture = adversarial_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();
    expect_ok(engine.value().publish_failure_report(
                  make_failure(fixture, "alpha", 4, ObservationState::Down, 1000, "monitor",
                               publisher, 1, 1)),
              "failure");
    FailoverRequest request = failover_request(fixture, "alpha", 4, {});
    for (std::uint32_t i = 0; i < kMaxPolicyCandidates + 1; ++i) {
        request.candidates.push_back(candidate(fixture, "link-" + std::to_string(i)));
    }
    const Result<FailoverDecision> decision = engine.value().request_failover(request);
    LFF_CHECK(!decision.ok());
    expect_outcome(decision.status(), Outcome::LimitExceeded, "candidate flood");
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(adversarial, malformed_and_extreme_observations_are_refused) {
    TempDir dir("adv-malformed");
    FabricFixture fixture = adversarial_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const Incarnation publisher = Incarnation::generate();

    FailureReport zero_generation = make_failure(fixture, "alpha", 0, ObservationState::Down, 1000,
                                                 "monitor", publisher, 1, 1);
    LFF_CHECK(!engine.value().publish_failure_report(zero_generation).ok());

    FailureReport zero_topology = make_failure(fixture, "alpha", 4, ObservationState::Down, 1000,
                                               "monitor", publisher, 1, 1);
    zero_topology.topology_generation = TopologyGeneration::from(0);
    LFF_CHECK(!engine.value().publish_failure_report(zero_topology).ok());

    FailureReport empty_publisher = make_failure(fixture, "alpha", 4, ObservationState::Down, 1000,
                                                 "monitor", publisher, 1, 1);
    empty_publisher.stamp.publisher = PublisherName{};
    LFF_CHECK(!engine.value().publish_failure_report(empty_publisher).ok());

    FailureReport huge_sequence = make_failure(fixture, "alpha", 4, ObservationState::Down, 1000,
                                               "monitor", publisher, 1,
                                               0xFFFFFFFFFFFFFFFFull);
    expect_ok(engine.value().publish_failure_report(huge_sequence), "huge sequence");
    const Status exhausted = engine.value().publish_failure_report(
        make_failure(fixture, "alpha", 4, ObservationState::Down, 1000, "monitor", publisher, 1,
                     0xFFFFFFFFFFFFFFFFull));
    LFF_CHECK(!exhausted.ok());
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(adversarial, selection_outcome_is_revalidated_before_authority) {
    // The engine re-validates its own selection. A corrupted candidate set must
    // never produce authority, so the validation contract itself is exercised.
    FabricFixture fixture = adversarial_fixture();
    SelectionInput input;
    input.fabric = fixture.fabric;
    input.subject.fabric = fixture.fabric;
    input.subject.link = LinkName::parse("alpha").value();
    input.subject_generation = LinkGeneration::from(4);
    input.topology_generation = fixture.topology.generation;
    input.policy_generation = fixture.policy.generation;
    input.policy = fixture.policy;
    input.min_replacement_capacity = 1;
    ResolvedCandidate entry;
    entry.candidate.fabric = fixture.fabric;
    entry.candidate.link = LinkName::parse("beta").value();
    entry.link_generation = LinkGeneration::from(5);
    entry.evidence_present = true;
    entry.report.state = ObservationState::Up;
    entry.report.confidence = make_confidence(1000);
    entry.report.capacity_units = 4000;
    entry.report.adjacency_authorized = true;
    entry.freshness = FreshnessClass::Current;
    input.candidates.push_back(entry);
    const SelectionOutcome outcome = select_replacement(input);
    LFF_CHECK(outcome.status == SelectionStatus::Selected);
    expect_ok(validate_selection(input, outcome), "selection");
    // A duplicated link breaks the totality of the final tie-break.
    input.candidates.push_back(entry);
    const SelectionOutcome duplicated = select_replacement(input);
    const Status invalid = validate_selection(input, duplicated);
    LFF_CHECK(!invalid.ok());
    expect_reason(invalid, ReasonCode::InvalidArgument, "duplicate candidate detection");
}

LFF_TEST(adversarial, torn_tail_and_corruption_are_distinguished_by_the_store) {
    TempDir dir("adv-store");
    FabricFixture fixture = adversarial_fixture();
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        expect_ok(engine.value().close(), "close");
    }
    const std::filesystem::path journal = dir.file("lineage.jrnl");
    LFF_CHECK(file_exists(journal));
    const std::uint64_t size = byte_size(journal);

    // A torn tail is recovered by default and the lineage survives.
    std::vector<std::uint8_t> bytes = read_bytes(journal);
    bytes.insert(bytes.end(), 12, 0xAB);
    write_bytes(journal, bytes);
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        LFF_CHECK_EQ(engine.value().inspect(0).boot_count, 2ull);
        expect_ok(engine.value().close(), "close after recovery");
    }
    LFF_CHECK_EQ(byte_size(journal) >= size, true);

    // A corrupted payload is fatal and nothing is truncated.
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        expect_ok(engine.value().close(), "close");
    }
    std::vector<std::uint8_t> corrupt = read_bytes(journal);
    LFF_CHECK(corrupt.size() > kJournalHeaderSize + kRecordHeaderSize + 4);
    const std::size_t payload_offset = kJournalHeaderSize + kRecordHeaderSize + 3;
    corrupt[payload_offset] ^= 0xFF;
    write_bytes(journal, corrupt);
    const std::uint64_t corrupt_size = byte_size(journal);
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(!engine.ok());
    expect_outcome(engine.status(), Outcome::Corrupt, "corrupt payload");
    LFF_CHECK_EQ(byte_size(journal), corrupt_size);
}

LFF_TEST(adversarial, strict_mode_refuses_a_torn_tail) {
    TempDir dir("adv-strict");
    FabricFixture fixture = adversarial_fixture();
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        expect_ok(engine.value().close(), "close");
    }
    const std::filesystem::path journal = dir.file("lineage.jrnl");
    std::vector<std::uint8_t> bytes = read_bytes(journal);
    bytes.insert(bytes.end(), 5, 0x11);
    write_bytes(journal, bytes);
    const std::uint64_t size = byte_size(journal);
    EngineOptions options = engine_options(fixture, dir.path());
    options.repair_torn_tail = false;
    options.enable_snapshot = false;
    Result<Engine> engine = Engine::open(options);
    LFF_CHECK(!engine.ok());
    expect_outcome(engine.status(), Outcome::Corrupt, "strict torn tail");
    LFF_CHECK_EQ(byte_size(journal), size);
}

LFF_TEST(adversarial, lineage_of_a_foreign_fabric_is_refused) {
    TempDir dir("adv-foreign");
    FabricFixture fixture = adversarial_fixture();
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        expect_ok(engine.value().checkpoint(), "checkpoint");
        expect_ok(engine.value().close(), "close");
    }
    FabricFixture other = make_fabric("other-fabric", 1, {{"alpha", 4, 1000}});
    Result<Engine> engine = Engine::open(engine_options(other, dir.path()));
    LFF_CHECK(!engine.ok());
    expect_outcome(engine.status(), Outcome::Conflict, "foreign lineage");
}

LFF_TEST(adversarial, snapshot_corruption_is_refused) {
    TempDir dir("adv-snapshot");
    FabricFixture fixture = adversarial_fixture();
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        expect_ok(engine.value().checkpoint(), "checkpoint");
        expect_ok(engine.value().close(), "close");
    }
    const std::filesystem::path snapshot = dir.file("lineage.snap");
    LFF_CHECK(file_exists(snapshot));
    std::vector<std::uint8_t> bytes = read_bytes(snapshot);
    bytes[0] ^= 0xFF;
    write_bytes(snapshot, bytes);
    const std::uint64_t size = byte_size(snapshot);
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(!engine.ok());
    expect_outcome(engine.status(), Outcome::Corrupt, "corrupt snapshot");
    LFF_CHECK_EQ(byte_size(snapshot), size);
}

LFF_TEST(adversarial, impossible_snapshot_element_counts_are_refused) {
    TempDir dir("adv-snapshot-count");
    FabricFixture fixture = adversarial_fixture();
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        expect_ok(engine.value().checkpoint(), "checkpoint");
        expect_ok(engine.value().close(), "close");
    }
    const std::filesystem::path snapshot = dir.file("lineage.snap");
    std::vector<std::uint8_t> bytes = read_bytes(snapshot);
    // Corrupt the payload and repair both checksums so only the content is wrong.
    for (std::size_t i = kJournalHeaderSize; i < bytes.size(); ++i) {
        bytes[i] ^= 0x5A;
    }
    const std::uint32_t payload_crc = crc32c(bytes.data() + 32, bytes.size() - 32);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[12 + i] = static_cast<std::uint8_t>((payload_crc >> (i * 8)) & 0xFFu);
    }
    const std::uint32_t header_crc = crc32c(bytes.data(), 28);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[28 + i] = static_cast<std::uint8_t>((header_crc >> (i * 8)) & 0xFFu);
    }
    write_bytes(snapshot, bytes);
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(!engine.ok());
    LFF_CHECK(engine.status().outcome() == Outcome::Corrupt ||
              engine.status().outcome() == Outcome::Unsupported ||
              engine.status().outcome() == Outcome::LimitExceeded);
}

LFF_TEST(adversarial, epoch_and_incarnation_are_fenced_across_reopen) {
    TempDir dir("adv-fence");
    FabricFixture fixture = adversarial_fixture();
    Incarnation first;
    {
        Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
        LFF_CHECK(engine.ok());
        first = engine.value().incarnation();
        expect_ok(engine.value().close(), "close");
    }
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    LFF_CHECK(engine.value().incarnation() != first);
    LFF_CHECK_EQ(engine.value().epoch().value(), 2ull);
    LFF_CHECK_EQ(engine.value().inspect(0).active_authorities, std::size_t{0});
    expect_ok(engine.value().close(), "close");
}