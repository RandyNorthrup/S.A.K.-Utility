#pragma once

#include "sak/error_codes.h"
#include <expected>

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
        System = 0x01,           ///< Keep system awake
        Display = 0x02,          ///< Keep display awake
        Both = 0x03              ///< Keep both system and display awake
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
    [[nodiscard]] static bool is_active() noexcept;

private:
    static inline bool s_is_active = false;
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
    explicit KeepAwakeGuard(
        KeepAwake::PowerRequest request = KeepAwake::PowerRequest::System,
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
    [[nodiscard]] bool is_active() const noexcept { return m_is_active; }

private:
    bool m_is_active{false};
};

} // namespace sak

#endif // _WIN32
