// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// lff-worker — the external applier.
//
// The worker is a separate operating-system process. It claims apply directives
// from the coordinator, performs the replacement as a real filesystem effect in
// its own effect directory, reports the acknowledgement, then observes the
// effect and reports the verification. It holds no authority of its own: every
// report it sends carries the attempt identity and the applier incarnation the
// coordinator bound, and the coordinator refuses anything else.
//
// Fault injection: with LFF_CRASH_POINT=before-completion the worker writes the
// effect and then terminates immediately, before reporting. That is the
// "effect exists, completion does not" boundary.
#include "detail/fault.hpp"
#include "detail/tool_common.hpp"
#include "lff/client.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int usage() {
    std::cerr <<
        "usage: lff-worker --endpoint=HOST:PORT --effect-dir=DIR [options]\n"
        "\n"
        "  --endpoint=HOST:PORT   coordinator endpoint (required)\n"
        "  --effect-dir=DIR       directory for applied effect records (required)\n"
        "  --id=NAME              worker name reported to the coordinator\n"
        "  --iterations=N         stop after N claim rounds (0 = run until stopped)\n"
        "  --idle-ms=N            delay between empty claim rounds (default 10)\n"
        "  --reject               refuse every directive instead of applying it\n"
        "  --verify=false         do not report verification after applying\n"
        "  --ready-file=PATH      write readiness once the coordinator is reached\n"
        "  --exit-file=PATH       stop when this file appears\n";
    return lff::tool::kExitUsage;
}

std::string effect_text(const lff::ApplyDirective& directive, const std::string& worker) {
    std::ostringstream out;
    out << "subject=" << directive.subject.to_string() << "\n";
    out << "subject_generation=" << directive.subject_generation.value() << "\n";
    out << "replacement=" << directive.replacement.to_string() << "\n";
    out << "replacement_generation=" << directive.replacement_generation.value() << "\n";
    out << "attempt_seq=" << directive.attempt_seq.value() << "\n";
    out << "attempt_id=" << directive.attempt_id.hex() << "\n";
    out << "authority=" << directive.authority.digest().hex() << "\n";
    out << "worker=" << worker << "\n";
    return out.str();
}

bool write_effect(const std::string& directory, const lff::ApplyDirective& directive,
                  const std::string& worker, std::string& path_out) {
    std::string name;
    name.append(directive.subject.fabric.value());
    name.push_back('_');
    name.append(directive.subject.link.value());
    name.push_back('_');
    name.append(std::to_string(directive.subject_generation.value()));
    name.push_back('_');
    name.append(std::to_string(directive.attempt_seq.value()));
    name.append(".applied");
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path.append(name);
    const std::string text = effect_text(directive, worker);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << text;
    stream.flush();
    if (!stream) {
        return false;
    }
    stream.close();
    path_out = path;
    return true;
}

