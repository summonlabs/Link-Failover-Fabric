// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "framework.hpp"
#include "lff/bytes.hpp"
#include "lff/store.hpp"
#include "support.hpp"

#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

std::vector<std::uint8_t> sample_payload() {
    Writer writer;
    writer.u16(0xBEEF);
    writer.u32(0xDEADBEEF);
    writer.u64(0x0123456789ABCDEFull);
    writer.bool8(true);
    writer.str("synthetic-agent");
    writer.digest(sha256("digest"));
    writer.incarnation(Incarnation::generate());
    writer.u32(0);
    return writer.take();
}

}  // namespace

LFF_TEST(unit, writer_reader_round_trip) {
    const std::vector<std::uint8_t> bytes = sample_payload();
    Reader reader(bytes.data(), bytes.size());
    LFF_CHECK_EQ(reader.u16(), std::uint16_t{0xBEEF});
    LFF_CHECK_EQ(reader.u32(), 0xDEADBEEFu);
    LFF_CHECK_EQ(reader.u64(), 0x0123456789ABCDEFull);
    LFF_CHECK(reader.bool8());
    LFF_CHECK_EQ(reader.str(), std::string("synthetic-agent"));
    LFF_CHECK(reader.digest() == sha256("digest"));
    (void)reader.incarnation();
    LFF_CHECK_EQ(reader.u32(), 0u);
    LFF_CHECK(reader.ok());
    LFF_CHECK(reader.at_end());
    LFF_CHECK_EQ(reader.remaining(), std::size_t{0});
}

// Every truncated prefix of a well-formed encoding must fail rather than
// produce a partially valid object.
LFF_TEST(unit, reader_is_total_for_every_truncated_prefix) {
    const std::vector<std::uint8_t> bytes = sample_payload();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        Reader reader(bytes.data(), length);
        (void)reader.u16();
        (void)reader.u32();
        (void)reader.u64();
        (void)reader.bool8();
        (void)reader.str();
        (void)reader.digest();
        (void)reader.incarnation();
        (void)reader.u32();
        if (reader.ok() && reader.at_end()) {
            LFF_FAIL(format_string("truncated prefix of %zu bytes decoded as complete", length));
        }
    }
    Reader full(bytes.data(), bytes.size());
    // A complete buffer must decode cleanly and land exactly at its end.
    (void)full.u16();
    (void)full.u32();
    (void)full.u64();
    (void)full.bool8();
    (void)full.str();
    (void)full.digest();
    (void)full.incarnation();
    (void)full.u32();
    LFF_CHECK(full.ok());
    LFF_CHECK(full.at_end());
}

LFF_TEST(unit, reader_is_sticky_after_first_failure) {
    const std::vector<std::uint8_t> bytes = sample_payload();
    Reader reader(bytes.data(), 2);
    LFF_CHECK_EQ(reader.u16(), std::uint16_t{0xBEEF});
    LFF_CHECK(reader.ok());
    (void)reader.u32();
    LFF_CHECK(!reader.ok());
    // Later reads stay failed and return neutral values rather than garbage.
    LFF_CHECK_EQ(reader.u8(), std::uint8_t{0});
    LFF_CHECK_EQ(reader.u64(), 0ull);
    LFF_CHECK_EQ(reader.str(), std::string());
    LFF_CHECK(!reader.ok());
    LFF_CHECK_EQ(reader.remaining(), std::size_t{0});
}

LFF_TEST(unit, writer_refuses_oversized_strings_and_blobs) {
    Writer writer;
    writer.str(std::string(kMaxEncodedString + 1, 'a'));
    LFF_CHECK(!writer.ok());
    LFF_CHECK_EQ(writer.size(), std::size_t{0});
    Writer blob_writer;
    blob_writer.blob(std::vector<std::uint8_t>(kMaxEncodedBlob + 1));
    LFF_CHECK(!blob_writer.ok());
    Writer fine;
    fine.str(std::string(kMaxEncodedString, 'a'));
    LFF_CHECK(fine.ok());
}

LFF_TEST(unit, reader_refuses_oversized_declared_lengths) {
    Writer writer;
    writer.u32(0xFFFFFFFFu);
    const std::vector<std::uint8_t> bytes = writer.take();
    Reader reader(bytes.data(), bytes.size());
    (void)reader.blob();
    LFF_CHECK(!reader.ok());
    Reader string_reader(bytes.data(), bytes.size());
    LFF_CHECK_EQ(string_reader.str(), std::string());
    LFF_CHECK(!string_reader.ok());
}

