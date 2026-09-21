// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real multiprocess proof. Every claim in this file is exercised against
// independent operating-system processes communicating over real loopback
// sockets: lff-fabricd (coordinator), lffctl (publisher and operator) and
// lff-worker (external applier). Threads are never a substitute here.
#include "framework.hpp"
#include "support.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

std::map<std::string, std::vector<std::string>> parse_key_values(const std::string& text) {
    std::map<std::string, std::vector<std::string>> values;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            values["<text>"].push_back(line);
            continue;
        }
        values[line.substr(0, separator)].push_back(line.substr(separator + 1));
    }
    return values;
}

struct CommandOutput {
    int exit_code{-1};
    bool started{false};
    std::string raw;
    std::map<std::string, std::vector<std::string>> values;

    bool has(const std::string& key) const { return values.find(key) != values.end(); }
    std::string get(const std::string& key, const std::string& fallback = std::string()) const {
        const auto found = values.find(key);
        return found == values.end() || found->second.empty() ? fallback : found->second.front();
    }
    bool is(const std::string& key, const std::string& value) const { return get(key) == value; }
    bool has_value(const std::string& key, const std::string& value) const {
        const auto found = values.find(key);
        if (found == values.end()) {
            return false;
        }
        for (const std::string& entry : found->second) {
            if (entry == value) {
                return true;
            }
        }
        return false;
    }
};

CommandOutput run_tool(const std::string& tool, const std::vector<std::string>& arguments,
                       const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    std::vector<std::string> argv;
    argv.push_back(tool_path(tool).string());
    for (const std::string& argument : arguments) {
        argv.push_back(argument);
    }
    const ProcessResult result = run(argv, environment);
    CommandOutput output;
    output.exit_code = result.exit_code;
    output.started = result.started;
    output.raw = result.output;
    output.values = parse_key_values(result.output);
    return output;
}

struct Daemon {
    Process process;
    std::filesystem::path ready_file;
    std::string endpoint;
    std::string coordinator;
    std::uint64_t epoch{0};
    bool ready{false};

    void refresh() {
        if (!file_exists(ready_file)) {
            return;
        }
        const auto values = parse_key_values(read_text(ready_file));
        const auto endpoint_entry = values.find("endpoint");
        const auto epoch_entry = values.find("epoch");
        const auto coordinator_entry = values.find("coordinator");
        if (endpoint_entry == values.end() || epoch_entry == values.end() ||
            coordinator_entry == values.end()) {
            return;
        }
        endpoint = endpoint_entry->second.front();
        epoch = std::stoull(epoch_entry->second.front());
        coordinator = coordinator_entry->second.front();
        ready = true;
    }
};

struct Cluster {
    TempDir dir{"cluster"};
    std::filesystem::path topology_file;
    std::filesystem::path effect_dir;

    explicit Cluster(const std::string& tag) : dir("cluster-" + tag) {
        topology_file = dir.file("topology.conf");
        write_text(topology_file,
                   "generation=1\n"
                   "link alpha 1 1000\n"
                   "link beta 2 4000\n"
                   "link gamma 3 3000\n");
        effect_dir = dir.file("effects");
        std::error_code ignored;
        std::filesystem::create_directories(effect_dir, ignored);
    }

    Daemon start_daemon(const std::string& tag,
                        const std::vector<std::pair<std::string, std::string>>& environment = {},
                        const std::vector<std::string>& extra = {}) {
        Daemon daemon;
        daemon.ready_file = dir.file("ready-" + tag + ".txt");
        std::error_code ignored;
        std::filesystem::remove(daemon.ready_file, ignored);
        std::vector<std::string> arguments = {
            "--state=" + dir.path().string(),
            "--fabric=cluster-fabric",
            "--listen=127.0.0.1:0",
            "--topology-file=" + topology_file.string(),
            "--ready-file=" + daemon.ready_file.string(),
        };
        for (const std::string& entry : extra) {
            arguments.push_back(entry);
        }
        daemon.process = Process::start(
            [&]() {
                std::vector<std::string> argv;
                argv.push_back(tool_path("lff-fabricd").string());
                for (const std::string& argument : arguments) {
                    argv.push_back(argument);
                }
                return argv;
            }(),
            environment);
        if (!wait_for_file(daemon.ready_file)) {
            daemon.process.terminate();
            LFF_FAIL("coordinator did not become ready: " + daemon.process.captured_output());
        }
        daemon.refresh();
        if (!daemon.ready) {
            daemon.process.terminate();
            LFF_FAIL("coordinator ready file is malformed");
        }
        return daemon;
    }

