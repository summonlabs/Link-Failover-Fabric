// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Crash-point fault injection. This facility exists so that the durability and
// fencing claims can be proven against real process termination at meaningful
// boundaries, instead of being argued from serialization round trips.
//
// It is compiled out entirely when LFF_ENABLE_FAULT_INJECTION is off, and it
// does nothing unless the environment variable LFF_CRASH_POINT names a point.
// A hit terminates the process immediately: no destructors run, no buffers are
// flushed and no shutdown path executes.
#pragma once

#include <string_view>

namespace lff::detail {

enum class CrashPoint : int {
    None = 0,
    BeforeCommit = 1,      // after the request is validated, before the journal append
    AfterCommit = 2,       // after the durable append and flush, before any response
    AfterApply = 3,        // after an applier reports acceptance, before completion
    BeforeCompletion = 4,  // after verification, before the completion record
    DuringCheckpoint = 5,  // between writing the snapshot and rotating the journal
};

bool fault_injection_compiled() noexcept;

// Parses LFF_CRASH_POINT once per process.
CrashPoint configured_crash_point() noexcept;

bool crash_point_configured(CrashPoint point) noexcept;

// Terminates the process with an immediate exit when the configured point
// matches. Returns normally otherwise.
void hit_crash_point(CrashPoint point);

std::string_view crash_point_name(CrashPoint point) noexcept;
bool crash_point_parse(std::string_view text, CrashPoint& out) noexcept;

}  // namespace lff::detail
