// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "framework.hpp"
#include "support.hpp"
#include "lff/bytes.hpp"
#include "lff/hash.hpp"
#include "lff/ids.hpp"
#include "lff/policy.hpp"

#include <array>
#include <set>
#include <string>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

}  // namespace

LFF_TEST(unit, checked_arithmetic_never_wraps) {
    std::uint64_t value = 0;
    LFF_CHECK(checked_add_u64(1, 2, value) && value == 3);
    LFF_CHECK(!checked_add_u64(0xFFFFFFFFFFFFFFFFull, 1, value));
    LFF_CHECK(checked_add_u64(0xFFFFFFFFFFFFFFFEull, 1, value) && value == 0xFFFFFFFFFFFFFFFFull);
    LFF_CHECK(!checked_mul_u64(0xFFFFFFFFFFFFFFFFull, 2, value));
    LFF_CHECK(checked_mul_u64(0, 0xFFFFFFFFFFFFFFFFull, value) && value == 0);
    std::uint32_t small = 0;
    LFF_CHECK(!checked_add_u32(0xFFFFFFFFu, 1u, small));
    LFF_CHECK(!checked_mul_u32(0x10000u, 0x10000u, small));
    LFF_CHECK(checked_mul_u32(0xFFFFu, 0xFFFFu, small) && small == 0xFFFE0001u);
}

LFF_TEST(unit, generation_next_refuses_to_wrap) {
    LinkGeneration generation = LinkGeneration::from(0xFFFFFFFFFFFFFFFFull);
    LinkGeneration next;
    LFF_CHECK(!generation.next(next));
    generation = LinkGeneration::from(41);
    LFF_CHECK(generation.next(next));
    LFF_CHECK_EQ(next.value(), 42ull);
}

LFF_TEST(unit, name_grammar_is_strict) {
    LFF_CHECK(is_valid_name("uplink-a"));
    LFF_CHECK(is_valid_name("a.b_c:d-1"));
    LFF_CHECK(is_valid_name(std::string(64, 'a')));
    LFF_CHECK(!is_valid_name(""));
    LFF_CHECK(!is_valid_name(std::string(65, 'a')));
    LFF_CHECK(!is_valid_name("."));
    LFF_CHECK(!is_valid_name(".."));
    LFF_CHECK(!is_valid_name("has space"));
    LFF_CHECK(!is_valid_name("slash/inside"));
    LFF_CHECK(!is_valid_name("back\\slash"));
    LFF_CHECK(!is_valid_name(std::string_view("null\0byte", 9)));
    LFF_CHECK(!is_valid_name("semi;colon"));
    LFF_CHECK(!is_valid_name("new\nline"));
    LFF_CHECK(!FabricName::parse("../escape").ok());
    LFF_CHECK(FabricName::parse("fabric.one").ok());
    LFF_CHECK(LinkName::parse("uplink-1").ok());
    // Distinct types: a link name cannot be used where a fabric name is needed.
    const Result<LinkName> link = LinkName::parse("x");
    LFF_CHECK(link.ok());
}

LFF_TEST(unit, link_key_orders_by_fabric_then_link) {
    const LinkKey a{FabricName::parse("a").value(), LinkName::parse("z").value()};
    const LinkKey b{FabricName::parse("b").value(), LinkName::parse("a").value()};
    LFF_CHECK(a < b);
    LFF_CHECK(!(b < a));
    LFF_CHECK_EQ(a.to_string(), std::string("a/z"));
    std::set<LinkKey> ordered;
    ordered.insert(b);
    ordered.insert(a);
    LFF_CHECK_EQ(ordered.size(), std::size_t{2});
    LFF_CHECK(*ordered.begin() == a);
}

LFF_TEST(unit, incarnation_round_trip_and_ordering) {
    const Incarnation generated = Incarnation::generate();
    LFF_CHECK(!generated.is_zero());
    Incarnation parsed;
    LFF_CHECK(Incarnation::parse(generated.hex(), parsed));
    LFF_CHECK(parsed == generated);
    LFF_CHECK(!Incarnation::parse("", parsed));
    LFF_CHECK(!Incarnation::parse("zzzz", parsed));
    LFF_CHECK(!Incarnation::parse(std::string(32, 'z'), parsed));
    LFF_CHECK(!Incarnation::parse(generated.hex().substr(0, 31), parsed));
    const Incarnation second = Incarnation::generate();
    LFF_CHECK(second != generated);
    LFF_CHECK((generated < second) != (second < generated));
}

