// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// lff-fabricd — the Link Failover Fabric coordinator daemon.
//
// Owns one durable lineage directory, one coordinator epoch and one framed
// session service. The daemon is the process whose termination is used to prove
// the restart and fencing semantics.
#include "detail/fault.hpp"
#include "detail/fsutil.hpp"
#include "detail/tool_common.hpp"
#include "lff/engine.hpp"
#include "lff/server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested.store(true);
}

int usage() {
    std::cerr <<
        "usage: lff-fabricd --state=DIR [options]\n"
        "\n"
        "  --state=DIR                 durable lineage directory (required)\n"
        "  --fabric=NAME               fabric name (default: synthetic-fabric)\n"
        "  --listen=HOST:PORT          listen endpoint (default: 127.0.0.1:0)\n"
        "  --topology-file=PATH        topology definition file\n"
        "  --policy-file=PATH          policy definition file\n"
        "  --topology-generation=N     generation for a generated topology\n"
        "  --link=NAME:GEN:CAPACITY    add one generated topology link (repeatable via comma)\n"
        "  --max-sessions=N            concurrent session bound (default 64)\n"
        "  --max-frame=N               maximum frame payload in bytes (default 262144)\n"
        "  --snapshot-every=N          snapshot every N lineage records (default 1024)\n"
        "  --no-snapshot               disable snapshot compaction\n"
        "  --strict-tail               refuse a torn journal tail instead of repairing it\n"
        "  --ready-file=PATH           write host/port/epoch once the service is listening\n"
        "  --exit-after=N              stop after N handled requests (0 = never)\n";
    return lff::tool::kExitUsage;
}

std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == ',') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

bool parse_decimal(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    std::uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        std::uint64_t next = 0;
        if (!lff::checked_mul_u64(value, 10, next) ||
            !lff::checked_add_u64(next, static_cast<std::uint64_t>(ch - '0'), value)) {
            return false;
        }
    }
    out = value;
    return true;
}

