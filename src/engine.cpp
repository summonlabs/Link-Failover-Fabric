// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/engine.hpp"

#include "detail/fault.hpp"
#include "lff/bytes.hpp"
#include "lff/hash.hpp"
#include "lff/version.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace lff {
namespace {

struct PublisherState {
    Incarnation incarnation;
    Epoch epoch;
    ObservationSeq highest_seq;
    std::uint64_t first_seen_tick{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected{0};
};

struct FailureVerdict {
    bool established{false};
    Outcome outcome{Outcome::Unknown};
    ReasonCode reason{ReasonCode::EvidenceMissing};
    std::string detail;
    PublisherName publisher;
    Incarnation publisher_incarnation;
    ObservationSeq publisher_seq;
    Digest evidence_digest;
    std::vector<Reason> extra;
};

FreshnessClass classify_receipt(const Incarnation& receipt_coordinator, std::uint64_t receipt_tick,
                                const Incarnation& current, std::uint64_t now,
                                std::uint64_t max_age, std::uint64_t& age_out) {
    age_out = 0;
    if (receipt_coordinator != current) {
        return FreshnessClass::PriorIncarnation;
    }
    if (receipt_tick > now) {
        return FreshnessClass::Future;
    }
    age_out = now - receipt_tick;
    return age_out > max_age ? FreshnessClass::Expired : FreshnessClass::Current;
}

std::string u64_text(std::uint64_t value) {
    return std::to_string(value);
}

}  // namespace

// ---------------------------------------------------------------------------
// ApplyDirective
// ---------------------------------------------------------------------------
void ApplyDirective::encode(Writer& writer) const {
    writer.u16(kIdentityFormatVersion);
    writer.str(subject.fabric.value());
    writer.str(subject.link.value());
    writer.u64(subject_generation.value());
    writer.str(replacement.fabric.value());
    writer.str(replacement.link.value());
    writer.u64(replacement_generation.value());
    writer.u64(attempt_seq.value());
    writer.digest(attempt_id);
    authority.encode(writer);
    writer.u64(issued_tick);
}

bool ApplyDirective::decode(Reader& reader, ApplyDirective& out) {
    const std::uint16_t format_version = reader.u16();
    ApplyDirective value;
    const std::string subject_fabric = reader.str();
    const std::string subject_link = reader.str();
    const std::uint64_t subject_generation = reader.u64();
    const std::string replacement_fabric = reader.str();
    const std::string replacement_link = reader.str();
    const std::uint64_t replacement_generation = reader.u64();
    const std::uint64_t attempt_seq = reader.u64();
    const Digest attempt_id = reader.digest();
    AuthorityVector authority;
    const bool authority_ok = AuthorityVector::decode(reader, authority);
    const std::uint64_t issued_tick = reader.u64();
    if (!reader.ok() || !authority_ok || format_version != kIdentityFormatVersion) {
        return false;
    }
    const Result<FabricName> subject_fabric_name = FabricName::parse(subject_fabric);
    const Result<LinkName> subject_link_name = LinkName::parse(subject_link);
    const Result<FabricName> replacement_fabric_name = FabricName::parse(replacement_fabric);
    const Result<LinkName> replacement_link_name = LinkName::parse(replacement_link);
    if (!subject_fabric_name.ok() || !subject_link_name.ok() || !replacement_fabric_name.ok() ||
        !replacement_link_name.ok()) {
        return false;
    }
    value.subject.fabric = subject_fabric_name.value();
    value.subject.link = subject_link_name.value();
    value.subject_generation = LinkGeneration::from(subject_generation);
    value.replacement.fabric = replacement_fabric_name.value();
    value.replacement.link = replacement_link_name.value();
    value.replacement_generation = LinkGeneration::from(replacement_generation);
    value.attempt_seq = AttemptSeq::from(attempt_seq);
    value.attempt_id = attempt_id;
    value.authority = authority;
    value.issued_tick = issued_tick;
    out = std::move(value);
    return true;
}

Digest ApplyDirective::digest() const {
    Writer writer;
    encode(writer);
    return sha256(writer.data().data(), writer.size());
}

// ---------------------------------------------------------------------------
// Engine::Impl
// ---------------------------------------------------------------------------
struct Engine::Impl {
    EngineOptions options;
    LineageStore store;
    Incarnation incarnation;
    Epoch epoch;
    std::uint64_t tick{0};
    mutable std::mutex mu;
    // A deque, not a vector: the retained decision log is a bounded FIFO, and
    // erasing from the front of a vector would make every record after the bound
    // is reached cost O(retained) instead of O(1).
    std::deque<FailoverDecision> decisions;
    std::map<PublisherName, PublisherState> publishers;
    std::map<LinkKey, std::map<PublisherName, RetainedFailureEvidence>> failure_evidence;
    std::map<LinkKey, std::map<PublisherName, RetainedReplacementEvidence>> replacement_evidence;
    std::map<LinkKey, std::map<PublisherName, ObservationSeq>> last_observation_seq;
    std::size_t evidence_entries{0};
    bool closed{false};
    bool store_failed{false};

    AuthorityVector current_authority(const LinkKey& subject, LinkGeneration subject_generation) const {
        const DurableState& state = store.state();
        AuthorityVector current;
        current.fabric = options.fabric;
        current.subject = subject;
        current.subject_generation = subject_generation;
        current.topology_generation = state.topology.generation;
        current.topology_digest = state.topology.digest();
        current.policy_generation = state.policy.generation;
        current.policy_digest = state.policy.digest();
        current.epoch = epoch;
        current.coordinator = incarnation;
        return current;
    }

    void remember_decision(const FailoverDecision& decision) {
        decisions.push_back(decision);
        while (decisions.size() > options.limits.max_retained_decisions) {
            decisions.pop_front();
        }
    }

    FailoverDecision base_decision(const LinkKey& subject, LinkGeneration subject_generation) const {
        FailoverDecision decision;
        decision.fabric = options.fabric;
        decision.subject = subject;
        decision.subject_generation = subject_generation;
        decision.epoch = epoch;
        decision.coordinator = incarnation;
        return decision;
    }

    // Records a decision durably. Returns false when the durable record could
    // not be written; the decision is still returned to the caller with
    // `durable = false`, which is explicitly not a claim of authority.
    //
    // Callers pass `durable_allowed = false` for decisions that change nothing
    // the next boot would observe: an expectation mismatch, a policy-disabled
    // refusal, an already-authoritative refusal, an idempotent replay or a
    // duplicate completion. Writing those would let a client spend an fsync per
    // request without changing any authority.
    bool commit_decision(FailoverDecision& decision) {
        const std::vector<std::uint8_t> payload = encode_decision_payload(decision);
        const Status status = store.commit(RecordType::DecisionCommit, payload,
                                           [&decision](LineageSeq seq) {
                                               decision.lineage_seq = seq;
                                               decision.durable = true;
                                           });
        if (!status.ok()) {
            store_failed = true;
            decision.durable = false;
            decision.reasons.emplace_back(ReasonCode::InternalError,
                                          "decision could not be recorded");
            return false;
        }
        decision.decision_id = decision.compute_id();
        return true;
    }

    void commit_idempotency(const FailoverDecision& decision, const Digest& key) {
        if (key.is_zero() || !decision.durable) {
            return;
        }
        IdempotencyEntry entry;
        entry.key = key;
        entry.decision_id = decision.decision_id;
        entry.kind = decision.kind;
        entry.assertion = decision.assertion;
        entry.outcome = decision.outcome;
        entry.subject = decision.subject;
        entry.subject_generation = decision.subject_generation;
        entry.has_replacement = decision.has_replacement;
        entry.replacement = decision.replacement;
        entry.replacement_generation = decision.replacement_generation;
        entry.attempt_seq = decision.attempt_seq;
        entry.seq = decision.lineage_seq;
        entry.reasons = decision.reasons;
        const std::vector<std::uint8_t> payload = encode_idempotency_payload(entry);
        if (payload.size() > kMaxIdempotencyDecisionBytes) {
            return;
        }
        const Status status = store.commit(RecordType::IdempotencyIndex, payload,
                                           [this, entry](LineageSeq) {
                                               store.note_idempotency(entry);
                                           });
        if (!status.ok()) {
            store_failed = true;
        }
    }

    // Compaction is driven by how much has been appended since the snapshot that
    // is currently on the medium. The absolute lineage sequence is unbounded, so
    // using it as the trigger would checkpoint once per record forever after the
    // first snapshot — which is both slow and a source of quadratic behaviour.
    void maybe_checkpoint() {
        if (!options.enable_snapshot || options.snapshot_every_records == 0) {
            return;
        }
        if (store.state().record_count == 0) {
            return;
        }
        if (store.records_since_snapshot() < options.snapshot_every_records) {
            return;
        }
        const Result<LineageSeq> result = store.checkpoint();
        if (!result.ok()) {
            store_failed = true;
        }
    }

    FailoverDecision replay_decision(const IdempotencyEntry& entry) const {
        FailoverDecision decision = base_decision(entry.subject, entry.subject_generation);
        decision.kind = entry.kind;
        decision.assertion = entry.assertion;
        decision.outcome = entry.outcome;
        decision.decision_id = entry.decision_id;
        decision.has_replacement = entry.has_replacement;
        decision.replacement = entry.replacement;
        decision.replacement_generation = entry.replacement_generation;
        decision.attempt_seq = entry.attempt_seq;
        decision.lineage_seq = entry.seq;
        decision.durable = true;
        decision.idempotent_replay = true;
        decision.reasons = entry.reasons;
        decision.reasons.emplace_back(ReasonCode::IdempotentReplay,
                                      "the original decision is returned; no new authority");
        return decision;
    }