LFF_TEST(unit, endpoint_parse_rejects_malformed) {
    LFF_CHECK(Endpoint::parse("127.0.0.1:8080").ok());
    LFF_CHECK_EQ(Endpoint::parse("127.0.0.1:8080").value().port, std::uint16_t{8080});
    LFF_CHECK(Endpoint::parse("[::1]:1").ok());
    LFF_CHECK(!Endpoint::parse("127.0.0.1").ok());
    LFF_CHECK(!Endpoint::parse("127.0.0.1:").ok());
    LFF_CHECK(!Endpoint::parse(":80").ok());
    // Port zero is a valid *bind* request; the bound port is reported back.
    LFF_CHECK(Endpoint::parse("127.0.0.1:0").ok());
    LFF_CHECK_EQ(Endpoint::parse("127.0.0.1:0").value().port, std::uint16_t{0});
    LFF_CHECK(!Endpoint::parse("127.0.0.1:65536").ok());
    LFF_CHECK(!Endpoint::parse("127.0.0.1:99999999999999999999999").ok());
    LFF_CHECK(!Endpoint::parse("host with space:80").ok());
    LFF_CHECK(!Endpoint::parse(std::string(400, 'a') + ":80").ok());
    LFF_CHECK_EQ(Endpoint::parse("localhost:9").value().to_string(), std::string("localhost:9"));
}

LFF_TEST(unit, digest_hex_and_parse) {
    const Digest digest = sha256("link failover fabric");
    LFF_CHECK_EQ(digest.hex().size(), std::size_t{64});
    Digest parsed;
    LFF_CHECK(Digest::parse(digest.hex(), parsed));
    LFF_CHECK(parsed == digest);
    LFF_CHECK(Digest::parse("0000000000000000000000000000000000000000000000000000000000000000", parsed));
    LFF_CHECK(parsed.is_zero());
    LFF_CHECK(!Digest::parse(digest.hex().substr(0, 63), parsed));
    LFF_CHECK(!Digest::parse(std::string(64, 'g'), parsed));
    LFF_CHECK(Digest{}.is_zero());
}

