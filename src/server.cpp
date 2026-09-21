// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "lff/server.hpp"

#include "detail/socket.hpp"
#include "lff/bytes.hpp"
#include "lff/wire.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lff {
namespace {

constexpr std::size_t kReceiveChunk = 64 * 1024;
constexpr int kMaxConsecutiveAcceptFailures = 8;
// How long the accept loop waits before re-checking its stop flag. This is a
// responsiveness bound on shutdown, not a timeout on any operation: the loop
// never gives up on a pending connection, it only re-reads its own state.
constexpr int kAcceptPollMilliseconds = 50;
// The same responsiveness bound for session reads.
constexpr int kSessionPollMilliseconds = 50;

struct SessionState {
    std::uint64_t id{0};
    // The handle is shared between the accept path, the stop path and the
    // session thread. It is atomic and is exchanged away exactly once, so it can
    // be neither closed twice nor used after a descriptor has been recycled.
    std::atomic<detail::SocketHandle> socket{detail::kInvalidSocket};
    std::atomic<bool> closed{false};
    Incarnation client;
    Epoch epoch;
    SessionSeq last_seq;
    bool handshaken{false};

    // Read by the stop path; the value is never reused after serve() closes it.
    detail::SocketHandle handle() const noexcept { return socket.load(); }
};

// Closes a session socket exactly once, from the session thread that owns it.
// The `closed` flag is the single authority for "the descriptor has been
// returned to the operating system", so no descriptor is ever closed twice and
// none is leaked when the stop path and the session thread both finish.
void close_session_socket(const std::shared_ptr<SessionState>& session) noexcept {
    if (session->closed.exchange(true)) {
        return;
    }
    const detail::SocketHandle handle = session->socket.exchange(detail::kInvalidSocket);
    if (handle == detail::kInvalidSocket) {
        return;
    }
    detail::shutdown_socket(handle);
    detail::close_socket(handle);
}

Status send_response(detail::SocketHandle socket, std::uint16_t op, const Status& status,
                     const std::vector<std::uint8_t>& body, std::size_t max_payload) {
    ResponseEnvelope envelope;
    envelope.op = op;
    envelope.outcome = status.outcome();
    envelope.reasons = status.reasons();
    const std::vector<std::uint8_t> head = envelope.encode();
    std::vector<std::uint8_t> payload;
    payload.reserve(head.size() + body.size());
    payload.insert(payload.end(), head.begin(), head.end());
    payload.insert(payload.end(), body.begin(), body.end());
    std::vector<std::uint8_t> frame;
    Status encoded = encode_frame(MessageType::Response, 0, payload, max_payload, frame);
    if (!encoded.ok()) {
        return encoded;
    }
    return detail::send_all(socket, frame);
}

Status send_control(detail::SocketHandle socket, MessageType type,
                    const std::vector<std::uint8_t>& payload, std::size_t max_payload) {
    std::vector<std::uint8_t> frame;
    Status encoded = encode_frame(type, 0, payload, max_payload, frame);
    if (!encoded.ok()) {
        return encoded;
    }
    return detail::send_all(socket, frame);
}

}  // namespace

struct Server::Impl {
    ServerOptions options;
    Engine* engine{nullptr};
    detail::SocketHandle listener{detail::kInvalidSocket};
    Endpoint bound;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> run_active{false};
    std::atomic<bool> bind_ok{false};
    std::atomic<std::uint64_t> requests_handled{0};
    std::atomic<std::uint64_t> sessions_accepted{0};
    std::atomic<std::uint64_t> sessions_rejected{0};
    std::atomic<std::uint64_t> next_session_id{1};
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::thread accept_thread;
    mutable std::mutex registry_mu;
    std::vector<std::shared_ptr<SessionState>> sessions;
    std::vector<std::thread> session_threads;
    bool torn_down{false};

    void request_stop() {
        stop_requested.store(true);
        {
            std::lock_guard<std::mutex> guard(registry_mu);
            // Releasing blocked reads and writes is what makes teardown prompt:
            // no thread is left waiting on a socket that will never answer.
            for (const std::shared_ptr<SessionState>& session : sessions) {
                const detail::SocketHandle handle = session->handle();
                if (handle != detail::kInvalidSocket) {
                    // Shutdown, never close: closing from here could recycle the
                    // descriptor while the session thread is still using it.
                    detail::shutdown_socket(handle);
                }
            }
        }
        detail::shutdown_socket(listener);
        {
            std::lock_guard<std::mutex> guard(wait_mu);
            wait_cv.notify_all();
        }
    }