LFF_TEST(unit, canonical_encodings_are_stable) {
    FabricFixture fixture = make_fabric("codec-fabric", 3, {{"alpha", 1, 100}, {"beta", 2, 200}});
    const std::vector<std::uint8_t> first = encode_topology_payload(fixture.topology);
    TopologySnapshot reordered = fixture.topology;
    std::reverse(reordered.links.begin(), reordered.links.end());
    const std::vector<std::uint8_t> second = encode_topology_payload(reordered);
    LFF_CHECK(first != second);
    reordered.canonicalise();
    LFF_CHECK(encode_topology_payload(reordered) == first);
    LFF_CHECK_EQ(reordered.digest(), fixture.topology.digest());

    TopologySnapshot decoded;
    LFF_CHECK(decode_topology_payload(first.data(), first.size(), decoded));
    LFF_CHECK_EQ(decoded.digest(), fixture.topology.digest());
    LFF_CHECK_EQ(decoded.generation.value(), 3ull);
    LFF_CHECK_EQ(decoded.links.size(), std::size_t{2});
}

LFF_TEST(unit, payload_decoders_refuse_trailing_bytes) {
    FabricFixture fixture = make_fabric("codec-fabric", 1, {{"alpha", 1, 100}});
    std::vector<std::uint8_t> bytes = encode_topology_payload(fixture.topology);
    bytes.push_back(0);
    TopologySnapshot decoded;
    LFF_CHECK(!decode_topology_payload(bytes.data(), bytes.size(), decoded));
    std::vector<std::uint8_t> policy_bytes = encode_policy_payload(fixture.policy);
    policy_bytes.insert(policy_bytes.end(), {1, 2, 3});
    FailoverPolicy policy;
    LFF_CHECK(!decode_policy_payload(policy_bytes.data(), policy_bytes.size(), policy));
}

LFF_TEST(unit, policy_payload_rejects_unsupported_version_and_enums) {
    FabricFixture fixture = make_fabric("codec-fabric", 1, {{"alpha", 1, 100}});
    std::vector<std::uint8_t> bytes = encode_policy_payload(fixture.policy);
    FailoverPolicy policy;
    // The first two bytes are the identity format version.
    std::vector<std::uint8_t> bumped = bytes;
    bumped[0] = 99;
    LFF_CHECK(!decode_policy_payload(bumped.data(), bumped.size(), policy));
    // The agreement enum sits after version(2) + generation(8) + two
    // confidences(4+4) + age(8) + capacity(4) + four booleans(1 each).
    std::vector<std::uint8_t> bad_enum = bytes;
    const std::size_t agreement_offset = 2 + 8 + 4 + 4 + 8 + 4 + 1 + 1 + 1 + 1;
    bad_enum[agreement_offset] = 9;
    bad_enum[agreement_offset + 1] = 0;
    LFF_CHECK(!decode_policy_payload(bad_enum.data(), bad_enum.size(), policy));
}

LFF_TEST(unit, topology_validate_rejects_duplicates_and_zero_generations) {
    FabricFixture fixture = make_fabric("codec-fabric", 1, {{"alpha", 1, 100}});
    TopologySnapshot duplicate = fixture.topology;
    duplicate.links.push_back(duplicate.links.front());
    LFF_CHECK(!duplicate.validate().ok());
    TopologySnapshot zero = fixture.topology;
    zero.links.front().generation = LinkGeneration::from(0);
    LFF_CHECK(!zero.validate().ok());
    TopologySnapshot unsorted = fixture.topology;
    unsorted.links.push_back(LinkDefinition{});
    LFF_CHECK(!unsorted.validate().ok());
}

LFF_TEST(unit, topology_link_bound_is_enforced) {
    TopologySnapshot topology;
    topology.generation = TopologyGeneration::from(1);
    for (std::size_t i = 0; i <= kMaxTopologyLinks; ++i) {
        LinkDefinition definition;
        definition.link.fabric = FabricName::parse("bound-fabric").value();
        definition.link.link = LinkName::parse("l" + std::to_string(i)).value();
        definition.generation = LinkGeneration::from(1);
        definition.capacity_units = 1;
        topology.links.push_back(definition);
    }
    topology.canonicalise();
    LFF_CHECK(!topology.validate().ok());
    LFF_CHECK(topology.validate().outcome() == Outcome::LimitExceeded);
}

