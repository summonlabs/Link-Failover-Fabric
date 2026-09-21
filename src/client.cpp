// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/client.hpp"

#include "detail/socket.hpp"
#include "lff/bytes.hpp"

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace lff {
namespace {

constexpr std::size_t kClientChunk = 64 * 1024;

struct CallResult {
    Outcome outcome{Outcome::Ok};
    std::vector<Reason> reasons;
    std::vector<std::uint8_t> body;
    bool has_body{false};
};

Status transport_failure(ReasonCode code, const char* detail) {
    return Status::failure(Outcome::IoError, code, detail);
}

}  // namespace

struct Client::Impl {
    detail::SocketHandle socket{detail::kInvalidSocket};
    Endpoint endpoint;
    std::string role;
    HelloResponse hello;
    SessionSeq seq;
    Incarnation incarnation;
    std::size_t max_payload{kDefaultFramePayload};
    std::mutex mu;

    Status handshake() {
        HelloRequest request;
        request.protocol_version = kWireProtocolVersion;
        request.client_incarnation = incarnation;
        request.client_epoch = Epoch{};
        request.role = role;
        std::vector<std::uint8_t> frame;
        Status encoded = encode_frame(MessageType::Hello, 0, request.encode(),
                                            kDefaultFramePayload, frame);
        if (!encoded.ok()) {
            return encoded;
        }
        Status sent = detail::send_all(socket, frame);
        if (!sent.ok()) {
            return sent;
        }
        FrameDecoder decoder(max_payload);
        std::vector<std::uint8_t> chunk(kClientChunk);
        while (true) {
            Result<std::size_t> received = detail::recv_some(socket, chunk);
            if (!received.ok()) {
                return received.status();
            }
            if (received.value() == 0) {
                return Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed,
                                       "the coordinator closed the connection");
            }
            const std::span<const std::uint8_t> slice(chunk.data(), received.value());
            bool have_frame = false;
            FrameHeader header;
            std::vector<std::uint8_t> payload;
            Status pushed = decoder.push(slice, [&](const FrameHeader& decoded,
                                                          std::span<const std::uint8_t> body) {
                if (have_frame) {
                    return;
                }
                header = decoded;
                payload.assign(body.begin(), body.end());
                have_frame = true;
            });
            if (!pushed.ok()) {
                return pushed;
            }
            if (!have_frame) {
                continue;
            }
            if (header.type != MessageType::HelloAck) {
                ResponseEnvelope envelope;
                if (header.type == MessageType::Response &&
                    ResponseEnvelope::decode(payload.data(), payload.size(), envelope)) {
                    Status status = Status::of(envelope.outcome);
                    for (const Reason& reason : envelope.reasons) {
                        status.add(reason.code, reason.detail);
                    }
                    return status;
                }
                return Status::failure(Outcome::Invalid, ReasonCode::UnknownMessageType,
                                       "the coordinator did not answer the handshake");
            }
            HelloResponse response;
            if (!HelloResponse::decode(payload.data(), payload.size(), response)) {
                return Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                       "malformed handshake response");
            }
            if (response.protocol_version != kWireProtocolVersion) {
                return Status::failure(Outcome::Unsupported, ReasonCode::ProtocolVersionMismatch,
                                       "the coordinator speaks a different protocol version");
            }
            hello = response;
            max_payload = response.max_payload;
            seq = SessionSeq{};
            return Status::success();
        }
    }

    Result<CallResult> call(MessageType op, const std::vector<std::uint8_t>& body) {
        SessionSeq next;
        if (!seq.next(next)) {
            return Result<CallResult>::failure(
                Status::failure(Outcome::Exhausted, ReasonCode::CheckedArithmeticOverflow,
                                "session sequence space is exhausted"));
        }
        SessionEnvelope envelope;
        envelope.session_id = hello.session_id;
        envelope.epoch = hello.epoch;
        envelope.incarnation = incarnation;
        envelope.seq = next;

        Writer envelope_writer;
        envelope.encode(envelope_writer);
        std::vector<std::uint8_t> payload = envelope_writer.take();
        payload.insert(payload.end(), body.begin(), body.end());

        std::vector<std::uint8_t> frame;
        Status encoded = encode_frame(op, 0, payload, max_payload, frame);
        if (!encoded.ok()) {
            return Result<CallResult>::failure(encoded);
        }
        const Status sent = detail::send_all(socket, frame);
        if (!sent.ok()) {
            return Result<CallResult>::failure(sent);
        }
        seq = next;

        FrameDecoder decoder(max_payload);
        std::vector<std::uint8_t> chunk(kClientChunk);
        while (true) {
            Result<std::size_t> received = detail::recv_some(socket, chunk);
            if (!received.ok()) {
                return Result<CallResult>::failure(received.status());
            }
            if (received.value() == 0) {
                return Result<CallResult>::failure(
                    Status::failure(Outcome::Closed, ReasonCode::ConnectionClosed,
                                    "the coordinator closed the connection"));
            }
            const std::span<const std::uint8_t> slice(chunk.data(), received.value());
            bool have_frame = false;
            FrameHeader header;
            std::vector<std::uint8_t> payload_out;
            Status pushed = decoder.push(slice, [&](const FrameHeader& decoded,
                                                          std::span<const std::uint8_t> data) {
                if (have_frame) {
                    return;
                }
                header = decoded;
                payload_out.assign(data.begin(), data.end());
                have_frame = true;
            });
            if (!pushed.ok()) {
                return Result<CallResult>::failure(pushed);
            }
            if (!have_frame) {
                continue;
            }
            if (header.type != MessageType::Response) {
                return Result<CallResult>::failure(
                    Status::failure(Outcome::Invalid, ReasonCode::UnknownMessageType,
                                    "the coordinator sent an unexpected frame"));
            }
            ResponseEnvelope envelope_out;
            if (!ResponseEnvelope::decode(payload_out.data(), payload_out.size(), envelope_out)) {
                return Result<CallResult>::failure(
                    Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                    "malformed response envelope"));
            }
            // The envelope re-encodes to exactly its own canonical byte length,
            // which fixes the body offset without any length field in the frame.
            const std::size_t envelope_bytes = envelope_out.encode().size();
            if (envelope_bytes > payload_out.size()) {
                return Result<CallResult>::failure(
                    Status::failure(Outcome::Invalid, ReasonCode::TrailingBytes,
                                    "response envelope is longer than the frame"));
            }
            CallResult result;
            result.outcome = envelope_out.outcome;
            result.reasons = envelope_out.reasons;
            result.body.assign(payload_out.begin() + static_cast<std::ptrdiff_t>(envelope_bytes),
                               payload_out.end());
            result.has_body = !result.body.empty();
            return Result<CallResult>::success(std::move(result));
        }
    }

    Status close_socket() {
        if (socket != detail::kInvalidSocket) {
            detail::shutdown_socket(socket);
            detail::close_socket(socket);
            socket = detail::kInvalidSocket;
        }
        return Status::success();
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
    close();
}