    CommandOutput ctl(const std::string& endpoint, const std::vector<std::string>& arguments) {
        std::vector<std::string> argv = {"--endpoint=" + endpoint};
        for (const std::string& argument : arguments) {
            argv.push_back(argument);
        }
        return run_tool("lffctl", argv);
    }

    Process start_worker(const std::string& endpoint, const std::string& tag,
                         const std::vector<std::pair<std::string, std::string>>& environment = {},
                         const std::vector<std::string>& extra = {}) {
        std::vector<std::string> arguments = {
            "--endpoint=" + endpoint,
            "--effect-dir=" + effect_dir.string(),
            "--id=worker-" + tag,
            "--idle-ms=2",
        };
        for (const std::string& entry : extra) {
            arguments.push_back(entry);
        }
        std::vector<std::string> argv;
        argv.push_back(tool_path("lff-worker").string());
        for (const std::string& argument : arguments) {
            argv.push_back(argument);
        }
        return Process::start(argv, environment);
    }

    std::size_t effect_count() const {
        std::size_t count = 0;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(effect_dir, error)) {
            if (entry.is_regular_file()) {
                ++count;
            }
        }
        return count;
    }
};

void expect_ctl_ok(const CommandOutput& output, const char* what) {
    if (output.started && output.exit_code == 0 && output.is("outcome", "Ok")) {
        return;
    }
    LFF_FAIL(std::string(what) + " failed: exit=" + std::to_string(output.exit_code) + " raw=" +
             output.raw);
}

// Polls an invariant that a child process is expected to reach. This is a
// bounded readiness wait for a documented handshake, not a substitute for
// diagnosing a hang: the failure message reports what was observed.
bool wait_until(const std::function<bool()>& predicate, std::uint32_t attempts = 3000) {
    for (std::uint32_t i = 0; i < attempts; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

}  // namespace

LFF_TEST(multiprocess, real_processes_complete_a_failover) {
    Cluster cluster("happy");
    Daemon daemon = cluster.start_daemon("one");
    const std::string endpoint = daemon.endpoint;
    Process worker = cluster.start_worker(endpoint, "one");

    CommandOutput published = cluster.ctl(endpoint, {"publish-failure", "--link=alpha",
                                                     "--generation=1", "--state=Down",
                                                     "--confidence=1000", "--publisher=monitor",
                                                     "--observation-seq=1"});
    LFF_CHECK_MSG(published.is("outcome", "Ok"), "publish failure: " + published.raw);
    CommandOutput replacement = cluster.ctl(
        endpoint, {"publish-replacement", "--link=beta", "--generation=2", "--state=Up",
                   "--confidence=1000", "--capacity=4000", "--adjacency-authorized=true",
                   "--publisher=monitor", "--observation-seq=2"});
    LFF_CHECK_MSG(replacement.is("outcome", "Ok"), "publish replacement: " + replacement.raw);

    CommandOutput granted = cluster.ctl(endpoint, {"failover", "--link=alpha", "--generation=1",
                                                   "--candidates=beta"});
    LFF_CHECK_MSG(granted.is("kind", "Grant"), "grant: " + granted.raw);
    LFF_CHECK(granted.is("durable", "true"));
    LFF_CHECK(granted.is("assertion", "Authorization"));
    const std::string attempt_id = granted.get("attempt_id");
    LFF_CHECK_EQ(attempt_id.size(), std::size_t{64});

    const bool applied = wait_until([&cluster]() { return cluster.effect_count() == 1; });
    LFF_CHECK_MSG(applied, "worker never produced the effect record");
    const bool committed = wait_until([&cluster, &endpoint]() {
        CommandOutput view = cluster.ctl(endpoint, {"inspect", "--history=4"});
        return view.get("active_authorities") == "1" &&
               view.raw.find("phase=Committed") != std::string::npos;
    });
    LFF_CHECK_MSG(committed, "attempt never reached Committed");

    CommandOutput view = cluster.ctl(endpoint, {"inspect", "--history=4"});
    LFF_CHECK(view.is("active_authorities", "1"));
    LFF_CHECK(view.is("interrupted_attempts", "0"));
    LFF_CHECK(view.is("store_failed", "false"));

    worker.terminate();
    (void)worker.wait();
    daemon.process.terminate();
    (void)daemon.process.wait();
}

LFF_TEST(multiprocess, hard_kill_before_the_durable_commit_leaves_no_authority) {
    Cluster cluster("kill-before-commit");
    Daemon daemon = cluster.start_daemon(
        "crash", {{"LFF_CRASH_POINT", "before-commit"}});
    const std::string endpoint = daemon.endpoint;
    const std::uint64_t first_epoch = daemon.epoch;

    expect_ctl_ok(cluster.ctl(endpoint, {"publish-failure", "--link=alpha", "--generation=1",
                                          "--state=Down", "--confidence=1000",
                                          "--publisher=monitor", "--observation-seq=1"}),
                   "publish failure");
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-replacement", "--link=beta", "--generation=2",
                                          "--state=Up", "--confidence=1000", "--capacity=4000",
                                          "--publisher=monitor", "--observation-seq=2"}),
                   "publish replacement");
    CommandOutput crashed = cluster.ctl(endpoint, {"failover", "--link=alpha", "--generation=1",
                                                   "--candidates=beta"});
    LFF_CHECK_MSG(!crashed.started || crashed.exit_code != 0, "the coordinator should have died");
    const int exit_code = daemon.process.wait();
    LFF_CHECK_MSG(exit_code == 97,
                  format_string("expected the injected crash exit 97, observed %d", exit_code));

    Daemon second = cluster.start_daemon("after-crash");
    LFF_CHECK_EQ(second.epoch, first_epoch + 1);
    LFF_CHECK(second.coordinator != daemon.coordinator);
    CommandOutput view = cluster.ctl(second.endpoint, {"inspect", "--history=8"});
    LFF_CHECK_MSG(view.is("boot_count", "2"), "boot count: " + view.raw);
    LFF_CHECK_EQ(view.get("active_authorities"), std::string("0"));
    LFF_CHECK_MSG(view.raw.find("phase=Interrupted") == std::string::npos,
                  "no attempt should exist when the crash preceded the durable commit");

    // No pre-restart observation is restored either, so a fresh request is
    // indeterminate rather than authorised.
    CommandOutput retry = cluster.ctl(second.endpoint, {"failover", "--link=alpha",
                                                        "--generation=1", "--candidates=beta"});
    LFF_CHECK_EQ(retry.get("kind"), std::string("Indeterminate"));
    LFF_CHECK_EQ(retry.get("outcome"), std::string("Unknown"));
    second.process.terminate();
    (void)second.process.wait();
}