    void detach_accept_thread() {
        if (accept_thread.joinable() &&
            accept_thread.get_id() != std::this_thread::get_id()) {
            accept_thread.join();
        } else if (accept_thread.joinable()) {
            accept_thread.detach();
        }
    }

    void teardown() {
        {
            std::lock_guard<std::mutex> guard(registry_mu);
            if (torn_down) {
                return;
            }
            torn_down = true;
        }
        request_stop();
        // The listener is shut down (not closed) before the accept thread is
        // joined, so no descriptor can be recycled while that thread is still
        // using it.
        detail::shutdown_socket(listener);
        detach_accept_thread();
        detail::close_socket(listener);
        listener = detail::kInvalidSocket;
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> guard(registry_mu);
            threads.swap(session_threads);
            sessions.clear();
        }
        const std::thread::id self = std::this_thread::get_id();
        for (std::thread& thread : threads) {
            if (!thread.joinable()) {
                continue;
            }
            if (thread.get_id() == self) {
                thread.detach();
            } else {
                thread.join();
            }
        }
        running.store(false);
    }

    void reap_finished_sessions() {
        std::vector<std::thread> finished;
        {
            std::lock_guard<std::mutex> guard(registry_mu);
            for (auto it = sessions.begin(); it != sessions.end();) {
                if ((*it)->id == 0) {
                    it = sessions.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void handle_frame(const std::shared_ptr<SessionState>& session, const FrameHeader& header,
                      std::span<const std::uint8_t> payload);

    void serve(const std::shared_ptr<SessionState>& session);
};

namespace {

// Validates the session authority of one request. The comparison is
// field-by-field and generation-complete: matching session identity is never
// enough, and one session can never act under another session's boot or epoch.
Status validate_envelope(const SessionState& session, const SessionEnvelope& envelope,
                         Epoch current_epoch, bool& sequence_ok) {
    sequence_ok = false;
    if (envelope.session_id != session.id) {
        return Status::failure(Outcome::Conflict, ReasonCode::SessionMismatch,
                               "the request does not belong to this session");
    }
    if (envelope.incarnation != session.client) {
        return Status::failure(Outcome::Conflict, ReasonCode::IncarnationMismatch,
                               "the request does not carry this session's incarnation");
    }
    if (envelope.epoch != current_epoch) {
        Status status = Status::failure(Outcome::Stale, ReasonCode::EpochMismatch,
                                        "the request binds a coordinator epoch that is not current");
        status.add(ReasonCode::StaleAuthority, std::to_string(envelope.epoch.value()));
        return status;
    }
    if (!(session.last_seq < envelope.seq)) {
        Status status = Status::failure(Outcome::Stale, ReasonCode::SessionSequenceReplayed,
                                        "the request sequence does not advance");
        status.add(ReasonCode::SequenceRegression, std::to_string(envelope.seq.value()));
        return status;
    }
    sequence_ok = true;
    return Status::success();
}

}  // namespace

void Server::Impl::handle_frame(const std::shared_ptr<SessionState>& session,
                                const FrameHeader& header,
                                std::span<const std::uint8_t> payload) {
    if (header.type == MessageType::Hello) {
        HelloRequest request;
        if (!HelloRequest::decode(payload.data(), payload.size(), request)) {
            send_response(session->handle(), static_cast<std::uint16_t>(header.type),
                          Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                          "malformed hello"),
                          {}, options.max_frame_payload);
            detail::shutdown_socket(session->handle());
            return;
        }
        if (request.protocol_version != kWireProtocolVersion) {
            send_response(session->handle(), static_cast<std::uint16_t>(header.type),
                          Status::failure(Outcome::Unsupported,
                                          ReasonCode::ProtocolVersionMismatch,
                                          "unsupported protocol version"),
                          {}, options.max_frame_payload);
            detail::shutdown_socket(session->handle());
            return;
        }
        if (session->handshaken) {
            send_response(session->handle(), static_cast<std::uint16_t>(header.type),
                          Status::failure(Outcome::Conflict, ReasonCode::SessionMismatch,
                                          "the session has already completed its handshake"),
                          {}, options.max_frame_payload);
            detail::shutdown_socket(session->handle());
            return;
        }
        session->client = request.client_incarnation;
        session->epoch = engine->epoch();
        session->last_seq = SessionSeq{};
        session->handshaken = true;

        HelloResponse response;
        response.protocol_version = kWireProtocolVersion;
        response.epoch = engine->epoch();
        response.coordinator = engine->incarnation();
        response.session_id = session->id;
        response.max_payload = static_cast<std::uint32_t>(options.max_frame_payload);
        response.server_tick = 0;
        Status sent = send_control(session->handle(), MessageType::HelloAck, response.encode(),
                                         options.max_frame_payload);
        if (!sent.ok()) {
            detail::shutdown_socket(session->handle());
        }
        return;
    }

    if (!session->handshaken) {
        send_response(session->handle(), static_cast<std::uint16_t>(header.type),
                      Status::failure(Outcome::Unauthenticated, ReasonCode::SessionMismatch,
                                      "the session has not completed its handshake"),
                      {}, options.max_frame_payload);
        detail::shutdown_socket(session->handle());
        return;
    }

    const std::size_t envelope_size = 8 + 8 + 16 + 8;
    if (payload.size() < envelope_size) {
        send_response(session->handle(), static_cast<std::uint16_t>(header.type),
                      Status::failure(Outcome::Invalid, ReasonCode::TruncatedFrame,
                                      "request is shorter than the session envelope"),
                      {}, options.max_frame_payload);
        detail::shutdown_socket(session->handle());
        return;
    }
    Reader envelope_reader(payload.data(), payload.size());
    SessionEnvelope envelope;
    if (!SessionEnvelope::decode(envelope_reader, envelope)) {
        send_response(session->handle(), static_cast<std::uint16_t>(header.type),
                      Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                      "malformed session envelope"),
                      {}, options.max_frame_payload);
        detail::shutdown_socket(session->handle());
        return;
    }
    const std::span<const std::uint8_t> body = payload.subspan(envelope_size);

    bool sequence_ok = false;
    const Status authority =
        validate_envelope(*session, envelope, engine->epoch(), sequence_ok);
    if (!authority.ok()) {
        send_response(session->handle(), static_cast<std::uint16_t>(header.type), authority, {},
                      options.max_frame_payload);
        return;
    }

    requests_handled.fetch_add(1);
    Status status = Status::success();
    std::vector<std::uint8_t> response_body;
    bool stop_after = false;

    switch (header.type) {
        case MessageType::PublishFailure: {
            FailureReport report;
            Reader reader(body.data(), body.size());
            if (!FailureReport::decode(reader, report) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed failure report");
                break;
            }
            status = engine->publish_failure_report(report);
            break;
        }
        case MessageType::PublishReplacement: {
            ReplacementReport report;
            Reader reader(body.data(), body.size());
            if (!ReplacementReport::decode(reader, report) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed replacement report");
                break;
            }
            status = engine->publish_replacement_report(report);
            break;
        }
        case MessageType::SetPolicy: {
            FailoverPolicy policy;
            Reader reader(body.data(), body.size());
            if (!FailoverPolicy::decode(reader, policy) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed policy");
                break;
            }
            status = engine->set_policy(policy);
            break;
        }
        case MessageType::SetTopology: {
            TopologySnapshot topology;
            Reader reader(body.data(), body.size());
            if (!TopologySnapshot::decode(reader, topology) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed topology");
                break;
            }
            status = engine->set_topology(topology);
            break;
        }
        case MessageType::RequestFailover: {
            FailoverRequest request;
            Reader reader(body.data(), body.size());
            if (!FailoverRequest::decode(reader, request) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed failover request");
                break;
            }
            Result<FailoverDecision> decision = engine->request_failover(request);
            if (!decision.ok()) {
                status = decision.status();
                break;
            }
            Writer writer;
            decision.value().encode(writer);
            response_body = writer.take();
            status = Status::of(decision.value().outcome);
            break;
        }
        case MessageType::RequestRollback: {
            RollbackRequest request;
            Reader reader(body.data(), body.size());
            if (!RollbackRequest::decode(reader, request) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed rollback request");
                break;
            }
            Result<FailoverDecision> decision = engine->request_rollback(request);
            if (!decision.ok()) {
                status = decision.status();
                break;
            }
            Writer writer;
            decision.value().encode(writer);
            response_body = writer.take();
            status = Status::of(decision.value().outcome);
            break;
        }
        case MessageType::ReportApplication: {
            ApplicationReport report;
            Reader reader(body.data(), body.size());
            if (!ApplicationReport::decode(reader, report) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed application report");
                break;
            }
            Result<FailoverDecision> decision = engine->record_application(report);
            if (!decision.ok()) {
                status = decision.status();
                break;
            }
            Writer writer;
            decision.value().encode(writer);
            response_body = writer.take();
            status = Status::of(decision.value().outcome);
            break;
        }
        case MessageType::ReportVerification: {
            VerificationReport report;
            Reader reader(body.data(), body.size());
            if (!VerificationReport::decode(reader, report) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed verification report");
                break;
            }
            Result<FailoverDecision> decision = engine->record_verification(report);
            if (!decision.ok()) {
                status = decision.status();
                break;
            }
            Writer writer;
            decision.value().encode(writer);
            response_body = writer.take();
            status = Status::of(decision.value().outcome);
            break;
        }
        case MessageType::ResolveInterrupted: {
            RevalidationRequest request;
            Reader reader(body.data(), body.size());
            if (!RevalidationRequest::decode(reader, request) || !reader.at_end()) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed revalidation request");
                break;
            }
            Result<FailoverDecision> decision = engine->resolve_interrupted(request);
            if (!decision.ok()) {
                status = decision.status();
                break;
            }
            Writer writer;
            decision.value().encode(writer);
            response_body = writer.take();
            status = Status::of(decision.value().outcome);
            break;
        }
        case MessageType::Inspect: {
            InspectRequest request;
            if (!InspectRequest::decode(body.data(), body.size(), request)) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed inspect request");
                break;
            }
            response_body = encode_engine_view(engine->inspect(request.history_limit));
            break;
        }
        case MessageType::ClaimDirective: {
            ClaimRequest request;
            if (!ClaimRequest::decode(body.data(), body.size(), request)) {
                status = Status::failure(Outcome::Invalid, ReasonCode::MalformedEncoding,
                                         "malformed claim request");
                break;
            }
            ClaimResponse response;
            response.directives = engine->pending_directives(request.max_count);
            response_body = response.encode();
            break;
        }
        case MessageType::Ping: {
            Writer writer;
            writer.u16(kWireProtocolVersion);
            writer.u64(engine->epoch().value());
            writer.incarnation(engine->incarnation());
            response_body = writer.take();
            break;
        }
        case MessageType::Shutdown: {
            status = Status::success();
            stop_after = options.stop_on_shutdown_message;
            break;
        }
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::Response:
        default:
            status = Status::failure(Outcome::Unsupported, ReasonCode::UnknownMessageType,
                                     "message type is not accepted from a client");
            break;
    }

    // The session sequence advances only for requests that passed the authority
    // check, so a refused replay cannot consume a sequence slot.
    session->last_seq = envelope.seq;

    Status sent = send_response(session->handle(), static_cast<std::uint16_t>(header.type),
                                      status, response_body, options.max_frame_payload);
    if (!sent.ok()) {
        detail::shutdown_socket(session->handle());
        return;
    }
    if (stop_after) {
        request_stop();
        detail::shutdown_socket(session->handle());
    }
}

