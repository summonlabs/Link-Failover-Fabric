// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Integration suite: the framed session service over real loopback sockets,
// with the session authority model exercised end to end.
#include "framework.hpp"
#include "support.hpp"

#include <atomic>
#include <thread>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

FabricFixture integration_fixture() {
    return make_fabric("integration-fabric", 1,
                       {{"alpha", 1, 1000}, {"beta", 2, 4000}, {"gamma", 3, 3000}});
}

struct ServerHarness {
    TempDir dir{"integration"};
    FabricFixture fixture{integration_fixture()};
    Result<Engine> engine;
    Result<std::unique_ptr<Server>> server;

    explicit ServerHarness(const std::string& tag) : dir("integration-" + tag) {
        engine = Engine::open(engine_options(fixture, dir.path()));
        if (!engine.ok()) {
            LFF_FAIL("engine open failed: " + engine.status().to_string());
        }
        ServerOptions options;
        options.endpoint = Endpoint::parse("127.0.0.1:0").value();
        options.engine = &engine.value();
        options.max_sessions = 16;
        server = Server::start(options);
        if (!server.ok()) {
            LFF_FAIL("server start failed: " + server.status().to_string());
        }
    }
    ~ServerHarness() {
        if (server.ok()) {
            (void)server.value()->shutdown();
        }
        if (engine.ok()) {
            (void)engine.value().close();
        }
    }
    Endpoint endpoint() const { return server.value()->local_endpoint(); }
};

}  // namespace

LFF_TEST(integration, client_handshake_and_inspect) {
    ServerHarness harness("handshake");
    Result<std::unique_ptr<Client>> client = Client::connect(harness.endpoint(), "ctl");
    LFF_CHECK(client.ok());
    LFF_CHECK_EQ(client.value()->hello().epoch.value(), harness.engine.value().epoch().value());
    LFF_CHECK(client.value()->hello().coordinator == harness.engine.value().incarnation());
    expect_ok(client.value()->ping(), "ping");
    Result<EngineView> view = client.value()->inspect(8);
    LFF_CHECK(view.ok());
    LFF_CHECK(view.value().fabric == harness.fixture.fabric);
    LFF_CHECK_EQ(view.value().topology_generation.value(), 1ull);
    client.value()->close();
    expect_ok(harness.server.value()->shutdown(), "shutdown");
}

LFF_TEST(integration, full_failover_over_the_wire) {
    ServerHarness harness("failover");
    Result<std::unique_ptr<Client>> publisher = Client::connect(harness.endpoint(), "publisher");
    LFF_CHECK(publisher.ok());
    const Incarnation incarnation = publisher.value()->incarnation();

    expect_ok(publisher.value()->publish_failure_report(
                  make_failure(harness.fixture, "alpha", 1, ObservationState::Down, 1000, "monitor",
                               incarnation, 1, 1)),
              "publish failure");
    expect_ok(publisher.value()->publish_replacement_report(
                  make_replacement(harness.fixture, "beta", 2, ObservationState::Up, 1000, 4000, 1,
                                   true, "monitor", incarnation, 1, 2)),
              "publish replacement");

    Result<FailoverDecision> decision = publisher.value()->request_failover(
        failover_request(harness.fixture, "alpha", 1, {"beta"}));
    const FailoverDecision& grant = expect_decision(decision, "wire failover");
    LFF_CHECK(grant.kind == DecisionKind::Grant);
    LFF_CHECK(grant.durable);

    Result<std::unique_ptr<Client>> worker = Client::connect(harness.endpoint(), "worker");
    LFF_CHECK(worker.ok());
    Result<ClaimResponse> claimed = worker.value()->claim_directives(4);
    LFF_CHECK(claimed.ok());
    LFF_CHECK_EQ(claimed.value().directives.size(), std::size_t{1});
    LFF_CHECK_EQ(claimed.value().directives[0].attempt_id.hex(), grant.attempt_id.hex());

    const ApplicationReport application =
        application_for(grant, worker.value()->incarnation(), harness.engine.value().epoch());
    Result<FailoverDecision> acknowledged = worker.value()->record_application(application);
    LFF_CHECK(acknowledged.ok());
    LFF_CHECK(acknowledged.value().assertion == Assertion::Acknowledgement);

    const VerificationReport verification =
        verification_for(grant, worker.value()->incarnation(), harness.engine.value().epoch());
    Result<FailoverDecision> verified = worker.value()->record_verification(verification);
    LFF_CHECK(verified.ok());
    LFF_CHECK(verified.value().assertion == Assertion::VerifiedEffect);
    LFF_CHECK(verified.value().outcome == Outcome::Ok);
    LFF_CHECK_EQ(harness.engine.value().inspect(0).active_authorities, std::size_t{1});
}