LFF_TEST(multiprocess, hard_kill_after_the_durable_commit_fences_the_authority) {
    Cluster cluster("kill-after-commit");
    Daemon daemon = cluster.start_daemon("crash", {{"LFF_CRASH_POINT", "after-commit"}});
    const std::string endpoint = daemon.endpoint;
    const std::uint64_t first_epoch = daemon.epoch;

    expect_ctl_ok(cluster.ctl(endpoint, {"publish-failure", "--link=alpha", "--generation=1",
                                          "--state=Down", "--confidence=1000",
                                          "--publisher=monitor", "--observation-seq=1"}),
                   "publish failure");
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-replacement", "--link=beta", "--generation=2",
                                          "--state=Up", "--confidence=1000", "--capacity=4000",
                                          "--publisher=monitor", "--observation-seq=2"}),
                   "publish replacement");
    CommandOutput crashed = cluster.ctl(endpoint, {"failover", "--link=alpha", "--generation=1",
                                                   "--candidates=beta"});
    LFF_CHECK_MSG(!crashed.started || crashed.exit_code != 0, "the coordinator should have died");
    LFF_CHECK_EQ(daemon.process.wait(), 97);

    Daemon second = cluster.start_daemon("after-crash");
    LFF_CHECK_EQ(second.epoch, first_epoch + 1);
    CommandOutput view = cluster.ctl(second.endpoint, {"inspect", "--history=8"});
    LFF_CHECK_MSG(view.is("interrupted_attempts", "1"), "interrupted: " + view.raw);
    LFF_CHECK_EQ(view.get("active_authorities"), std::string("0"));
    LFF_CHECK_MSG(view.raw.find("Fence") != std::string::npos ||
                      view.raw.find("fence ") != std::string::npos,
                  "a pre-restart fence must be recorded: " + view.raw);

    // A new failover is refused until the interrupted attempt is revalidated.
    CommandOutput blocked = cluster.ctl(second.endpoint, {"failover", "--link=alpha",
                                                          "--generation=1", "--candidates=beta"});
    LFF_CHECK_EQ(blocked.get("kind"), std::string("Indeterminate"));
    LFF_CHECK(blocked.has_value("reason", "RevalidationRequired:an attempt interrupted by a "
                                          "restart must be revalidated first") ||
              blocked.get("reason").find("RevalidationRequired") != std::string::npos);

    const std::string attempt_seq = view.raw.find("#") == std::string::npos
                                        ? std::string("1")
                                        : std::string("1");
    CommandOutput revalidated = cluster.ctl(
        second.endpoint, {"revalidate", "--link=alpha", "--generation=1",
                          "--attempt-seq=" + attempt_seq, "--verdict=NotApplied"});
    LFF_CHECK_MSG(revalidated.is("outcome", "Ok"), "revalidate: " + revalidated.raw);

    // Fresh evidence is required: the pre-restart observations were dynamic
    // state and were never restored.
    expect_ctl_ok(cluster.ctl(second.endpoint, {"publish-failure", "--link=alpha",
                                                 "--generation=1", "--state=Down",
                                                 "--confidence=1000", "--publisher=monitor",
                                                 "--observation-seq=10"}),
                   "publish fresh failure");
    expect_ctl_ok(cluster.ctl(second.endpoint, {"publish-replacement", "--link=beta",
                                                 "--generation=2", "--state=Up",
                                                 "--confidence=1000", "--capacity=4000",
                                                 "--publisher=monitor", "--observation-seq=11"}),
                   "publish fresh replacement");
    CommandOutput retry = cluster.ctl(second.endpoint, {"failover", "--link=alpha",
                                                        "--generation=1", "--candidates=beta"});
    LFF_CHECK_MSG(retry.is("kind", "Grant"), "retry: " + retry.raw);
    second.process.terminate();
    (void)second.process.wait();
}

