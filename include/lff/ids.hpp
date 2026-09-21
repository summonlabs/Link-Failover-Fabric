// Link Failover Fabric — strongly typed identities and generations.
//
// Interchangeable strings and bare integers are not used to name subjects or to
// bind authority. Every authority-bearing counter has its own type so that a
// topology generation cannot be passed where a policy generation is required.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"
#include "lff/hash.hpp"
#include "lff/outcome.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lff {

// ---------------------------------------------------------------------------
// Checked arithmetic
//
// Every counter that participates in authority binding is advanced through
// these helpers. Wraparound is a hard failure, never a silent restart at zero.
// ---------------------------------------------------------------------------
LFF_API bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;
LFF_API bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;
LFF_API bool checked_add_u32(std::uint32_t a, std::uint32_t b, std::uint32_t& out) noexcept;
LFF_API bool checked_mul_u32(std::uint32_t a, std::uint32_t b, std::uint32_t& out) noexcept;

// ---------------------------------------------------------------------------
// StrongUint<Tag, Rep>
// ---------------------------------------------------------------------------
template <class Tag, class Rep>
class StrongUint {
public:
    using rep_type = Rep;

    constexpr StrongUint() noexcept = default;
    constexpr explicit StrongUint(Rep value) noexcept : value_(value) {}

    static constexpr StrongUint from(Rep value) noexcept { return StrongUint(value); }
    static constexpr StrongUint zero() noexcept { return StrongUint(0); }

    constexpr Rep value() const noexcept { return value_; }
    constexpr bool is_zero() const noexcept { return value_ == 0; }

    // Returns false instead of wrapping.
    bool next(StrongUint& out) const noexcept {
        Rep candidate{};
        if constexpr (sizeof(Rep) == 8) {
            if (!checked_add_u64(value_, 1, candidate)) {
                return false;
            }
        } else {
            if (!checked_add_u32(value_, 1, candidate)) {
                return false;
            }
        }
        out = StrongUint(candidate);
        return true;
    }

    friend constexpr bool operator==(StrongUint a, StrongUint b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(StrongUint a, StrongUint b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(StrongUint a, StrongUint b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator<=(StrongUint a, StrongUint b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>(StrongUint a, StrongUint b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator>=(StrongUint a, StrongUint b) noexcept { return a.value_ >= b.value_; }

private:
    Rep value_{};
};

struct LinkGenerationTag;
struct TopologyGenerationTag;
struct PolicyGenerationTag;
struct EpochTag;
struct AttemptSeqTag;
struct ObservationSeqTag;
struct LineageSeqTag;
struct SessionSeqTag;
struct ConfidenceTag;

using LinkGeneration = StrongUint<LinkGenerationTag, std::uint64_t>;
using TopologyGeneration = StrongUint<TopologyGenerationTag, std::uint64_t>;
using PolicyGeneration = StrongUint<PolicyGenerationTag, std::uint64_t>;
using Epoch = StrongUint<EpochTag, std::uint64_t>;
using AttemptSeq = StrongUint<AttemptSeqTag, std::uint64_t>;
using ObservationSeq = StrongUint<ObservationSeqTag, std::uint64_t>;
using LineageSeq = StrongUint<LineageSeqTag, std::uint64_t>;
using SessionSeq = StrongUint<SessionSeqTag, std::uint64_t>;
// Confidence in parts per thousand. 1000 == fully confident.
using Confidence = StrongUint<ConfidenceTag, std::uint32_t>;

inline constexpr std::uint32_t kMaxConfidence = 1000;

LFF_API Confidence make_confidence(std::uint32_t permille) noexcept;

// ---------------------------------------------------------------------------
// StrongName<Tag> — validated, but never used to build filesystem paths
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxNameLength = 64;

LFF_API bool is_valid_name(std::string_view text) noexcept;

template <class Tag>
class StrongName {
public:
    StrongName() = default;
    explicit StrongName(std::string value) : value_(std::move(value)) {}

    static Result<StrongName> parse(std::string_view text) {
        if (!is_valid_name(text)) {
            return Result<StrongName>::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                               "name rejected by identity grammar");
        }
        return Result<StrongName>::success(StrongName(std::string(text)));
    }

    const std::string& value() const noexcept { return value_; }
    bool empty() const noexcept { return value_.empty(); }

    friend bool operator==(const StrongName& a, const StrongName& b) noexcept { return a.value_ == b.value_; }
    friend bool operator!=(const StrongName& a, const StrongName& b) noexcept { return a.value_ != b.value_; }
    friend bool operator<(const StrongName& a, const StrongName& b) noexcept { return a.value_ < b.value_; }

private:
    std::string value_;
};

using FabricName = StrongName<struct FabricNameTag>;
using LinkName = StrongName<struct LinkNameTag>;

// Decodes a name that may legitimately be absent from a durable or transported
// structure: an empty text yields an empty name, and a non-empty text must
// satisfy the identity grammar. Strict validation of *externally supplied*
// identities happens at the runtime boundary, not in the codec, so that every
// value the runtime can hold round-trips exactly.
template <class Name>
bool decode_name(std::string_view text, Name& out) {
    if (text.empty()) {
        out = Name{};
        return true;
    }
    const Result<Name> parsed = Name::parse(text);
    if (!parsed.ok()) {
        return false;
    }
    out = parsed.value();
    return true;
}

// A link is identified by (fabric, link). Canonical ordering is fabric then link.
struct LinkKey {
    FabricName fabric;
    LinkName link;

    bool empty() const noexcept { return fabric.empty() || link.empty(); }
    std::string to_string() const;

    friend bool operator==(const LinkKey& a, const LinkKey& b) noexcept {
        return a.fabric == b.fabric && a.link == b.link;
    }
    friend bool operator!=(const LinkKey& a, const LinkKey& b) noexcept { return !(a == b); }
    friend bool operator<(const LinkKey& a, const LinkKey& b) noexcept {
        if (a.fabric < b.fabric) {
            return true;
        }
        if (b.fabric < a.fabric) {
            return false;
        }
        return a.link < b.link;
    }
};

// ---------------------------------------------------------------------------
// Incarnation — the identity of one process boot
//
// An incarnation is created exactly once per process start and never reused. It
// is the fence that separates "this runtime instance" from "the runtime
// instance that used to hold authority". Restarting a process preserves the
// durable lineage but always produces a new incarnation.
// ---------------------------------------------------------------------------
struct Incarnation {
    std::array<std::uint8_t, 16> bytes{};

    bool is_zero() const noexcept;
    std::string hex() const;
    static bool parse(std::string_view text, Incarnation& out) noexcept;

    // Derives an incarnation from a fresh random nonce, the process id and a
    // wall-clock seed. Uniqueness is per boot, not cryptographic.
    static Incarnation generate() noexcept;
};

LFF_API bool operator==(const Incarnation& a, const Incarnation& b) noexcept;
LFF_API bool operator!=(const Incarnation& a, const Incarnation& b) noexcept;
LFF_API bool operator<(const Incarnation& a, const Incarnation& b) noexcept;

// ---------------------------------------------------------------------------
// Endpoint — a loopback/interface transport address
// ---------------------------------------------------------------------------
struct Endpoint {
    std::string host;
    // Zero means "any free port", which is only meaningful when binding a
    // listener; the endpoint actually bound is reported separately.
    std::uint16_t port{0};

    std::string to_string() const;
    static Result<Endpoint> parse(std::string_view text);
};

// ---------------------------------------------------------------------------
// Identity digest helpers
// ---------------------------------------------------------------------------
LFF_API Digest link_identity_digest(const LinkKey& key);

}  // namespace lff
