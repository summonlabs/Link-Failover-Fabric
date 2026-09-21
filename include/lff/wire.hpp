// Link Failover Fabric — bounded framed transport codec.
//
// Frame layout (20-byte header + payload)
//   offset  size  field
//        0     4  magic 'L','F','F','1'
//        4     2  protocol version
//        6     2  message type
//        8     2  flags (must be zero; reserved bits are refused)
//       10     2  reserved (must be zero)
//       12     4  payload length
//       16     4  CRC-32C over the 16 header bytes with the CRC field zeroed,
//                 followed by the payload
//
// Decoding is total and sticky: a decoder that has seen one bad frame never
// returns success again. A declared length is validated against the configured
// maximum *before* any allocation happens, so an oversized claim cannot be used
// to exhaust memory. Every enum, range and reserved field is validated, trailing
// bytes inside a body are refused, and truncated prefixes are reported as
// Outcome::Invalid with ReasonCode::TruncatedFrame rather than being guessed at.
//
// Trust boundary: the protocol is a plain TCP framing with integrity checks and
// a session authority model. It performs no authentication and no encryption,
// and it must not be exposed to an untrusted network. See the README.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/decision.hpp"
#include "lff/engine.hpp"
#include "lff/export.hpp"
#include "lff/ids.hpp"
#include "lff/outcome.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace lff {

inline constexpr std::size_t kFrameHeaderSize = 20;
inline constexpr std::size_t kMaxFramePayload = 1u << 20;      // 1 MiB hard ceiling
inline constexpr std::size_t kDefaultFramePayload = 1u << 18;  // 256 KiB default

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    Response = 3,

    PublishFailure = 10,
    PublishReplacement = 11,
    SetPolicy = 12,
    SetTopology = 13,

    RequestFailover = 20,
    RequestRollback = 21,
    ReportApplication = 22,
    ReportVerification = 23,
    ResolveInterrupted = 24,
    Inspect = 25,

    // The applier boundary. A worker claims directives, performs the effect
    // through its own means, and reports back with ReportApplication. There is
    // no separate acknowledgement message: an acknowledgement is a report.
    ClaimDirective = 30,

    Shutdown = 40,
    Ping = 41,
};
inline constexpr std::uint16_t kMessageTypeCount = 16;

LFF_API std::string_view message_type_name(MessageType value) noexcept;
LFF_API bool message_type_parse(std::string_view text, MessageType& out) noexcept;
LFF_API bool message_type_is_known(std::uint16_t raw) noexcept;

struct FrameHeader {
    std::uint16_t version{kWireProtocolVersion};
    MessageType type{MessageType::Ping};
    std::uint16_t flags{0};
    std::uint32_t length{0};
    std::uint32_t crc{0};
};

// Encodes one frame. Oversized payloads are refused before the output vector is
// grown.
LFF_API Status encode_frame(MessageType type, std::uint16_t flags,
                            std::span<const std::uint8_t> payload, std::size_t max_payload,
                            std::vector<std::uint8_t>& out);

// Streaming decoder with sticky failure.
class LFF_API FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_payload = kDefaultFramePayload) : max_payload_(max_payload) {}

    using FrameHandler = std::function<void(const FrameHeader&, std::span<const std::uint8_t>)>;

    // Consumes a chunk. Returns Ok when the chunk was accepted, which may or may
    // not have produced complete frames. Once a failure is returned, every later
    // call returns the same status.
    Status push(std::span<const std::uint8_t> chunk, const FrameHandler& on_frame);

    bool failed() const noexcept { return failed_; }
    const Status& failure() const noexcept { return failure_; }
    std::size_t buffered() const noexcept { return buffer_.size(); }
    void reset();

private:
    std::size_t max_payload_;
    std::vector<std::uint8_t> buffer_;
    bool failed_{false};
    Status failure_;
};

// Session authority envelope carried by every post-handshake request.
struct SessionEnvelope {
    std::uint64_t session_id{0};
    Epoch epoch;
    Incarnation incarnation;
    SessionSeq seq;

    void encode(class Writer& writer) const;
    static bool decode(class Reader& reader, SessionEnvelope& out);
};

struct HelloRequest {
    std::uint16_t protocol_version{kWireProtocolVersion};
    Incarnation client_incarnation;
    Epoch client_epoch;
    std::string role;

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, HelloRequest& out);
};

struct HelloResponse {
    std::uint16_t protocol_version{kWireProtocolVersion};
    Epoch epoch;
    Incarnation coordinator;
    std::uint64_t session_id{0};
    std::uint32_t max_payload{static_cast<std::uint32_t>(kDefaultFramePayload)};
    std::uint64_t server_tick{0};

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, HelloResponse& out);
};

struct ResponseEnvelope {
    std::uint16_t op{0};
    Outcome outcome{Outcome::Ok};
    std::vector<Reason> reasons;

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, ResponseEnvelope& out);
};

struct ClaimRequest {
    std::size_t max_count{1};

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, ClaimRequest& out);
};

struct ClaimResponse {
    std::vector<ApplyDirective> directives;

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, ClaimResponse& out);
};

struct InspectRequest {
    std::size_t history_limit{16};

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, InspectRequest& out);
};

LFF_API std::vector<std::uint8_t> encode_engine_view(const EngineView& view);
LFF_API bool decode_engine_view(const std::uint8_t* data, std::size_t size, EngineView& out);

}  // namespace lff