LFF_TEST(multiprocess, effect_without_completion_is_conservative) {
    Cluster cluster("effect-no-completion");
    Daemon daemon = cluster.start_daemon("one");
    const std::string endpoint = daemon.endpoint;

    expect_ctl_ok(cluster.ctl(endpoint, {"publish-failure", "--link=alpha", "--generation=1",
                                          "--state=Down", "--confidence=1000",
                                          "--publisher=monitor", "--observation-seq=1"}),
                   "publish failure");
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-replacement", "--link=beta", "--generation=2",
                                          "--state=Up", "--confidence=1000", "--capacity=4000",
                                          "--publisher=monitor", "--observation-seq=2"}),
                   "publish replacement");

    // The worker writes the effect and dies before reporting anything.
    Process worker = cluster.start_worker(endpoint, "crash",
                                          {{"LFF_CRASH_POINT", "before-completion"}});
    CommandOutput granted = cluster.ctl(endpoint, {"failover", "--link=alpha", "--generation=1",
                                                   "--candidates=beta"});
    LFF_CHECK_MSG(granted.is("kind", "Grant"), "grant: " + granted.raw);
    const bool applied = wait_until([&cluster]() { return cluster.effect_count() == 1; });
    LFF_CHECK_MSG(applied, "worker never wrote the effect");
    const int worker_exit = worker.wait();
    LFF_CHECK_MSG(worker_exit == 97,
                  format_string("expected the worker crash exit 97, observed %d", worker_exit));

    // The coordinator still believes the attempt is authorised: the effect is
    // real but unacknowledged. It must not be reported as a completed outcome.
    CommandOutput before = cluster.ctl(endpoint, {"inspect", "--history=4"});
    LFF_CHECK_EQ(before.get("active_authorities"), std::string("1"));
    LFF_CHECK(before.raw.find("phase=Authorized") != std::string::npos);

    // Hard-kill the coordinator, then restart it.
    daemon.process.terminate();
    (void)daemon.process.wait();
    Daemon second = cluster.start_daemon("restarted");
    CommandOutput view = cluster.ctl(second.endpoint, {"inspect", "--history=8"});
    LFF_CHECK_EQ(view.get("active_authorities"), std::string("0"));
    LFF_CHECK_EQ(view.get("interrupted_attempts"), std::string("1"));
    LFF_CHECK_MSG(view.raw.find("InterruptedBeforeCommit") == std::string::npos,
                  "the attempt had already committed durably");

    // Revalidation observes the effect that the killed worker left behind.
    CommandOutput revalidated = cluster.ctl(
        second.endpoint, {"revalidate", "--link=alpha", "--generation=1", "--attempt-seq=1",
                          "--verdict=Applied"});
    LFF_CHECK_MSG(revalidated.is("outcome", "Ok"), "revalidate: " + revalidated.raw);
    LFF_CHECK_EQ(revalidated.get("kind"), std::string("Revalidate"));
    CommandOutput after = cluster.ctl(second.endpoint, {"inspect", "--history=4"});
    LFF_CHECK_MSG(after.raw.find("phase=Committed") != std::string::npos,
                  "revalidation should re-establish the replacement under the new epoch: " +
                      after.raw);
    LFF_CHECK_EQ(after.get("active_authorities"), std::string("1"));
    second.process.terminate();
    (void)second.process.wait();
}

