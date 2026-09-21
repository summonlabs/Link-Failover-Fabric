// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shared helpers for the lff command line tools.
#pragma once

#include "lff/decision.hpp"
#include "lff/engine.hpp"
#include "lff/outcome.hpp"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace lff::tool {

// Exit codes shared by every tool.
inline constexpr int kExitOk = 0;         // the operation reported Outcome::Ok
inline constexpr int kExitNegative = 1;   // a well-formed negative result
inline constexpr int kExitUsage = 2;      // the command line is wrong
inline constexpr int kExitTransport = 3;  // the runtime could not be reached

struct Arguments {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    bool has(const std::string& name) const;
    std::string get(const std::string& name, const std::string& fallback = std::string()) const;
    Result<std::uint64_t> get_u64(const std::string& name, std::uint64_t fallback) const;
    Result<bool> get_bool(const std::string& name, bool fallback) const;
    Result<std::uint32_t> get_u32(const std::string& name, std::uint32_t fallback) const;

    static Result<Arguments> parse(const std::vector<std::string>& argv);
};

// Reads a whole text file, refusing anything larger than 1 MiB.
Result<std::string> read_text_file(const std::string& path);

// Parses a topology definition:
//   generation=3
//   link <name> <generation> <capacity_units>
Result<TopologySnapshot> parse_topology(const std::string& text, const FabricName& fabric);

// Deterministic synthetic fixtures. Every fixture produced here is labelled
// SYNTHETIC wherever it is reported.
LFF_API FailureReport synthetic_failure_report(const FabricName& fabric, const std::string& link,
                                               std::uint64_t link_generation,
                                               std::uint64_t topology_generation,
                                               ObservationState state, std::uint32_t confidence,
                                               const std::string& publisher,
                                               Incarnation publisher_incarnation,
                                               std::uint64_t publisher_epoch,
                                               std::uint64_t observation_seq);

LFF_API ReplacementReport synthetic_replacement_report(
    const FabricName& fabric, const std::string& link, std::uint64_t link_generation,
    std::uint64_t topology_generation, ObservationState state, std::uint32_t confidence,
    std::uint32_t capacity_units, std::uint32_t cost_units, bool adjacency_authorized,
    const std::string& publisher, Incarnation publisher_incarnation, std::uint64_t publisher_epoch,
    std::uint64_t observation_seq);

// A deterministic publisher incarnation for (name, epoch). Using the same name
// and epoch always yields the same incarnation, and raising the epoch models a
// publisher restart. This keeps synthetic fixtures reproducible.
LFF_API Incarnation deterministic_incarnation(const std::string& name, std::uint64_t epoch);

void print_status(std::ostream& out, const Status& status);
void print_decision(std::ostream& out, const FailoverDecision& decision);
void print_limits(std::ostream& out, const char* label);

}  // namespace lff::tool
