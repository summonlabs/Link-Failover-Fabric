// Link Failover Fabric — integrity and digest primitives.
//
// Both primitives are implemented here so the runtime has no third-party
// dependency. CRC-32C (Castagnoli) is used for frame and record integrity;
// SHA-256 is used for identity digests (attempt ids, decision ids, evidence
// digests). Neither is used as a security mechanism: see the trust boundary in
// the README.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lff {

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------
LFF_API std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
LFF_API std::uint32_t crc32c_extend(std::uint32_t seed, const void* data, std::size_t size) noexcept;
LFF_API std::uint32_t crc32c_concat(
    std::uint32_t seed, const void* first, std::size_t first_size,
    const void* second, std::size_t second_size) noexcept;

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
class LFF_API Sha256 {
public:
    Sha256() noexcept { reset(); }
    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }
    // Finalises and writes 32 bytes into out. The object must be reset before reuse.
    void finish(std::uint8_t out[32]) noexcept;

private:
    void compress(const std::uint8_t block[64]) noexcept;
    std::uint32_t state_[8]{};
    std::uint64_t bit_count_{0};
    std::uint8_t buffer_[64]{};
    std::size_t buffer_size_{0};
    bool finished_{false};
};

// A 256-bit identity digest. Zero digests are never produced for real subjects;
// Digest{} means "absent".
struct Digest {
    std::array<std::uint8_t, 32> bytes{};

    bool is_zero() const noexcept;
    std::string hex() const;
    static bool parse(std::string_view text, Digest& out) noexcept;
};

LFF_API bool operator==(const Digest& a, const Digest& b) noexcept;
LFF_API bool operator!=(const Digest& a, const Digest& b) noexcept;
LFF_API bool operator<(const Digest& a, const Digest& b) noexcept;

LFF_API Digest sha256(const void* data, std::size_t size);
LFF_API Digest sha256(std::string_view text);

// A deterministic digest over an ordered list of byte strings. Elements are
// length-prefixed so that ("ab","c") and ("a","bc") never collide.
class LFF_API DigestBuilder {
public:
    DigestBuilder() = default;
    DigestBuilder& add(std::string_view label, const void* data, std::size_t size);
    DigestBuilder& add(std::string_view label, std::string_view text) {
        return add(label, text.data(), text.size());
    }
    DigestBuilder& add_u64(std::string_view label, std::uint64_t value);
    DigestBuilder& add_bytes(std::string_view label, const std::vector<std::uint8_t>& value);
    Digest finish() const;

private:
    std::vector<std::uint8_t> buffer_;
};

}  // namespace lff