LFF_TEST(unit, attempt_payload_round_trip_is_exact) {
    FabricFixture fixture = make_fabric("codec-fabric", 2, {{"alpha", 4, 100}, {"beta", 5, 200}});
    AttemptRecord record;
    record.subject.fabric = fixture.fabric;
    record.subject.link = LinkName::parse("alpha").value();
    record.subject_generation = LinkGeneration::from(4);
    record.attempt_seq = AttemptSeq::from(3);
    record.attempt_id = compute_attempt_id(fixture.fabric, record.subject,
                                           record.subject_generation, record.attempt_seq);
    record.idempotency_key = sha256("key");
    record.phase = AttemptPhase::Acknowledged;
    record.replacement = LinkKey{fixture.fabric, LinkName::parse("beta").value()};
    record.replacement_generation = LinkGeneration::from(5);
    record.evidence_digest = sha256("evidence");
    record.authority.fabric = fixture.fabric;
    record.authority.subject = record.subject;
    record.authority.subject_generation = record.subject_generation;
    record.authority.replacement = record.replacement;
    record.authority.replacement_generation = record.replacement_generation;
    record.authority.topology_generation = fixture.topology.generation;
    record.authority.policy_generation = fixture.policy.generation;
    record.authority.policy_digest = fixture.policy.digest();
    record.authority.epoch = Epoch::from(2);
    record.authority.coordinator = Incarnation::generate();
    record.authority.evidence_publisher = PublisherName::parse("monitor").value();
    record.authority.publisher_incarnation = Incarnation::generate();
    record.authority.attempt_seq = record.attempt_seq;
    record.created_tick = 12;
    record.created_epoch = Epoch::from(2);
    record.coordinator = Incarnation::generate();
    record.create_seq = LineageSeq::from(9);
    record.update_seq = LineageSeq::from(11);
    record.predecessor = sha256("predecessor");
    record.applier_incarnation = Incarnation::generate();
    record.revalidated = true;
    record.revalidation = RevalidationVerdict::NotApplied;
    record.reasons.emplace_back(ReasonCode::AcknowledgementNotEffect, "ack");
    record.reasons.emplace_back(ReasonCode::Fenced, "fenced");

    const std::vector<std::uint8_t> bytes = encode_attempt_payload(record);
    AttemptRecord decoded;
    LFF_CHECK(decode_attempt_payload(bytes.data(), bytes.size(), decoded));
    LFF_CHECK_EQ(decoded.digest(), record.digest());
    LFF_CHECK_EQ(decoded.attempt_seq.value(), 3ull);
    LFF_CHECK(decoded.phase == AttemptPhase::Acknowledged);
    LFF_CHECK(decoded.predecessor.has_value());
    LFF_CHECK(decoded.revalidated);
    LFF_CHECK(decoded.revalidation == RevalidationVerdict::NotApplied);
    LFF_CHECK_EQ(decoded.reasons.size(), std::size_t{2});

    // Every truncated prefix must be refused.
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        AttemptRecord partial;
        if (decode_attempt_payload(bytes.data(), length, partial)) {
            LFF_FAIL(format_string("attempt payload truncated to %zu bytes was accepted", length));
        }
    }
}