    // --- evidence ----------------------------------------------------------
    FailureVerdict evaluate_failure(const LinkKey& subject, LinkGeneration subject_generation,
                                    const FailoverPolicy& policy,
                                    TopologyGeneration topology_generation) const {
        FailureVerdict verdict;
        const auto found = failure_evidence.find(subject);
        if (found == failure_evidence.end() || found->second.empty()) {
            verdict.outcome = Outcome::Unknown;
            verdict.reason = ReasonCode::EvidenceMissing;
            verdict.detail = "no failure observation has been published for this link";
            return verdict;
        }

        std::size_t qualified = 0;
        std::size_t failures = 0;
        bool saw_stale = false;
        bool saw_low_confidence = false;
        bool saw_unknown = false;
        bool saw_generation = false;
        bool saw_conflict = false;
        const RetainedFailureEvidence* best = nullptr;
        std::uint64_t best_age = 0;

        for (const auto& entry : found->second) {
            const RetainedFailureEvidence& retained = entry.second;
            const auto publisher = publishers.find(entry.first);
            if (publisher == publishers.end() ||
                publisher->second.incarnation != retained.report.stamp.publisher_incarnation) {
                saw_stale = true;
                continue;
            }
            if (retained.report.link_generation != subject_generation ||
                retained.report.topology_generation != topology_generation) {
                saw_generation = true;
                continue;
            }
            std::uint64_t age = 0;
            const FreshnessClass freshness =
                classify_receipt(retained.receipt_coordinator, retained.receipt_tick, incarnation,
                                 tick, policy.evidence_max_age_ticks, age);
            if (freshness != FreshnessClass::Current) {
                saw_stale = true;
                continue;
            }
            if (retained.report.confidence.value() < policy.min_failure_confidence.value()) {
                saw_low_confidence = true;
                continue;
            }
            if (retained.report.state == ObservationState::Unknown) {
                saw_unknown = true;
                continue;
            }
            ++qualified;
            const bool is_failure = observation_state_is_failure(
                retained.report.state, policy.treat_degraded_as_failed);
            if (is_failure) {
                ++failures;
                if (best == nullptr || retained.report.confidence.value() > best->report.confidence.value() ||
                    (retained.report.confidence.value() == best->report.confidence.value() &&
                     (age < best_age || (age == best_age && entry.first < best->report.stamp.publisher)))) {
                    best = &retained;
                    best_age = age;
                }
            }
        }

        if (qualified == 0) {
            verdict.outcome = Outcome::Unknown;
            verdict.reason = ReasonCode::EvidenceMissing;
            verdict.detail = "no usable failure observation is current";
            if (saw_stale) {
                verdict.reason = ReasonCode::EvidenceStale;
                verdict.detail = "every published failure observation is stale";
                verdict.extra.emplace_back(ReasonCode::EvidenceStale, "outside the policy window");
            }
            if (saw_low_confidence) {
                if (verdict.reason == ReasonCode::EvidenceMissing) {
                    verdict.reason = ReasonCode::EvidenceLowConfidence;
                    verdict.detail = "no observation reaches the policy confidence";
                }
                verdict.extra.emplace_back(ReasonCode::EvidenceLowConfidence, "below policy threshold");
            }
            if (saw_unknown) {
                if (verdict.reason == ReasonCode::EvidenceMissing) {
                    verdict.reason = ReasonCode::EvidenceUnknown;
                    verdict.detail = "publishers reported an unknown state";
                }
                verdict.extra.emplace_back(ReasonCode::EvidenceUnknown, "publisher state is unknown");
            }
            if (saw_generation) {
                if (verdict.reason == ReasonCode::EvidenceMissing) {
                    verdict.reason = ReasonCode::GenerationMismatch;
                    verdict.detail = "observations bind a different link or topology generation";
                }
                verdict.extra.emplace_back(ReasonCode::GenerationMismatch, "generation mismatch");
            }
            return verdict;
        }

        bool established = false;
        if (policy.agreement == AgreementRule::Unanimous) {
            established = failures == qualified;
            if (!established && failures > 0) {
                saw_conflict = true;
            }
        } else {
            const std::size_t doubled = failures * 2;
            established = doubled > qualified;
            if (!established && doubled == qualified) {
                saw_conflict = true;
            }
        }

        if (established && best != nullptr) {
            verdict.established = true;
            verdict.outcome = Outcome::Ok;
            verdict.reason = ReasonCode::SelectedHighestObjective;
            verdict.detail = "failure is established under the current policy";
            verdict.publisher = best->report.stamp.publisher;
            verdict.publisher_incarnation = best->report.stamp.publisher_incarnation;
            verdict.publisher_seq = best->report.stamp.observation_seq;
            verdict.evidence_digest = best->digest;
            return verdict;
        }
        if (saw_conflict) {
            verdict.outcome = Outcome::Indeterminate;
            verdict.reason = ReasonCode::EvidenceConflicted;
            verdict.detail = "fresh qualified observations contradict each other";
            return verdict;
        }
        verdict.outcome = Outcome::Refused;
        verdict.reason = ReasonCode::SubjectNotFailed;
        verdict.detail = "current qualified observations report the subject as healthy";
        return verdict;
    }
};

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------
Engine::~Engine() = default;
Engine::Engine(Engine&& other) noexcept : impl_(std::move(other.impl_)) {}

Engine& Engine::operator=(Engine&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Result<Engine> Engine::open(const EngineOptions& options) {
    const Status policy_status = options.policy.validate();
    if (!policy_status.ok()) {
        return Result<Engine>::failure(policy_status);
    }
    const Status topology_status = options.topology.validate();
    if (!topology_status.ok()) {
        return Result<Engine>::failure(topology_status);
    }
    if (options.fabric.empty()) {
        return Result<Engine>::failure(Status::failure(Outcome::Invalid, ReasonCode::UnknownFabric,
                                                       "engine fabric name must be set"));
    }
    if (options.topology.generation.is_zero()) {
        return Result<Engine>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "topology generation must be non-zero"));
    }

    StoreOptions store_options;
    store_options.directory = options.state_dir;
    store_options.fabric = options.fabric;
    store_options.policy = options.policy;
    store_options.topology = options.topology;
    store_options.max_attempts = options.limits.max_attempts;
    store_options.max_fences = options.limits.max_fences;
    store_options.max_idempotency = options.limits.max_idempotency;
    store_options.snapshot_every_records = options.snapshot_every_records;
    store_options.enable_snapshot = options.enable_snapshot;
    store_options.repair_torn_tail = options.repair_torn_tail;

    Result<LineageStore> opened = LineageStore::open(store_options);
    if (!opened.ok()) {
        return Result<Engine>::failure(opened.status());
    }

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->store = std::move(opened.value());
    impl->incarnation = Incarnation::generate();

    const DurableState& state = impl->store.state();
    std::uint64_t next_epoch = 0;
    if (!checked_add_u64(state.epoch.value(), 1, next_epoch)) {
        return Result<Engine>::failure(
            Status::failure(Outcome::Exhausted, ReasonCode::CheckedArithmeticOverflow,
                            "coordinator epoch space is exhausted"));
    }
    impl->epoch = Epoch::from(next_epoch);
    std::uint64_t boot_count = 0;
    std::uint64_t epoch_advances = 0;
    if (!checked_add_u64(state.boot_count, 1, boot_count) ||
        !checked_add_u64(state.epoch_advances, 1, epoch_advances)) {
        return Result<Engine>::failure(
            Status::failure(Outcome::Exhausted, ReasonCode::CheckedArithmeticOverflow,
                            "boot counter space is exhausted"));
    }

    BootPayload boot;
    boot.epoch = impl->epoch;
    boot.coordinator = impl->incarnation;
    boot.boot_count = boot_count;
    boot.epoch_advances = epoch_advances;
    boot.toolchain = std::string(build_toolchain());
    boot.architecture = std::string(build_architecture());

    const Status booted = impl->store.commit(
        RecordType::Boot, encode_boot_payload(boot),
        [&impl, &boot](LineageSeq) {
            impl->store.note_epoch(boot.epoch, boot.coordinator, boot.boot_count,
                                   boot.epoch_advances);
        });
    if (!booted.ok()) {
        return Result<Engine>::failure(booted);
    }

    // Fence everything the previous incarnation left in flight. Persistence is
    // not liveness: no attempt survives a restart as live authority.
    const Result<std::size_t> interrupted =
        impl->store.fence_restart_authority(impl->epoch, impl->incarnation);
    if (!interrupted.ok()) {
        return Result<Engine>::failure(interrupted.status());
    }

    Engine engine;
    engine.impl_ = std::move(impl);
    return Result<Engine>::success(std::move(engine));
}

Status Engine::close() {
    if (impl_ == nullptr) {
        return Status::success();
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return Status::success();
    }
    impl.closed = true;
    Writer writer;
    writer.u16(kIdentityFormatVersion);
    writer.u64(impl.epoch.value());
    writer.incarnation(impl.incarnation);
    writer.u64(impl.store.state().last_seq.value());
    const Status shutdown = impl.store.commit(RecordType::Shutdown, writer.take(),
                                             [](LineageSeq) {});
    if (shutdown.ok() && impl.options.enable_snapshot) {
        const Result<LineageSeq> checkpoint = impl.store.checkpoint();
        if (!checkpoint.ok()) {
            impl.store_failed = true;
        }
    }
    impl.store.close();
    return shutdown;
}

