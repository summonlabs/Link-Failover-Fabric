// Link Failover Fabric — framed session service.
//
// The server accepts bounded framed sessions on a bound endpoint, establishes a
// per-session authority record during the handshake, and refuses every request
// whose session identity, boot incarnation, epoch or monotonic sequence does not
// match the established record. One session can never act under another
// session's identity, boot or epoch.
//
// Shutdown is explicit and prompt: the listener is closed first, then every
// session socket is shut down so that blocked reads and writes are released,
// and only then are the threads joined. No thread is ever joined while it is
// waiting on a lock held by the joining thread.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/engine.hpp"
#include "lff/export.hpp"
#include "lff/wire.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lff {

struct ServerOptions {
    Endpoint endpoint;
    Engine* engine{nullptr};
    std::size_t max_sessions{64};
    std::size_t max_frame_payload{kDefaultFramePayload};
    int backlog{32};
    // When true the server stops after the first Shutdown message.
    bool stop_on_shutdown_message{true};
};

class LFF_API Server {
public:
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and listens, then starts the accept loop. Returns Outcome::IoError
    // with a descriptive reason when the endpoint cannot be bound.
    static Result<std::unique_ptr<Server>> start(const ServerOptions& options);

    Endpoint local_endpoint() const;
    bool is_running() const noexcept;
    std::size_t session_count() const noexcept;
    std::uint64_t requests_handled() const noexcept;
    std::uint64_t sessions_accepted() const noexcept;
    std::uint64_t sessions_rejected() const noexcept;
    bool bind_failed() const noexcept;

    // Blocks until shutdown() is called or (when configured) a Shutdown message
    // arrives. Returns Ok on a clean stop.
    Status run();
    Status shutdown();

    struct Impl;

private:
    Server();
    std::unique_ptr<Impl> impl_;
};

}  // namespace lff