bool read_effect(const std::string& path, std::string& text_out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    text_out = buffer.str();
    return !text_out.empty();
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
    const std::string endpoint_text = args.get("endpoint");
    const std::string effect_dir = args.get("effect-dir");
    if (endpoint_text.empty() || effect_dir.empty()) {
        return usage();
    }
    const lff::Result<lff::Endpoint> endpoint = lff::Endpoint::parse(endpoint_text);
    if (!endpoint.ok()) {
        std::cerr << "invalid endpoint\n";
        return lff::tool::kExitUsage;
    }
    const lff::Result<std::uint64_t> iterations = args.get_u64("iterations", 0);
    const lff::Result<std::uint64_t> idle_ms = args.get_u64("idle-ms", 10);
    const lff::Result<bool> reject = args.get_bool("reject", false);
    const lff::Result<bool> verify = args.get_bool("verify", true);
    if (!iterations.ok() || !idle_ms.ok() || !reject.ok() || !verify.ok()) {
        std::cerr << "numeric option error\n";
        return lff::tool::kExitUsage;
    }
    const std::string worker_name = args.get("id", "synthetic-worker");

    lff::Result<std::unique_ptr<lff::Client>> client =
        lff::Client::connect(endpoint.value(), "worker", lff::kDefaultFramePayload);
    if (!client.ok()) {
        std::cerr << "connect failed: " << client.status().to_string() << "\n";
        return lff::tool::kExitTransport;
    }
    const lff::Incarnation incarnation = client.value()->incarnation();

    if (args.has("ready-file")) {
        std::string text;
        text.append("endpoint=").append(endpoint.value().to_string()).append("\n");
        text.append("incarnation=").append(incarnation.hex()).append("\n");
        text.append("epoch=").append(std::to_string(client.value()->epoch().value())).append("\n");
        text.append("worker=").append(worker_name).append("\n");
        std::ofstream stream(args.get("ready-file"), std::ios::binary | std::ios::trunc);
        stream << text;
        stream.flush();
        if (!stream) {
            std::cerr << "cannot write ready file\n";
            return lff::tool::kExitTransport;
        }
    } else {
        std::cout << "incarnation=" << incarnation.hex() << "\n";
        std::cout.flush();
    }

    const std::string exit_file = args.get("exit-file");
    std::uint64_t round = 0;
    std::uint64_t applied = 0;
    while (true) {
        ++round;
        lff::Result<lff::ClaimResponse> claimed = client.value()->claim_directives(8);
        if (!claimed.ok()) {
            std::cerr << "claim failed: " << claimed.status().to_string() << "\n";
            return lff::tool::kExitTransport;
        }
        for (const lff::ApplyDirective& directive : claimed.value().directives) {
            if (reject.value()) {
                lff::ApplicationReport report;
                report.subject = directive.subject;
                report.subject_generation = directive.subject_generation;
                report.attempt_seq = directive.attempt_seq;
                report.attempt_id = directive.attempt_id;
                report.applier_incarnation = incarnation;
                report.applier_epoch = client.value()->epoch();
                report.accepted = false;
                report.detail = "synthetic rejection";
                lff::Result<lff::FailoverDecision> decision =
                    client.value()->record_application(report);
                if (!decision.ok()) {
                    std::cerr << "report failed: " << decision.status().to_string() << "\n";
                    return lff::tool::kExitTransport;
                }
                std::cout << "rejected=" << directive.attempt_id.hex() << "\n";
                std::cout.flush();
                continue;
            }

            std::string path;
            if (!write_effect(effect_dir, directive, worker_name, path)) {
                std::cerr << "cannot write effect record\n";
                return lff::tool::kExitTransport;
            }
            ++applied;
            std::cout << "applied=" << path << "\n";
            std::cout.flush();

            // The effect now exists. A crash here leaves an unacknowledged
            // effect, which is exactly the boundary the proofs exercise.
            lff::detail::hit_crash_point(lff::detail::CrashPoint::BeforeCompletion);

            lff::ApplicationReport report;
            report.subject = directive.subject;
            report.subject_generation = directive.subject_generation;
            report.attempt_seq = directive.attempt_seq;
            report.attempt_id = directive.attempt_id;
            report.applier_incarnation = incarnation;
            report.applier_epoch = client.value()->epoch();
            report.accepted = true;
            report.detail = "synthetic apply";
            lff::Result<lff::FailoverDecision> acknowledged =
                client.value()->record_application(report);
            if (!acknowledged.ok()) {
                std::cerr << "acknowledge failed: " << acknowledged.status().to_string() << "\n";
                return lff::tool::kExitTransport;
            }
            std::cout << "acknowledged=" << directive.attempt_id.hex() << "\n";
            std::cout.flush();

            if (!verify.value()) {
                continue;
            }
            std::string observed;
            lff::VerificationReport verification;
            verification.subject = directive.subject;
            verification.subject_generation = directive.subject_generation;
            verification.attempt_seq = directive.attempt_seq;
            verification.attempt_id = directive.attempt_id;
            verification.verifier_incarnation = incarnation;
            verification.verifier_epoch = client.value()->epoch();
            verification.effect_observed = read_effect(path, observed);
            verification.detail = verification.effect_observed ? "effect record read back"
                                                               : "effect record missing";
            lff::Result<lff::FailoverDecision> verified =
                client.value()->record_verification(verification);
            if (!verified.ok()) {
                std::cerr << "verify failed: " << verified.status().to_string() << "\n";
                return lff::tool::kExitTransport;
            }
            std::cout << "verified=" << directive.attempt_id.hex() << "\n";
            std::cout.flush();
        }

        if (iterations.value() > 0 && round >= iterations.value()) {
            break;
        }
        if (!exit_file.empty()) {
            std::ifstream stop(exit_file, std::ios::binary);
            if (stop) {
                break;
            }
        }
        if (claimed.value().directives.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms.value()));
        }
    }
    std::cout << "rounds=" << round << " applied=" << applied << "\n";
    return lff::tool::kExitOk;
}