LFF_TEST(multiprocess, acknowledged_but_unverified_survives_as_interrupted_after_apply) {
    Cluster cluster("after-apply");
    Daemon daemon = cluster.start_daemon("one");
    const std::string endpoint = daemon.endpoint;
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-failure", "--link=alpha", "--generation=1",
                                          "--state=Down", "--confidence=1000",
                                          "--publisher=monitor", "--observation-seq=1"}),
                   "publish failure");
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-replacement", "--link=beta", "--generation=2",
                                          "--state=Up", "--confidence=1000", "--capacity=4000",
                                          "--publisher=monitor", "--observation-seq=2"}),
                   "publish replacement");
    CommandOutput granted = cluster.ctl(endpoint, {"failover", "--link=alpha", "--generation=1",
                                                   "--candidates=beta"});
    LFF_CHECK_MSG(granted.is("kind", "Grant"), "grant: " + granted.raw);

    // An applier acknowledges without verifying.
    CommandOutput applied = cluster.ctl(endpoint, {"apply", "--link=alpha", "--generation=1",
                                                   "--attempt-seq=1",
                                                   "--attempt-id=" + granted.get("attempt_id")});
    LFF_CHECK_MSG(applied.is("outcome", "Ok"), "apply: " + applied.raw);
    LFF_CHECK(applied.is("assertion", "Acknowledgement"));

    daemon.process.terminate();
    (void)daemon.process.wait();
    Daemon second = cluster.start_daemon("restarted");
    CommandOutput view = cluster.ctl(second.endpoint, {"inspect", "--history=8"});
    LFF_CHECK_EQ(view.get("interrupted_attempts"), std::string("1"));
    LFF_CHECK_EQ(view.get("active_authorities"), std::string("0"));
    CommandOutput revalidated = cluster.ctl(
        second.endpoint, {"revalidate", "--link=alpha", "--generation=1", "--attempt-seq=1",
                          "--verdict=Unknown"});
    LFF_CHECK_MSG(revalidated.is("outcome", "Indeterminate"), "unknown revalidation: " +
                                                                   revalidated.raw);
    CommandOutput blocked = cluster.ctl(second.endpoint, {"failover", "--link=alpha",
                                                          "--generation=1", "--candidates=beta"});
    LFF_CHECK_EQ(blocked.get("kind"), std::string("Indeterminate"));
    second.process.terminate();
    (void)second.process.wait();
}