// ---------------------------------------------------------------------------
// Observations
// ---------------------------------------------------------------------------
Status Engine::publish_failure_report(const FailureReport& report) {
    if (impl_ == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return Status::failure(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    if (impl.store_failed) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "durable lineage is unavailable");
    }
    if (!(report.subject.fabric == impl.options.fabric)) {
        Status status = Status::failure(Outcome::NotFound, ReasonCode::UnknownFabric,
                                        "observation names an unknown fabric");
        status.add(ReasonCode::InvalidArgument, report.subject.fabric.value());
        return status;
    }
    if (report.subject.link.empty() || report.link_generation.is_zero() ||
        report.topology_generation.is_zero()) {
        return Status::failure(Outcome::Invalid, ReasonCode::NullField,
                               "observation must bind a link and non-zero generations");
    }
    if (report.stamp.publisher.empty() || report.stamp.publisher_incarnation.is_zero()) {
        return Status::failure(Outcome::Invalid, ReasonCode::NullField,
                               "observation must name a publisher and its incarnation");
    }
    ++impl.tick;

    const auto publisher = impl.publishers.find(report.stamp.publisher);
    if (publisher == impl.publishers.end()) {
        if (impl.publishers.size() >= impl.options.limits.max_tracked_publishers) {
            return Status::failure(Outcome::Exhausted, ReasonCode::LimitExceeded,
                                   "publisher registry is full");
        }
        PublisherState state;
        state.incarnation = report.stamp.publisher_incarnation;
        state.epoch = report.stamp.publisher_epoch;
        state.first_seen_tick = impl.tick;
        impl.publishers.emplace(report.stamp.publisher, state);
    } else if (publisher->second.incarnation != report.stamp.publisher_incarnation) {
        if (report.stamp.publisher_epoch < publisher->second.epoch) {
            Status status = Status::failure(Outcome::Stale, ReasonCode::PublisherIncarnationMismatch,
                                            "observation comes from a retired publisher incarnation");
            status.add(ReasonCode::PreRestartAuthority, report.stamp.publisher_incarnation.hex());
            return status;
        }
        if (report.stamp.publisher_epoch == publisher->second.epoch) {
            return Status::failure(Outcome::Conflict, ReasonCode::PublisherIncarnationMismatch,
                                   "two incarnations claim the same publisher epoch");
        }
        // The publisher restarted. Every observation it made under the previous
        // incarnation is retired; matching identity is not matching generation.
        publisher->second.incarnation = report.stamp.publisher_incarnation;
        publisher->second.epoch = report.stamp.publisher_epoch;
        for (auto& subject_entry : impl.failure_evidence) {
            impl.evidence_entries -= subject_entry.second.erase(report.stamp.publisher);
        }
        for (auto& subject_entry : impl.replacement_evidence) {
            impl.evidence_entries -= subject_entry.second.erase(report.stamp.publisher);
        }
        for (auto& subject_entry : impl.last_observation_seq) {
            subject_entry.second.erase(report.stamp.publisher);
        }
    }

    const auto last = impl.last_observation_seq.find(report.subject);
    if (last != impl.last_observation_seq.end()) {
        const auto sequence = last->second.find(report.stamp.publisher);
        if (sequence != last->second.end() &&
            !(sequence->second < report.stamp.observation_seq)) {
            Status status = Status::failure(Outcome::Stale, ReasonCode::SessionSequenceReplayed,
                                            "observation sequence does not advance");
            status.add(ReasonCode::SequenceRegression, u64_text(report.stamp.observation_seq.value()));
            return status;
        }
    }

    auto& bucket = impl.failure_evidence[report.subject];
    const bool replacing = bucket.find(report.stamp.publisher) != bucket.end();
    if (!replacing && impl.evidence_entries >= impl.options.limits.max_evidence_entries) {
        return Status::failure(Outcome::Exhausted, ReasonCode::LimitExceeded,
                               "evidence table is full");
    }

    RetainedFailureEvidence retained;
    retained.report = report;
    retained.digest = report.digest();
    retained.receipt_seq = LineageSeq{};
    retained.receipt_tick = impl.tick;
    retained.receipt_epoch = impl.epoch;
    retained.receipt_coordinator = impl.incarnation;
    retained.durable = false;
    bucket[report.stamp.publisher] = retained;
    impl.last_observation_seq[report.subject][report.stamp.publisher] =
        report.stamp.observation_seq;
    if (!replacing) {
        ++impl.evidence_entries;
    }
    impl.publishers[report.stamp.publisher].highest_seq = report.stamp.observation_seq;
    ++impl.publishers[report.stamp.publisher].accepted;
    return Status::success();
}

