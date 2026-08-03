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

    // OR the new request into the accumulated union. Re-apply whenever the
    // union changes (or on first activation) so a later request's flags are no
    // longer ignored -- the old refcount installed only the first request's
    // flags and dropped every later one.
    const int new_flags = s_active_flags | static_cast<int>(request);
    if (s_active_count == 0 || new_flags != s_active_flags) {
        const auto state = static_cast<EXECUTION_STATE>(executionStateForFlags(new_flags));
        if (SetThreadExecutionState(state) == 0) {
            DWORD error = GetLastError();
            sak::logError("Failed to set thread execution state: error {}", error);
            // Fail closed: do not count a request whose state was not installed.
            return std::unexpected(sak::error_code::platform_not_supported);
        }
    }

    s_active_flags = new_flags;
    ++s_active_count;
    sak::logInfo("KeepAwake started: {}", reason);

    return {};
}

auto KeepAwake::stop() -> std::expected<void, sak::error_code> {
    std::lock_guard<std::mutex> lock(s_mutex);

    // A stray stop() with nothing outstanding is a no-op (never underflows).
    if (s_active_count == 0) {
        return {};
    }

    // Still-active guards keep the accumulated state; only the final release
    // (1 -> 0) clears the real execution state.
    if (s_active_count > 1) {
        --s_active_count;
        return {};
    }

    if (SetThreadExecutionState(ES_CONTINUOUS) == 0) {
        DWORD error = GetLastError();
        sak::logError("Failed to clear thread execution state: error {}", error);
        // Fail closed: keep the reference (and union) so callers stay awake.
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    s_active_count = 0;
    s_active_flags = 0;
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
