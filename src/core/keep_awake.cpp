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

auto KeepAwake::start(PowerRequest request, const char* reason)
    -> std::expected<void, sak::error_code> {
    // Only the first outstanding request installs the real execution state;
    // overlapping guards just bump the count so an early stop() cannot drop
    // a still-active guard.
    if (s_active_count.fetch_add(1, std::memory_order_acq_rel) != 0) {
        sak::logInfo("KeepAwake already active");
        return {};
    }

    EXECUTION_STATE flags = ES_CONTINUOUS;

    if (static_cast<int>(request) & static_cast<int>(PowerRequest::System)) {
        flags |= ES_SYSTEM_REQUIRED;
    }

    if (static_cast<int>(request) & static_cast<int>(PowerRequest::Display)) {
        flags |= ES_DISPLAY_REQUIRED;
    }

    EXECUTION_STATE result = SetThreadExecutionState(flags);

    if (result == 0) {
        DWORD error = GetLastError();
        sak::logError("Failed to set thread execution state: error {}", error);
        // Fail closed: undo the count so state reflects the failed request.
        s_active_count.fetch_sub(1, std::memory_order_acq_rel);
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    sak::logInfo("KeepAwake started: {}", reason);

    return {};
}

auto KeepAwake::stop() -> std::expected<void, sak::error_code> {
    // Release one reference, clamping at zero so a stray stop() never
    // underflows. Only the final release (1 -> 0) clears the real state.
    int prev = s_active_count.load(std::memory_order_acquire);
    while (prev > 0 && !s_active_count.compare_exchange_weak(
                           prev, prev - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {}

    if (prev != 1) {
        return {};
    }

    EXECUTION_STATE result = SetThreadExecutionState(ES_CONTINUOUS);

    if (result == 0) {
        DWORD error = GetLastError();
        sak::logError("Failed to clear thread execution state: error {}", error);
        // Fail closed: restore the reference so callers stay awake.
        s_active_count.fetch_add(1, std::memory_order_acq_rel);
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    sak::logInfo("KeepAwake stopped");

    return {};
}

bool KeepAwake::isActive() noexcept {
    return s_active_count.load(std::memory_order_acquire) > 0;
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
