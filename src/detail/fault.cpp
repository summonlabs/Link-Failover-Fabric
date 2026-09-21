// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "detail/fault.hpp"

#include <cstddef>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#  include <process.h>
#endif

namespace lff::detail {
namespace {

struct CrashPointName {
    CrashPoint value;
    std::string_view name;
};

constexpr CrashPointName kCrashPointNames[] = {
    {CrashPoint::None, "none"},
    {CrashPoint::BeforeCommit, "before-commit"},
    {CrashPoint::AfterCommit, "after-commit"},
    {CrashPoint::AfterApply, "after-apply"},
    {CrashPoint::BeforeCompletion, "before-completion"},
    {CrashPoint::DuringCheckpoint, "during-checkpoint"},
};

std::string read_environment(const char* name) {
#if defined(_WIN32)
    char* raw = nullptr;
    std::size_t length = 0;
    if (::_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) {
        std::free(raw);
        return {};
    }
    std::string value(raw);
    std::free(raw);
    return value;
#else
    const char* raw = std::getenv(name);
    return raw == nullptr ? std::string() : std::string(raw);
#endif
}

CrashPoint parse_environment() {
#if defined(LFF_ENABLE_FAULT_INJECTION)
    const std::string raw = read_environment("LFF_CRASH_POINT");
    if (raw.empty()) {
        return CrashPoint::None;
    }
    CrashPoint point = CrashPoint::None;
    if (crash_point_parse(raw, point)) {
        return point;
    }
    return CrashPoint::None;
#else
    return CrashPoint::None;
#endif
}

}  // namespace

bool fault_injection_compiled() noexcept {
#if defined(LFF_ENABLE_FAULT_INJECTION)
    return true;
#else
    return false;
#endif
}

CrashPoint configured_crash_point() noexcept {
    static const CrashPoint point = parse_environment();
    return point;
}

bool crash_point_configured(CrashPoint point) noexcept {
    if (point == CrashPoint::None) {
        return false;
    }
    return configured_crash_point() == point;
}

void hit_crash_point(CrashPoint point) {
    if (!crash_point_configured(point)) {
        return;
    }
    // Immediate termination: no destructors, no flush, no shutdown record.
#if defined(_WIN32)
    ::_exit(97);
#else
    std::_Exit(97);
#endif
}

std::string_view crash_point_name(CrashPoint point) noexcept {
    for (const CrashPointName& entry : kCrashPointNames) {
        if (entry.value == point) {
            return entry.name;
        }
    }
    return "unrecognised";
}

bool crash_point_parse(std::string_view text, CrashPoint& out) noexcept {
    for (const CrashPointName& entry : kCrashPointNames) {
        if (entry.name == text) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

}  // namespace lff::detail
