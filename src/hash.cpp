// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/hash.hpp"

#include "lff/bytes.hpp"

#include <array>
#include <cstring>

namespace lff {
namespace {

constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78u;

struct Crc32cTable {
    std::array<std::uint32_t, 256> entries{};

    Crc32cTable() noexcept {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                if ((crc & 1u) != 0u) {
                    crc = (crc >> 1) ^ kCrc32cPolynomial;
                } else {
                    crc >>= 1;
                }
            }
            entries[i] = crc;
        }
    }
};

const Crc32cTable& crc_table() noexcept {
    static const Crc32cTable table;
    return table;
}

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
    return (value >> shift) | (value << (32u - shift));
}

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t seed, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const Crc32cTable& table = crc_table();
    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table.entries[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
    return crc32c_extend(0u, data, size);
}

std::uint32_t crc32c_concat(std::uint32_t seed, const void* first, std::size_t first_size,
                            const void* second, std::size_t second_size) noexcept {
    return crc32c_extend(crc32c_extend(seed, first, first_size), second, second_size);
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
void Sha256::reset() noexcept {
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
    bit_count_ = 0;
    buffer_size_ = 0;
    finished_ = false;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
    if (finished_ || size == 0) {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    bit_count_ += static_cast<std::uint64_t>(size) * 8u;
    std::size_t offset = 0;
    if (buffer_size_ > 0) {
        while (buffer_size_ < 64 && offset < size) {
            buffer_[buffer_size_++] = bytes[offset++];
        }
        if (buffer_size_ == 64) {
            compress(buffer_);
            buffer_size_ = 0;
        }
    }
    while (size - offset >= 64) {
        compress(bytes + offset);
        offset += 64;
    }
    while (offset < size) {
        buffer_[buffer_size_++] = bytes[offset++];
    }
}

void Sha256::finish(std::uint8_t out[32]) noexcept {
    if (!finished_) {
        const std::uint64_t bits = bit_count_;
        const std::uint8_t pad = 0x80u;
        update(&pad, 1);
        const std::uint8_t zero = 0x00u;
        while (buffer_size_ != 56) {
            update(&zero, 1);
        }
        std::uint8_t length_bytes[8];
        for (std::size_t i = 0; i < 8; ++i) {
            length_bytes[i] = static_cast<std::uint8_t>((bits >> ((7u - i) * 8u)) & 0xFFu);
        }
        update(length_bytes, 8);
        finished_ = true;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
    }
}

// ---------------------------------------------------------------------------
// Digest
// ---------------------------------------------------------------------------
bool Digest::is_zero() const noexcept {
    for (std::uint8_t byte : bytes) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string Digest::hex() const {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
        out.push_back(kDigits[byte & 0x0Fu]);
    }
    return out;
}

bool Digest::parse(std::string_view text, Digest& out) noexcept {
    if (text.size() != 64) {
        return false;
    }
    Digest value;
    for (std::size_t i = 0; i < 32; ++i) {
        unsigned nibble_high = 0;
        unsigned nibble_low = 0;
        for (int half = 0; half < 2; ++half) {
            const char ch = text[i * 2 + static_cast<std::size_t>(half)];
            unsigned digit = 0;
            if (ch >= '0' && ch <= '9') {
                digit = static_cast<unsigned>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = static_cast<unsigned>(ch - 'a') + 10u;
            } else if (ch >= 'A' && ch <= 'F') {
                digit = static_cast<unsigned>(ch - 'A') + 10u;
            } else {
                return false;
            }
            if (half == 0) {
                nibble_high = digit;
            } else {
                nibble_low = digit;
            }
        }
        value.bytes[i] = static_cast<std::uint8_t>((nibble_high << 4) | nibble_low);
    }
    out = value;
    return true;
}

bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.bytes == b.bytes;
}

bool operator!=(const Digest& a, const Digest& b) noexcept {
    return a.bytes != b.bytes;
}

bool operator<(const Digest& a, const Digest& b) noexcept {
    return a.bytes < b.bytes;
}

Digest sha256(const void* data, std::size_t size) {
    Sha256 hasher;
    hasher.update(data, size);
    Digest out;
    hasher.finish(out.bytes.data());
    return out;
}

Digest sha256(std::string_view text) {
    return sha256(text.data(), text.size());
}

DigestBuilder& DigestBuilder::add(std::string_view label, const void* data, std::size_t size) {
    const std::uint32_t label_length = static_cast<std::uint32_t>(label.size());
    const std::uint64_t value_length = static_cast<std::uint64_t>(size);
    for (std::size_t i = 0; i < 4; ++i) {
        buffer_.push_back(static_cast<std::uint8_t>((label_length >> (i * 8)) & 0xFFu));
    }
    for (std::size_t i = 0; i < 8; ++i) {
        buffer_.push_back(static_cast<std::uint8_t>((value_length >> (i * 8)) & 0xFFu));
    }
    buffer_.insert(buffer_.end(), label.begin(), label.end());
    if (size > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
    }
    return *this;
}

DigestBuilder& DigestBuilder::add_u64(std::string_view label, std::uint64_t value) {
    std::uint8_t raw[8];
    for (std::size_t i = 0; i < 8; ++i) {
        raw[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
    }
    return add(label, raw, sizeof(raw));
}

DigestBuilder& DigestBuilder::add_bytes(std::string_view label, const std::vector<std::uint8_t>& value) {
    return add(label, value.data(), value.size());
}

Digest DigestBuilder::finish() const {
    return sha256(buffer_.data(), buffer_.size());
}

}  // namespace lff
