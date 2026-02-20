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
    -> std::expected<void, sak::error_code>
{
    if (s_is_active) {
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
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    s_is_active = true;
    sak::logInfo("KeepAwake started: {}", reason);
    
    return {};
}

auto KeepAwake::stop() -> std::expected<void, sak::error_code>
{
    if (!s_is_active) {
        return {};
    }

    EXECUTION_STATE result = SetThreadExecutionState(ES_CONTINUOUS);
    
    if (result == 0) {
        DWORD error = GetLastError();
        sak::logError("Failed to clear thread execution state: error {}", error);
        return std::unexpected(sak::error_code::platform_not_supported);
    }

    s_is_active = false;
    sak::logInfo("KeepAwake stopped");
    
    return {};
}

bool KeepAwake::isActive() noexcept
{
    return s_is_active;
}

// ============================================================================
// KeepAwakeGuard Implementation
// ============================================================================

KeepAwakeGuard::KeepAwakeGuard(KeepAwake::PowerRequest request, const char* reason)
{
    auto result = KeepAwake::start(request, reason);
    m_is_active = result.has_value();
    
    if (!m_is_active) {
        sak::logWarning("KeepAwakeGuard: Failed to activate keep awake");
    }
}

KeepAwakeGuard::~KeepAwakeGuard()
{
    if (m_is_active) {
        auto result = KeepAwake::stop();
        if (!result) {
            sak::logWarning("KeepAwakeGuard: Failed to deactivate keep awake");
        }
    }
}

} // namespace sak

#endif // _WIN32