std::vector<std::string> split_colon(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == ':') {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    parts.push_back(current);
    return parts;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    const lff::Result<lff::tool::Arguments> parsed = lff::tool::Arguments::parse(arguments);
    if (!parsed.ok()) {
        std::cerr << "argument error: " << parsed.status().to_string() << "\n";
        return lff::tool::kExitUsage;
    }
    const lff::tool::Arguments& args = parsed.value();

    const std::string state = args.get("state");
    if (state.empty()) {
        return usage();
    }
    const lff::Result<lff::FabricName> fabric =
        lff::FabricName::parse(args.get("fabric", "synthetic-fabric"));
    if (!fabric.ok()) {
        std::cerr << "invalid fabric name: " << fabric.status().to_string() << "\n";
        return lff::tool::kExitUsage;
    }
    const lff::Result<lff::Endpoint> endpoint =
        lff::Endpoint::parse(args.get("listen", "127.0.0.1:0"));
    if (!endpoint.ok()) {
        std::cerr << "invalid listen endpoint: " << endpoint.status().to_string() << "\n";
        return lff::tool::kExitUsage;
    }
    const lff::Result<std::uint64_t> max_sessions = args.get_u64("max-sessions", 64);
    const lff::Result<std::uint64_t> max_frame = args.get_u64("max-frame", 262144);
    const lff::Result<std::uint64_t> snapshot_every = args.get_u64("snapshot-every", 1024);
    const lff::Result<std::uint64_t> exit_after = args.get_u64("exit-after", 0);
    const lff::Result<bool> no_snapshot = args.get_bool("no-snapshot", false);
    const lff::Result<bool> strict_tail = args.get_bool("strict-tail", false);
    if (!max_sessions.ok() || !max_frame.ok() || !snapshot_every.ok() || !exit_after.ok() ||
        !no_snapshot.ok() || !strict_tail.ok()) {
        std::cerr << "numeric option error\n";
        return lff::tool::kExitUsage;
    }

    lff::FailoverPolicy policy = lff::FailoverPolicy::defaults();
    policy.generation = lff::PolicyGeneration::from(1);
    if (args.has("policy-file")) {
        const lff::Result<std::string> text = lff::tool::read_text_file(args.get("policy-file"));
        if (!text.ok()) {
            std::cerr << "cannot read policy: " << text.status().to_string() << "\n";
            return lff::tool::kExitUsage;
        }
        if (!lff::failover_policy_parse(text.value(), policy)) {
            std::cerr << "policy file is malformed\n";
            return lff::tool::kExitUsage;
        }
    }

    lff::TopologySnapshot topology;
    topology.generation = lff::TopologyGeneration::from(1);
    if (args.has("topology-file")) {
        const lff::Result<std::string> text = lff::tool::read_text_file(args.get("topology-file"));
        if (!text.ok()) {
            std::cerr << "cannot read topology: " << text.status().to_string() << "\n";
            return lff::tool::kExitUsage;
        }
        lff::Result<lff::TopologySnapshot> parsed_topology =
            lff::tool::parse_topology(text.value(), fabric.value());
        if (!parsed_topology.ok()) {
            std::cerr << "topology file is malformed: " << parsed_topology.status().to_string()
                      << "\n";
            return lff::tool::kExitUsage;
        }
        topology = std::move(parsed_topology.value());
    } else if (args.has("link")) {
        const lff::Result<std::uint64_t> generation = args.get_u64("topology-generation", 1);
        if (!generation.ok()) {
            std::cerr << "invalid topology generation\n";
            return lff::tool::kExitUsage;
        }
        topology.generation = lff::TopologyGeneration::from(generation.value());
        for (const std::string& spec : split_csv(args.get("link"))) {
            const std::vector<std::string> fields = split_colon(spec);
            if (fields.size() != 3) {
                std::cerr << "link specification must be NAME:GEN:CAPACITY\n";
                return lff::tool::kExitUsage;
            }
            const lff::Result<lff::LinkName> link = lff::LinkName::parse(fields[0]);
            if (!link.ok()) {
                std::cerr << "invalid link name\n";
                return lff::tool::kExitUsage;
            }
            std::uint64_t link_generation = 0;
            std::uint64_t capacity = 0;
            if (!parse_decimal(fields[1], link_generation) ||
                !parse_decimal(fields[2], capacity) || link_generation == 0 ||
                capacity > 0xFFFFFFFFull) {
                std::cerr << "link generation and capacity must be decimal and in range\n";
                return lff::tool::kExitUsage;
            }
            lff::LinkDefinition definition;
            definition.link.fabric = fabric.value();
            definition.link.link = link.value();
            definition.generation = lff::LinkGeneration::from(link_generation);
            definition.capacity_units = static_cast<std::uint32_t>(capacity);
            topology.links.push_back(definition);
        }
        topology.canonicalise();
    }

    lff::EngineOptions options;
    options.fabric = fabric.value();
    options.policy = policy;
    options.topology = topology;
    options.state_dir = state;
    options.snapshot_every_records = snapshot_every.value();
    options.enable_snapshot = !no_snapshot.value();
    options.repair_torn_tail = !strict_tail.value();

    lff::Result<lff::Engine> engine = lff::Engine::open(options);
    if (!engine.ok()) {
        std::cerr << "engine open failed: " << engine.status().to_string() << "\n";
        return lff::tool::kExitTransport;
    }

    lff::ServerOptions server_options;
    server_options.endpoint = endpoint.value();
    server_options.engine = &engine.value();
    server_options.max_sessions = static_cast<std::size_t>(max_sessions.value());
    server_options.max_frame_payload = static_cast<std::size_t>(max_frame.value());

    lff::Result<std::unique_ptr<lff::Server>> server = lff::Server::start(server_options);
    if (!server.ok()) {
        std::cerr << "listen failed: " << server.status().to_string() << "\n";
        return lff::tool::kExitTransport;
    }

    const lff::Endpoint bound = server.value()->local_endpoint();
    if (args.has("ready-file")) {
        std::string text;
        text.append("endpoint=").append(bound.to_string()).append("\n");
        text.append("host=").append(bound.host).append("\n");
        text.append("port=").append(std::to_string(bound.port)).append("\n");
        text.append("epoch=").append(std::to_string(engine.value().epoch().value())).append("\n");
        text.append("coordinator=").append(engine.value().incarnation().hex()).append("\n");
        text.append("fault_injection=")
            .append(lff::detail::fault_injection_compiled() ? "enabled" : "disabled")
            .append("\n");
        const std::vector<std::uint8_t> bytes(text.begin(), text.end());
        const lff::Status written =
            lff::detail::write_file_atomic(args.get("ready-file"), bytes);
        if (!written.ok()) {
            std::cerr << "cannot write ready file: " << written.to_string() << "\n";
            return lff::tool::kExitTransport;
        }
    } else {
        std::cout << "endpoint=" << bound.to_string() << "\n";
        std::cout << "epoch=" << engine.value().epoch().value() << "\n";
        std::cout << "coordinator=" << engine.value().incarnation().hex() << "\n";
        std::cout.flush();
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::thread watcher;
    const std::uint64_t request_budget = exit_after.value();
    if (request_budget > 0) {
        watcher = std::thread([&server, request_budget]() {
            while (!g_stop_requested.load()) {
                if (server.value()->requests_handled() >= request_budget) {
                    server.value()->shutdown();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    } else {
        watcher = std::thread([&server]() {
            while (!g_stop_requested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            server.value()->shutdown();
        });
    }

    server.value()->run();
    g_stop_requested.store(true);
    if (watcher.joinable()) {
        watcher.join();
    }
    const lff::Status closed = engine.value().close();
    if (!closed.ok()) {
        std::cerr << "engine close failed: " << closed.to_string() << "\n";
        return lff::tool::kExitTransport;
    }
    return lff::tool::kExitOk;
}