LFF_TEST(unit, sha256_matches_known_vectors) {
    LFF_CHECK_EQ(sha256("").hex(),
                 std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    LFF_CHECK_EQ(sha256("abc").hex(),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    LFF_CHECK_EQ(sha256(std::string(1000, 'a')).hex(),
                 std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

LFF_TEST(unit, crc32c_matches_known_vectors) {
    LFF_CHECK_EQ(crc32c("", 0), 0u);
    LFF_CHECK_EQ(crc32c("123456789", 9), 0xE3069283u);
    const char* text = "the quick brown fox";
    const std::uint32_t split = crc32c_concat(0, text, 5, text + 5, 14);
    LFF_CHECK_EQ(split, crc32c(text, 19));
    const std::uint32_t seeded = crc32c_extend(0x12345678u, text, 19);
    LFF_CHECK(seeded != crc32c(text, 19));
}

LFF_TEST(unit, digest_builder_is_unambiguous) {
    const Digest first = DigestBuilder().add("a", "ab").add("b", "c").finish();
    const Digest second = DigestBuilder().add("a", "a").add("b", "bc").finish();
    LFF_CHECK(first != second);
    const Digest third = DigestBuilder().add("a", "ab").add("b", "c").finish();
    LFF_CHECK(first == third);
    const Digest labelled = DigestBuilder().add("x", "ab").add("b", "c").finish();
    LFF_CHECK(first != labelled);
}

LFF_TEST(unit, outcome_and_reason_vocabulary_round_trips) {
    for (std::uint16_t raw = 0; raw < kOutcomeCount; ++raw) {
        const Outcome value = static_cast<Outcome>(raw);
        Outcome parsed{};
        LFF_CHECK(outcome_parse(outcome_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    Outcome unknown{};
    LFF_CHECK(!outcome_parse("NotAnOutcome", unknown));
    for (std::uint16_t raw = 0; raw < kReasonCodeCount; ++raw) {
        const ReasonCode value = static_cast<ReasonCode>(raw);
        ReasonCode parsed{};
        if (!reason_code_parse(reason_code_name(value), parsed)) {
            LFF_FAIL(format_string("reason code %u has no parsable name", raw));
        }
        LFF_CHECK(parsed == value);
    }
    ReasonCode parsed{};
    LFF_CHECK(!reason_code_parse("NotAReason", parsed));
}

LFF_TEST(unit, reason_detail_is_bounded) {
    const Reason reason(ReasonCode::InternalError, std::string(10000, 'x'));
    LFF_CHECK_EQ(reason.detail.size(), kMaxReasonDetail);
    Status status;
    for (std::size_t i = 0; i < kMaxReasons + 50; ++i) {
        status.add(ReasonCode::InternalError, "x");
    }
    LFF_CHECK_EQ(status.reasons().size(), kMaxReasons);
}

LFF_TEST(unit, observation_state_vocabulary_and_failure_semantics) {
    for (std::uint16_t raw = 0; raw < kObservationStateCount; ++raw) {
        const ObservationState value = static_cast<ObservationState>(raw);
        ObservationState parsed{};
        LFF_CHECK(observation_state_parse(observation_state_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    LFF_CHECK(observation_state_is_failure(ObservationState::Down, false));
    LFF_CHECK(observation_state_is_failure(ObservationState::Unusable, false));
    LFF_CHECK(!observation_state_is_failure(ObservationState::Degraded, false));
    LFF_CHECK(observation_state_is_failure(ObservationState::Degraded, true));
    LFF_CHECK(!observation_state_is_failure(ObservationState::Up, true));
    LFF_CHECK(!observation_state_is_failure(ObservationState::Unknown, true));
}

LFF_TEST(unit, enum_vocabularies_round_trip) {
    for (std::uint16_t raw = 0; raw < kAttemptPhaseCount; ++raw) {
        const AttemptPhase value = static_cast<AttemptPhase>(raw);
        AttemptPhase parsed{};
        LFF_CHECK(attempt_phase_parse(attempt_phase_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    for (std::uint16_t raw = 0; raw < kDecisionKindCount; ++raw) {
        const DecisionKind value = static_cast<DecisionKind>(raw);
        DecisionKind parsed{};
        LFF_CHECK(decision_kind_parse(decision_kind_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    for (std::uint16_t raw = 0; raw < kAssertionCount; ++raw) {
        const Assertion value = static_cast<Assertion>(raw);
        Assertion parsed{};
        LFF_CHECK(assertion_parse(assertion_name(value), parsed));
        LFF_CHECK(parsed == value);
        LFF_CHECK_EQ(assertion_rank(value), raw);
    }
    for (std::uint16_t raw = 0; raw < kFenceKindCount; ++raw) {
        const FenceKind value = static_cast<FenceKind>(raw);
        FenceKind parsed{};
        LFF_CHECK(fence_kind_parse(fence_kind_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    for (std::uint16_t raw = 0; raw < kRevalidationVerdictCount; ++raw) {
        const RevalidationVerdict value = static_cast<RevalidationVerdict>(raw);
        RevalidationVerdict parsed{};
        LFF_CHECK(revalidation_verdict_parse(revalidation_verdict_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    for (std::uint16_t raw = 0; raw < kRecordTypeCount; ++raw) {
        const RecordType value = static_cast<RecordType>(raw);
        RecordType parsed{};
        LFF_CHECK(record_type_parse(record_type_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    // Message type values are deliberately sparse; the vocabulary is closed
    // against anything that is not a declared enumerator.
    static_assert(kMessageTypeCount == 16, "the message vocabulary changed");
    const std::array<MessageType, kMessageTypeCount> kMessages = {
        MessageType::Hello,        MessageType::HelloAck,      MessageType::Response,
        MessageType::PublishFailure, MessageType::PublishReplacement, MessageType::SetPolicy,
        MessageType::SetTopology,  MessageType::RequestFailover, MessageType::RequestRollback,
        MessageType::ReportApplication, MessageType::ReportVerification,
        MessageType::ResolveInterrupted, MessageType::Inspect, MessageType::ClaimDirective,
        MessageType::Shutdown,     MessageType::Ping};
    for (MessageType value : kMessages) {
        LFF_CHECK(message_type_is_known(static_cast<std::uint16_t>(value)));
        MessageType parsed{};
        LFF_CHECK(message_type_parse(message_type_name(value), parsed));
        LFF_CHECK(parsed == value);
    }
    LFF_CHECK(!message_type_is_known(0));
    LFF_CHECK(!message_type_is_known(4));
    LFF_CHECK(!message_type_is_known(999));
    for (std::uint16_t raw = 0; raw < kSelectionStatusCount; ++raw) {
        LFF_CHECK(!selection_status_name(static_cast<SelectionStatus>(raw)).empty());
    }
    for (std::uint16_t raw = 0; raw < kVerdictCount; ++raw) {
        LFF_CHECK(!verdict_name(static_cast<Verdict>(raw)).empty());
    }
    for (std::uint16_t raw = 0; raw < kFreshnessClassCount; ++raw) {
        LFF_CHECK(!freshness_class_name(static_cast<FreshnessClass>(raw)).empty());
    }
    for (std::uint16_t raw = 0; raw < kAgreementRuleCount; ++raw) {
        AgreementRule parsed{};
        LFF_CHECK(agreement_rule_parse(agreement_rule_name(static_cast<AgreementRule>(raw)), parsed));
    }
    for (std::uint16_t raw = 0; raw < kAttemptPhaseCount; ++raw) {
        const AttemptPhase value = static_cast<AttemptPhase>(raw);
        if (attempt_phase_holds_authority(value)) {
            LFF_CHECK(!attempt_phase_is_terminal(value) || value == AttemptPhase::Committed);
        }
    }
}

LFF_TEST(unit, policy_validate_rejects_out_of_range) {
    FailoverPolicy policy = FailoverPolicy::defaults();
    LFF_CHECK(!policy.validate().ok());
    policy.generation = PolicyGeneration::from(1);
    LFF_CHECK(policy.validate().ok());
    FailoverPolicy bad = policy;
    bad.evidence_max_age_ticks = 0;
    LFF_CHECK(!bad.validate().ok());
    bad = policy;
    bad.max_candidates = 0;
    LFF_CHECK(!bad.validate().ok());
    bad = policy;
    bad.max_candidates = kMaxPolicyCandidates + 1;
    LFF_CHECK(!bad.validate().ok());
    bad = policy;
    bad.max_attempts_per_generation = 0;
    LFF_CHECK(!bad.validate().ok());
    bad = policy;
    bad.min_failure_confidence = Confidence::from(kMaxConfidence + 1u);
    LFF_CHECK(!bad.validate().ok());
}

LFF_TEST(unit, policy_text_round_trip_and_rejection) {
    FailoverPolicy policy = FailoverPolicy::defaults();
    policy.generation = PolicyGeneration::from(7);
    policy.agreement = AgreementRule::MajorityOfFreshQualified;
    policy.allow_degraded_replacement = true;
    policy.min_replacement_capacity = 512;
    const std::string text = failover_policy_to_string(policy);
    FailoverPolicy parsed;
    LFF_CHECK(failover_policy_parse(text, parsed));
    LFF_CHECK_EQ(parsed.generation.value(), 7ull);
    LFF_CHECK(parsed.agreement == AgreementRule::MajorityOfFreshQualified);
    LFF_CHECK(parsed.allow_degraded_replacement);
    LFF_CHECK_EQ(parsed.min_replacement_capacity, 512u);
    LFF_CHECK_EQ(parsed.digest(), policy.digest());
    LFF_CHECK(!failover_policy_parse("generation=1\nnonsense=1\n", parsed));
    LFF_CHECK(!failover_policy_parse("generation=abc\n", parsed));
    LFF_CHECK(!failover_policy_parse("agreement=perhaps\n", parsed));
    LFF_CHECK(!failover_policy_parse("no-equals-sign\n", parsed));
    LFF_CHECK(failover_policy_parse("# comment\n\ngeneration=2\n", parsed));
    LFF_CHECK_EQ(parsed.generation.value(), 2ull);
}