void Server::Impl::serve(const std::shared_ptr<SessionState>& session) {
    FrameDecoder decoder(options.max_frame_payload);
    std::vector<std::uint8_t> chunk(kReceiveChunk);
    bool close_session = false;
    while (!close_session && !stop_requested.load()) {
        // A blocking recv is not reliably released by shutdown() on every
        // platform, so the loop waits for readability with a bounded timeout and
        // re-reads its own stop flag. Shutdown latency is therefore bounded by
        // the poll interval rather than by a transport timeout.
        const Result<bool> readable =
            detail::wait_readable(session->handle(), kSessionPollMilliseconds);
        if (!readable.ok()) {
            break;
        }
        if (!readable.value()) {
            continue;
        }
        const Result<std::size_t> received = detail::recv_some(session->handle(), chunk);
        if (!received.ok()) {
            break;
        }
        if (received.value() == 0) {
            break;
        }
        const std::span<const std::uint8_t> slice(chunk.data(), received.value());
        const Status pushed = decoder.push(slice, [this, &session, &close_session](
                                                       const FrameHeader& header,
                                                       std::span<const std::uint8_t> payload) {
            handle_frame(session, header, payload);
            if (stop_requested.load()) {
                close_session = true;
            }
        });
        if (!pushed.ok()) {
            // A sticky decode failure ends the session: the stream can no longer
            // be trusted to be frame-aligned.
            break;
        }
    }
    close_session_socket(session);
    {
        std::lock_guard<std::mutex> guard(registry_mu);
        session->id = 0;  // marks the slot free for the reaper
    }
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::~Server() {
    if (impl_ != nullptr) {
        impl_->teardown();
    }
}

Result<std::unique_ptr<Server>> Server::start(const ServerOptions& options) {
    if (options.engine == nullptr) {
        return Result<std::unique_ptr<Server>>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "a server requires an engine"));
    }
    if (options.max_sessions == 0) {
        return Result<std::unique_ptr<Server>>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "max_sessions must be greater than zero"));
    }
    if (options.max_frame_payload == 0 || options.max_frame_payload > kMaxFramePayload) {
        return Result<std::unique_ptr<Server>>::failure(
            Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                            "max_frame_payload is outside the supported range"));
    }
    const Status started = detail::initialise_networking();
    if (!started.ok()) {
        return Result<std::unique_ptr<Server>>::failure(started);
    }
    Endpoint bound;
    Result<detail::SocketHandle> listener =
        detail::create_listener(options.endpoint, options.backlog, bound);
    if (!listener.ok()) {
        return Result<std::unique_ptr<Server>>::failure(listener.status());
    }

    std::unique_ptr<Server> server(new Server());
    server->impl_->options = options;
    server->impl_->engine = options.engine;
    server->impl_->listener = listener.value();
    server->impl_->bound = bound;
    server->impl_->bind_ok.store(true);
    server->impl_->running.store(true);

    Server::Impl* impl = server->impl_.get();
    impl->accept_thread = std::thread([impl]() {
        int consecutive_failures = 0;
        while (!impl->stop_requested.load()) {
            const Result<bool> readable =
                detail::wait_readable(impl->listener, kAcceptPollMilliseconds);
            if (!readable.ok()) {
                if (impl->stop_requested.load()) {
                    break;
                }
                ++consecutive_failures;
                if (consecutive_failures >= kMaxConsecutiveAcceptFailures) {
                    break;
                }
                continue;
            }
            if (!readable.value()) {
                continue;  // nothing pending; re-check the stop flag
            }
            Result<detail::SocketHandle> accepted = detail::accept_socket(impl->listener);
            if (!accepted.ok()) {
                if (impl->stop_requested.load()) {
                    break;
                }
                ++consecutive_failures;
                if (consecutive_failures >= kMaxConsecutiveAcceptFailures) {
                    break;
                }
                continue;
            }
            consecutive_failures = 0;
            {
                std::lock_guard<std::mutex> guard(impl->registry_mu);
                std::size_t live = 0;
                for (const std::shared_ptr<SessionState>& existing : impl->sessions) {
                    if (existing->id != 0) {
                        ++live;
                    }
                }
                if (live >= impl->options.max_sessions) {
                    impl->sessions_rejected.fetch_add(1);
                    detail::close_socket(accepted.value());
                    continue;
                }
                auto session = std::make_shared<SessionState>();
                session->id = impl->next_session_id.fetch_add(1);
                session->socket.store(accepted.value());
                impl->sessions.push_back(session);
                impl->sessions_accepted.fetch_add(1);
                impl->session_threads.emplace_back(
                    [impl, session]() { impl->serve(session); });
            }
            impl->reap_finished_sessions();
        }
    });
    return Result<std::unique_ptr<Server>>::success(std::move(server));
}