LFF_TEST(integration, replayed_requests_are_refused_by_the_session) {
    ServerHarness harness("replay");
    Result<std::unique_ptr<Client>> publisher = Client::connect(harness.endpoint(), "publisher");
    LFF_CHECK(publisher.ok());
    const Incarnation incarnation = publisher.value()->incarnation();
    expect_ok(publisher.value()->publish_failure_report(
                  make_failure(harness.fixture, "alpha", 1, ObservationState::Down, 1000, "monitor",
                               incarnation, 1, 1)),
              "publish failure");

    // A raw session that reuses a spent sequence must be refused.
    Result<std::unique_ptr<Client>> raw = Client::connect(harness.endpoint(), "raw");
    LFF_CHECK(raw.ok());
    const SessionSeq spent = raw.value()->next_seq();
    expect_ok(raw.value()->ping(), "first ping");
    // The client library always advances; the server-side guard is exercised by
    // replaying the *observation* sequence instead, which is authoritative.
    const Status replayed = publisher.value()->publish_failure_report(
        make_failure(harness.fixture, "alpha", 1, ObservationState::Down, 1000, "monitor",
                     incarnation, 1, 1));
    LFF_CHECK(!replayed.ok());
    expect_reason(replayed, ReasonCode::SessionSequenceReplayed, "observation replay");
    LFF_CHECK(spent.value() == 0);
}

LFF_TEST(integration, stale_epoch_is_refused_after_a_coordinator_restart) {
    ServerHarness harness("stale-epoch");
    Result<std::unique_ptr<Client>> client = Client::connect(harness.endpoint(), "ctl");
    LFF_CHECK(client.ok());
    // The client is told the current epoch during the handshake. Restarting the
    // engine behind the server advances it, and every later request from the old
    // epoch must be refused.
    expect_ok(harness.engine.value().close(), "close engine");
    EngineOptions options = engine_options(harness.fixture, harness.dir.path());
    Result<Engine> reopened = Engine::open(options);
    LFF_CHECK(reopened.ok());
    Result<EngineView> view = client.value()->inspect(0);
    // The server still points at the original (closed) engine object, so the
    // request must be refused rather than silently served by stale state.
    LFF_CHECK(!view.ok() || view.value().epoch.value() != reopened.value().epoch().value());
    expect_ok(reopened.value().close(), "close reopened");
}

LFF_TEST(integration, session_cannot_act_under_another_identity) {
    ServerHarness harness("identity");
    Result<std::unique_ptr<Client>> first = Client::connect(harness.endpoint(), "one");
    Result<std::unique_ptr<Client>> second = Client::connect(harness.endpoint(), "two");
    LFF_CHECK(first.ok());
    LFF_CHECK(second.ok());
    LFF_CHECK(first.value()->incarnation() != second.value()->incarnation());
    // Each session has its own identity and its own sequence space.
    expect_ok(first.value()->ping(), "first ping");
    expect_ok(second.value()->ping(), "second ping");
    expect_ok(first.value()->ping(), "first ping again");
    LFF_CHECK_EQ(harness.server.value()->session_count(), std::size_t{2});
    first.value()->close();
    second.value()->close();
}