Result<std::unique_ptr<Client>> Client::connect(const Endpoint& endpoint, std::string role,
                                                std::size_t max_frame_payload) {
    if (max_frame_payload == 0 || max_frame_payload > kMaxFramePayload) {
        return Result<std::unique_ptr<Client>>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "max_frame_payload is outside the supported range"));
    }
    Result<detail::SocketHandle> socket = detail::connect_socket(endpoint);
    if (!socket.ok()) {
        Status status = socket.status();
        status.add(ReasonCode::NotListening, endpoint.to_string());
        return Result<std::unique_ptr<Client>>::failure(status);
    }
    std::unique_ptr<Client> client(new Client());
    client->impl_->socket = socket.value();
    client->impl_->endpoint = endpoint;
    client->impl_->role = std::move(role);
    client->impl_->incarnation = Incarnation::generate();
    client->impl_->max_payload = max_frame_payload;
    const Status handshaken = client->impl_->handshake();
    if (!handshaken.ok()) {
        client->impl_->close_socket();
        return Result<std::unique_ptr<Client>>::failure(handshaken);
    }
    return Result<std::unique_ptr<Client>>::success(std::move(client));
}

HelloResponse Client::hello() const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->hello;
}

Epoch Client::epoch() const noexcept {
    return impl_->hello.epoch;
}

Incarnation Client::incarnation() const noexcept {
    return impl_->incarnation;
}

SessionSeq Client::next_seq() const noexcept {
    return impl_->seq;
}

const Endpoint& Client::endpoint() const noexcept {
    return impl_->endpoint;
}

Status Client::refresh_handshake() {
    std::lock_guard<std::mutex> guard(impl_->mu);
    impl_->close_socket();
    Result<detail::SocketHandle> socket = detail::connect_socket(impl_->endpoint);
    if (!socket.ok()) {
        return socket.status();
    }
    impl_->socket = socket.value();
    impl_->incarnation = Incarnation::generate();
    return impl_->handshake();
}

namespace {

Status to_status(const CallResult& result) {
    Status status = Status::of(result.outcome);
    for (const Reason& reason : result.reasons) {
        status.add(reason.code, reason.detail);
    }
    return status;
}

}  // namespace

Status Client::publish_failure_report(const FailureReport& report) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    Writer writer;
    report.encode(writer);
    Result<CallResult> result = impl_->call(MessageType::PublishFailure, writer.take());
    if (!result.ok()) {
        return result.status();
    }
    return to_status(result.value());
}

