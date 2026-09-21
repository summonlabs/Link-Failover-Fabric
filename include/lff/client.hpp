// Link Failover Fabric — framed session client.
//
// The client performs the handshake once, then stamps every request with the
// session identity, the coordinator epoch it was told about, its own process
// incarnation and a strictly increasing sequence. A replayed or regressed
// sequence is refused by the server, so a duplicated request can never create a
// second authority.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/engine.hpp"
#include "lff/export.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"
#include "lff/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lff {

class LFF_API Client {
public:
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    static Result<std::unique_ptr<Client>> connect(
        const Endpoint& endpoint, std::string role,
        std::size_t max_frame_payload = kDefaultFramePayload);

    // Returned by value: the cached handshake answer is refreshed by ping() and
    // refresh_handshake(), so a reference to it would be a moving target.
    HelloResponse hello() const;
    Epoch epoch() const noexcept;
    Incarnation incarnation() const noexcept;
    SessionSeq next_seq() const noexcept;
    const Endpoint& endpoint() const noexcept;

    // Refreshes the cached coordinator epoch after the coordinator restarts.
    Status refresh_handshake();

    Status publish_failure_report(const FailureReport& report);
    Status publish_replacement_report(const ReplacementReport& report);
    Status set_policy(const FailoverPolicy& policy);
    Status set_topology(const TopologySnapshot& topology);

    Result<FailoverDecision> request_failover(const FailoverRequest& request);
    Result<FailoverDecision> request_rollback(const RollbackRequest& request);
    Result<FailoverDecision> record_application(const ApplicationReport& report);
    Result<FailoverDecision> record_verification(const VerificationReport& report);
    Result<FailoverDecision> resolve_interrupted(const RevalidationRequest& request);
    Result<EngineView> inspect(std::size_t history_limit = 16);

    Result<ClaimResponse> claim_directives(std::size_t max_count);
    Status ping();

    Status shutdown_server();
    void close() noexcept;

    struct Impl;

private:
    Client();
    std::unique_ptr<Impl> impl_;
};

}  // namespace lff
