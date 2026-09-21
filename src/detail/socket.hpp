// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal blocking-socket helpers. Not part of the installed interface.
//
// Every operation is bounded and every blocked call can be released by
// shutdown_socket(), which is how the server guarantees prompt teardown.
#pragma once

#include "lff/ids.hpp"
#include "lff/outcome.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lff::detail {

#if defined(_WIN32)
using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~static_cast<std::uintptr_t>(0));
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Idempotent process-wide network initialisation.
Status initialise_networking();

// Creates, binds and listens. `bound` receives the address actually bound,
// which resolves a caller-supplied port of 0 to the assigned port.
Result<SocketHandle> create_listener(const Endpoint& endpoint, int backlog, Endpoint& bound);

Result<SocketHandle> connect_socket(const Endpoint& endpoint);

// Waits until the socket has a pending operation, up to `timeout_ms`.
// Returns true when it is readable, false on timeout. This is what makes the
// accept loop releasable: a blocked accept() cannot be relied upon to return
// when another thread closes or shuts down the listening socket, so the loop
// waits with a bounded timeout and re-checks its own stop flag.
Result<bool> wait_readable(SocketHandle socket, int timeout_ms);

// Blocks until a connection arrives or the listener is shut down.
Result<SocketHandle> accept_socket(SocketHandle listener);

// Releases any thread blocked in accept/recv/send on this socket.
void shutdown_socket(SocketHandle socket) noexcept;

void close_socket(SocketHandle socket) noexcept;

// Sends the whole buffer, or reports the first failure.
Status send_all(SocketHandle socket, std::span<const std::uint8_t> data);

// Reads at least one byte, or reports peer close as Outcome::Closed.
Result<std::size_t> recv_some(SocketHandle socket, std::span<std::uint8_t> buffer);

std::string last_socket_error_text();

}  // namespace lff::detail