Status Client::publish_replacement_report(const ReplacementReport& report) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    Writer writer;
    report.encode(writer);
    Result<CallResult> result = impl_->call(MessageType::PublishReplacement, writer.take());
    if (!result.ok()) {
        return result.status();
    }
    return to_status(result.value());
}

Status Client::set_policy(const FailoverPolicy& policy) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    Writer writer;
    policy.encode(writer);
    Result<CallResult> result = impl_->call(MessageType::SetPolicy, writer.take());
    if (!result.ok()) {
        return result.status();
    }
    return to_status(result.value());
}

Status Client::set_topology(const TopologySnapshot& topology) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    Writer writer;
    topology.encode(writer);
    Result<CallResult> result = impl_->call(MessageType::SetTopology, writer.take());
    if (!result.ok()) {
        return result.status();
    }
    return to_status(result.value());
}

#define LFF_CLIENT_DECISION_CALL(name, message, type)                                       \
    Result<FailoverDecision> Client::name(const type& request) {                            \
        std::lock_guard<std::mutex> guard(impl_->mu);                                       \
        Writer writer;                                                                      \
        request.encode(writer);                                                             \
        Result<CallResult> result = impl_->call(message, writer.take());                     \
        if (!result.ok()) {                                                                 \
            return Result<FailoverDecision>::failure(result.status());                       \
        }                                                                                   \
        FailoverDecision decision;                                                          \
        if (!result.value().has_body ||                                                     \
            !decode_decision_payload(result.value().body.data(), result.value().body.size(), \
                                     decision)) {                                           \
            Status status = to_status(result.value());                                      \
            status.add(ReasonCode::MalformedEncoding, "the response carried no decision");   \
            return Result<FailoverDecision>::failure(status);                                \
        }                                                                                   \
        return Result<FailoverDecision>::success(std::move(decision));                       \
    }

LFF_CLIENT_DECISION_CALL(request_failover, MessageType::RequestFailover, FailoverRequest)
LFF_CLIENT_DECISION_CALL(request_rollback, MessageType::RequestRollback, RollbackRequest)
LFF_CLIENT_DECISION_CALL(record_application, MessageType::ReportApplication, ApplicationReport)
LFF_CLIENT_DECISION_CALL(record_verification, MessageType::ReportVerification, VerificationReport)
LFF_CLIENT_DECISION_CALL(resolve_interrupted, MessageType::ResolveInterrupted, RevalidationRequest)

#undef LFF_CLIENT_DECISION_CALL

Result<EngineView> Client::inspect(std::size_t history_limit) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    InspectRequest request;
    request.history_limit = history_limit;
    Result<CallResult> result = impl_->call(MessageType::Inspect, request.encode());
    if (!result.ok()) {
        return Result<EngineView>::failure(result.status());
    }
    EngineView view;
    if (!result.value().has_body ||
        !decode_engine_view(result.value().body.data(), result.value().body.size(), view)) {
        Status status = to_status(result.value());
        status.add(ReasonCode::MalformedEncoding, "the response carried no view");
        return Result<EngineView>::failure(status);
    }
    return Result<EngineView>::success(std::move(view));
}

Result<ClaimResponse> Client::claim_directives(std::size_t max_count) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    ClaimRequest request;
    request.max_count = max_count;
    Result<CallResult> result = impl_->call(MessageType::ClaimDirective, request.encode());
    if (!result.ok()) {
        return Result<ClaimResponse>::failure(result.status());
    }
    ClaimResponse response;
    if (!result.value().has_body ||
        !ClaimResponse::decode(result.value().body.data(), result.value().body.size(), response)) {
        Status status = to_status(result.value());
        status.add(ReasonCode::MalformedEncoding, "the response carried no directives");
        return Result<ClaimResponse>::failure(status);
    }
    return Result<ClaimResponse>::success(std::move(response));
}

Status Client::ping() {
    std::lock_guard<std::mutex> guard(impl_->mu);
    Result<CallResult> result = impl_->call(MessageType::Ping, {});
    if (!result.ok()) {
        return result.status();
    }
    if (result.value().body.size() >= 10) {
        Reader reader(result.value().body.data(), result.value().body.size());
        const std::uint16_t version = reader.u16();
        const std::uint64_t epoch = reader.u64();
        if (reader.ok() && version == kWireProtocolVersion) {
            impl_->hello.epoch = Epoch::from(epoch);
            impl_->hello.coordinator = reader.incarnation();
        }
    }
    return to_status(result.value());
}

Status Client::shutdown_server() {
    std::lock_guard<std::mutex> guard(impl_->mu);
    Result<CallResult> result = impl_->call(MessageType::Shutdown, {});
    if (!result.ok()) {
        return result.status();
    }
    return to_status(result.value());
}

void Client::close() noexcept {
    if (impl_ != nullptr) {
        impl_->close_socket();
    }
}

}  // namespace lff
