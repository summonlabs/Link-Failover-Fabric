// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/wire.hpp"

#include "lff/bytes.hpp"
#include "lff/hash.hpp"

#include <array>
#include <cstring>
#include <utility>

namespace lff {
namespace {

struct MessageName {
    MessageType value;
    std::string_view name;
};

constexpr MessageName kMessageNames[] = {
    {MessageType::Hello, "Hello"},
    {MessageType::HelloAck, "HelloAck"},
    {MessageType::Response, "Response"},
    {MessageType::PublishFailure, "PublishFailure"},
    {MessageType::PublishReplacement, "PublishReplacement"},
    {MessageType::SetPolicy, "SetPolicy"},
    {MessageType::SetTopology, "SetTopology"},
    {MessageType::RequestFailover, "RequestFailover"},
    {MessageType::RequestRollback, "RequestRollback"},
    {MessageType::ReportApplication, "ReportApplication"},
    {MessageType::ReportVerification, "ReportVerification"},
    {MessageType::ResolveInterrupted, "ResolveInterrupted"},
    {MessageType::Inspect, "Inspect"},
    {MessageType::ClaimDirective, "ClaimDirective"},
    {MessageType::Shutdown, "Shutdown"},
    {MessageType::Ping, "Ping"},
};

constexpr std::array<std::uint8_t, 4> kMagic = {'L', 'F', 'F', '1'};

void store_u16(std::uint8_t* out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void store_u32(std::uint8_t* out, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
    }
}

std::uint16_t load_u16(const std::uint8_t* in, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset]) |
                                      (static_cast<std::uint16_t>(in[offset + 1]) << 8));
}

std::uint32_t load_u32(const std::uint8_t* in, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= (static_cast<std::uint32_t>(in[offset + i]) << (i * 8u));
    }
    return value;
}

Status make_failure(Outcome outcome, ReasonCode code, std::string detail) {
    return Status::failure(outcome, code, std::move(detail));
}

}  // namespace

