// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace lff::test {
namespace {

std::atomic<std::uint64_t> g_counter{0};

std::string unique_token(const std::string& tag) {
    const std::uint64_t counter = g_counter.fetch_add(1);
    std::ostringstream out;
    out << tag << "-" << counter;
    return out.str();
}

std::string quote_argument(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::filesystem::path temp_root() {
    return std::filesystem::temp_directory_path() / "lff-tests";
}

TempDir::TempDir(const std::string& tag) {
    path_ = temp_root() / unique_token(tag);
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    std::filesystem::create_directories(path_, ignored);
}

TempDir::~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

std::filesystem::path TempDir::file(const std::string& name) const {
    return path_ / name;
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        LFF_FAIL("cannot read " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string read_text(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> bytes = read_bytes(path);
    return std::string(bytes.begin(), bytes.end());
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        LFF_FAIL("cannot write " + path.string());
    }
    if (!data.empty()) {
        stream.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    stream.flush();
    if (!stream) {
        LFF_FAIL("short write to " + path.string());
    }
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    write_bytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::uint64_t byte_size(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        LFF_FAIL("cannot stat " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
struct Process::Impl {
    std::filesystem::path output_path;
#if defined(_WIN32)
    HANDLE process{nullptr};
#else
    int process{-1};
#endif
};

Process::~Process() {
    if (impl_ != nullptr) {
#if defined(_WIN32)
        if (impl_->process != nullptr) {
            ::CloseHandle(impl_->process);
        }
#else
        if (impl_->process > 0) {
            int status = 0;
            ::waitpid(impl_->process, &status, WNOHANG);
        }
#endif
        std::error_code ignored;
        std::filesystem::remove(impl_->output_path, ignored);
        delete impl_;
        impl_ = nullptr;
    }
}

Process::Process(Process&& other) noexcept : impl_(other.impl_), pid_(other.pid_) {
    other.impl_ = nullptr;
    other.pid_ = -1;
}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        // Release this process' resources without destroying *this: calling the
        // destructor and then assigning to the dead object would be undefined.
        if (impl_ != nullptr) {
#if defined(_WIN32)
            if (impl_->process != nullptr) {
                ::CloseHandle(impl_->process);
            }
#else
            if (impl_->process > 0) {
                int status = 0;
                ::waitpid(impl_->process, &status, WNOHANG);
            }
#endif
            std::error_code ignored;
            std::filesystem::remove(impl_->output_path, ignored);
            delete impl_;
        }
        impl_ = other.impl_;
        pid_ = other.pid_;
        other.impl_ = nullptr;
        other.pid_ = -1;
    }
    return *this;
}

Process Process::start(const std::vector<std::string>& arguments,
                       const std::vector<std::pair<std::string, std::string>>& environment) {
    if (arguments.empty()) {
        LFF_FAIL("cannot start an empty command");
    }
    Process process;
    process.impl_ = new Impl();
    process.impl_->output_path = temp_root() / unique_token("proc-output") ;

    std::string command;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) {
            command.push_back(' ');
        }
        command += quote_argument(arguments[i]);
    }

    for (const auto& entry : environment) {
#if defined(_WIN32)
        ::SetEnvironmentVariableA(entry.first.c_str(), entry.second.c_str());
#else
        ::setenv(entry.first.c_str(), entry.second.c_str(), 1);
#endif
    }

#if defined(_WIN32)
    std::error_code ignored;
    std::filesystem::create_directories(process.impl_->output_path.parent_path(), ignored);
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE output = ::CreateFileW(process.impl_->output_path.wstring().c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        delete process.impl_;
        process.impl_ = nullptr;
        LFF_FAIL("cannot create process output file");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    std::wstring wide(command.begin(), command.end());
    const BOOL created = ::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    ::CloseHandle(output);
    if (created == FALSE) {
        delete process.impl_;
        process.impl_ = nullptr;
        LFF_FAIL("cannot start process: " + arguments.front());
    }
    ::CloseHandle(info.hThread);
    process.impl_->process = info.hProcess;
    process.pid_ = static_cast<int>(info.dwProcessId);
#else
    std::filesystem::create_directories(process.impl_->output_path.parent_path());
    const pid_t child = ::fork();
    if (child < 0) {
        delete process.impl_;
        process.impl_ = nullptr;
        LFF_FAIL("cannot fork");
    }
    if (child == 0) {
        const int descriptor = ::open(process.impl_->output_path.c_str(),
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (descriptor >= 0) {
            ::dup2(descriptor, STDOUT_FILENO);
            ::dup2(descriptor, STDERR_FILENO);
        }
        std::vector<char*> argv;
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    process.impl_->process = static_cast<int>(child);
    process.pid_ = static_cast<int>(child);
#endif

    for (const auto& entry : environment) {
#if defined(_WIN32)
        ::SetEnvironmentVariableA(entry.first.c_str(), nullptr);
#else
        ::unsetenv(entry.first.c_str());
#endif
    }
    return process;
}

bool Process::running() const {
    if (impl_ == nullptr || pid_ < 0) {
        return false;
    }
#if defined(_WIN32)
    return ::WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
#else
    int status = 0;
    const pid_t result = ::waitpid(impl_->process, &status, WNOHANG);
    return result == 0;
#endif
}

int Process::wait() {
    if (impl_ == nullptr || pid_ < 0) {
        return -1;
    }
#if defined(_WIN32)
    ::WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(impl_->process, &code);
    return static_cast<int>(code);
#else
    int status = 0;
    ::waitpid(impl_->process, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

void Process::terminate() {
    if (impl_ == nullptr || pid_ < 0) {
        return;
    }
#if defined(_WIN32)
    // Hard kill: the target runs no shutdown path, flush or destructor.
    ::TerminateProcess(impl_->process, 97);
#else
    ::kill(impl_->process, SIGKILL);
#endif
}

std::string Process::captured_output() {
    if (impl_ == nullptr) {
        return {};
    }
    if (!file_exists(impl_->output_path)) {
        return {};
    }
    return read_text(impl_->output_path);
}

ProcessResult run(const std::vector<std::string>& arguments,
                  const std::vector<std::pair<std::string, std::string>>& environment) {
    ProcessResult result;
    try {
        Process process = Process::start(arguments, environment);
        result.started = true;
        result.exit_code = process.wait();
        result.output = process.captured_output();
    } catch (const CaseFailure&) {
        result.started = false;
    }
    return result;
}

bool wait_for_file(const std::filesystem::path& path, std::uint32_t attempts) {
    for (std::uint32_t i = 0; i < attempts; ++i) {
        if (file_exists(path) && byte_size(path) > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::filesystem::path tool_path(const std::string& name) {
    std::filesystem::path directory(LFF_TOOL_DIR);
#if defined(_WIN32)
    return directory / (name + ".exe");
#else
    return directory / name;
#endif
}

std::string source_dir() {
    return std::string(LFF_SOURCE_DIR);
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
FabricFixture make_fabric(
    const std::string& name, std::uint64_t topology_generation,
    const std::vector<std::tuple<std::string, std::uint64_t, std::uint32_t>>& links) {
    FabricFixture fixture;
    const Result<FabricName> fabric = FabricName::parse(name);
    if (!fabric.ok()) {
        LFF_FAIL("fixture fabric name rejected");
    }
    fixture.fabric = fabric.value();
    fixture.policy = FailoverPolicy::defaults();
    fixture.policy.generation = PolicyGeneration::from(1);
    fixture.policy.min_replacement_capacity = 1;
    fixture.topology.generation = TopologyGeneration::from(topology_generation);
    for (const auto& entry : links) {
        const Result<LinkName> link = LinkName::parse(std::get<0>(entry));
        if (!link.ok()) {
            LFF_FAIL("fixture link name rejected");
        }
        LinkDefinition definition;
        definition.link.fabric = fixture.fabric;
        definition.link.link = link.value();
        definition.generation = LinkGeneration::from(std::get<1>(entry));
        definition.capacity_units = std::get<2>(entry);
        fixture.topology.links.push_back(definition);
    }
    fixture.topology.canonicalise();
    return fixture;
}

FailureReport make_failure(const FabricFixture& fixture, const std::string& link,
                           std::uint64_t link_generation, ObservationState state,
                           std::uint32_t confidence, const std::string& publisher,
                           const Incarnation& incarnation, std::uint64_t epoch,
                           std::uint64_t sequence) {
    FailureReport report;
    report.subject.fabric = fixture.fabric;
    report.subject.link = LinkName::parse(link).value();
    report.link_generation = LinkGeneration::from(link_generation);
    report.topology_generation = fixture.topology.generation;
    report.state = state;
    report.confidence = make_confidence(confidence);
    report.stamp.publisher = PublisherName::parse(publisher).value();
    report.stamp.publisher_incarnation = incarnation;
    report.stamp.publisher_epoch = Epoch::from(epoch);
    report.stamp.observation_seq = ObservationSeq::from(sequence);
    return report;
}

ReplacementReport make_replacement(const FabricFixture& fixture, const std::string& link,
                                   std::uint64_t link_generation, ObservationState state,
                                   std::uint32_t confidence, std::uint32_t capacity,
                                   std::uint32_t cost, bool adjacency_authorized,
                                   const std::string& publisher, const Incarnation& incarnation,
                                   std::uint64_t epoch, std::uint64_t sequence) {
    ReplacementReport report;
    report.candidate.fabric = fixture.fabric;
    report.candidate.link = LinkName::parse(link).value();
    report.link_generation = LinkGeneration::from(link_generation);
    report.topology_generation = fixture.topology.generation;
    report.state = state;
    report.confidence = make_confidence(confidence);
    report.capacity_units = capacity;
    report.cost_units = cost;
    report.adjacency_authorized = adjacency_authorized;
    report.stamp.publisher = PublisherName::parse(publisher).value();
    report.stamp.publisher_incarnation = incarnation;
    report.stamp.publisher_epoch = Epoch::from(epoch);
    report.stamp.observation_seq = ObservationSeq::from(sequence);
    return report;
}

ReplacementCandidate candidate(const FabricFixture& fixture, const std::string& link,
                               std::uint64_t expected_generation) {
    ReplacementCandidate entry;
    entry.candidate.fabric = fixture.fabric;
    entry.candidate.link = LinkName::parse(link).value();
    entry.expected_generation = LinkGeneration::from(expected_generation);
    return entry;
}

FailoverRequest failover_request(const FabricFixture& fixture, const std::string& link,
                                 std::uint64_t link_generation,
                                 const std::vector<std::string>& candidates) {
    FailoverRequest request;
    request.subject.fabric = fixture.fabric;
    request.subject.link = LinkName::parse(link).value();
    request.subject_generation = LinkGeneration::from(link_generation);
    for (const std::string& name : candidates) {
        request.candidates.push_back(candidate(fixture, name));
    }
    return request;
}

EngineOptions engine_options(const FabricFixture& fixture, const std::filesystem::path& state_dir) {
    EngineOptions options;
    options.fabric = fixture.fabric;
    options.policy = fixture.policy;
    options.topology = fixture.topology;
    options.state_dir = state_dir;
    return options;
}

ApplicationReport application_for(const FailoverDecision& decision, const Incarnation& applier,
                                  Epoch epoch, bool accepted) {
    ApplicationReport report;
    report.subject = decision.subject;
    report.subject_generation = decision.subject_generation;
    report.attempt_seq = decision.attempt_seq;
    report.attempt_id = compute_attempt_id(decision.fabric, decision.subject,
                                           decision.subject_generation, decision.attempt_seq);
    report.applier_incarnation = applier;
    report.applier_epoch = epoch;
    report.accepted = accepted;
    report.detail = "synthetic applier";
    return report;
}

VerificationReport verification_for(const FailoverDecision& decision, const Incarnation& verifier,
                                    Epoch epoch, bool observed) {
    VerificationReport report;
    report.subject = decision.subject;
    report.subject_generation = decision.subject_generation;
    report.attempt_seq = decision.attempt_seq;
    report.attempt_id = compute_attempt_id(decision.fabric, decision.subject,
                                           decision.subject_generation, decision.attempt_seq);
    report.verifier_incarnation = verifier;
    report.verifier_epoch = epoch;
    report.effect_observed = observed;
    report.detail = "synthetic verifier";
    return report;
}

void expect_ok(const Status& status, const char* what) {
    if (!status.ok()) {
        LFF_FAIL(std::string(what) + " expected Ok but was " + status.to_string());
    }
}

void expect_outcome(const Status& status, Outcome outcome, const char* what) {
    if (status.outcome() != outcome) {
        LFF_FAIL(std::string(what) + " expected outcome " +
                 std::string(outcome_name(outcome)) + " but was " + status.to_string());
    }
}

void expect_reason(const Status& status, ReasonCode code, const char* what) {
    if (!status.has(code)) {
        LFF_FAIL(std::string(what) + " expected reason " + std::string(reason_code_name(code)) +
                 " but was " + status.to_string());
    }
}

FailoverDecision expect_decision(const Result<FailoverDecision>& result, const char* what) {
    if (!result.ok()) {
        LFF_FAIL(std::string(what) + " expected a decision but failed with " +
                 result.status().to_string());
    }
    return result.value();
}

}  // namespace lff::test
