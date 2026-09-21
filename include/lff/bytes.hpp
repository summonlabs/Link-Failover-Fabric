// Link Failover Fabric — canonical, deterministic byte encoding.
//
// Every durable record, wire message and identity digest is produced by writing
// fixed-width little-endian fields in a declared order. There is no padding, no
// platform-dependent layout, and no use of raw struct memory. Decoding is
// total: a Reader never throws, records the first failure stickily, and returns
// neutral values afterwards so callers can check once at the end.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"
#include "lff/hash.hpp"
#include "lff/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lff {

// Bounds applied before any allocation happens.
inline constexpr std::uint32_t kMaxEncodedString = 4096;
inline constexpr std::uint32_t kMaxEncodedBlob = 1u << 20;  // 1 MiB

class LFF_API Writer {
public:
    Writer() = default;

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void raw(const void* data, std::size_t size);
    void bytes(std::span<const std::uint8_t> value);
    void bool8(bool value) { u8(value ? 1u : 0u); }

    // Length-prefixed forms. A value that exceeds the bound marks the writer
    // failed and writes nothing further.
    void blob(std::span<const std::uint8_t> value, std::uint32_t max_len = kMaxEncodedBlob);
    void str(std::string_view value, std::uint32_t max_len = kMaxEncodedString);
    void digest(const Digest& value) { raw(value.bytes.data(), value.bytes.size()); }
    void incarnation(const Incarnation& value) { raw(value.bytes.data(), value.bytes.size()); }

    bool ok() const noexcept { return ok_; }
    std::size_t size() const noexcept { return buffer_.size(); }
    const std::vector<std::uint8_t>& data() const noexcept { return buffer_; }
    std::span<const std::uint8_t> span() const noexcept {
        return std::span<const std::uint8_t>(buffer_.data(), buffer_.size());
    }
    std::vector<std::uint8_t> take() { return std::move(buffer_); }
    void clear() noexcept { buffer_.clear(); ok_ = true; }

private:
    std::vector<std::uint8_t> buffer_;
    bool ok_{true};
};

class LFF_API Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    explicit Reader(std::span<const std::uint8_t> value) noexcept
        : data_(value.data()), size_(value.size()) {}

    bool ok() const noexcept { return ok_; }
    bool at_end() const noexcept { return offset_ == size_; }
    std::size_t remaining() const noexcept { return size_ - offset_; }
    std::size_t offset() const noexcept { return offset_; }
    void fail() noexcept { ok_ = false; }

    std::uint8_t u8() noexcept;
    std::uint16_t u16() noexcept;
    std::uint32_t u32() noexcept;
    std::uint64_t u64() noexcept;
    std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }
    bool bool8() noexcept { return u8() != 0; }

    std::span<const std::uint8_t> raw(std::size_t size) noexcept;
    std::span<const std::uint8_t> blob(std::uint32_t max_len = kMaxEncodedBlob) noexcept;
    std::string str(std::uint32_t max_len = kMaxEncodedString) noexcept;
    Digest digest() noexcept;
    Incarnation incarnation() noexcept;

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};
    bool ok_{true};
};

// Canonical byte-vector helpers used by tests and digests.
LFF_API std::vector<std::uint8_t> to_byte_vector(std::string_view text);

}  // namespace lff