std::string_view message_type_name(MessageType value) noexcept {
    for (const MessageName& entry : kMessageNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UnrecognisedMessageType";
}

bool message_type_parse(std::string_view text, MessageType& out) noexcept {
    for (const MessageName& entry : kMessageNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

bool message_type_is_known(std::uint16_t raw) noexcept {
    for (const MessageName& entry : kMessageNames) {
        if (static_cast<std::uint16_t>(entry.value) == raw) {
            return true;
        }
    }
    return false;
}

Status encode_frame(MessageType type, std::uint16_t flags, std::span<const std::uint8_t> payload,
                    std::size_t max_payload, std::vector<std::uint8_t>& out) {
    if (max_payload > kMaxFramePayload) {
        max_payload = kMaxFramePayload;
    }
    if (payload.size() > max_payload) {
        Status status = make_failure(Outcome::LimitExceeded, ReasonCode::FrameTooLarge,
                                "frame payload exceeds the negotiated maximum");
        status.add(ReasonCode::InvalidArgument, std::to_string(payload.size()));
        return status;
    }
    if (flags != 0) {
        return make_failure(Outcome::Invalid, ReasonCode::ReservedBitsSet,
                       "frame flags must be zero");
    }
    out.assign(kFrameHeaderSize + payload.size(), 0);
    std::memcpy(out.data(), kMagic.data(), kMagic.size());
    store_u16(out.data(), 4, kWireProtocolVersion);
    store_u16(out.data(), 6, static_cast<std::uint16_t>(type));
    store_u16(out.data(), 8, flags);
    store_u16(out.data(), 10, 0);
    store_u32(out.data(), 12, static_cast<std::uint32_t>(payload.size()));
    store_u32(out.data(), 16, 0);
    if (!payload.empty()) {
        std::memcpy(out.data() + kFrameHeaderSize, payload.data(), payload.size());
    }
    // Integrity covers the 16 header bytes that precede the CRC field, followed
    // by the payload. The CRC field itself is excluded.
    store_u32(out.data(), 16,
              crc32c_concat(0, out.data(), 16, out.data() + kFrameHeaderSize, payload.size()));
    return Status::success();
}

// ---------------------------------------------------------------------------
// FrameDecoder
// ---------------------------------------------------------------------------
void FrameDecoder::reset() {
    buffer_.clear();
    failed_ = false;
    failure_ = Status::success();
}

Status FrameDecoder::push(std::span<const std::uint8_t> chunk, const FrameHandler& on_frame) {
    if (failed_) {
        return failure_;
    }
    if (buffer_.size() + chunk.size() > max_payload_ + kFrameHeaderSize) {
        // The buffered prefix can never be a valid frame once it exceeds the
        // negotiated bound; refuse before growing further.
        failed_ = true;
        failure_ = make_failure(Outcome::LimitExceeded, ReasonCode::FrameTooLarge,
                           "framing buffer exceeds the negotiated maximum");
        return failure_;
    }
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());

    std::size_t offset = 0;
    while (buffer_.size() - offset >= kFrameHeaderSize) {
        const std::uint8_t* header = buffer_.data() + offset;
        if (std::memcmp(header, kMagic.data(), kMagic.size()) != 0) {
            failed_ = true;
            failure_ = make_failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                               "frame magic mismatch");
            return failure_;
        }
        const std::uint16_t version = load_u16(header, 4);
        if (version != kWireProtocolVersion) {
            failed_ = true;
            Status status = make_failure(Outcome::Unsupported, ReasonCode::ProtocolVersionMismatch,
                                    "unsupported protocol version");
            status.add(ReasonCode::InvalidArgument, std::to_string(version));
            failure_ = status;
            return failure_;
        }
        const std::uint16_t raw_type = load_u16(header, 6);
        if (!message_type_is_known(raw_type)) {
            failed_ = true;
            Status status = make_failure(Outcome::Unsupported, ReasonCode::UnknownMessageType,
                                    "unknown message type");
            status.add(ReasonCode::InvalidArgument, std::to_string(raw_type));
            failure_ = status;
            return failure_;
        }
        if (load_u16(header, 8) != 0 || load_u16(header, 10) != 0) {
            failed_ = true;
            failure_ = make_failure(Outcome::Invalid, ReasonCode::ReservedBitsSet,
                               "frame flags and reserved fields must be zero");
            return failure_;
        }
        const std::uint32_t length = load_u32(header, 12);
        if (length > max_payload_) {
            failed_ = true;
            Status status = make_failure(Outcome::LimitExceeded, ReasonCode::FrameTooLarge,
                                    "declared frame payload exceeds the negotiated maximum");
            status.add(ReasonCode::InvalidArgument, std::to_string(length));
            failure_ = status;
            return failure_;
        }
        const std::size_t total = kFrameHeaderSize + static_cast<std::size_t>(length);
        if (buffer_.size() - offset < total) {
            break;  // need more bytes
        }
        const std::uint32_t expected_crc = load_u32(header, 16);
        const std::uint32_t actual_crc =
            crc32c_concat(0, header, 16, header + kFrameHeaderSize,
                          static_cast<std::size_t>(length));
        if (expected_crc != actual_crc) {
            failed_ = true;
            failure_ = make_failure(Outcome::Corrupt, ReasonCode::ChecksumMismatch,
                               "frame checksum mismatch");
            return failure_;
        }
        FrameHeader decoded;
        decoded.version = version;
        decoded.type = static_cast<MessageType>(raw_type);
        decoded.flags = 0;
        decoded.length = length;
        decoded.crc = expected_crc;
        const std::span<const std::uint8_t> payload(header + kFrameHeaderSize,
                                                    static_cast<std::size_t>(length));
        if (on_frame) {
            on_frame(decoded, payload);
        }
        offset += total;
    }
    if (offset > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
void SessionEnvelope::encode(Writer& writer) const {
    writer.u64(session_id);
    writer.u64(epoch.value());
    writer.incarnation(incarnation);
    writer.u64(seq.value());
}

bool SessionEnvelope::decode(Reader& reader, SessionEnvelope& out) {
    SessionEnvelope value;
    value.session_id = reader.u64();
    value.epoch = Epoch::from(reader.u64());
    value.incarnation = reader.incarnation();
    value.seq = SessionSeq::from(reader.u64());
    if (!reader.ok()) {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::uint8_t> HelloRequest::encode() const {
    Writer writer;
    writer.u16(kWireProtocolVersion);
    writer.u16(protocol_version);
    writer.incarnation(client_incarnation);
    writer.u64(client_epoch.value());
    writer.str(role, 64);
    return writer.take();
}

bool HelloRequest::decode(const std::uint8_t* data, std::size_t size, HelloRequest& out) {
    Reader reader(data, size);
    const std::uint16_t envelope_version = reader.u16();
    HelloRequest value;
    value.protocol_version = reader.u16();
    value.client_incarnation = reader.incarnation();
    value.client_epoch = Epoch::from(reader.u64());
    value.role = reader.str(64);
    if (!reader.ok() || !reader.at_end() || envelope_version != kWireProtocolVersion) {
        return false;
    }
    if (value.protocol_version != kWireProtocolVersion) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> HelloResponse::encode() const {
    Writer writer;
    writer.u16(kWireProtocolVersion);
    writer.u16(protocol_version);
    writer.u64(epoch.value());
    writer.incarnation(coordinator);
    writer.u64(session_id);
    writer.u32(max_payload);
    writer.u64(server_tick);
    return writer.take();
}

bool HelloResponse::decode(const std::uint8_t* data, std::size_t size, HelloResponse& out) {
    Reader reader(data, size);
    const std::uint16_t envelope_version = reader.u16();
    HelloResponse value;
    value.protocol_version = reader.u16();
    value.epoch = Epoch::from(reader.u64());
    value.coordinator = reader.incarnation();
    value.session_id = reader.u64();
    value.max_payload = reader.u32();
    value.server_tick = reader.u64();
    if (!reader.ok() || !reader.at_end() || envelope_version != kWireProtocolVersion) {
        return false;
    }
    if (value.protocol_version != kWireProtocolVersion) {
        return false;
    }
    if (value.max_payload == 0 || value.max_payload > kMaxFramePayload) {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::uint8_t> ResponseEnvelope::encode() const {
    Writer writer;
    writer.u16(kWireProtocolVersion);
    writer.u16(op);
    writer.u16(static_cast<std::uint16_t>(outcome));
    writer.u32(static_cast<std::uint32_t>(reasons.size()));
    for (const Reason& reason : reasons) {
        writer.u16(static_cast<std::uint16_t>(reason.code));
        writer.str(reason.detail, static_cast<std::uint32_t>(kMaxReasonDetail));
    }
    return writer.take();
}

bool ResponseEnvelope::decode(const std::uint8_t* data, std::size_t size, ResponseEnvelope& out) {
    Reader reader(data, size);
    const std::uint16_t envelope_version = reader.u16();
    ResponseEnvelope value;
    value.op = reader.u16();
    const std::uint16_t outcome = reader.u16();
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || envelope_version != kWireProtocolVersion) {
        return false;
    }
    if (outcome >= kOutcomeCount || count > kMaxReasons) {
        return false;
    }
    value.outcome = static_cast<Outcome>(outcome);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t code = reader.u16();
        const std::string detail = reader.str(static_cast<std::uint32_t>(kMaxReasonDetail));
        if (!reader.ok() || code >= kReasonCodeCount) {
            return false;
        }
        value.reasons.emplace_back(static_cast<ReasonCode>(code), detail);
    }
    if (!reader.ok()) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> ClaimRequest::encode() const {
    Writer writer;
    writer.u16(kWireProtocolVersion);
    writer.u32(static_cast<std::uint32_t>(max_count));
    return writer.take();
}

bool ClaimRequest::decode(const std::uint8_t* data, std::size_t size, ClaimRequest& out) {
    Reader reader(data, size);
    const std::uint16_t envelope_version = reader.u16();
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || !reader.at_end() || envelope_version != kWireProtocolVersion) {
        return false;
    }
    if (count == 0 || count > 64) {
        return false;
    }
    out.max_count = static_cast<std::size_t>(count);
    return true;
}

std::vector<std::uint8_t> ClaimResponse::encode() const {
    Writer writer;
    writer.u16(kWireProtocolVersion);
    writer.u32(static_cast<std::uint32_t>(directives.size()));
    for (const ApplyDirective& directive : directives) {
        directive.encode(writer);
    }
    return writer.take();
}

bool ClaimResponse::decode(const std::uint8_t* data, std::size_t size, ClaimResponse& out) {
    Reader reader(data, size);
    const std::uint16_t envelope_version = reader.u16();
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || envelope_version != kWireProtocolVersion) {
        return false;
    }
    if (count > 64) {
        return false;
    }
    ClaimResponse value;
    value.directives.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ApplyDirective directive;
        if (!ApplyDirective::decode(reader, directive)) {
            return false;
        }
        value.directives.push_back(std::move(directive));
    }
    if (!reader.ok() || !reader.at_end()) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::vector<std::uint8_t> InspectRequest::encode() const {
    Writer writer;
    writer.u16(kWireProtocolVersion);
    writer.u32(static_cast<std::uint32_t>(history_limit));
    return writer.take();
}

bool InspectRequest::decode(const std::uint8_t* data, std::size_t size, InspectRequest& out) {
    Reader reader(data, size);
    const std::uint16_t envelope_version = reader.u16();
    const std::uint32_t limit = reader.u32();
    if (!reader.ok() || !reader.at_end() || envelope_version != kWireProtocolVersion) {
        return false;
    }
    if (limit > 256) {
        return false;
    }
    out.history_limit = static_cast<std::size_t>(limit);
    return true;
}

std::vector<std::uint8_t> encode_engine_view(const EngineView& view) {
    Writer writer;
    writer.u16(kWireProtocolVersion);
    writer.str(view.fabric.value());
    writer.u64(view.epoch.value());
    writer.incarnation(view.coordinator);
    writer.u64(view.policy_generation.value());
    writer.u64(view.topology_generation.value());
    writer.digest(view.policy_digest);
    writer.digest(view.topology_digest);
    writer.u64(view.boot_count);
    writer.u64(view.last_seq.value());
    writer.u64(static_cast<std::uint64_t>(view.tracked_publishers));
    writer.u64(static_cast<std::uint64_t>(view.evidence_entries));
    writer.u64(static_cast<std::uint64_t>(view.retained_attempts));
    writer.u64(static_cast<std::uint64_t>(view.retained_fences));
    writer.u64(static_cast<std::uint64_t>(view.retained_decisions));
    writer.u64(static_cast<std::uint64_t>(view.pending_directives));
    writer.u64(static_cast<std::uint64_t>(view.active_authorities));
    writer.u64(static_cast<std::uint64_t>(view.interrupted_attempts));
    writer.bool8(view.store_failed);
    writer.u32(static_cast<std::uint32_t>(view.recent_attempts.size()));
    for (const AttemptRecord& record : view.recent_attempts) {
        record.encode(writer);
    }
    writer.u32(static_cast<std::uint32_t>(view.recent_fences.size()));
    for (const FenceRecord& record : view.recent_fences) {
        record.encode(writer);
    }
    return writer.take();
}

bool decode_engine_view(const std::uint8_t* data, std::size_t size, EngineView& out) {
    Reader reader(data, size);
    const std::uint16_t envelope_version = reader.u16();
    EngineView value;
    const std::string fabric = reader.str();
    value.epoch = Epoch::from(reader.u64());
    value.coordinator = reader.incarnation();
    value.policy_generation = PolicyGeneration::from(reader.u64());
    value.topology_generation = TopologyGeneration::from(reader.u64());
    value.policy_digest = reader.digest();
    value.topology_digest = reader.digest();
    value.boot_count = reader.u64();
    value.last_seq = LineageSeq::from(reader.u64());
    const std::uint64_t tracked = reader.u64();
    const std::uint64_t evidence = reader.u64();
    const std::uint64_t attempts = reader.u64();
    const std::uint64_t fences = reader.u64();
    const std::uint64_t decisions = reader.u64();
    const std::uint64_t pending = reader.u64();
    const std::uint64_t active = reader.u64();
    const std::uint64_t interrupted = reader.u64();
    value.store_failed = reader.bool8();
    if (!reader.ok() || envelope_version != kWireProtocolVersion) {
        return false;
    }
    const Result<FabricName> fabric_name = FabricName::parse(fabric);
    if (!fabric_name.ok()) {
        return false;
    }
    if (tracked > 1000000ull || evidence > 1000000ull || attempts > 1000000ull ||
        fences > 1000000ull || decisions > 1000000ull || pending > 1000000ull ||
        active > 1000000ull || interrupted > 1000000ull) {
        return false;
    }
    value.fabric = fabric_name.value();
    value.tracked_publishers = static_cast<std::size_t>(tracked);
    value.evidence_entries = static_cast<std::size_t>(evidence);
    value.retained_attempts = static_cast<std::size_t>(attempts);
    value.retained_fences = static_cast<std::size_t>(fences);
    value.retained_decisions = static_cast<std::size_t>(decisions);
    value.pending_directives = static_cast<std::size_t>(pending);
    value.active_authorities = static_cast<std::size_t>(active);
    value.interrupted_attempts = static_cast<std::size_t>(interrupted);

    const std::uint32_t attempt_count = reader.u32();
    if (!reader.ok() || attempt_count > 256) {
        return false;
    }
    value.recent_attempts.reserve(attempt_count);
    for (std::uint32_t i = 0; i < attempt_count; ++i) {
        AttemptRecord record;
        if (!AttemptRecord::decode(reader, record)) {
            return false;
        }
        value.recent_attempts.push_back(std::move(record));
    }
    const std::uint32_t fence_count = reader.u32();
    if (!reader.ok() || fence_count > 256) {
        return false;
    }
    value.recent_fences.reserve(fence_count);
    for (std::uint32_t i = 0; i < fence_count; ++i) {
        FenceRecord record;
        if (!FenceRecord::decode(reader, record)) {
            return false;
        }
        value.recent_fences.push_back(record);
    }
    if (!reader.ok() || !reader.at_end()) {
        return false;
    }
    out = std::move(value);
    return true;
}

}  // namespace lff
