// Link Failover Fabric — authority binding and fences.
//
// An AuthorityVector is the complete set of generations and incarnations that
// made one decision legal. Comparing identity is never enough: two records can
// name the same link and the same attempt while binding different epochs,
// incarnations or generations, and only the generation-complete comparison may
// be used to decide whether authority still holds.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"
#include "lff/evidence.hpp"
#include "lff/hash.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"
#include "lff/policy.hpp"

#include <optional>
#include <string>
#include <vector>

namespace lff {

struct AuthorityVector {
    FabricName fabric;

    LinkKey subject;
    LinkGeneration subject_generation;

    LinkKey replacement;
    LinkGeneration replacement_generation;

    TopologyGeneration topology_generation;
    Digest topology_digest;
    PolicyGeneration policy_generation;
    Digest policy_digest;

    Epoch epoch;
    Incarnation coordinator;

    PublisherName evidence_publisher;
    Incarnation publisher_incarnation;
    ObservationSeq publisher_seq;

    // Zero until an external applier is bound to this authority.
    Incarnation applier_incarnation;

    AttemptSeq attempt_seq;
    Digest evidence_digest;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, AuthorityVector& out);
    Digest digest() const;
    std::string to_string() const;
};

// The subset of authority a caller may assert when submitting a request. Every
// field is optional and absent fields are explicitly checked rather than
// assumed to match.
struct AuthorityExpectation {
    std::optional<Epoch> epoch;
    std::optional<Incarnation> coordinator;
    std::optional<TopologyGeneration> topology_generation;
    std::optional<PolicyGeneration> policy_generation;
    std::optional<LinkGeneration> subject_generation;
    std::optional<Digest> policy_digest;
    std::optional<Digest> topology_digest;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, AuthorityExpectation& out);
};

// One field-by-field mismatch record. Comparison reports every difference rather
// than the first, so explanations stay complete and deterministic.
struct AuthorityMismatch {
    ReasonCode code{ReasonCode::None};
    std::string field;
    std::string expected;
    std::string actual;
};

LFF_API std::vector<AuthorityMismatch> compare_authority(
    const AuthorityExpectation& expectation, const AuthorityVector& current);

// A fence is a durable statement that a specific pre-existing authority is no
// longer live. Fences are how restart and single-winner guarantees are made
// observable rather than implied.
enum class FenceKind : std::uint16_t {
    AttemptSuperseded = 0,
    PreRestartAuthority = 1,
    SessionRevoked = 2,
    PolicyChanged = 3,
    TopologyChanged = 4,
    EpochAdvanced = 5,
    RollbackRevoked = 6,
};
inline constexpr std::uint16_t kFenceKindCount = 7;

LFF_API std::string_view fence_kind_name(FenceKind value) noexcept;
LFF_API bool fence_kind_parse(std::string_view text, FenceKind& out) noexcept;

struct FenceRecord {
    FenceKind kind{FenceKind::PreRestartAuthority};
    LinkKey subject;
    LinkGeneration subject_generation;
    AttemptSeq attempt_seq;
    Epoch epoch;
    Incarnation coordinator;
    LineageSeq seq;
    std::string detail;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, FenceRecord& out);
    Digest digest() const;
};

}  // namespace lff
