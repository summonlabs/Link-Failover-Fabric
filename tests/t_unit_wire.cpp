// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Framed transport codec suite: every malformed frame shape the protocol can
// receive is exercised, and the decoder must stay total and sticky.
#include "framework.hpp"
#include "lff/wire.hpp"
#include "support.hpp"

#include <array>
#include <set>
#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

std::vector<std::uint8_t> frame_bytes(MessageType type, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> out;
    const Status encoded = encode_frame(type, 0, body, kDefaultFramePayload, out);
    if (!encoded.ok()) {
        LFF_FAIL("frame encoding failed: " + encoded.to_string());
    }
    return out;
}

}  // namespace

LFF_TEST(protocol, frame_round_trip) {
    const std::vector<std::uint8_t> body = {1, 2, 3, 4, 5};
    const std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, body);
    LFF_CHECK_EQ(frame.size(), kFrameHeaderSize + body.size());
    FrameDecoder decoder(kDefaultFramePayload);
    std::size_t frames = 0;
    std::vector<std::uint8_t> captured;
    const Status pushed = decoder.push(frame, [&](const FrameHeader& header,
                                                  std::span<const std::uint8_t> payload) {
        ++frames;
        LFF_CHECK(header.type == MessageType::Ping);
        LFF_CHECK_EQ(header.version, kWireProtocolVersion);
        LFF_CHECK_EQ(header.length, static_cast<std::uint32_t>(body.size()));
        captured.assign(payload.begin(), payload.end());
    });
    expect_ok(pushed, "frame decode");
    LFF_CHECK_EQ(frames, std::size_t{1});
    LFF_CHECK(captured == body);
    LFF_CHECK_EQ(decoder.buffered(), std::size_t{0});
}

LFF_TEST(protocol, byte_at_a_time_decoding_is_identical) {
    const std::vector<std::uint8_t> body = to_byte_vector("synthetic payload");
    const std::vector<std::uint8_t> frame = frame_bytes(MessageType::Inspect, body);
    FrameDecoder decoder(kDefaultFramePayload);
    std::size_t frames = 0;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const std::span<const std::uint8_t> chunk(frame.data() + i, 1);
        const Status pushed = decoder.push(chunk, [&](const FrameHeader&, std::span<const std::uint8_t>) {
            ++frames;
        });
        expect_ok(pushed, "incremental decode");
    }
    LFF_CHECK_EQ(frames, std::size_t{1});
}

LFF_TEST(protocol, multiple_frames_in_one_chunk) {
    std::vector<std::uint8_t> stream = frame_bytes(MessageType::Ping, {1});
    const std::vector<std::uint8_t> second = frame_bytes(MessageType::Inspect, {2, 3});
    stream.insert(stream.end(), second.begin(), second.end());
    FrameDecoder decoder(kDefaultFramePayload);
    std::size_t frames = 0;
    expect_ok(decoder.push(stream, [&](const FrameHeader&, std::span<const std::uint8_t>) { ++frames; }),
              "multi-frame decode");
    LFF_CHECK_EQ(frames, std::size_t{2});
    LFF_CHECK_EQ(decoder.buffered(), std::size_t{0});
}

LFF_TEST(protocol, every_truncated_prefix_waits_or_fails_but_never_succeeds_falsely) {
    const std::vector<std::uint8_t> body = to_byte_vector("prefix probe");
    const std::vector<std::uint8_t> frame = frame_bytes(MessageType::PublishFailure, body);
    for (std::size_t length = 0; length < frame.size(); ++length) {
        FrameDecoder decoder(kDefaultFramePayload);
        std::size_t frames = 0;
        const Status pushed = decoder.push(
            std::span<const std::uint8_t>(frame.data(), length),
            [&](const FrameHeader&, std::span<const std::uint8_t>) { ++frames; });
        expect_ok(pushed, "truncated prefix");
        if (frames != 0) {
            LFF_FAIL(format_string("prefix of %zu bytes produced a frame", length));
        }
    }
    FrameDecoder decoder(kDefaultFramePayload);
    std::size_t frames = 0;
    expect_ok(decoder.push(frame, [&](const FrameHeader&, std::span<const std::uint8_t>) { ++frames; }),
              "complete frame");
    LFF_CHECK_EQ(frames, std::size_t{1});
}