Endpoint Server::local_endpoint() const {
    return impl_ == nullptr ? Endpoint{} : impl_->bound;
}

bool Server::is_running() const noexcept {
    return impl_ != nullptr && impl_->running.load();
}

std::size_t Server::session_count() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(impl_->registry_mu);
    std::size_t live = 0;
    for (const std::shared_ptr<SessionState>& session : impl_->sessions) {
        if (session->id != 0) {
            ++live;
        }
    }
    return live;
}

std::uint64_t Server::requests_handled() const noexcept {
    return impl_ == nullptr ? 0 : impl_->requests_handled.load();
}

std::uint64_t Server::sessions_accepted() const noexcept {
    return impl_ == nullptr ? 0 : impl_->sessions_accepted.load();
}

std::uint64_t Server::sessions_rejected() const noexcept {
    return impl_ == nullptr ? 0 : impl_->sessions_rejected.load();
}

bool Server::bind_failed() const noexcept {
    return impl_ == nullptr || !impl_->bind_ok.load();
}

Status Server::run() {
    if (impl_ == nullptr) {
        return Status::failure(Outcome::Closed, ReasonCode::NotListening, "server is not started");
    }
    impl_->run_active.store(true);
    {
        std::unique_lock<std::mutex> lock(impl_->wait_mu);
        impl_->wait_cv.wait(lock, [this]() { return impl_->stop_requested.load(); });
    }
    impl_->teardown();
    impl_->run_active.store(false);
    return Status::success();
}

Status Server::shutdown() {
    if (impl_ == nullptr) {
        return Status::success();
    }
    impl_->request_stop();
    if (!impl_->run_active.load()) {
        impl_->teardown();
    }
    return Status::success();
}

}  // namespace lff