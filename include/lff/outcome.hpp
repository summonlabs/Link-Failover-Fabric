// Link Failover Fabric — outcomes, reason codes, status and result types.
//
// The outcome vocabulary is deliberately explicit. UNKNOWN, STALE, CONFLICT,
// INVALID and UNSUPPORTED are first-class results and are never folded into
// "success" or into ordinary "not found"; only Outcome::Ok asserts that the
// runtime actually established what the caller asked for.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Every externally visible enumeration declares a fixed 16-bit base type on
// purpose. Its values are written to the durable journal and to the wire at a
// frozen width, so narrowing the type to the minimum that fits today's
// enumerators would silently change the format. Static analysis that suggests a
// narrower base type is declined for that reason.
namespace lff {

// ---------------------------------------------------------------------------
// Outcome
// ---------------------------------------------------------------------------
enum class Outcome : std::uint16_t {
    Ok = 0,             // the requested assertion was established
    Refused = 1,        // conclusively not allowed under current evidence/policy
    Unknown = 2,        // evidence or effect state is not knowable right now
    Stale = 3,          // bound generation/epoch/incarnation is no longer current
    Conflict = 4,       // another authority, attempt or session owns this subject
    Invalid = 5,        // the input is structurally or semantically malformed
    Unsupported = 6,    // understood but outside the supported problem class
    NotFound = 7,       // the subject does not exist in this runtime
    LimitExceeded = 8,  // a configured bound was reached; nothing was mutated
    Corrupt = 9,        // durable bytes failed integrity checks
    Indeterminate = 10, // no total order or proof could decide the question
    Interrupted = 11,   // a pre-restart in-flight operation with unknown effect
    Exhausted = 12,     // a bounded resource pool is exhausted
    IoError = 13,       // a durable or network medium failed
    Closed = 14,        // the runtime or session is shutting down
    Unauthenticated = 15, // the caller did not establish session authority
};
inline constexpr std::uint16_t kOutcomeCount = 16;

LFF_API std::string_view outcome_name(Outcome value) noexcept;
LFF_API bool outcome_parse(std::string_view text, Outcome& out) noexcept;
LFF_API bool outcome_is_ok(Outcome value) noexcept;

// ---------------------------------------------------------------------------
// Reason codes
//
// Reason codes are part of the deterministic explanation surface: two runs that
// receive the same inputs produce the same ordered reason codes. Numeric values
// are stable and are covered by round-trip tests.
// ---------------------------------------------------------------------------
enum class ReasonCode : std::uint16_t {
    None = 0,

    // Structural / encoding
    InvalidArgument = 1,
    MalformedEncoding = 2,
    UnsupportedValue = 3,
    LimitExceeded = 4,
    TrailingBytes = 5,
    ChecksumMismatch = 6,
    UnsupportedFormatVersion = 7,
    CorruptRecord = 8,
    SequenceRegression = 9,
    TornTailRecovered = 10,
    NullField = 11,

    // Identity and generations
    UnknownFabric = 12,
    UnknownLink = 13,
    UnknownGeneration = 14,
    GenerationMismatch = 15,
    PolicyGenerationMismatch = 16,
    TopologyGenerationMismatch = 17,
    SubjectGenerationMismatch = 18,
    ReplacementGenerationMismatch = 19,

    // Authority and sessions
    EpochMismatch = 20,
    IncarnationMismatch = 21,
    StaleAuthority = 22,
    SessionMismatch = 23,
    SessionSequenceRegressed = 24,
    SessionSequenceReplayed = 25,
    CoordinatorIncarnationMismatch = 26,
    PublisherIncarnationMismatch = 27,
    WorkerIncarnationMismatch = 28,
    PreRestartAuthority = 29,

    // Evidence
    EvidenceMissing = 30,
    EvidenceUnknown = 31,
    EvidenceStale = 32,
    EvidenceConflicted = 33,
    EvidenceLowConfidence = 34,
    EvidenceDigestMismatch = 35,
    EvidenceFromPriorIncarnation = 36,

    // Eligibility and selection
    SubjectNotFailed = 37,
    SubjectUnknown = 38,
    ReplacementNotEligible = 39,
    ReplacementInsufficientCapacity = 40,
    ReplacementNotAdjacencyAuthorized = 41,
    ReplacementStateUnusable = 42,
    ReplacementIsSubject = 43,
    NoEligibleReplacement = 44,
    SelectionIndeterminate = 45,
    SelectedHighestObjective = 46,
    CertifiedInfeasible = 47,
    CandidateSetEmpty = 48,

    // Attempt lifecycle
    AlreadyAuthoritative = 49,
    AttemptInFlight = 50,
    AttemptCompleted = 51,
    AttemptUnknown = 52,
    DuplicateCompletion = 53,
    LateAcknowledgement = 54,
    ReplayDetected = 55,
    IdempotentReplay = 56,
    RevalidationRequired = 57,
    FenceRequired = 58,
    Fenced = 59,
    AttemptsExhausted = 60,

    // Effect
    BackendRejected = 61,
    BackendUnknown = 62,
    EffectUnverified = 63,
    EffectVerified = 64,
    AcknowledgementNotEffect = 65,

