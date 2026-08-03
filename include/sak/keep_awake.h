// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/error_codes.h"

#include <expected>
#include <mutex>

#ifdef _WIN32

namespace sak {

/**
 * @brief Windows power management utility
 *
 * Prevents system from entering sleep mode during long-running operations.
 * Uses SetThreadExecutionState API to request system stay awake.
 *
 * Thread-Safety: Can be used from any thread
 */
class KeepAwake {
public:
    /**
     * @brief Power request flags
     */
    enum class PowerRequest {
        System = 0x01,   ///< Keep system awake
        Display = 0x02,  ///< Keep display awake
        Both = 0x03      ///< Keep both system and display awake
    };

    /**
     * @brief Start preventing system sleep
     * @param request Type of power request
     * @param reason Optional reason string for logging
     * @return Success or error code
     */
    static auto start(PowerRequest request = PowerRequest::System,
                      const char* reason = "SAK Utility operation in progress")
        -> std::expected<void, sak::error_code>;

    /**
     * @brief Stop preventing system sleep
     * @return Success or error code
     */
    static auto stop() -> std::expected<void, sak::error_code>;

    /**
     * @brief Check if keep awake is currently active
     * @return True if active
     */
    [[nodiscard]] static bool isActive() noexcept;

    /**
     * @brief Translate accumulated PowerRequest bits to the Win32
     *        EXECUTION_STATE bitmask (ES_CONTINUOUS always set).
     * @param power_flags OR of PowerRequest values currently requested.
     * @return EXECUTION_STATE bitmask (returned as unsigned so the header need
     *         not pull in <windows.h>).
     * @note Pure + static so the flag-union logic can be unit tested directly.
     */
    [[nodiscard]] static unsigned executionStateForFlags(int power_flags) noexcept;

private:
    // Serializes every execution-state transition so the refcount, the
    // accumulated flag union, and the SetThreadExecutionState call stay
    // coherent when overlapping guards start/stop from different threads.
    static inline std::mutex s_mutex;

    // Reference count of outstanding requests. The real execution-state request
    // is installed on 0 -> 1 and cleared only on the final 1 -> 0 transition.
    static inline int s_active_count{0};

    // Union of PowerRequest bits requested by outstanding guards. A later
    // request's flags (e.g. Display added after a System-only guard) are OR'd
    // in and re-applied so they are no longer ignored. Accumulates while any
    // guard is active (never downgraded early -- staying awake is the safe
    // direction) and resets to 0 on full release.
    static inline int s_active_flags{0};
};

/**
 * @brief RAII wrapper for KeepAwake
 *
 * Automatically enables keep awake on construction and disables on destruction.
 * Perfect for use within long-running operations.
 */
class KeepAwakeGuard {
public:
    /**
     * @brief Constructor - starts keep awake
     * @param request Type of power request
     * @param reason Optional reason string
     */
    explicit KeepAwakeGuard(KeepAwake::PowerRequest request = KeepAwake::PowerRequest::System,
                            const char* reason = "SAK Utility operation in progress");

    /**
     * @brief Destructor - stops keep awake
     */
    ~KeepAwakeGuard();

    // Disable copy and move
    KeepAwakeGuard(const KeepAwakeGuard&) = delete;
    KeepAwakeGuard& operator=(const KeepAwakeGuard&) = delete;
    KeepAwakeGuard(KeepAwakeGuard&&) = delete;
    KeepAwakeGuard& operator=(KeepAwakeGuard&&) = delete;

    /**
     * @brief Check if guard successfully activated
     * @return True if active
     */
    [[nodiscard]] bool isActive() const noexcept { return m_is_active; }

private:
    bool m_is_active{false};
};

}  // namespace sak

#endif  // _WIN32
