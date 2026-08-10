// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/keep_awake.h"

#ifdef _WIN32

#include "sak/logger.h"

#include <windows.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots

namespace sak {

namespace {
// Per-thread count of active KeepAwake requests installed on THIS thread.
// SetThreadExecutionState is thread-affine: a continuous request set on one thread is
// cleared only by that same thread (or when the thread exits). So every starting thread
// installs and clears its OWN request; KeepAwake::s_active_count tracks the process-wide
// total that drives isActive().
thread_local int t_thread_active_count = 0;
}  // namespace

unsigned KeepAwake::executionStateForFlags(int power_flags) noexcept {
    EXECUTION_STATE flags = ES_CONTINUOUS;
    if (power_flags & static_cast<int>(PowerRequest::System)) {
        flags |= ES_SYSTEM_REQUIRED;
    }
    if (power_flags & static_cast<int>(PowerRequest::Display)) {
        flags |= ES_DISPLAY_REQUIRED;
    }
    return static_cast<unsigned>(flags);
}

auto KeepAwake::start(PowerRequest request, const char* reason)
    -> std::expected<void, sak::error_code> {
    std::lock_guard<std::mutex> lock(s_mutex);

    // Install (re-apply) this thread's continuous request on EVERY start. The old code
    // installed only on the first thread's start and let every later (cross-thread) start
    // ride on it -- so if that first thread exited while a second guard was still active,
    // the OS dropped the request and the system could sleep while s_active_count still read
    // active. Installing on each starting thread keeps protection tied to a live thread. The
    // accumulated flag union drives the state so a later Display request is still honored.
    const int new_flags = s_active_flags | static_cast<int>(request);
    const auto state = static_cast<EXECUTION_STATE>(executionStateForFlags(new_flags));
    if (SetThreadExecutionState(state) == 0) {
        DWORD error = GetLastError();
        sak::logError("Failed to set thread execution state: error {}", error);
        // Fail closed: do not count a request whose state was not installed.
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    s_active_flags = new_flags;
    ++t_thread_active_count;
    ++s_active_count;
    sak::logInfo("KeepAwake started: {}", reason);

    return {};
}

auto KeepAwake::stop() -> std::expected<void, sak::error_code> {
    std::lock_guard<std::mutex> lock(s_mutex);

    // A stop() from a thread that installed nothing is a no-op: it must never clear
    // another thread's request or underflow the count. This also fail-closes a stray or
    // stale stop() -- it can only ever release a request THIS thread actually holds.
    if (t_thread_active_count == 0) {
        return {};
    }

    // Not this thread's final release: keep its request installed on this thread.
    if (t_thread_active_count > 1) {
        --t_thread_active_count;
        --s_active_count;
        return {};
    }

    // Final release on THIS thread: clear the continuous request this thread installed.
    if (SetThreadExecutionState(ES_CONTINUOUS) == 0) {
        DWORD error = GetLastError();
        sak::logError("Failed to clear thread execution state: error {}", error);
        // Fail closed: keep the reference (and union) so callers stay awake.
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    t_thread_active_count = 0;
    --s_active_count;
    // Once the last thread across the process releases, drop the flag union so a fresh
    // cycle starts from the newly requested flags only.
    if (s_active_count <= 0) {
        s_active_count = 0;
        s_active_flags = 0;
    }
    sak::logInfo("KeepAwake stopped");

    return {};
}

bool KeepAwake::isActive() noexcept {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_active_count > 0;
}

// ============================================================================
// KeepAwakeGuard Implementation
// ============================================================================

KeepAwakeGuard::KeepAwakeGuard(KeepAwake::PowerRequest request, const char* reason) {
    auto result = KeepAwake::start(request, reason);
    m_is_active = result.has_value();

    if (!m_is_active) {
        sak::logWarning("KeepAwakeGuard: Failed to activate keep awake");
    }
}

KeepAwakeGuard::~KeepAwakeGuard() {
    if (m_is_active) {
        auto result = KeepAwake::stop();
        if (!result) {
            sak::logWarning("KeepAwakeGuard: Failed to deactivate keep awake");
        }
    }
}

}  // namespace sak

#endif  // _WIN32