    // Rollback / restore
    RollbackRequested = 66,
    RollbackNotAuthorized = 67,
    RollbackCompleted = 68,
    RestoreRequiresNewGeneration = 69,
    OriginalLinkRestored = 70,

    // Restart
    InterruptedBeforeCommit = 71,
    InterruptedAfterCommit = 72,
    InterruptedAfterApply = 73,
    EpochAdvancedOnBoot = 74,
    DurableLineagePreserved = 75,
    DynamicStateNotRestored = 76,

    // Transport
    ProtocolVersionMismatch = 77,
    FrameTooLarge = 78,
    UnknownMessageType = 79,
    StickyDecodeFailure = 80,
    ConnectionClosed = 81,
    SocketError = 82,
    NotListening = 83,
    Shutdown = 84,
    TruncatedFrame = 85,
    ReservedBitsSet = 86,

    // Policy
    PolicyRejected = 87,
    PolicyConfidenceUnreachable = 88,
    PolicyDisabled = 89,

    // Internal
    InternalError = 90,
    CheckedArithmeticOverflow = 91,
    FeatureUnsupported = 92,
    SyntheticFixture = 93,
};
inline constexpr std::uint16_t kReasonCodeCount = 94;

LFF_API std::string_view reason_code_name(ReasonCode value) noexcept;
LFF_API bool reason_code_parse(std::string_view text, ReasonCode& out) noexcept;

// ---------------------------------------------------------------------------
// Reason and Status
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxReasonDetail = 160;
inline constexpr std::size_t kMaxReasons = 24;

struct Reason {
    ReasonCode code{ReasonCode::None};
    std::string detail;

    Reason() = default;
    Reason(ReasonCode c, std::string d = {}) : code(c), detail(std::move(d)) {
        if (detail.size() > kMaxReasonDetail) {
            detail.resize(kMaxReasonDetail);
        }
    }
};

LFF_API bool operator==(const Reason& a, const Reason& b) noexcept;
LFF_API bool operator<(const Reason& a, const Reason& b) noexcept;

// Status is a bounded, deterministic explanation carrier. It never throws and
// never allocates without a bound.
class LFF_API Status {
public:
    Status() = default;
    explicit Status(Outcome outcome) : outcome_(outcome) {}

    static Status success() { return Status(Outcome::Ok); }
    static Status of(Outcome outcome) { return Status(outcome); }
    static Status failure(Outcome outcome, ReasonCode code, std::string detail = {}) {
        Status s(outcome);
        s.add(code, std::move(detail));
        return s;
    }

    Status& add(ReasonCode code, std::string detail = {}) {
        if (reasons_.size() < kMaxReasons) {
            reasons_.emplace_back(code, std::move(detail));
        }
        return *this;
    }
    Status& with(ReasonCode code, std::string detail = {}) { return add(code, std::move(detail)); }

    Outcome outcome() const noexcept { return outcome_; }
    void set_outcome(Outcome value) noexcept { outcome_ = value; }
    bool ok() const noexcept { return outcome_ == Outcome::Ok; }
    const std::vector<Reason>& reasons() const noexcept { return reasons_; }
    void clear() noexcept { outcome_ = Outcome::Ok; reasons_.clear(); }

    bool has(ReasonCode code) const noexcept;
    bool any_of(Outcome outcome) const noexcept { return outcome_ == outcome; }

    // "Ok" or "Stale(EvidenceStale, EpochMismatch)"
    std::string to_string() const;

    // Stable canonical encoding used inside digests. The status bytes only ever
    // describe a decision that already exists; they never carry authority.
    std::string canonical_key() const;

private:
    Outcome outcome_{Outcome::Ok};
    std::vector<Reason> reasons_;
};

LFF_API std::string to_string(const Status& status);

// ---------------------------------------------------------------------------
// Result<T>
// ---------------------------------------------------------------------------
template <class T>
class Result {
public:
    // A default-constructed Result holds no value and reports Indeterminate.
    // It exists so that a Result can be a member that is assigned later.
    Result() : status_(Outcome::Indeterminate) {}
    Result(T value) : status_(Outcome::Ok), value_(std::move(value)) {}   // NOLINT(google-explicit-constructor)
    Result(Status status) : status_(std::move(status)) {}                 // NOLINT(google-explicit-constructor)

    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;
    Result(const Result&) = default;
    Result& operator=(const Result&) = default;

    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Status status) { return Result(std::move(status)); }
    static Result failure(Outcome outcome, ReasonCode code, std::string detail = {}) {
        return Result(Status::failure(outcome, code, std::move(detail)));
    }

    bool ok() const noexcept { return value_.has_value(); }
    const Status& status() const noexcept { return status_; }
    Outcome outcome() const noexcept { return status_.outcome(); }

    // Unchecked accessors. `ok()` is a precondition: callers must not reach for
    // the value of a failed result.
    const T& value() const noexcept { return *value_; }
    T& value() noexcept { return *value_; }
    T value_or(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }
    const T* operator->() const noexcept { return &*value_; }
    const T& operator*() const noexcept { return *value_; }

private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace lff
