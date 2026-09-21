// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/ids.hpp"

#include "lff/bytes.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace lff {
namespace {

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

constexpr char kHexDigits[] = "0123456789abcdef";

void append_hex(std::string& out, const std::uint8_t* data, std::size_t size) {
    out.reserve(out.size() + size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHexDigits[(data[i] >> 4) & 0x0Fu]);
        out.push_back(kHexDigits[data[i] & 0x0Fu]);
    }
}

int hex_value(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

}  // namespace

bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a > (0xFFFFFFFFFFFFFFFFull - b)) {
        return false;
    }
    out = a + b;
    return true;
}

bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > (0xFFFFFFFFFFFFFFFFull / a)) {
        return false;
    }
    out = a * b;
    return true;
}

bool checked_add_u32(std::uint32_t a, std::uint32_t b, std::uint32_t& out) noexcept {
    if (a > (0xFFFFFFFFu - b)) {
        return false;
    }
    out = a + b;
    return true;
}

bool checked_mul_u32(std::uint32_t a, std::uint32_t b, std::uint32_t& out) noexcept {
    if (a != 0 && b > (0xFFFFFFFFu / a)) {
        return false;
    }
    out = a * b;
    return true;
}

Confidence make_confidence(std::uint32_t permille) noexcept {
    if (permille > kMaxConfidence) {
        permille = kMaxConfidence;
    }
    return Confidence::from(permille);
}

bool is_valid_name(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxNameLength) {
        return false;
    }
    if (text == "." || text == "..") {
        return false;
    }
    for (char ch : text) {
        const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9');
        const bool punctuation = ch == '.' || ch == '_' || ch == '-' || ch == ':';
        if (!alnum && !punctuation) {
            return false;
        }
    }
    return true;
}

std::string LinkKey::to_string() const {
    std::string out;
    out.reserve(fabric.value().size() + link.value().size() + 1);
    out.append(fabric.value());
    out.push_back('/');
    out.append(link.value());
    return out;
}

bool Incarnation::is_zero() const noexcept {
    for (std::uint8_t byte : bytes) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string Incarnation::hex() const {
    std::string out;
    append_hex(out, bytes.data(), bytes.size());
    return out;
}

bool Incarnation::parse(std::string_view text, Incarnation& out) noexcept {
    if (text.size() != 32) {
        return false;
    }
    Incarnation value;
    for (std::size_t i = 0; i < 16; ++i) {
        const int high = hex_value(text[i * 2]);
        const int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        value.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    out = value;
    return true;
}

Incarnation Incarnation::generate() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const auto steady = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto wall = std::chrono::system_clock::now().time_since_epoch().count();
    const std::uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t entropy = 0;
    try {
        std::random_device device;
        entropy = (static_cast<std::uint64_t>(device()) << 32) ^
                  static_cast<std::uint64_t>(device());
    } catch (...) {
        entropy = 0x9E3779B97F4A7C15ull ^ sequence;
    }
    const Digest digest = DigestBuilder()
                              .add_u64("steady", static_cast<std::uint64_t>(steady))
                              .add_u64("wall", static_cast<std::uint64_t>(wall))
                              .add_u64("pid", current_process_id())
                              .add_u64("seq", sequence)
                              .add_u64("entropy", entropy)
                              .finish();
    Incarnation out;
    std::memcpy(out.bytes.data(), digest.bytes.data(), out.bytes.size());
    return out;
}

bool operator==(const Incarnation& a, const Incarnation& b) noexcept {
    return a.bytes == b.bytes;
}

bool operator!=(const Incarnation& a, const Incarnation& b) noexcept {
    return a.bytes != b.bytes;
}

bool operator<(const Incarnation& a, const Incarnation& b) noexcept {
    return a.bytes < b.bytes;
}

std::string Endpoint::to_string() const {
    std::string out = host;
    out.push_back(':');
    out.append(std::to_string(port));
    return out;
}

Result<Endpoint> Endpoint::parse(std::string_view text) {
    if (text.empty() || text.size() > 300) {
        return Result<Endpoint>::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                         "endpoint is empty or too long");
    }
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
        return Result<Endpoint>::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                         "endpoint must be host:port");
    }
    const std::string_view host = text.substr(0, colon);
    const std::string_view port_text = text.substr(colon + 1);
    if (host.size() > 255) {
        return Result<Endpoint>::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                         "host is too long");
    }
    for (char ch : host) {
        const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9');
        if (!alnum && ch != '.' && ch != '-' && ch != '_' && ch != ':' && ch != '[' && ch != ']') {
            return Result<Endpoint>::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                             "host contains an unsupported character");
        }
    }
    std::uint32_t port = 0;
    for (char ch : port_text) {
        if (ch < '0' || ch > '9') {
            return Result<Endpoint>::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                             "port must be decimal");
        }
        const std::uint32_t digit = static_cast<std::uint32_t>(ch - '0');
        std::uint32_t next = 0;
        if (!checked_mul_u32(port, 10u, next) || !checked_add_u32(next, digit, port)) {
            return Result<Endpoint>::failure(Outcome::Invalid, ReasonCode::CheckedArithmeticOverflow,
                                             "port overflows");
        }
    }
    // Port zero is meaningful for a listener: it asks the operating system for
    // any free port, and the bound endpoint is reported back afterwards. It is
    // not a connectable address, and a client that uses one simply fails to
    // connect.
    if (port > 65535u) {
        return Result<Endpoint>::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                         "port out of range");
    }
    Endpoint endpoint;
    endpoint.host.assign(host);
    endpoint.port = static_cast<std::uint16_t>(port);
    return Result<Endpoint>::success(std::move(endpoint));
}

Digest link_identity_digest(const LinkKey& key) {
    return DigestBuilder()
        .add("fabric", key.fabric.value())
        .add("link", key.link.value())
        .finish();
}

}  // namespace lff
