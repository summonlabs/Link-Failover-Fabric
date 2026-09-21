// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/bytes.hpp"

#include <cstring>

namespace lff {

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void Writer::u8(std::uint8_t value) {
    if (!ok_) {
        return;
    }
    buffer_.push_back(value);
}

void Writer::u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFu));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void Writer::u32(std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        u8(static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu));
    }
}

void Writer::u64(std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        u8(static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu));
    }
}

void Writer::raw(const void* data, std::size_t size) {
    if (!ok_ || size == 0) {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void Writer::bytes(std::span<const std::uint8_t> value) {
    raw(value.data(), value.size());
}

void Writer::blob(std::span<const std::uint8_t> value, std::uint32_t max_len) {
    if (!ok_) {
        return;
    }
    if (value.size() > max_len) {
        ok_ = false;
        return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(value);
}

void Writer::str(std::string_view value, std::uint32_t max_len) {
    if (!ok_) {
        return;
    }
    if (value.size() > max_len) {
        ok_ = false;
        return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value.data(), value.size());
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
std::uint8_t Reader::u8() noexcept {
    if (!ok_ || offset_ >= size_) {
        ok_ = false;
        return 0;
    }
    return data_[offset_++];
}

std::uint16_t Reader::u16() noexcept {
    std::uint16_t value = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        value = static_cast<std::uint16_t>(value | (static_cast<std::uint16_t>(u8()) << (i * 8u)));
    }
    return value;
}

std::uint32_t Reader::u32() noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= (static_cast<std::uint32_t>(u8()) << (i * 8u));
    }
    return value;
}

std::uint64_t Reader::u64() noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= (static_cast<std::uint64_t>(u8()) << (i * 8u));
    }
    return value;
}

std::span<const std::uint8_t> Reader::raw(std::size_t size) noexcept {
    if (!ok_ || size > (size_ - offset_)) {
        ok_ = false;
        return {};
    }
    const std::span<const std::uint8_t> out(data_ + offset_, size);
    offset_ += size;
    return out;
}

std::span<const std::uint8_t> Reader::blob(std::uint32_t max_len) noexcept {
    const std::uint32_t length = u32();
    if (!ok_) {
        return {};
    }
    if (length > max_len) {
        ok_ = false;
        return {};
    }
    return raw(static_cast<std::size_t>(length));
}

std::string Reader::str(std::uint32_t max_len) noexcept {
    const std::span<const std::uint8_t> value = blob(max_len);
    if (!ok_) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

Digest Reader::digest() noexcept {
    Digest value;
    const std::span<const std::uint8_t> raw_bytes = raw(value.bytes.size());
    if (!ok_) {
        return value;
    }
    std::memcpy(value.bytes.data(), raw_bytes.data(), value.bytes.size());
    return value;
}

Incarnation Reader::incarnation() noexcept {
    Incarnation value;
    const std::span<const std::uint8_t> raw_bytes = raw(value.bytes.size());
    if (!ok_) {
        return value;
    }
    std::memcpy(value.bytes.data(), raw_bytes.data(), value.bytes.size());
    return value;
}

std::vector<std::uint8_t> to_byte_vector(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace lff
