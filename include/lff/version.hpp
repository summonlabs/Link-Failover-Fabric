// Link Failover Fabric — version and durable/wire format identifiers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "lff/export.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace lff {

inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;
inline constexpr std::uint16_t kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

// Durable and on-wire format identifiers. Raising any of these is a breaking
// change: older artefacts are refused with Outcome::Unsupported rather than
// being silently reinterpreted.
inline constexpr std::uint16_t kIdentityFormatVersion = 1;
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;
inline constexpr std::uint16_t kJournalFormatVersion = 1;
inline constexpr std::uint16_t kWireProtocolVersion = 1;

// Human-readable build identification for diagnostics and lineage records.
LFF_API std::string_view build_toolchain() noexcept;
LFF_API std::string_view build_architecture() noexcept;

}  // namespace lff
