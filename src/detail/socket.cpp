// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "detail/socket.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#endif

namespace lff::detail {
namespace {

std::once_flag g_startup_once;
Status g_startup_status = Status::success();

void perform_startup() {
#if defined(_WIN32)
    WSADATA data{};
    const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        g_startup_status = Status::failure(Outcome::IoError, ReasonCode::SocketError,
                                           "WSAStartup failed");
    }
#else
    g_startup_status = Status::success();
#endif
}

int last_error_code() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

Status socket_failure(const char* what) {
    Status status = Status::failure(Outcome::IoError, ReasonCode::SocketError, what);
    status.add(ReasonCode::InternalError, last_socket_error_text());
    return status;
}

bool make_address(const Endpoint& endpoint, int socktype, bool passive, addrinfo** out) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    const std::string port = std::to_string(endpoint.port);
    std::string host = endpoint.host;
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    return ::getaddrinfo(host.c_str(), port.c_str(), &hints, out) == 0;
}

void configure_stream(SocketHandle socket) {
    const int one = 1;
#if defined(_WIN32)
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                 sizeof(one));
#else
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

}  // namespace

Status initialise_networking() {
    std::call_once(g_startup_once, perform_startup);
    return g_startup_status;
}

Result<SocketHandle> create_listener(const Endpoint& endpoint, int backlog, Endpoint& bound) {
    const Status started = initialise_networking();
    if (!started.ok()) {
        return Result<SocketHandle>::failure(started);
    }
    addrinfo* results = nullptr;
    if (!make_address(endpoint, SOCK_STREAM, true, &results)) {
        return Result<SocketHandle>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "endpoint address could not be resolved"));
    }
    SocketHandle listener = kInvalidSocket;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        SocketHandle socket = ::socket(candidate->ai_family, candidate->ai_socktype,
                                       candidate->ai_protocol);
        if (socket == kInvalidSocket) {
            continue;
        }
        const int one = 1;
#if defined(_WIN32)
        ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                     sizeof(one));
#else
        ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
        if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
            close_socket(socket);
            continue;
        }
        if (::listen(socket, backlog) != 0) {
            close_socket(socket);
            continue;
        }
        listener = socket;
        break;
    }
    ::freeaddrinfo(results);
    if (listener == kInvalidSocket) {
        return Result<SocketHandle>::failure(socket_failure("bind/listen failed"));
    }

    sockaddr_storage address{};
    int address_length = static_cast<int>(sizeof(address));
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
        close_socket(listener);
        return Result<SocketHandle>::failure(socket_failure("getsockname failed"));
    }
    Endpoint actual;
    actual.host = endpoint.host;
    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        actual.port = ::ntohs(ipv4->sin_port);
    } else if (address.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        actual.port = ::ntohs(ipv6->sin6_port);
    } else {
        close_socket(listener);
        return Result<SocketHandle>::failure(
            Status::failure(Outcome::Unsupported, ReasonCode::FeatureUnsupported,
                            "unsupported address family"));
    }
    bound = actual;
    return Result<SocketHandle>::success(listener);
}

Result<SocketHandle> connect_socket(const Endpoint& endpoint) {
    const Status started = initialise_networking();
    if (!started.ok()) {
        return Result<SocketHandle>::failure(started);
    }
    addrinfo* results = nullptr;
    if (!make_address(endpoint, SOCK_STREAM, false, &results)) {
        return Result<SocketHandle>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "endpoint address could not be resolved"));
    }
    SocketHandle socket = kInvalidSocket;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        SocketHandle candidate_socket = ::socket(candidate->ai_family, candidate->ai_socktype,
                                                 candidate->ai_protocol);
        if (candidate_socket == kInvalidSocket) {
            continue;
        }
        if (::connect(candidate_socket, candidate->ai_addr,
                      static_cast<int>(candidate->ai_addrlen)) != 0) {
            close_socket(candidate_socket);
            continue;
        }
        socket = candidate_socket;
        break;
    }
    ::freeaddrinfo(results);
    if (socket == kInvalidSocket) {
        return Result<SocketHandle>::failure(socket_failure("connect failed"));
    }
    configure_stream(socket);
    return Result<SocketHandle>::success(socket);
}

Result<bool> wait_readable(SocketHandle socket, int timeout_ms) {
    if (socket == kInvalidSocket) {
        return Result<bool>::failure(
            Status::failure(Outcome::Closed, ReasonCode::NotListening, "socket is closed"));
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int ready = ::select(socket + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (ready < 0) {
        return Result<bool>::failure(socket_failure("select failed"));
    }
    return Result<bool>::success(ready > 0);
}

Result<SocketHandle> accept_socket(SocketHandle listener) {
    SocketHandle accepted = ::accept(listener, nullptr, nullptr);
    if (accepted == kInvalidSocket) {
        return Result<SocketHandle>::failure(socket_failure("accept failed"));
    }
    configure_stream(accepted);
    return Result<SocketHandle>::success(accepted);
}

void shutdown_socket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

void close_socket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

Status send_all(SocketHandle socket, std::span<const std::uint8_t> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
        const int sent = ::send(socket, reinterpret_cast<const char*>(data.data() + offset), chunk,
#if defined(_WIN32)
                                0);
#else
                                MSG_NOSIGNAL);
#endif
        if (sent <= 0) {
            return socket_failure("send failed");
        }
        offset += static_cast<std::size_t>(sent);
    }
    return Status::success();
}

Result<std::size_t> recv_some(SocketHandle socket, std::span<std::uint8_t> buffer) {
    if (buffer.empty()) {
        return Result<std::size_t>::success(0);
    }
    const int chunk = static_cast<int>(buffer.size() > 1u << 20 ? 1u << 20 : buffer.size());
    const int received = ::recv(socket, reinterpret_cast<char*>(buffer.data()), chunk, 0);
    if (received == 0) {
        return Result<std::size_t>::failure(
            Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed, "peer closed"));
    }
    if (received < 0) {
        return Result<std::size_t>::failure(socket_failure("recv failed"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(received));
}

std::string last_socket_error_text() {
    const int code = last_error_code();
    return "code=" + std::to_string(code);
}

}  // namespace lff::detail