LFF_TEST(unit, decision_payload_round_trip_and_bounds) {
    FabricFixture fixture = make_fabric("codec-fabric", 2, {{"alpha", 4, 100}, {"beta", 5, 200}});
    FailoverDecision decision;
    decision.fabric = fixture.fabric;
    decision.kind = DecisionKind::Grant;
    decision.assertion = Assertion::Authorization;
    decision.outcome = Outcome::Ok;
    decision.subject.fabric = fixture.fabric;
    decision.subject.link = LinkName::parse("alpha").value();
    decision.subject_generation = LinkGeneration::from(4);
    decision.has_replacement = true;
    decision.replacement = LinkKey{fixture.fabric, LinkName::parse("beta").value()};
    decision.replacement_generation = LinkGeneration::from(5);
    decision.attempt_seq = AttemptSeq::from(1);
    decision.epoch = Epoch::from(2);
    decision.coordinator = Incarnation::generate();
    decision.evidence_digest = sha256("evidence");
    decision.authority.fabric = fixture.fabric;
    decision.authority.subject = decision.subject;
    decision.authority.subject_generation = decision.subject_generation;
    decision.authority.replacement = decision.replacement;
    decision.authority.replacement_generation = decision.replacement_generation;
    decision.authority.topology_generation = fixture.topology.generation;
    decision.authority.policy_generation = fixture.policy.generation;
    decision.authority.epoch = decision.epoch;
    decision.authority.coordinator = decision.coordinator;
    decision.authority.evidence_publisher = PublisherName::parse("monitor").value();
    decision.authority.publisher_incarnation = Incarnation::generate();
    decision.lineage_seq = LineageSeq::from(42);
    decision.durable = true;
    decision.attempt_id = compute_attempt_id(decision.fabric, decision.subject,
                                             decision.subject_generation, decision.attempt_seq);
    decision.reasons.emplace_back(ReasonCode::SelectedHighestObjective, "best");
    CandidateAssessment assessment;
    assessment.candidate = decision.replacement;
    assessment.link_generation = LinkGeneration::from(5);
    assessment.verdict = Verdict::Eligible;
    assessment.reason = ReasonCode::SelectedHighestObjective;
    assessment.state = ObservationState::Up;
    assessment.confidence = make_confidence(990);
    assessment.freshness = FreshnessClass::Current;
    assessment.capacity_units = 200;
    assessment.adjacency_authorized = true;
    decision.candidates.push_back(assessment);
    decision.decision_id = decision.compute_id();

    const std::vector<std::uint8_t> bytes = encode_decision_payload(decision);
    FailoverDecision decoded;
    LFF_CHECK(decode_decision_payload(bytes.data(), bytes.size(), decoded));
    LFF_CHECK_EQ(decoded.decision_id, decision.decision_id);
    LFF_CHECK_EQ(decoded.canonical_key(), decision.canonical_key());
    LFF_CHECK_EQ(decoded.candidates.size(), std::size_t{1});
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        FailoverDecision partial;
        if (decode_decision_payload(bytes.data(), length, partial)) {
            LFF_FAIL(format_string("decision payload truncated to %zu bytes was accepted", length));
        }
    }
}

LFF_TEST(unit, idempotency_payload_round_trip_and_rejection) {
    IdempotencyEntry entry;
    entry.key = sha256("key");
    entry.decision_id = sha256("decision");
    entry.kind = DecisionKind::Refuse;
    entry.assertion = Assertion::Eligibility;
    entry.outcome = Outcome::Refused;
    entry.subject.fabric = FabricName::parse("codec-fabric").value();
    entry.subject.link = LinkName::parse("alpha").value();
    entry.subject_generation = LinkGeneration::from(4);
    entry.attempt_seq = AttemptSeq::from(2);
    entry.seq = LineageSeq::from(9);
    entry.reasons.emplace_back(ReasonCode::SubjectNotFailed, "healthy");

    const std::vector<std::uint8_t> bytes = encode_idempotency_payload(entry);
    IdempotencyEntry decoded;
    LFF_CHECK(decode_idempotency_payload(bytes.data(), bytes.size(), decoded));
    LFF_CHECK_EQ(decoded.key, entry.key);
    LFF_CHECK(decoded.kind == DecisionKind::Refuse);
    LFF_CHECK(decoded.outcome == Outcome::Refused);
    LFF_CHECK_EQ(decoded.reasons.size(), std::size_t{1});

    // A malformed enum must be refused rather than silently mapped.
    std::vector<std::uint8_t> broken = bytes;
    const std::size_t kind_offset = 2 + 32 + 32;
    broken[kind_offset] = 200;
    broken[kind_offset + 1] = 0;
    LFF_CHECK(!decode_idempotency_payload(broken.data(), broken.size(), decoded));
    // A reason count larger than the bound must be refused before allocation.
    // The payload is rebuilt field by field so the count offset is exact.
    {
        Writer writer;
        writer.u16(1);
        writer.digest(entry.key);
        writer.digest(entry.decision_id);
        writer.u16(static_cast<std::uint16_t>(entry.kind));
        writer.u16(static_cast<std::uint16_t>(entry.assertion));
        writer.u16(static_cast<std::uint16_t>(entry.outcome));
        writer.str(entry.subject.fabric.value());
        writer.str(entry.subject.link.value());
        writer.u64(entry.subject_generation.value());
        writer.bool8(false);
        writer.u64(entry.attempt_seq.value());
        writer.u64(entry.seq.value());
        writer.u32(0xFFFFFFFFu);
        const std::vector<std::uint8_t> huge = writer.take();
        LFF_CHECK(!decode_idempotency_payload(huge.data(), huge.size(), decoded));
    }
}

