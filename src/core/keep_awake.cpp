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
// Process-global power request created on demand (PowerCreateRequest). Guarded by
// KeepAwake::s_mutex. Unlike SetThreadExecutionState -- which is thread-affine and is dropped
// when the installing thread exits -- a power request is owned by the PROCESS, so one worker
// thread can install it and any other thread can release it. That is what lets the refcount
// and flag union below be process-global and correct across overlapping worker threads.
HANDLE s_power_request = nullptr;

// Reason string surfaced by `powercfg /requests` while a request is held.
constexpr wchar_t kPowerRequestReason[] = L"SAK Utility operation in progress";

// Create the process power request if one is not already held. Fail closed: return false
// (leaving s_power_request null) when the OS refuses so start() reports the real error.
bool ensurePowerRequest() {
    if (s_power_request != nullptr) {
        return true;
    }
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    // SimpleReasonString is non-const in the SDK struct but the API never mutates it.
    context.Reason.SimpleReasonString = const_cast<wchar_t*>(kPowerRequestReason);
    HANDLE const handle = PowerCreateRequest(&context);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    s_power_request = handle;
    return true;
}

// Install every request type newly present in @p new_flags but not @p old_flags. Fail closed
// on the first PowerSetRequest failure; releasePowerRequest clears whatever was set, so a
// partial application never leaks a stuck request.
bool addPowerRequestTypes(int old_flags, int new_flags) {
    const int added = new_flags & ~old_flags;
    if ((added & static_cast<int>(KeepAwake::PowerRequest::System)) != 0 &&
        PowerSetRequest(s_power_request, PowerRequestSystemRequired) == 0) {
        return false;
    }
    if ((added & static_cast<int>(KeepAwake::PowerRequest::Display)) != 0 &&
        PowerSetRequest(s_power_request, PowerRequestDisplayRequired) == 0) {
        return false;
    }
    return true;
}

// Clear the installed request types and destroy the process power request. Requests are
// cleared explicitly before the handle is closed (MSDN: release every request first).
void releasePowerRequest(int flags) {
    if (s_power_request == nullptr) {
        return;
    }
    if ((flags & static_cast<int>(KeepAwake::PowerRequest::System)) != 0) {
        PowerClearRequest(s_power_request, PowerRequestSystemRequired);
    }
    if ((flags & static_cast<int>(KeepAwake::PowerRequest::Display)) != 0) {
        PowerClearRequest(s_power_request, PowerRequestDisplayRequired);
    }
    CloseHandle(s_power_request);
    s_power_request = nullptr;
}
}  // namespace

unsigned KeepAwake::executionStateForFlags(int power_flags) noexcept {
    EXECUTION_STATE flags = ES_CONTINUOUS;
    if ((power_flags & static_cast<int>(PowerRequest::System)) != 0) {
        flags |= ES_SYSTEM_REQUIRED;
    }
    if ((power_flags & static_cast<int>(PowerRequest::Display)) != 0) {
        flags |= ES_DISPLAY_REQUIRED;
    }
    return static_cast<unsigned>(flags);
}

auto KeepAwake::start(PowerRequest request, const char* reason)
    -> std::expected<void, sak::error_code> {
    const std::lock_guard<std::mutex> lock(s_mutex);

    // A process power request is process-global, so it stays in force regardless of which
    // worker thread later calls stop() -- unlike the old per-thread SetThreadExecutionState,
    // which a second overlapping guard on another thread could silently defeat or leak.
    if (!ensurePowerRequest()) {
        DWORD const error = GetLastError();
        sak::logError("Failed to create power request: error {}", error);
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    const int new_flags = s_active_flags | static_cast<int>(request);
    if (!addPowerRequestTypes(s_active_flags, new_flags)) {
        DWORD const error = GetLastError();
        sak::logError("Failed to set power request: error {}", error);
        // Fail closed: nothing is counted. If this start created the request, drop it so a
        // partially-applied request cannot linger.
        if (s_active_count == 0) {
            releasePowerRequest(new_flags);
        }
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    s_active_flags = new_flags;
    ++s_active_count;
    sak::logInfo("KeepAwake started: {}", reason);

    return {};
}

auto KeepAwake::stop() -> std::expected<void, sak::error_code> {
    const std::lock_guard<std::mutex> lock(s_mutex);

    // A stop() with nothing outstanding is a no-op: never underflow the count or release a
    // request the process does not hold. This fail-closes a stray or doubled stop().
    if (s_active_count <= 0) {
        return {};
    }

    --s_active_count;
    if (s_active_count > 0) {
        // Other guards remain active -- keep the process request installed.
        return {};
    }

    // Final release across the whole process: clear every installed request type and drop the
    // handle so a fresh cycle starts from the newly requested flags only.
    releasePowerRequest(s_active_flags);
    s_active_flags = 0;
    sak::logInfo("KeepAwake stopped");

    return {};
}

bool KeepAwake::isActive() noexcept {
    const std::lock_guard<std::mutex> lock(s_mutex);
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