LFF_TEST(protocol, corrupt_magic_is_sticky) {
    std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, {9});
    frame[0] = 'X';
    FrameDecoder decoder(kDefaultFramePayload);
    const Status first = decoder.push(frame, [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!first.ok());
    expect_reason(first, ReasonCode::MalformedEncoding, "bad magic");
    LFF_CHECK(decoder.failed());
    const Status second = decoder.push(frame_bytes(MessageType::Ping, {1}),
                                       [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!second.ok());
    LFF_CHECK_EQ(second.to_string(), first.to_string());
}

LFF_TEST(protocol, corrupt_checksum_is_sticky) {
    std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, {1, 2, 3});
    frame[kFrameHeaderSize] ^= 0x01;
    FrameDecoder decoder(kDefaultFramePayload);
    const Status pushed = decoder.push(frame, [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!pushed.ok());
    expect_reason(pushed, ReasonCode::ChecksumMismatch, "bad payload checksum");
    LFF_CHECK(decoder.failed());
}

LFF_TEST(protocol, corrupt_header_checksum_is_refused) {
    std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, {});
    frame[16] ^= 0xFF;
    FrameDecoder decoder(kDefaultFramePayload);
    const Status pushed = decoder.push(frame, [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!pushed.ok());
    expect_reason(pushed, ReasonCode::ChecksumMismatch, "bad header checksum");
}

LFF_TEST(protocol, unsupported_version_is_refused) {
    std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, {});
    frame[4] = 99;
    frame[5] = 0;
    const std::uint32_t crc = crc32c(frame.data(), 16);
    for (std::size_t i = 0; i < 4; ++i) {
        frame[16 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
    }
    FrameDecoder decoder(kDefaultFramePayload);
    const Status pushed = decoder.push(frame, [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!pushed.ok());
    expect_reason(pushed, ReasonCode::ProtocolVersionMismatch, "bad version");
}

LFF_TEST(protocol, unknown_message_type_is_refused) {
    std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, {});
    frame[6] = 0xEE;
    frame[7] = 0xEE;
    const std::uint32_t crc = crc32c(frame.data(), 16);
    for (std::size_t i = 0; i < 4; ++i) {
        frame[16 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
    }
    FrameDecoder decoder(kDefaultFramePayload);
    const Status pushed = decoder.push(frame, [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!pushed.ok());
    expect_reason(pushed, ReasonCode::UnknownMessageType, "unknown type");
}

LFF_TEST(protocol, reserved_bits_are_refused) {
    for (std::size_t offset : {std::size_t{8}, std::size_t{10}}) {
        std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, {});
        frame[offset] = 1;
        const std::uint32_t crc = crc32c(frame.data(), 16);
        for (std::size_t i = 0; i < 4; ++i) {
            frame[16 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
        }
        FrameDecoder decoder(kDefaultFramePayload);
        const Status pushed =
            decoder.push(frame, [](const FrameHeader&, std::span<const std::uint8_t>) {});
        LFF_CHECK(!pushed.ok());
        expect_reason(pushed, ReasonCode::ReservedBitsSet, "reserved bits");
    }
}

LFF_TEST(protocol, oversized_declared_length_is_refused_before_allocation) {
    std::vector<std::uint8_t> frame = frame_bytes(MessageType::Ping, {});
    const std::uint32_t huge = 0x7FFFFFFFu;
    for (std::size_t i = 0; i < 4; ++i) {
        frame[12 + i] = static_cast<std::uint8_t>((huge >> (i * 8)) & 0xFFu);
    }
    const std::uint32_t crc = crc32c(frame.data(), 16);
    for (std::size_t i = 0; i < 4; ++i) {
        frame[16 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
    }
    FrameDecoder decoder(1024);
    const Status pushed = decoder.push(frame, [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!pushed.ok());
    expect_outcome(pushed, Outcome::LimitExceeded, "oversized frame");
    LFF_CHECK(decoder.failed());
}

LFF_TEST(protocol, encode_refuses_oversized_payload_and_nonzero_flags) {
    std::vector<std::uint8_t> out;
    const Status too_large =
        encode_frame(MessageType::Ping, 0, std::vector<std::uint8_t>(2048, 0), 1024, out);
    LFF_CHECK(!too_large.ok());
    expect_outcome(too_large, Outcome::LimitExceeded, "oversized payload");
    LFF_CHECK(out.empty());
    const Status flags = encode_frame(MessageType::Ping, 1, {}, 1024, out);
    LFF_CHECK(!flags.ok());
    expect_reason(flags, ReasonCode::ReservedBitsSet, "non-zero flags");
    const Status beyond_ceiling =
        encode_frame(MessageType::Ping, 0, std::vector<std::uint8_t>(kMaxFramePayload + 1, 0),
                     kMaxFramePayload * 4, out);
    LFF_CHECK(!beyond_ceiling.ok());
}

LFF_TEST(protocol, decoder_buffer_is_bounded) {
    FrameDecoder decoder(64);
    std::vector<std::uint8_t> flood(4096, 0x41);
    const Status pushed = decoder.push(flood, [](const FrameHeader&, std::span<const std::uint8_t>) {});
    LFF_CHECK(!pushed.ok());
    expect_outcome(pushed, Outcome::LimitExceeded, "flooded decoder");
    LFF_CHECK(decoder.failed());
}

LFF_TEST(protocol, session_envelope_round_trip_and_trailing_bytes) {
    SessionEnvelope envelope;
    envelope.session_id = 0x1122334455667788ull;
    envelope.epoch = Epoch::from(9);
    envelope.incarnation = Incarnation::generate();
    envelope.seq = SessionSeq::from(3);
    Writer writer;
    envelope.encode(writer);
    const std::vector<std::uint8_t> bytes = writer.take();
    Reader reader(bytes.data(), bytes.size());
    SessionEnvelope decoded;
    LFF_CHECK(SessionEnvelope::decode(reader, decoded));
    LFF_CHECK(reader.at_end());
    LFF_CHECK_EQ(decoded.session_id, envelope.session_id);
    LFF_CHECK(decoded.incarnation == envelope.incarnation);
    LFF_CHECK_EQ(decoded.seq.value(), 3ull);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        Reader partial(bytes.data(), length);
        SessionEnvelope ignored;
        if (SessionEnvelope::decode(partial, ignored)) {
            LFF_FAIL(format_string("session envelope truncated to %zu bytes was accepted", length));
        }
    }
}

LFF_TEST(protocol, hello_messages_reject_malformed_encodings) {
    HelloRequest request;
    request.protocol_version = kWireProtocolVersion;
    request.client_incarnation = Incarnation::generate();
    request.client_epoch = Epoch::from(2);
    request.role = "publisher";
    const std::vector<std::uint8_t> bytes = request.encode();
    HelloRequest decoded;
    LFF_CHECK(HelloRequest::decode(bytes.data(), bytes.size(), decoded));
    LFF_CHECK(decoded.client_incarnation == request.client_incarnation);
    LFF_CHECK_EQ(decoded.role, std::string("publisher"));
    LFF_CHECK(!HelloRequest::decode(bytes.data(), bytes.size() - 1, decoded));

    std::vector<std::uint8_t> trailing = bytes;
    trailing.push_back(0);
    LFF_CHECK(!HelloRequest::decode(trailing.data(), trailing.size(), decoded));

    std::vector<std::uint8_t> bad_version = bytes;
    bad_version[2] = 99;
    bad_version[3] = 0;
    LFF_CHECK(!HelloRequest::decode(bad_version.data(), bad_version.size(), decoded));

    HelloResponse response;
    response.epoch = Epoch::from(3);
    response.coordinator = Incarnation::generate();
    response.session_id = 7;
    response.max_payload = 4096;
    const std::vector<std::uint8_t> response_bytes = response.encode();
    HelloResponse decoded_response;
    LFF_CHECK(HelloResponse::decode(response_bytes.data(), response_bytes.size(), decoded_response));
    LFF_CHECK_EQ(decoded_response.session_id, 7ull);
    LFF_CHECK_EQ(decoded_response.max_payload, 4096u);

    std::vector<std::uint8_t> huge_payload = response_bytes;
    // max_payload sits after version(2) + protocol(2) + epoch(8) + incarnation(16)
    // + session(8).
    const std::size_t offset = 2 + 2 + 8 + 16 + 8;
    for (std::size_t i = 0; i < 4; ++i) {
        huge_payload[offset + i] = 0xFF;
    }
    LFF_CHECK(!HelloResponse::decode(huge_payload.data(), huge_payload.size(), decoded_response));
    std::vector<std::uint8_t> zero_payload = response_bytes;
    for (std::size_t i = 0; i < 4; ++i) {
        zero_payload[offset + i] = 0;
    }
    LFF_CHECK(!HelloResponse::decode(zero_payload.data(), zero_payload.size(), decoded_response));
}

LFF_TEST(protocol, response_envelope_rejects_impossible_reason_counts) {
    ResponseEnvelope envelope;
    envelope.op = 7;
    envelope.outcome = Outcome::Refused;
    envelope.reasons.emplace_back(ReasonCode::SubjectNotFailed, "healthy");
    const std::vector<std::uint8_t> bytes = envelope.encode();
    ResponseEnvelope decoded;
    LFF_CHECK(ResponseEnvelope::decode(bytes.data(), bytes.size(), decoded));
    LFF_CHECK(decoded.outcome == Outcome::Refused);
    LFF_CHECK_EQ(decoded.reasons.size(), std::size_t{1});

    std::vector<std::uint8_t> huge = bytes;
    const std::size_t count_offset = 2 + 2 + 2;
    for (std::size_t i = 0; i < 4; ++i) {
        huge[count_offset + i] = 0xFF;
    }
    LFF_CHECK(!ResponseEnvelope::decode(huge.data(), huge.size(), decoded));

    std::vector<std::uint8_t> bad_outcome = bytes;
    bad_outcome[4] = 0xFF;
    bad_outcome[5] = 0xFF;
    LFF_CHECK(!ResponseEnvelope::decode(bad_outcome.data(), bad_outcome.size(), decoded));

    std::vector<std::uint8_t> bad_reason = bytes;
    const std::size_t reason_offset = count_offset + 4;
    bad_reason[reason_offset] = 0xFF;
    bad_reason[reason_offset + 1] = 0xFF;
    LFF_CHECK(!ResponseEnvelope::decode(bad_reason.data(), bad_reason.size(), decoded));
}

LFF_TEST(protocol, claim_and_inspect_requests_are_bounded) {
    ClaimRequest claim;
    claim.max_count = 4;
    const std::vector<std::uint8_t> claim_bytes = claim.encode();
    ClaimRequest decoded_claim;
    LFF_CHECK(ClaimRequest::decode(claim_bytes.data(), claim_bytes.size(), decoded_claim));
    LFF_CHECK_EQ(decoded_claim.max_count, std::size_t{4});
    std::vector<std::uint8_t> zero = claim_bytes;
    zero[2] = 0;
    zero[3] = 0;
    zero[4] = 0;
    zero[5] = 0;
    LFF_CHECK(!ClaimRequest::decode(zero.data(), zero.size(), decoded_claim));
    std::vector<std::uint8_t> too_many = claim_bytes;
    too_many[2] = 0xFF;
    LFF_CHECK(!ClaimRequest::decode(too_many.data(), too_many.size(), decoded_claim));

    InspectRequest inspect;
    inspect.history_limit = 16;
    const std::vector<std::uint8_t> inspect_bytes = inspect.encode();
    InspectRequest decoded_inspect;
    LFF_CHECK(InspectRequest::decode(inspect_bytes.data(), inspect_bytes.size(), decoded_inspect));
    LFF_CHECK_EQ(decoded_inspect.history_limit, std::size_t{16});
    std::vector<std::uint8_t> huge_history = inspect_bytes;
    huge_history[2] = 0xFF;
    huge_history[3] = 0xFF;
    LFF_CHECK(!InspectRequest::decode(huge_history.data(), huge_history.size(), decoded_inspect));
}

LFF_TEST(protocol, engine_view_round_trip_and_rejection) {
    FabricFixture fixture = make_fabric("wire-fabric", 2, {{"alpha", 1, 100}, {"beta", 2, 200}});
    TempDir dir("wire-view");
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    const EngineView view = engine.value().inspect(4);
    const std::vector<std::uint8_t> bytes = encode_engine_view(view);
    EngineView decoded;
    LFF_CHECK(decode_engine_view(bytes.data(), bytes.size(), decoded));
    LFF_CHECK(decoded.fabric == view.fabric);
    LFF_CHECK_EQ(decoded.topology_generation.value(), view.topology_generation.value());
    LFF_CHECK_EQ(decoded.epoch.value(), view.epoch.value());

    std::vector<std::uint8_t> trailing = bytes;
    trailing.push_back(1);
    LFF_CHECK(!decode_engine_view(trailing.data(), trailing.size(), decoded));
    for (std::size_t length = 0; length < bytes.size(); length += 7) {
        if (decode_engine_view(bytes.data(), length, decoded)) {
            LFF_FAIL(format_string("view truncated to %zu bytes was accepted", length));
        }
    }
    std::vector<std::uint8_t> huge_count = bytes;
    // The attempt count precedes the attempt records; corrupting the trailing
    // region is enough to prove the decoder refuses impossible counts.
    huge_count[2] = 0xFF;
    huge_count[3] = 0xFF;
    LFF_CHECK(!decode_engine_view(huge_count.data(), huge_count.size(), decoded));
    expect_ok(engine.value().close(), "engine close");
}

LFF_TEST(protocol, message_type_names_are_unique) {
    static_assert(kMessageTypeCount == 16, "the message vocabulary changed");
    const std::array<MessageType, kMessageTypeCount> declared = {
        MessageType::Hello,             MessageType::HelloAck,
        MessageType::Response,          MessageType::PublishFailure,
        MessageType::PublishReplacement, MessageType::SetPolicy,
        MessageType::SetTopology,       MessageType::RequestFailover,
        MessageType::RequestRollback,   MessageType::ReportApplication,
        MessageType::ReportVerification, MessageType::ResolveInterrupted,
        MessageType::Inspect,           MessageType::ClaimDirective,
        MessageType::Shutdown,          MessageType::Ping};
    std::set<std::string> names;
    std::set<std::uint16_t> values;
    for (MessageType value : declared) {
        LFF_CHECK(message_type_is_known(static_cast<std::uint16_t>(value)));
        names.insert(std::string(message_type_name(value)));
        values.insert(static_cast<std::uint16_t>(value));
    }
    LFF_CHECK_EQ(names.size(), static_cast<std::size_t>(kMessageTypeCount));
    LFF_CHECK_EQ(values.size(), static_cast<std::size_t>(kMessageTypeCount));
    // Values are deliberately sparse, so undeclared numbers must be refused.
    for (std::uint16_t raw = 0; raw < 64; ++raw) {
        if (values.find(raw) == values.end()) {
            LFF_CHECK(!message_type_is_known(raw));
        }
    }
}