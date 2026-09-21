// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shared test fixtures and real-process control.
//
// Every fixture produced here is SYNTHETIC: it describes a fabric on paper and
// does not touch a switch, NIC, RDMA device or any other physical hardware.
#pragma once

#include "framework.hpp"
#include "lff/bytes.hpp"
#include "lff/client.hpp"
#include "lff/engine.hpp"
#include "lff/server.hpp"
#include "lff/store.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace lff::test {

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------
std::filesystem::path temp_root();

class TempDir {
public:
    explicit TempDir(const std::string& tag);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    std::filesystem::path file(const std::string& name) const;

private:
    std::filesystem::path path_;
};

bool file_exists(const std::filesystem::path& path);
std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path);
std::string read_text(const std::filesystem::path& path);
void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data);
void write_text(const std::filesystem::path& path, const std::string& text);
std::uint64_t byte_size(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// Real operating-system processes
// ---------------------------------------------------------------------------
struct ProcessResult {
    int exit_code{-1};
    std::string output;
    bool started{false};
};

class Process {
public:
    Process() = default;
    ~Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Starts a process with the inherited environment plus `environment`
    // overrides. Standard output and error are captured into a temporary file.
    static Process start(const std::vector<std::string>& arguments,
                         const std::vector<std::pair<std::string, std::string>>& environment = {});

    bool running() const;
    int wait();                       // blocks until exit, returns the exit code
    void terminate();                 // hard kill: no graceful shutdown path runs
    std::string captured_output();
    int process_id() const noexcept { return pid_; }

private:
    struct Impl;
    Impl* impl_{nullptr};
    int pid_{-1};
};

// Runs a process to completion and returns its result.
ProcessResult run(const std::vector<std::string>& arguments,
                  const std::vector<std::pair<std::string, std::string>>& environment = {});

// Bounded readiness poll for a file that a child process publishes. This is not
// a watchdog for a blocked operation; it waits for a documented handshake.
bool wait_for_file(const std::filesystem::path& path, std::uint32_t attempts = 4000);

std::filesystem::path tool_path(const std::string& name);
std::string source_dir();

// ---------------------------------------------------------------------------
// Synthetic fixtures (labelled SYNTHETIC wherever they are reported)
// ---------------------------------------------------------------------------
struct FabricFixture {
    FabricName fabric;
    FailoverPolicy policy;
    TopologySnapshot topology;
};

FabricFixture make_fabric(const std::string& name, std::uint64_t topology_generation,
                          const std::vector<std::tuple<std::string, std::uint64_t, std::uint32_t>>& links);

FailureReport make_failure(const FabricFixture& fixture, const std::string& link,
                           std::uint64_t link_generation, ObservationState state,
                           std::uint32_t confidence, const std::string& publisher,
                           const Incarnation& incarnation, std::uint64_t epoch,
                           std::uint64_t sequence);

ReplacementReport make_replacement(const FabricFixture& fixture, const std::string& link,
                                   std::uint64_t link_generation, ObservationState state,
                                   std::uint32_t confidence, std::uint32_t capacity,
                                   std::uint32_t cost, bool adjacency_authorized,
                                   const std::string& publisher, const Incarnation& incarnation,
                                   std::uint64_t epoch, std::uint64_t sequence);

ReplacementCandidate candidate(const FabricFixture& fixture, const std::string& link,
                               std::uint64_t expected_generation = 0);

FailoverRequest failover_request(const FabricFixture& fixture, const std::string& link,
                                 std::uint64_t link_generation,
                                 const std::vector<std::string>& candidates);

EngineOptions engine_options(const FabricFixture& fixture, const std::filesystem::path& state_dir);

ApplicationReport application_for(const FailoverDecision& decision, const Incarnation& applier,
                                  Epoch epoch, bool accepted = true);

VerificationReport verification_for(const FailoverDecision& decision, const Incarnation& verifier,
                                    Epoch epoch, bool observed = true);

// ---------------------------------------------------------------------------
// Status helpers
// ---------------------------------------------------------------------------
void expect_ok(const Status& status, const char* what);
void expect_outcome(const Status& status, Outcome outcome, const char* what);
void expect_reason(const Status& status, ReasonCode code, const char* what);

// Returns by value: a reference into the temporary Result would dangle.
FailoverDecision expect_decision(const Result<FailoverDecision>& result, const char* what);

}  // namespace lff::test