Status Engine::publish_replacement_report(const ReplacementReport& report) {
    if (impl_ == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return Status::failure(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    if (impl.store_failed) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "durable lineage is unavailable");
    }
    if (!(report.candidate.fabric == impl.options.fabric)) {
        Status status = Status::failure(Outcome::NotFound, ReasonCode::UnknownFabric,
                                        "observation names an unknown fabric");
        status.add(ReasonCode::InvalidArgument, report.candidate.fabric.value());
        return status;
    }
    if (report.candidate.link.empty() || report.link_generation.is_zero() ||
        report.topology_generation.is_zero()) {
        return Status::failure(Outcome::Invalid, ReasonCode::NullField,
                               "observation must bind a link and non-zero generations");
    }
    if (report.stamp.publisher.empty() || report.stamp.publisher_incarnation.is_zero()) {
        return Status::failure(Outcome::Invalid, ReasonCode::NullField,
                               "observation must name a publisher and its incarnation");
    }
    ++impl.tick;

    const auto publisher = impl.publishers.find(report.stamp.publisher);
    if (publisher == impl.publishers.end()) {
        if (impl.publishers.size() >= impl.options.limits.max_tracked_publishers) {
            return Status::failure(Outcome::Exhausted, ReasonCode::LimitExceeded,
                                   "publisher registry is full");
        }
        PublisherState state;
        state.incarnation = report.stamp.publisher_incarnation;
        state.epoch = report.stamp.publisher_epoch;
        state.first_seen_tick = impl.tick;
        impl.publishers.emplace(report.stamp.publisher, state);
    } else if (publisher->second.incarnation != report.stamp.publisher_incarnation) {
        if (report.stamp.publisher_epoch < publisher->second.epoch) {
            Status status = Status::failure(Outcome::Stale, ReasonCode::PublisherIncarnationMismatch,
                                            "observation comes from a retired publisher incarnation");
            status.add(ReasonCode::PreRestartAuthority, report.stamp.publisher_incarnation.hex());
            return status;
        }
        if (report.stamp.publisher_epoch == publisher->second.epoch) {
            return Status::failure(Outcome::Conflict, ReasonCode::PublisherIncarnationMismatch,
                                   "two incarnations claim the same publisher epoch");
        }
        publisher->second.incarnation = report.stamp.publisher_incarnation;
        publisher->second.epoch = report.stamp.publisher_epoch;
        for (auto& subject_entry : impl.failure_evidence) {
            impl.evidence_entries -= subject_entry.second.erase(report.stamp.publisher);
        }
        for (auto& subject_entry : impl.replacement_evidence) {
            impl.evidence_entries -= subject_entry.second.erase(report.stamp.publisher);
        }
        for (auto& subject_entry : impl.last_observation_seq) {
            subject_entry.second.erase(report.stamp.publisher);
        }
    }

    const auto last = impl.last_observation_seq.find(report.candidate);
    if (last != impl.last_observation_seq.end()) {
        const auto sequence = last->second.find(report.stamp.publisher);
        if (sequence != last->second.end() &&
            !(sequence->second < report.stamp.observation_seq)) {
            Status status = Status::failure(Outcome::Stale, ReasonCode::SessionSequenceReplayed,
                                            "observation sequence does not advance");
            status.add(ReasonCode::SequenceRegression, u64_text(report.stamp.observation_seq.value()));
            return status;
        }
    }

    auto& bucket = impl.replacement_evidence[report.candidate];
    const bool replacing = bucket.find(report.stamp.publisher) != bucket.end();
    if (!replacing && impl.evidence_entries >= impl.options.limits.max_evidence_entries) {
        return Status::failure(Outcome::Exhausted, ReasonCode::LimitExceeded,
                               "evidence table is full");
    }

    RetainedReplacementEvidence retained;
    retained.report = report;
    retained.digest = report.digest();
    retained.receipt_tick = impl.tick;
    retained.receipt_epoch = impl.epoch;
    retained.receipt_coordinator = impl.incarnation;
    retained.durable = false;
    bucket[report.stamp.publisher] = retained;
    impl.last_observation_seq[report.candidate][report.stamp.publisher] =
        report.stamp.observation_seq;
    if (!replacing) {
        ++impl.evidence_entries;
    }
    impl.publishers[report.stamp.publisher].highest_seq = report.stamp.observation_seq;
    ++impl.publishers[report.stamp.publisher].accepted;
    return Status::success();
}

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------
Status Engine::set_policy(const FailoverPolicy& policy) {
    if (impl_ == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return Status::failure(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    Status valid = policy.validate();
    if (!valid.ok()) {
        return valid;
    }
    const FailoverPolicy& current = impl.store.state().policy;
    if (policy.generation < current.generation) {
        Status status = Status::failure(Outcome::Stale, ReasonCode::PolicyGenerationMismatch,
                                        "policy generation regresses");
        status.add(ReasonCode::SequenceRegression, u64_text(policy.generation.value()));
        return status;
    }
    if (policy.generation == current.generation) {
        if (policy.digest() == current.digest()) {
            return Status::success();
        }
        return Status::failure(Outcome::Conflict, ReasonCode::PolicyGenerationMismatch,
                               "same policy generation with different content");
    }
    Status committed = impl.store.commit(
        RecordType::PolicyCommit, encode_policy_payload(policy),
        [&impl, &policy](LineageSeq) { impl.store.mutable_state().policy = policy; });
    if (!committed.ok()) {
        impl.store_failed = true;
        return committed;
    }
    // Any authority granted under the previous policy generation is fenced: an
    // authority-bearing dependency changed.
    return fence_active_authorities(impl, FenceKind::PolicyChanged,
                                    "policy generation changed");
}

Status Engine::set_topology(const TopologySnapshot& topology) {
    if (impl_ == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return Status::failure(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    TopologySnapshot candidate = topology;
    candidate.canonicalise();
    Status valid = candidate.validate();
    if (!valid.ok()) {
        return valid;
    }
    const TopologySnapshot& current = impl.store.state().topology;
    if (candidate.generation < current.generation) {
        Status status = Status::failure(Outcome::Stale, ReasonCode::TopologyGenerationMismatch,
                                        "topology generation regresses");
        status.add(ReasonCode::SequenceRegression, u64_text(candidate.generation.value()));
        return status;
    }
    if (candidate.generation == current.generation) {
        if (candidate.digest() == current.digest()) {
            return Status::success();
        }
        return Status::failure(Outcome::Conflict, ReasonCode::TopologyGenerationMismatch,
                               "same topology generation with different content");
    }
    Status committed = impl.store.commit(
        RecordType::TopologyCommit, encode_topology_payload(candidate),
        [&impl, &candidate](LineageSeq) { impl.store.mutable_state().topology = candidate; });
    if (!committed.ok()) {
        impl.store_failed = true;
        return committed;
    }
    return fence_active_authorities(impl, FenceKind::TopologyChanged,
                                    "topology generation changed");
}

Status Engine::fence_active_authorities(Impl& impl, FenceKind kind, const std::string& detail) {
    const std::vector<AttemptRecord> snapshot = impl.store.state().attempts;
    for (const AttemptRecord& record : snapshot) {
        if (!attempt_phase_holds_authority(record.phase)) {
            continue;
        }
        AttemptRecord updated = record;
        updated.phase = AttemptPhase::Aborted;
        updated.reasons.emplace_back(ReasonCode::Fenced, detail);
        if (updated.reasons.size() > kMaxAttemptReasons) {
            updated.reasons.erase(updated.reasons.begin());
        }
        Status applied = impl.store.commit(
            RecordType::AttemptUpdate, encode_attempt_payload(updated),
            [&impl, &updated](LineageSeq seq) {
                AttemptRecord stored = updated;
                stored.update_seq = seq;
                impl.store.note_attempt(stored);
            });
        if (!applied.ok()) {
            impl.store_failed = true;
            return applied;
        }
        FenceRecord fence;
        fence.kind = kind;
        fence.subject = record.subject;
        fence.subject_generation = record.subject_generation;
        fence.attempt_seq = record.attempt_seq;
        fence.epoch = impl.epoch;
        fence.coordinator = impl.incarnation;
        fence.detail = detail;
        Status fenced = impl.store.commit(
            RecordType::Fence, encode_fence_payload(fence),
            [&impl, &fence](LineageSeq seq) {
                FenceRecord stored = fence;
                stored.seq = seq;
                impl.store.note_fence(stored);
            });
        if (!fenced.ok()) {
            impl.store_failed = true;
            return fenced;
        }
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// Failover
// ---------------------------------------------------------------------------
Result<FailoverDecision> Engine::request_failover(const FailoverRequest& request) {
    auto fail = [](Outcome outcome, ReasonCode code, std::string detail) {
        return Result<FailoverDecision>::failure(Status::failure(outcome, code, std::move(detail)));
    };
    if (impl_ == nullptr) {
        return fail(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return fail(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    if (impl.store_failed) {
        return fail(Outcome::IoError, ReasonCode::InternalError, "durable lineage is unavailable");
    }
    ++impl.tick;

    const DurableState& state = impl.store.state();
    FailoverDecision decision = impl.base_decision(request.subject, request.subject_generation);
    decision.authority = impl.current_authority(request.subject, request.subject_generation);

    auto settle = [&impl, &decision, &request](bool durable_allowed) {
        if (durable_allowed) {
            impl.commit_decision(decision);
            impl.commit_idempotency(decision, request.idempotency_key);
            impl.maybe_checkpoint();
        } else {
            decision.durable = false;
            decision.decision_id = decision.compute_id();
        }
        impl.remember_decision(decision);
        return Result<FailoverDecision>::success(decision);
    };

    // --- structural validation ---------------------------------------------
    if (!(request.subject.fabric == impl.options.fabric)) {
        return fail(Outcome::NotFound, ReasonCode::UnknownFabric,
                    "request names an unknown fabric");
    }
    if (request.subject.link.empty()) {
        return fail(Outcome::Invalid, ReasonCode::NullField, "request must name a subject link");
    }
    if (request.subject_generation.is_zero()) {
        return fail(Outcome::Invalid, ReasonCode::UnknownGeneration,
                    "request must bind the subject link generation");
    }
    if (request.candidates.size() > impl.options.limits.max_candidates_per_request ||
        request.candidates.size() > state.policy.max_candidates) {
        return fail(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                    "candidate set exceeds the configured bound");
    }
    {
        std::vector<LinkKey> seen;
        seen.reserve(request.candidates.size());
        for (const ReplacementCandidate& candidate : request.candidates) {
            if (!(candidate.candidate.fabric == impl.options.fabric)) {
                return fail(Outcome::NotFound, ReasonCode::UnknownFabric,
                            "candidate names an unknown fabric");
            }
            if (candidate.candidate == request.subject) {
                return fail(Outcome::Invalid, ReasonCode::ReplacementIsSubject,
                            "a link cannot be its own replacement");
            }
            if (std::find(seen.begin(), seen.end(), candidate.candidate) != seen.end()) {
                return fail(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "candidate set contains a duplicate link");
            }
            seen.push_back(candidate.candidate);
        }
    }

    // --- idempotent replay --------------------------------------------------
    if (const IdempotencyEntry* replay = impl.store.find_idempotency(request.idempotency_key)) {
        FailoverDecision replayed = impl.replay_decision(*replay);
        return Result<FailoverDecision>::success(replayed);
    }

    // --- subject binding ----------------------------------------------------
    const LinkDefinition* subject_definition = state.topology.find(request.subject);
    if (subject_definition == nullptr) {
        return fail(Outcome::NotFound, ReasonCode::UnknownLink,
                    "subject link is not present in the current topology");
    }
    // Copy the definition: later journal commits may reallocate topology
    // storage, and no pointer into durable state may outlive a commit.
    const LinkDefinition subject_link = *subject_definition;
    if (subject_link.generation != request.subject_generation) {
        Status status = Status::failure(Outcome::Stale, ReasonCode::SubjectGenerationMismatch,
                                        "subject generation is not the current topology generation");
        status.add(ReasonCode::GenerationMismatch,
                   u64_text(subject_link.generation.value()));
        return fail(status.outcome(), status.reasons().front().code,
                    status.reasons().front().detail);
    }
    decision.authority.subject = request.subject;
    decision.authority.subject_generation = subject_link.generation;

    const std::vector<AuthorityMismatch> mismatches =
        compare_authority(request.expected, decision.authority);
    if (!mismatches.empty()) {
        decision.kind = DecisionKind::Indeterminate;
        decision.assertion = Assertion::Observation;
        decision.outcome = Outcome::Stale;
        decision.reasons.emplace_back(ReasonCode::StaleAuthority,
                                      "the request binds authority that is no longer current");
        for (const AuthorityMismatch& mismatch : mismatches) {
            decision.reasons.emplace_back(mismatch.code, mismatch.field);
        }
        return settle(false);
    }
    if (!state.policy.failover_enabled) {
        decision.kind = DecisionKind::Refuse;
        decision.assertion = Assertion::Eligibility;
        decision.outcome = Outcome::Refused;
        decision.reasons.emplace_back(ReasonCode::PolicyDisabled, "failover is disabled by policy");
        // A decision that only restates a durable definition changes nothing
        // that survives restart, so it is not written to the medium.
        return settle(false);
    }

    // --- single winner ------------------------------------------------------
    if (const AttemptRecord* active =
            impl.store.active_authority(request.subject, request.subject_generation)) {
        decision.kind = DecisionKind::Refuse;
        decision.assertion = Assertion::Observation;
        decision.outcome = Outcome::Conflict;
        decision.attempt_seq = active->attempt_seq;
        decision.attempt_id = active->attempt_id;
        decision.reasons.emplace_back(ReasonCode::AlreadyAuthoritative,
                                      "another attempt already holds authority for this generation");
        // The authority is already on the medium. Restating the exclusion does
        // not change what happens next, so it is not durable either.
        return settle(false);
    }

    // --- pre-restart revalidation gate --------------------------------------
    if (const AttemptRecord* latest =
            impl.store.latest_attempt(request.subject, request.subject_generation)) {
        if (latest->phase == AttemptPhase::Interrupted) {
            decision.kind = DecisionKind::Indeterminate;
            decision.assertion = Assertion::Observation;
            decision.outcome = Outcome::Indeterminate;
            decision.attempt_seq = latest->attempt_seq;
            decision.reasons.emplace_back(
                latest->revalidated ? ReasonCode::BackendUnknown : ReasonCode::RevalidationRequired,
                latest->revalidated
                    ? "the interrupted attempt was revalidated as unknown"
                    : "an attempt interrupted by a restart must be revalidated first");
            return settle(true);
        }
    }

    // --- failure evidence ---------------------------------------------------
    const FailureVerdict verdict =
        impl.evaluate_failure(request.subject, request.subject_generation, state.policy,
                              state.topology.generation);
    if (!verdict.established) {
        const std::size_t attempt_count =
            impl.store.count_attempts(request.subject, request.subject_generation);
        const bool within_budget = attempt_count < state.policy.max_attempts_per_generation;
        decision.assertion = Assertion::Observation;
        decision.outcome = verdict.outcome;
        decision.reasons.emplace_back(verdict.reason, verdict.detail);
        for (const Reason& extra : verdict.extra) {
            decision.reasons.push_back(extra);
        }
        if (verdict.outcome == Outcome::Refused) {
            decision.kind = DecisionKind::Refuse;
        } else {
            decision.kind = DecisionKind::Indeterminate;
            if (verdict.outcome == Outcome::Unknown) {
                decision.outcome = Outcome::Unknown;
            }
        }
        decision.attempt_seq = AttemptSeq::from(attempt_count + 1);
        if (within_budget) {
            AttemptRecord record;
            record.subject = request.subject;
            record.subject_generation = request.subject_generation;
            record.attempt_seq = decision.attempt_seq;
            record.attempt_id = compute_attempt_id(impl.options.fabric, request.subject,
                                                   request.subject_generation, decision.attempt_seq);
            decision.attempt_id = record.attempt_id;
            record.idempotency_key = request.idempotency_key;
            record.phase = decision.kind == DecisionKind::Refuse ? AttemptPhase::Refused
                                                                 : AttemptPhase::Indeterminate;
            record.evidence_digest = verdict.evidence_digest;
            record.created_tick = impl.tick;
            record.created_epoch = impl.epoch;
            record.coordinator = impl.incarnation;
            record.reasons = decision.reasons;
            Status recorded = impl.store.commit(
                RecordType::AttemptCreate, encode_attempt_payload(record),
                [&impl, &record](LineageSeq seq) {
                    AttemptRecord stored = record;
                    stored.create_seq = seq;
                    stored.update_seq = seq;
                    impl.store.note_attempt(stored);
                });
            if (!recorded.ok()) {
                impl.store_failed = true;
                return fail(recorded.outcome(), ReasonCode::InternalError,
                            "the attempt record could not be written");
            }
        } else {
            decision.reasons.emplace_back(ReasonCode::AttemptsExhausted,
                                          "the attempt budget for this generation is exhausted");
        }
        return settle(within_budget);
    }

    // --- candidate resolution ----------------------------------------------
    std::vector<ResolvedCandidate> resolved;
    resolved.reserve(request.candidates.size());
    for (const ReplacementCandidate& candidate : request.candidates) {
        ResolvedCandidate entry;
        entry.candidate = candidate.candidate;
        entry.generation_asserted = !candidate.expected_generation.is_zero();

        const LinkDefinition* definition = state.topology.find(candidate.candidate);
        if (definition == nullptr) {
            entry.link_generation = candidate.expected_generation;
            entry.generation_matched = false;
            entry.evidence_present = false;
            resolved.push_back(std::move(entry));
            continue;
        }
        entry.link_generation = definition->generation;
        entry.generation_matched =
            !entry.generation_asserted || candidate.expected_generation == definition->generation;

        const auto bucket = impl.replacement_evidence.find(candidate.candidate);
        if (bucket == impl.replacement_evidence.end()) {
            resolved.push_back(std::move(entry));
            continue;
        }

        const RetainedReplacementEvidence* best = nullptr;
        std::uint64_t best_age = 0;
        bool conflicted = false;
        ObservationState best_state = ObservationState::Unknown;
        for (const auto& item : bucket->second) {
            const RetainedReplacementEvidence& retained = item.second;
            const auto publisher = impl.publishers.find(item.first);
            if (publisher == impl.publishers.end() ||
                publisher->second.incarnation != retained.report.stamp.publisher_incarnation) {
                continue;
            }
            if (retained.report.link_generation != definition->generation ||
                retained.report.topology_generation != state.topology.generation) {
                continue;
            }
            std::uint64_t age = 0;
            const FreshnessClass freshness =
                classify_receipt(retained.receipt_coordinator, retained.receipt_tick,
                                 impl.incarnation, impl.tick, state.policy.evidence_max_age_ticks,
                                 age);
            const bool qualified =
                freshness == FreshnessClass::Current &&
                retained.report.confidence.value() >= state.policy.min_replacement_confidence.value();
            if (!qualified) {
                if (best == nullptr) {
                    best = &retained;
                    best_age = age;
                    best_state = retained.report.state;
                }
                continue;
            }
            if (best == nullptr) {
                best = &retained;
                best_age = age;
                best_state = retained.report.state;
                continue;
            }
            const bool better =
                retained.report.confidence.value() > best->report.confidence.value() ||
                (retained.report.confidence.value() == best->report.confidence.value() &&
                 (age < best_age ||
                  (age == best_age && item.first < best->report.stamp.publisher)));
            if (better) {
                best = &retained;
                best_age = age;
                best_state = retained.report.state;
            }
        }
        if (best == nullptr) {
            resolved.push_back(std::move(entry));
            continue;
        }
        entry.evidence_present = true;
        entry.report = best->report;
        entry.evidence_digest = best->digest;
        entry.age_ticks = best_age;
        entry.freshness = classify_receipt(best->receipt_coordinator, best->receipt_tick,
                                           impl.incarnation, impl.tick,
                                           state.policy.evidence_max_age_ticks, entry.age_ticks);
        // A current, qualified observation that contradicts the chosen one turns
        // the candidate into an undecided one rather than a usable one.
        for (const auto& item : bucket->second) {
            const RetainedReplacementEvidence& retained = item.second;
            if (&retained == best) {
                continue;
            }
            const auto publisher = impl.publishers.find(item.first);
            if (publisher == impl.publishers.end() ||
                publisher->second.incarnation != retained.report.stamp.publisher_incarnation) {
                continue;
            }
            if (retained.report.link_generation != definition->generation ||
                retained.report.topology_generation != state.topology.generation) {
                continue;
            }
            std::uint64_t age = 0;
            const FreshnessClass freshness =
                classify_receipt(retained.receipt_coordinator, retained.receipt_tick,
                                 impl.incarnation, impl.tick, state.policy.evidence_max_age_ticks,
                                 age);
            if (freshness != FreshnessClass::Current ||
                retained.report.confidence.value() <
                    state.policy.min_replacement_confidence.value()) {
                continue;
            }
            if (retained.report.state != best_state) {
                conflicted = true;
                break;
            }
        }
        entry.report.state = best_state;
        entry.conflicted = conflicted;
        resolved.push_back(std::move(entry));
    }

    // Canonical order: the outcome must not depend on the order the caller
    // supplied its candidates in.
    std::sort(resolved.begin(), resolved.end(),
              [](const ResolvedCandidate& a, const ResolvedCandidate& b) {
                  return a.candidate < b.candidate;
              });

    SelectionInput selection;
    selection.fabric = impl.options.fabric;
    selection.subject = request.subject;
    selection.subject_generation = subject_link.generation;
    selection.topology_generation = state.topology.generation;
    selection.policy_generation = state.policy.generation;
    selection.policy = state.policy;
    selection.epoch = impl.epoch;
    selection.coordinator = impl.incarnation;
    selection.candidates = std::move(resolved);
    selection.min_replacement_capacity = state.policy.min_replacement_capacity;
    if (state.policy.require_subject_capacity &&
        subject_link.capacity_units > selection.min_replacement_capacity) {
        selection.min_replacement_capacity = subject_link.capacity_units;
    }

    const SelectionOutcome outcome = select_replacement(selection);
    const Status validated = validate_selection(selection, outcome);
    if (!validated.ok()) {
        return fail(Outcome::Indeterminate, ReasonCode::InternalError,
                    "internal selection validation failed; no authority was created");
    }
    decision.candidates = outcome.assessments;
    decision.evidence_digest = verdict.evidence_digest;

    const std::size_t attempt_count =
        impl.store.count_attempts(request.subject, request.subject_generation);
    const bool within_budget = attempt_count < state.policy.max_attempts_per_generation;

    if (outcome.status != SelectionStatus::Selected) {
        decision.assertion = Assertion::Eligibility;
        decision.attempt_seq = AttemptSeq::from(attempt_count + 1);
        decision.attempt_id = compute_attempt_id(impl.options.fabric, request.subject,
                                                 request.subject_generation, decision.attempt_seq);
        switch (outcome.status) {
            case SelectionStatus::ProvenInfeasible:
                decision.kind = DecisionKind::Refuse;
                decision.outcome = Outcome::Refused;
                decision.reasons.emplace_back(ReasonCode::CertifiedInfeasible,
                                              "every supplied alternate was conclusively disproven");
                decision.reasons.emplace_back(ReasonCode::NoEligibleReplacement,
                                              "no supplied alternate is eligible");
                break;
            case SelectionStatus::EmptyCandidateSet:
                decision.kind = DecisionKind::Refuse;
                decision.outcome = Outcome::Refused;
                decision.reasons.emplace_back(ReasonCode::CandidateSetEmpty,
                                              "no alternate was supplied");
                break;
            case SelectionStatus::Indeterminate:
            default:
                decision.kind = DecisionKind::Indeterminate;
                decision.outcome = Outcome::Indeterminate;
                decision.reasons.emplace_back(
                    ReasonCode::SelectionIndeterminate,
                    "at least one supplied alternate could not be decided");
                break;
        }
        if (within_budget) {
            AttemptRecord record;
            record.subject = request.subject;
            record.subject_generation = request.subject_generation;
            record.attempt_seq = decision.attempt_seq;
            record.attempt_id = decision.attempt_id;
            record.idempotency_key = request.idempotency_key;
            record.phase = decision.kind == DecisionKind::Refuse ? AttemptPhase::Refused
                                                                 : AttemptPhase::Indeterminate;
            record.evidence_digest = decision.evidence_digest;
            record.created_tick = impl.tick;
            record.created_epoch = impl.epoch;
            record.coordinator = impl.incarnation;
            record.reasons = decision.reasons;
            Status recorded = impl.store.commit(
                RecordType::AttemptCreate, encode_attempt_payload(record),
                [&impl, &record](LineageSeq seq) {
                    AttemptRecord stored = record;
                    stored.create_seq = seq;
                    stored.update_seq = seq;
                    impl.store.note_attempt(stored);
                });
            if (!recorded.ok()) {
                impl.store_failed = true;
                return fail(recorded.outcome(), ReasonCode::InternalError,
                            "the attempt record could not be written");
            }
        } else {
            decision.reasons.emplace_back(ReasonCode::AttemptsExhausted,
                                          "the attempt budget for this generation is exhausted");
        }
        return settle(within_budget);
    }

    if (!within_budget) {
        decision.kind = DecisionKind::Refuse;
        decision.assertion = Assertion::Eligibility;
        decision.outcome = Outcome::Refused;
        decision.reasons.emplace_back(ReasonCode::AttemptsExhausted,
                                      "the attempt budget for this generation is exhausted; no "
                                      "authority was created");
        return settle(false);
    }

    // --- grant --------------------------------------------------------------
    const CandidateAssessment& chosen = outcome.assessments[*outcome.selected_index];
    const ResolvedCandidate* chosen_resolved = nullptr;
    for (const ResolvedCandidate& entry : selection.candidates) {
        if (entry.candidate == chosen.candidate && entry.link_generation == chosen.link_generation) {
            chosen_resolved = &entry;
            break;
        }
    }
    if (chosen_resolved == nullptr) {
        return fail(Outcome::Indeterminate, ReasonCode::InternalError,
                    "internal selection bookkeeping failed; no authority was created");
    }

    std::uint64_t next_sequence = 0;
    if (!checked_add_u64(static_cast<std::uint64_t>(attempt_count), 1, next_sequence)) {
        return fail(Outcome::Exhausted, ReasonCode::CheckedArithmeticOverflow,
                    "attempt sequence space is exhausted");
    }

    decision.kind = DecisionKind::Grant;
    decision.assertion = Assertion::Authorization;
    decision.outcome = Outcome::Ok;
    decision.has_replacement = true;
    decision.replacement = chosen.candidate;
    decision.replacement_generation = chosen.link_generation;
    decision.attempt_seq = AttemptSeq::from(next_sequence);
    decision.attempt_id = compute_attempt_id(impl.options.fabric, request.subject,
                                             subject_link.generation, decision.attempt_seq);
    decision.reasons.emplace_back(ReasonCode::SelectedHighestObjective,
                                  "the highest-ranked eligible alternate was selected");
    decision.reasons.emplace_back(ReasonCode::Fenced,
                                  "prior authority for this generation is excluded");

    AuthorityVector authority =
        impl.current_authority(request.subject, subject_link.generation);
    authority.replacement = chosen.candidate;
    authority.replacement_generation = chosen.link_generation;
    authority.evidence_publisher = verdict.publisher;
    authority.publisher_incarnation = verdict.publisher_incarnation;
    authority.publisher_seq = verdict.publisher_seq;
    authority.attempt_seq = decision.attempt_seq;
    authority.evidence_digest = verdict.evidence_digest;
    decision.authority = authority;

    AttemptRecord attempt;
    attempt.subject = request.subject;
    attempt.subject_generation = subject_link.generation;
    attempt.attempt_seq = decision.attempt_seq;
    attempt.attempt_id = decision.attempt_id;
    attempt.idempotency_key = request.idempotency_key;
    attempt.phase = AttemptPhase::Evaluated;
    attempt.replacement = chosen.candidate;
    attempt.replacement_generation = chosen.link_generation;
    attempt.authority = authority;
    attempt.evidence_digest = verdict.evidence_digest;
    attempt.created_tick = impl.tick;
    attempt.created_epoch = impl.epoch;
    attempt.coordinator = impl.incarnation;
    attempt.reasons = decision.reasons;
    {
        const AttemptRecord* previous =
            impl.store.latest_attempt(request.subject, subject_link.generation);
        if (previous != nullptr && attempt_phase_holds_authority(previous->phase)) {
            attempt.predecessor = previous->attempt_id;
        }
    }

    detail::hit_crash_point(detail::CrashPoint::BeforeCommit);

    Status staged = impl.store.commit(
        RecordType::AttemptCreate, encode_attempt_payload(attempt),
        [&impl, &attempt](LineageSeq seq) {
            AttemptRecord stored = attempt;
            stored.create_seq = seq;
            stored.update_seq = seq;
            impl.store.note_attempt(stored);
        });
    if (!staged.ok()) {
        impl.store_failed = true;
        return fail(staged.outcome(), ReasonCode::InternalError,
                    "the attempt intent could not be written; no authority was created");
    }

    attempt.phase = AttemptPhase::Authorized;
    Status authorized = impl.store.commit(
        RecordType::AttemptUpdate, encode_attempt_payload(attempt),
        [&impl, &attempt](LineageSeq seq) {
            AttemptRecord stored = attempt;
            stored.update_seq = seq;
            impl.store.note_attempt(stored);
        });
    if (!authorized.ok()) {
        impl.store_failed = true;
        return fail(authorized.outcome(), ReasonCode::InternalError,
                    "the grant could not be written; no authority was created");
    }

    impl.commit_decision(decision);
    impl.commit_idempotency(decision, request.idempotency_key);
    impl.maybe_checkpoint();
    impl.remember_decision(decision);

    detail::hit_crash_point(detail::CrashPoint::AfterCommit);
    return Result<FailoverDecision>::success(decision);
}

// ---------------------------------------------------------------------------
// Application and verification
// ---------------------------------------------------------------------------
Result<FailoverDecision> Engine::record_application(const ApplicationReport& report) {
    auto fail = [](Outcome outcome, ReasonCode code, std::string detail) {
        return Result<FailoverDecision>::failure(Status::failure(outcome, code, std::move(detail)));
    };
    if (impl_ == nullptr) {
        return fail(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return fail(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    if (impl.store_failed) {
        return fail(Outcome::IoError, ReasonCode::InternalError, "durable lineage is unavailable");
    }
    ++impl.tick;

    const AttemptRecord* found = impl.store.find_attempt(report.subject, report.attempt_seq);
    if (found == nullptr) {
        return fail(Outcome::NotFound, ReasonCode::AttemptUnknown,
                    "no attempt with that identity exists");
    }
    if (found->subject_generation != report.subject_generation) {
        return fail(Outcome::Stale, ReasonCode::SubjectGenerationMismatch,
                    "the report binds a different subject generation");
    }
    if (found->attempt_id != report.attempt_id) {
        return fail(Outcome::Conflict, ReasonCode::AttemptUnknown,
                    "the report does not carry the attempt identity it claims");
    }
    if (report.applier_incarnation.is_zero()) {
        return fail(Outcome::Invalid, ReasonCode::WorkerIncarnationMismatch,
                    "an application report must name the applier incarnation");
    }
    FailoverDecision decision = impl.base_decision(found->subject, found->subject_generation);
    decision.attempt_seq = found->attempt_seq;
    decision.attempt_id = found->attempt_id;
    decision.authority = found->authority;
    decision.has_replacement = !found->replacement.empty();
    decision.replacement = found->replacement;
    decision.replacement_generation = found->replacement_generation;
    decision.kind = DecisionKind::Acknowledge;
    decision.assertion = Assertion::Acknowledgement;

    auto settle_undurable = [&impl, &decision]() {
        decision.durable = false;
        decision.decision_id = decision.compute_id();
        impl.remember_decision(decision);
        return Result<FailoverDecision>::success(decision);
    };

    if (attempt_phase_is_terminal(found->phase)) {
        decision.outcome = Outcome::Conflict;
        decision.reasons.emplace_back(
            found->phase == AttemptPhase::Committed || found->phase == AttemptPhase::RolledBack
                ? ReasonCode::LateAcknowledgement
                : ReasonCode::AttemptCompleted,
            "the attempt is already terminal");
        return settle_undurable();
    }
    if (found->phase == AttemptPhase::Evaluated) {
        decision.outcome = Outcome::Conflict;
        decision.reasons.emplace_back(ReasonCode::AttemptUnknown,
                                      "the attempt never became authoritative");
        return settle_undurable();
    }
    if (found->phase == AttemptPhase::Acknowledged || found->phase == AttemptPhase::Verified) {
        decision.outcome = Outcome::Conflict;
        decision.reasons.emplace_back(ReasonCode::DuplicateCompletion,
                                      "an application report was already accepted");
        return settle_undurable();
    }

    AttemptRecord updated = *found;
    if (!report.accepted) {
        updated.phase = AttemptPhase::Aborted;
        updated.reasons.emplace_back(ReasonCode::BackendRejected, report.detail);
        decision.outcome = Outcome::Refused;
        decision.reasons.emplace_back(ReasonCode::BackendRejected,
                                      "the applier refused the directive");
    } else {
        updated.applier_incarnation = report.applier_incarnation;
        updated.phase = impl.store.state().policy.require_verified_effect
                            ? AttemptPhase::Acknowledged
                            : AttemptPhase::Committed;
        updated.reasons.emplace_back(ReasonCode::AcknowledgementNotEffect,
                                     "an acknowledgement is not a verified effect");
        decision.outcome = Outcome::Ok;
        decision.reasons.emplace_back(ReasonCode::AcknowledgementNotEffect,
                                      "acceptance recorded; the effect is not yet verified");
    }
    if (updated.reasons.size() > kMaxAttemptReasons) {
        updated.reasons.erase(updated.reasons.begin());
    }

    const RecordType type = updated.phase == AttemptPhase::Committed ? RecordType::Completion
                                                                    : RecordType::AttemptUpdate;
    Status written = impl.store.commit(
        type, encode_attempt_payload(updated), [&impl, &updated](LineageSeq seq) {
            AttemptRecord stored = updated;
            stored.update_seq = seq;
            impl.store.note_attempt(stored);
        });
    if (!written.ok()) {
        impl.store_failed = true;
        return fail(written.outcome(), ReasonCode::InternalError,
                    "the application report could not be recorded");
    }

    if (updated.phase == AttemptPhase::Acknowledged) {
        detail::hit_crash_point(detail::CrashPoint::AfterApply);
    }
    impl.commit_decision(decision);
    impl.maybe_checkpoint();
    impl.remember_decision(decision);
    return Result<FailoverDecision>::success(decision);
}

Result<FailoverDecision> Engine::record_verification(const VerificationReport& report) {
    auto fail = [](Outcome outcome, ReasonCode code, std::string detail) {
        return Result<FailoverDecision>::failure(Status::failure(outcome, code, std::move(detail)));
    };
    if (impl_ == nullptr) {
        return fail(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return fail(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    if (impl.store_failed) {
        return fail(Outcome::IoError, ReasonCode::InternalError, "durable lineage is unavailable");
    }
    ++impl.tick;

    const AttemptRecord* found = impl.store.find_attempt(report.subject, report.attempt_seq);
    if (found == nullptr) {
        return fail(Outcome::NotFound, ReasonCode::AttemptUnknown,
                    "no attempt with that identity exists");
    }
    if (found->attempt_id != report.attempt_id) {
        return fail(Outcome::Conflict, ReasonCode::AttemptUnknown,
                    "the report does not carry the attempt identity it claims");
    }
    FailoverDecision decision = impl.base_decision(found->subject, found->subject_generation);
    decision.attempt_seq = found->attempt_seq;
    decision.attempt_id = found->attempt_id;
    decision.authority = found->authority;
    decision.has_replacement = !found->replacement.empty();
    decision.replacement = found->replacement;
    decision.replacement_generation = found->replacement_generation;
    decision.kind = DecisionKind::Verify;

    auto settle_undurable = [&impl, &decision]() {
        decision.durable = false;
        decision.decision_id = decision.compute_id();
        impl.remember_decision(decision);
        return Result<FailoverDecision>::success(decision);
    };

    if (found->phase == AttemptPhase::Committed || found->phase == AttemptPhase::Verified) {
        decision.assertion = Assertion::VerifiedEffect;
        decision.outcome = Outcome::Conflict;
        decision.reasons.emplace_back(ReasonCode::DuplicateCompletion,
                                      "the effect was already verified");
        return settle_undurable();
    }
    if (found->phase != AttemptPhase::Acknowledged) {
        decision.assertion = Assertion::Observation;
        decision.outcome = Outcome::Conflict;
        decision.reasons.emplace_back(ReasonCode::LateAcknowledgement,
                                      "verification requires an acknowledged attempt");
        return settle_undurable();
    }

    if (!report.effect_observed) {
        AttemptRecord updated = *found;
        updated.phase = AttemptPhase::Indeterminate;
        updated.reasons.emplace_back(ReasonCode::EffectUnverified, report.detail);
        if (updated.reasons.size() > kMaxAttemptReasons) {
            updated.reasons.erase(updated.reasons.begin());
        }
        Status written = impl.store.commit(
            RecordType::AttemptUpdate, encode_attempt_payload(updated),
            [&impl, &updated](LineageSeq seq) {
                AttemptRecord stored = updated;
                stored.update_seq = seq;
                impl.store.note_attempt(stored);
            });
        if (!written.ok()) {
            impl.store_failed = true;
            return fail(written.outcome(), ReasonCode::InternalError,
                        "the verification report could not be recorded");
        }
        decision.assertion = Assertion::Acknowledgement;
        decision.outcome = Outcome::Indeterminate;
        decision.reasons.emplace_back(ReasonCode::EffectUnverified,
                                      "the effect could not be observed");
        impl.commit_decision(decision);
        impl.remember_decision(decision);
        return Result<FailoverDecision>::success(decision);
    }

    AttemptRecord verified = *found;
    verified.phase = AttemptPhase::Verified;
    verified.reasons.emplace_back(ReasonCode::EffectVerified, report.detail);
    if (verified.reasons.size() > kMaxAttemptReasons) {
        verified.reasons.erase(verified.reasons.begin());
    }
    Status written = impl.store.commit(
        RecordType::AttemptUpdate, encode_attempt_payload(verified),
        [&impl, &verified](LineageSeq seq) {
            AttemptRecord stored = verified;
            stored.update_seq = seq;
            impl.store.note_attempt(stored);
        });
    if (!written.ok()) {
        impl.store_failed = true;
        return fail(written.outcome(), ReasonCode::InternalError,
                    "the verification report could not be recorded");
    }

    detail::hit_crash_point(detail::CrashPoint::BeforeCompletion);

    AttemptRecord committed = verified;
    committed.phase = AttemptPhase::Committed;
    Status completed = impl.store.commit(
        RecordType::Completion, encode_attempt_payload(committed),
        [&impl, &committed](LineageSeq seq) {
            AttemptRecord stored = committed;
            stored.update_seq = seq;
            impl.store.note_attempt(stored);
        });
    if (!completed.ok()) {
        impl.store_failed = true;
        return fail(completed.outcome(), ReasonCode::InternalError,
                    "the completion record could not be written");
    }

    decision.assertion = Assertion::VerifiedEffect;
    decision.outcome = Outcome::Ok;
    decision.reasons.emplace_back(ReasonCode::EffectVerified,
                                  "the replacement effect was independently observed");
    impl.commit_decision(decision);
    impl.maybe_checkpoint();
    impl.remember_decision(decision);
    return Result<FailoverDecision>::success(decision);
}

// ---------------------------------------------------------------------------
// Rollback and revalidation
// ---------------------------------------------------------------------------
Result<FailoverDecision> Engine::request_rollback(const RollbackRequest& request) {
    auto fail = [](Outcome outcome, ReasonCode code, std::string detail) {
        return Result<FailoverDecision>::failure(Status::failure(outcome, code, std::move(detail)));
    };
    if (impl_ == nullptr) {
        return fail(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return fail(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    ++impl.tick;

    if (const IdempotencyEntry* replay = impl.store.find_idempotency(request.idempotency_key)) {
        return Result<FailoverDecision>::success(impl.replay_decision(*replay));
    }

    const AttemptRecord* found = impl.store.find_attempt(request.subject, request.attempt_seq);
    if (found == nullptr) {
        return fail(Outcome::NotFound, ReasonCode::AttemptUnknown,
                    "no attempt with that identity exists");
    }
    FailoverDecision decision = impl.base_decision(found->subject, found->subject_generation);
    decision.attempt_seq = found->attempt_seq;
    decision.attempt_id = found->attempt_id;
    decision.authority = found->authority;
    decision.has_replacement = !found->replacement.empty();
    decision.replacement = found->replacement;
    decision.replacement_generation = found->replacement_generation;
    decision.kind = DecisionKind::Rollback;

    auto settle_undurable = [&impl, &decision]() {
        decision.durable = false;
        decision.decision_id = decision.compute_id();
        impl.remember_decision(decision);
        return Result<FailoverDecision>::success(decision);
    };

    const std::vector<AuthorityMismatch> mismatches = compare_authority(
        request.expected, impl.current_authority(request.subject, found->subject_generation));
    if (!mismatches.empty()) {
        decision.assertion = Assertion::Observation;
        decision.outcome = Outcome::Stale;
        decision.reasons.emplace_back(ReasonCode::StaleAuthority,
                                      "the rollback binds authority that is no longer current");
        for (const AuthorityMismatch& mismatch : mismatches) {
            decision.reasons.emplace_back(mismatch.code, mismatch.field);
        }
        return settle_undurable();
    }

    if (found->phase == AttemptPhase::RolledBack) {
        decision.assertion = Assertion::Authorization;
        decision.outcome = Outcome::Ok;
        decision.idempotent_replay = true;
        decision.reasons.emplace_back(ReasonCode::IdempotentReplay,
                                      "the attempt was already rolled back");
        return settle_undurable();
    }
    if (!attempt_phase_holds_authority(found->phase)) {
        decision.assertion = Assertion::Observation;
        decision.outcome = Outcome::Conflict;
        decision.reasons.emplace_back(ReasonCode::RollbackNotAuthorized,
                                      "the attempt does not hold authority to revert");
        return settle_undurable();
    }

    // Copy everything that is needed after the commits: `found` points into the
    // attempt table, which a commit may reallocate.
    const LinkKey subject = found->subject;
    const LinkGeneration subject_generation = found->subject_generation;
    const AttemptSeq attempt_seq = found->attempt_seq;

    AttemptRecord updated = *found;
    updated.phase = AttemptPhase::RolledBack;
    updated.reasons.emplace_back(ReasonCode::RollbackRequested, request.rationale);
    if (updated.reasons.size() > kMaxAttemptReasons) {
        updated.reasons.erase(updated.reasons.begin());
    }
    Status written = impl.store.commit(
        RecordType::Rollback, encode_attempt_payload(updated),
        [&impl, &updated](LineageSeq seq) {
            AttemptRecord stored = updated;
            stored.update_seq = seq;
            impl.store.note_attempt(stored);
        });
    if (!written.ok()) {
        impl.store_failed = true;
        return fail(written.outcome(), ReasonCode::InternalError,
                    "the rollback could not be recorded");
    }

    FenceRecord fence;
    fence.kind = FenceKind::RollbackRevoked;
    fence.subject = subject;
    fence.subject_generation = subject_generation;
    fence.attempt_seq = attempt_seq;
    fence.epoch = impl.epoch;
    fence.coordinator = impl.incarnation;
    fence.detail = "failover authority revoked by rollback";
    Status fenced = impl.store.commit(
        RecordType::Fence, encode_fence_payload(fence), [&impl, &fence](LineageSeq seq) {
            FenceRecord stored = fence;
            stored.seq = seq;
            impl.store.note_fence(stored);
        });
    if (!fenced.ok()) {
        impl.store_failed = true;
        return fail(fenced.outcome(), ReasonCode::InternalError,
                    "the rollback fence could not be written");
    }

    decision.assertion = Assertion::Authorization;
    decision.outcome = Outcome::Ok;
    decision.reasons.emplace_back(ReasonCode::RollbackCompleted,
                                  "failover authority was revoked");
    decision.reasons.emplace_back(
        ReasonCode::RestoreRequiresNewGeneration,
        "restoring the original link is a new generation-bound transition, not a resurrection");
    impl.commit_decision(decision);
    impl.commit_idempotency(decision, request.idempotency_key);
    impl.remember_decision(decision);
    return Result<FailoverDecision>::success(decision);
}

Result<FailoverDecision> Engine::resolve_interrupted(const RevalidationRequest& request) {
    auto fail = [](Outcome outcome, ReasonCode code, std::string detail) {
        return Result<FailoverDecision>::failure(Status::failure(outcome, code, std::move(detail)));
    };
    if (impl_ == nullptr) {
        return fail(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return fail(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    if (request.observer_incarnation.is_zero()) {
        return fail(Outcome::Invalid, ReasonCode::WorkerIncarnationMismatch,
                    "revalidation must name the observing incarnation");
    }
    if (request.observer_epoch != impl.epoch) {
        return fail(Outcome::Stale, ReasonCode::EpochMismatch,
                    "revalidation was produced for a different coordinator epoch");
    }
    ++impl.tick;

    const AttemptRecord* found = impl.store.find_attempt(request.subject, request.attempt_seq);
    if (found == nullptr) {
        return fail(Outcome::NotFound, ReasonCode::AttemptUnknown,
                    "no attempt with that identity exists");
    }
    FailoverDecision decision = impl.base_decision(found->subject, found->subject_generation);
    decision.attempt_seq = found->attempt_seq;
    decision.attempt_id = found->attempt_id;
    decision.authority = found->authority;
    decision.has_replacement = !found->replacement.empty();
    decision.replacement = found->replacement;
    decision.replacement_generation = found->replacement_generation;
    decision.kind = DecisionKind::Revalidate;
    decision.assertion = Assertion::Observation;

    auto settle_undurable = [&impl, &decision]() {
        decision.durable = false;
        decision.decision_id = decision.compute_id();
        impl.remember_decision(decision);
        return Result<FailoverDecision>::success(decision);
    };

    if (found->revalidated) {
        switch (found->revalidation) {
            case RevalidationVerdict::NotApplied:
                decision.outcome = Outcome::Ok;
                break;
            case RevalidationVerdict::Applied:
                decision.outcome = Outcome::Ok;
                decision.assertion = Assertion::Authorization;
                break;
            case RevalidationVerdict::Unknown:
            default:
                decision.outcome = Outcome::Indeterminate;
                break;
        }
        decision.idempotent_replay = true;
        decision.reasons.emplace_back(ReasonCode::IdempotentReplay,
                                      "the attempt was already revalidated");
        return settle_undurable();
    }
    if (found->phase != AttemptPhase::Interrupted) {
        decision.outcome = Outcome::Conflict;
        decision.reasons.emplace_back(ReasonCode::AttemptCompleted,
                                      "the attempt is not interrupted and was never revalidated");
        return settle_undurable();
    }

    AttemptRecord updated = *found;
    updated.revalidated = true;
    updated.revalidation = request.verdict;
    switch (request.verdict) {
        case RevalidationVerdict::NotApplied:
            updated.phase = AttemptPhase::Aborted;
            updated.reasons.emplace_back(ReasonCode::Fenced,
                                         "revalidation proved that no effect was applied");
            decision.outcome = Outcome::Ok;
            decision.reasons.emplace_back(ReasonCode::Fenced,
                                          "the interrupted authority is released");
            break;
        case RevalidationVerdict::Applied: {
            updated.phase = AttemptPhase::Committed;
            updated.reasons.emplace_back(ReasonCode::EffectVerified,
                                         "revalidation observed the replacement in effect");
            decision.outcome = Outcome::Ok;
            decision.assertion = Assertion::Authorization;
            decision.reasons.emplace_back(
                ReasonCode::EffectVerified,
                "the replacement is in effect and is now bound to the current epoch");
            // The authority is re-bound to the current coordinator, epoch and
            // generations. Without this the replacement would be observed as
            // effective while still being excluded from the single-winner gate,
            // and a second attempt could be granted for the same generation.
            const DurableState& state = impl.store.state();
            if (updated.authority.policy_generation != state.policy.generation) {
                decision.reasons.emplace_back(
                    ReasonCode::PolicyGenerationMismatch,
                    "the authority is re-bound under the current policy generation");
            }
            if (updated.authority.topology_generation != state.topology.generation) {
                decision.reasons.emplace_back(
                    ReasonCode::TopologyGenerationMismatch,
                    "the authority is re-bound under the current topology generation");
            }
            updated.authority.policy_generation = state.policy.generation;
            updated.authority.policy_digest = state.policy.digest();
            updated.authority.topology_generation = state.topology.generation;
            updated.authority.topology_digest = state.topology.digest();
            updated.authority.epoch = impl.epoch;
            updated.authority.coordinator = impl.incarnation;
            updated.coordinator = impl.incarnation;
            decision.authority = updated.authority;
            break;
        }
        case RevalidationVerdict::Unknown:
        default:
            updated.reasons.emplace_back(
                ReasonCode::BackendUnknown,
                "revalidation could not determine whether an effect exists");
            decision.outcome = Outcome::Indeterminate;
            decision.reasons.emplace_back(
                ReasonCode::BackendUnknown,
                "the interrupted attempt remains indeterminate and blocks new authority");
            break;
    }
    if (updated.reasons.size() > kMaxAttemptReasons) {
        updated.reasons.erase(updated.reasons.begin());
    }
    decision.reasons.emplace_back(ReasonCode::DurableLineagePreserved,
                                  "the pre-restart record is retained as history");
    decision.reasons.emplace_back(ReasonCode::DynamicStateNotRestored,
                                  "no pre-restart dynamic authority was restored");

    const RecordType type = updated.phase == AttemptPhase::Committed ? RecordType::Completion
                                                                    : RecordType::AttemptUpdate;
    Status written = impl.store.commit(
        type, encode_attempt_payload(updated), [&impl, &updated](LineageSeq seq) {
            AttemptRecord stored = updated;
            stored.update_seq = seq;
            impl.store.note_attempt(stored);
        });
    if (!written.ok()) {
        impl.store_failed = true;
        return fail(written.outcome(), ReasonCode::InternalError,
                    "the revalidation could not be recorded");
    }
    impl.commit_decision(decision);
    impl.remember_decision(decision);
    return Result<FailoverDecision>::success(decision);
}

// ---------------------------------------------------------------------------
// Applier boundary
// ---------------------------------------------------------------------------
std::vector<ApplyDirective> Engine::pending_directives(std::size_t max_count) const {
    std::vector<ApplyDirective> directives;
    if (impl_ == nullptr) {
        return directives;
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return directives;
    }
    std::vector<const AttemptRecord*> ordered;
    for (const AttemptRecord& record : impl.store.state().attempts) {
        if (record.phase == AttemptPhase::Authorized && !record.replacement.empty()) {
            ordered.push_back(&record);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const AttemptRecord* a, const AttemptRecord* b) {
        if (a->subject < b->subject) return true;
        if (b->subject < a->subject) return false;
        return a->attempt_seq < b->attempt_seq;
    });
    for (const AttemptRecord* record : ordered) {
        if (directives.size() >= max_count ||
            directives.size() >= impl.options.limits.max_pending_directives) {
            break;
        }
        ApplyDirective directive;
        directive.subject = record->subject;
        directive.subject_generation = record->subject_generation;
        directive.replacement = record->replacement;
        directive.replacement_generation = record->replacement_generation;
        directive.attempt_seq = record->attempt_seq;
        directive.attempt_id = record->attempt_id;
        directive.authority = record->authority;
        directive.issued_tick = impl.tick;
        directives.push_back(std::move(directive));
    }
    return directives;
}

bool Engine::take_directive(const LinkKey& subject, LinkGeneration generation, AttemptSeq seq,
                            ApplyDirective& out) const {
    if (impl_ == nullptr) {
        return false;
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return false;
    }
    const AttemptRecord* record = impl.store.find_attempt(subject, seq);
    if (record == nullptr || record->subject_generation != generation) {
        return false;
    }
    if (record->phase != AttemptPhase::Authorized || record->replacement.empty()) {
        return false;
    }
    ApplyDirective directive;
    directive.subject = record->subject;
    directive.subject_generation = record->subject_generation;
    directive.replacement = record->replacement;
    directive.replacement_generation = record->replacement_generation;
    directive.attempt_seq = record->attempt_seq;
    directive.attempt_id = record->attempt_id;
    directive.authority = record->authority;
    directive.issued_tick = impl.tick;
    out = std::move(directive);
    return true;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------
EngineView Engine::inspect(std::size_t history_limit) const {
    EngineView view;
    if (impl_ == nullptr) {
        return view;
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    const DurableState& state = impl.store.state();
    view.fabric = impl.options.fabric;
    view.epoch = impl.epoch;
    view.coordinator = impl.incarnation;
    view.policy_generation = state.policy.generation;
    view.topology_generation = state.topology.generation;
    view.policy_digest = state.policy.digest();
    view.topology_digest = state.topology.digest();
    view.boot_count = state.boot_count;
    view.last_seq = state.last_seq;
    view.tracked_publishers = impl.publishers.size();
    view.evidence_entries = impl.evidence_entries;
    view.retained_attempts = state.attempts.size();
    view.retained_fences = state.fences.size();
    view.retained_decisions = impl.decisions.size();
    view.store_failed = impl.store_failed || impl.store.failed();
    for (const AttemptRecord& record : state.attempts) {
        if (attempt_phase_holds_authority(record.phase) &&
            record.authority.epoch.value() == impl.epoch.value() &&
            record.authority.coordinator == impl.incarnation) {
            ++view.active_authorities;
        }
        if (record.phase == AttemptPhase::Interrupted) {
            ++view.interrupted_attempts;
        }
        if (record.phase == AttemptPhase::Authorized) {
            ++view.pending_directives;
        }
    }
    if (history_limit > 0) {
        std::vector<AttemptRecord> attempts = state.attempts;
        std::sort(attempts.begin(), attempts.end(),
                  [](const AttemptRecord& a, const AttemptRecord& b) {
                      if (a.subject < b.subject) return true;
                      if (b.subject < a.subject) return false;
                      return a.attempt_seq > b.attempt_seq;
                  });
        for (std::size_t i = 0; i < attempts.size() && i < history_limit; ++i) {
            view.recent_attempts.push_back(attempts[i]);
        }
        std::vector<FenceRecord> fences = state.fences;
        std::sort(fences.begin(), fences.end(),
                  [](const FenceRecord& a, const FenceRecord& b) { return a.seq > b.seq; });
        for (std::size_t i = 0; i < fences.size() && i < history_limit; ++i) {
            view.recent_fences.push_back(fences[i]);
        }
    }
    return view;
}

Epoch Engine::epoch() const noexcept {
    if (impl_ == nullptr) {
        return Epoch{};
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    return impl.epoch;
}

Incarnation Engine::incarnation() const noexcept {
    if (impl_ == nullptr) {
        return Incarnation{};
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    return impl.incarnation;
}

const FabricName& Engine::fabric() const noexcept {
    static const FabricName kEmpty;
    if (impl_ == nullptr) {
        return kEmpty;
    }
    return impl_->options.fabric;
}

FailoverPolicy Engine::policy() const {
    if (impl_ == nullptr) {
        return FailoverPolicy{};
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    return impl.store.state().policy;
}

TopologySnapshot Engine::topology() const {
    if (impl_ == nullptr) {
        return TopologySnapshot{};
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    return impl.store.state().topology;
}

Status Engine::checkpoint() {
    if (impl_ == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed, "engine is not open");
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> guard(impl.mu);
    if (impl.closed) {
        return Status::failure(Outcome::Closed, ReasonCode::Shutdown, "engine is shutting down");
    }
    const Result<LineageSeq> result = impl.store.checkpoint();
    if (!result.ok()) {
        impl.store_failed = true;
        return result.status();
    }
    return Status::success();
}

std::string engine_view_to_string(const EngineView& view) {
    std::ostringstream out;
    out << "fabric=" << view.fabric.value() << "\n";
    out << "epoch=" << view.epoch.value() << "\n";
    out << "coordinator=" << view.coordinator.hex() << "\n";
    out << "boot_count=" << view.boot_count << "\n";
    out << "policy_generation=" << view.policy_generation.value() << "\n";
    out << "topology_generation=" << view.topology_generation.value() << "\n";
    out << "policy_digest=" << view.policy_digest.hex() << "\n";
    out << "topology_digest=" << view.topology_digest.hex() << "\n";
    out << "last_lineage_seq=" << view.last_seq.value() << "\n";
    out << "tracked_publishers=" << view.tracked_publishers << "\n";
    out << "evidence_entries=" << view.evidence_entries << "\n";
    out << "retained_attempts=" << view.retained_attempts << "\n";
    out << "retained_fences=" << view.retained_fences << "\n";
    out << "retained_decisions=" << view.retained_decisions << "\n";
    out << "pending_directives=" << view.pending_directives << "\n";
    out << "active_authorities=" << view.active_authorities << "\n";
    out << "interrupted_attempts=" << view.interrupted_attempts << "\n";
    out << "store_failed=" << (view.store_failed ? "true" : "false") << "\n";
    for (const AttemptRecord& record : view.recent_attempts) {
        out << "attempt " << record.to_string() << "\n";
    }
    for (const FenceRecord& record : view.recent_fences) {
        out << "fence " << fence_kind_name(record.kind) << " " << record.subject.to_string() << "@"
            << record.subject_generation.value() << " seq=" << record.seq.value() << "\n";
    }
    return out.str();
}

}  // namespace lff