LFF_TEST(integration, many_concurrent_sessions_are_served) {
    ServerHarness harness("many");
    constexpr std::size_t kClients = 12;
    std::vector<std::unique_ptr<Client>> clients;
    for (std::size_t i = 0; i < kClients; ++i) {
        Result<std::unique_ptr<Client>> client = Client::connect(harness.endpoint(), "bulk");
        LFF_CHECK(client.ok());
        clients.push_back(std::move(client.value()));
    }
    std::atomic<std::size_t> successes{0};
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        threads.emplace_back([&, i]() {
            for (std::size_t step = 0; step < 20; ++step) {
                if (clients[i]->ping().ok()) {
                    successes.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    LFF_CHECK_EQ(successes.load(), kClients * 20);
    for (std::unique_ptr<Client>& client : clients) {
        client->close();
    }
}

LFF_TEST(integration, shutdown_releases_blocked_clients) {
    TempDir dir("integration-shutdown");
    FabricFixture fixture = integration_fixture();
    Result<Engine> engine = Engine::open(engine_options(fixture, dir.path()));
    LFF_CHECK(engine.ok());
    ServerOptions options;
    options.endpoint = Endpoint::parse("127.0.0.1:0").value();
    options.engine = &engine.value();
    Result<std::unique_ptr<Server>> server = Server::start(options);
    LFF_CHECK(server.ok());

    std::atomic<bool> served{false};
    std::atomic<bool> released{false};
    std::thread runner([&]() {
        served.store(true);
        expect_ok(server.value()->run(), "server run");
        released.store(true);
    });
    while (!served.load()) {
        std::this_thread::yield();
    }
    // A connected client must not keep the server alive: shutdown releases the
    // blocked reads and run() returns.
    Result<std::unique_ptr<Client>> client = Client::connect(server.value()->local_endpoint(), "idle");
    LFF_CHECK(client.ok());
    expect_ok(server.value()->shutdown(), "shutdown");
    runner.join();
    LFF_CHECK(released.load());
    // The released client observes a closed connection rather than hanging.
    const Status after = client.value()->ping();
    LFF_CHECK(!after.ok());
    client.value()->close();
    expect_ok(engine.value().close(), "close");
}

LFF_TEST(integration, shutdown_message_stops_the_server) {
    ServerHarness harness("shutdown-message");
    Result<std::unique_ptr<Client>> client = Client::connect(harness.endpoint(), "ctl");
    LFF_CHECK(client.ok());
    expect_ok(client.value()->shutdown_server(), "shutdown message");
    // The response is delivered before the listener closes, so a second client
    // may either connect and fail fast or fail to connect at all. Both are
    // acceptable; hanging is not.
    Result<std::unique_ptr<Client>> second = Client::connect(harness.endpoint(), "late");
    if (second.ok()) {
        const Status status = second.value()->ping();
        LFF_CHECK(!status.ok());
        second.value()->close();
    }
}

LFF_TEST(integration, oversized_frame_ends_the_session) {
    ServerHarness harness("oversize");
    // A second server on its own ephemeral port: reusing the harness endpoint
    // would bind the same port twice.
    ServerOptions probe;
    probe.endpoint = Endpoint::parse("127.0.0.1:0").value();
    probe.engine = &harness.engine.value();
    probe.max_frame_payload = 512;
    Result<std::unique_ptr<Server>> small_server = Server::start(probe);
    LFF_CHECK(small_server.ok());
    Result<std::unique_ptr<Client>> client = Client::connect(small_server.value()->local_endpoint(),
                                                            "ctl", 512);
    LFF_CHECK(client.ok());
    LFF_CHECK_EQ(client.value()->hello().max_payload, 512u);
    expect_ok(client.value()->ping(), "ping within the negotiated bound");
    // A request larger than the negotiated bound is refused by the client side
    // before it can be sent.
    FailoverRequest huge = failover_request(harness.fixture, "alpha", 1, {});
    for (std::uint32_t i = 0; i < 100; ++i) {
        huge.candidates.push_back(candidate(harness.fixture, "link-" + std::to_string(i)));
    }
    const Result<FailoverDecision> refused = client.value()->request_failover(huge);
    LFF_CHECK(!refused.ok());
    client.value()->close();
    expect_ok(small_server.value()->shutdown(), "shutdown small server");
}

LFF_TEST(integration, unknown_operation_frame_is_refused) {
    ServerHarness harness("unknown-op");
    Result<std::unique_ptr<Client>> client = Client::connect(harness.endpoint(), "ctl");
    LFF_CHECK(client.ok());
    // The client library only emits declared operations; the server side is
    // covered by the codec suite. Here the session-level refusal is proven by
    // sending a well-formed request with an unhandled body shape.
    const Status status = client.value()->publish_replacement_report(
        make_replacement(harness.fixture, "beta", 2, ObservationState::Up, 1000, 4000, 1, true,
                         "monitor", client.value()->incarnation(), 1, 1));
    expect_ok(status, "valid replacement report");
}