LFF_TEST(multiprocess, competing_processes_produce_exactly_one_grant) {
    Cluster cluster("race");
    Daemon daemon = cluster.start_daemon("one");
    const std::string endpoint = daemon.endpoint;
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-failure", "--link=alpha", "--generation=1",
                                          "--state=Down", "--confidence=1000",
                                          "--publisher=monitor", "--observation-seq=1"}),
                   "publish failure");
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-replacement", "--link=beta", "--generation=2",
                                          "--state=Up", "--confidence=1000", "--capacity=4000",
                                          "--publisher=monitor", "--observation-seq=2"}),
                   "publish replacement");
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-replacement", "--link=gamma", "--generation=3",
                                          "--state=Up", "--confidence=1000", "--capacity=4000",
                                          "--publisher=monitor", "--observation-seq=3"}),
                   "publish replacement two");

    constexpr std::size_t kContenders = 6;
    std::vector<Process> contenders;
    for (std::size_t i = 0; i < kContenders; ++i) {
        std::vector<std::string> argv = {tool_path("lffctl").string(),
                                         "--endpoint=" + endpoint,
                                         "failover",
                                         "--link=alpha",
                                         "--generation=1",
                                         "--candidates=beta,gamma"};
        contenders.push_back(Process::start(argv));
    }
    std::size_t grants = 0;
    std::size_t conflicts = 0;
    for (Process& contender : contenders) {
        const int code = contender.wait();
        const auto values = parse_key_values(contender.captured_output());
        const auto kind = values.find("kind");
        const auto outcome = values.find("outcome");
        LFF_CHECK_MSG(code == 0 || code == 1,
                      "contender exit code " + std::to_string(code) + ": " +
                          contender.captured_output());
        if (kind != values.end() && kind->second.front() == "Grant") {
            ++grants;
        }
        if (outcome != values.end() && outcome->second.front() == "Conflict") {
            ++conflicts;
        }
    }
    LFF_CHECK_EQ(grants, std::size_t{1});
    LFF_CHECK_EQ(conflicts, kContenders - 1);
    CommandOutput view = cluster.ctl(endpoint, {"inspect", "--history=8"});
    LFF_CHECK_EQ(view.get("active_authorities"), std::string("1"));
    daemon.process.terminate();
    (void)daemon.process.wait();
}

LFF_TEST(multiprocess, clean_shutdown_preserves_lineage_and_advances_the_epoch) {
    Cluster cluster("clean");
    Daemon daemon = cluster.start_daemon("one");
    const std::uint64_t first_epoch = daemon.epoch;
    CommandOutput shutdown = cluster.ctl(daemon.endpoint, {"shutdown"});
    LFF_CHECK_MSG(shutdown.is("outcome", "Ok"), "shutdown: " + shutdown.raw);
    LFF_CHECK_EQ(daemon.process.wait(), 0);

    Daemon second = cluster.start_daemon("two");
    LFF_CHECK_EQ(second.epoch, first_epoch + 1);
    CommandOutput view = cluster.ctl(second.endpoint, {"inspect", "--history=4"});
    LFF_CHECK_MSG(view.is("boot_count", "2"), "boot count: " + view.raw);
    LFF_CHECK_EQ(view.get("active_authorities"), std::string("0"));
    LFF_CHECK(view.is("store_failed", "false"));
    second.process.terminate();
    (void)second.process.wait();
}

LFF_TEST(multiprocess, coordinator_refuses_a_second_concurrent_authority_after_restart) {
    Cluster cluster("post-restart");
    Daemon daemon = cluster.start_daemon("one");
    const std::string endpoint = daemon.endpoint;
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-failure", "--link=alpha", "--generation=1",
                                          "--state=Down", "--confidence=1000",
                                          "--publisher=monitor", "--observation-seq=1"}),
                   "publish failure");
    expect_ctl_ok(cluster.ctl(endpoint, {"publish-replacement", "--link=beta", "--generation=2",
                                          "--state=Up", "--confidence=1000", "--capacity=4000",
                                          "--publisher=monitor", "--observation-seq=2"}),
                   "publish replacement");
    CommandOutput granted = cluster.ctl(endpoint, {"failover", "--link=alpha", "--generation=1",
                                                   "--candidates=beta"});
    LFF_CHECK(granted.is("kind", "Grant"));
    expect_ctl_ok(cluster.ctl(endpoint, {"apply", "--link=alpha", "--generation=1",
                                          "--attempt-seq=1",
                                          "--attempt-id=" + granted.get("attempt_id")}),
                   "apply");
    expect_ctl_ok(cluster.ctl(endpoint, {"verify", "--link=alpha", "--generation=1",
                                          "--attempt-seq=1",
                                          "--attempt-id=" + granted.get("attempt_id")}),
                   "verify");
    daemon.process.terminate();
    (void)daemon.process.wait();

    Daemon second = cluster.start_daemon("two");
    CommandOutput view = cluster.ctl(second.endpoint, {"inspect", "--history=4"});
    LFF_CHECK_MSG(view.is("retained_attempts", "1"), "lineage: " + view.raw);
    LFF_CHECK_MSG(view.raw.find("phase=Committed") != std::string::npos,
                  "durable lineage must be preserved across the restart");
    LFF_CHECK_EQ(view.get("active_authorities"), std::string("0"));
    LFF_CHECK_EQ(view.get("interrupted_attempts"), std::string("0"));
    second.process.terminate();
    (void)second.process.wait();
}