LFF_TEST(unit, boot_and_checkpoint_payload_round_trip) {
    BootPayload boot;
    boot.epoch = Epoch::from(9);
    boot.coordinator = Incarnation::generate();
    boot.boot_count = 4;
    boot.epoch_advances = 4;
    boot.toolchain = "msvc";
    boot.architecture = "x86_64";
    const std::vector<std::uint8_t> bytes = encode_boot_payload(boot);
    BootPayload decoded;
    LFF_CHECK(decode_boot_payload(bytes.data(), bytes.size(), decoded));
    LFF_CHECK_EQ(decoded.epoch.value(), 9ull);
    LFF_CHECK(decoded.coordinator == boot.coordinator);
    LFF_CHECK_EQ(decoded.boot_count, 4ull);

    CheckpointPayload checkpoint;
    checkpoint.covers_seq = LineageSeq::from(77);
    checkpoint.record_count = 12;
    checkpoint.snapshot_bytes = 4096;
    const std::vector<std::uint8_t> checkpoint_bytes = encode_checkpoint_payload(checkpoint);
    CheckpointPayload decoded_checkpoint;
    LFF_CHECK(decode_checkpoint_payload(checkpoint_bytes.data(), checkpoint_bytes.size(),
                                        decoded_checkpoint));
    LFF_CHECK_EQ(decoded_checkpoint.covers_seq.value(), 77ull);
    LFF_CHECK_EQ(decoded_checkpoint.snapshot_bytes, 4096ull);
}

LFF_TEST(unit, authority_vector_round_trip_and_mismatch_reporting) {
    AuthorityVector authority;
    authority.fabric = FabricName::parse("auth-fabric").value();
    authority.subject.fabric = authority.fabric;
    authority.subject.link = LinkName::parse("alpha").value();
    authority.subject_generation = LinkGeneration::from(4);
    authority.replacement.fabric = authority.fabric;
    authority.replacement.link = LinkName::parse("beta").value();
    authority.replacement_generation = LinkGeneration::from(5);
    authority.topology_generation = TopologyGeneration::from(2);
    authority.topology_digest = sha256("topology");
    authority.policy_generation = PolicyGeneration::from(3);
    authority.policy_digest = sha256("policy");
    authority.epoch = Epoch::from(4);
    authority.coordinator = Incarnation::generate();
    authority.evidence_publisher = PublisherName::parse("monitor").value();
    authority.publisher_incarnation = Incarnation::generate();
    authority.publisher_seq = ObservationSeq::from(8);
    authority.attempt_seq = AttemptSeq::from(1);
    authority.evidence_digest = sha256("evidence");

    Writer writer;
    authority.encode(writer);
    const std::vector<std::uint8_t> bytes = writer.take();
    Reader reader(bytes.data(), bytes.size());
    AuthorityVector decoded;
    LFF_CHECK(AuthorityVector::decode(reader, decoded));
    LFF_CHECK(reader.at_end());
    LFF_CHECK_EQ(decoded.digest(), authority.digest());

    AuthorityExpectation expectation;
    expectation.epoch = Epoch::from(4);
    expectation.coordinator = authority.coordinator;
    expectation.topology_generation = TopologyGeneration::from(2);
    expectation.policy_generation = PolicyGeneration::from(3);
    expectation.subject_generation = LinkGeneration::from(4);
    LFF_CHECK(compare_authority(expectation, authority).empty());

    AuthorityExpectation stale = expectation;
    stale.epoch = Epoch::from(3);
    stale.topology_generation = TopologyGeneration::from(1);
    const std::vector<AuthorityMismatch> mismatches = compare_authority(stale, authority);
    LFF_CHECK_EQ(mismatches.size(), std::size_t{2});
    bool saw_epoch = false;
    bool saw_topology = false;
    for (const AuthorityMismatch& mismatch : mismatches) {
        saw_epoch = saw_epoch || mismatch.code == ReasonCode::EpochMismatch;
        saw_topology = saw_topology || mismatch.code == ReasonCode::TopologyGenerationMismatch;
    }
    LFF_CHECK(saw_epoch);
    LFF_CHECK(saw_topology);

    AuthorityExpectation absent;
    LFF_CHECK(compare_authority(absent, authority).empty());

    Writer expectation_writer;
    expectation.encode(expectation_writer);
    Reader expectation_reader(expectation_writer.data().data(), expectation_writer.size());
    AuthorityExpectation decoded_expectation;
    LFF_CHECK(AuthorityExpectation::decode(expectation_reader, decoded_expectation));
    LFF_CHECK(compare_authority(decoded_expectation, authority).empty());
}