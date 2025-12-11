#pragma once

#include "sak/error_codes.h"
#include <expected>
#include <string>

#ifdef _WIN32

namespace sak {

/**
 * @brief Windows UAC elevation helper
 * 
 * Provides utilities for checking and requesting administrator privileges
 * on Windows systems using UAC (User Account Control).
 * 
 * Thread-Safety: Can be used from any thread
 */
class ElevationManager {
public:
    /**
     * @brief Check if current process is running as administrator
     * @return True if running with admin privileges
     */
    [[nodiscard]] static bool is_elevated() noexcept;

    /**
     * @brief Check if current process can elevate
     * @return True if UAC elevation is available
     */
    [[nodiscard]] static bool can_elevate() noexcept;

    /**
     * @brief Restart current process with administrator privileges
     * @param wait_for_exit If true, wait for elevated process to exit
     * @return Success or error code
     * 
     * @note This will terminate the current process if successful
     */
    static auto restart_elevated(bool wait_for_exit = false)
        -> std::expected<void, sak::error_code>;

    /**
     * @brief Execute command with administrator privileges
     * @param executable Path to executable
     * @param arguments Command line arguments
     * @param wait_for_exit If true, wait for process to complete
     * @return Success or error code
     */
    static auto execute_elevated(
        const std::string& executable,
        const std::string& arguments = "",
        bool wait_for_exit = true)
        -> std::expected<void, sak::error_code>;

    /**
     * @brief Get elevation error message
     * @param error_code Windows error code
     * @return Human-readable error message
     */
    [[nodiscard]] static std::string get_elevation_error_message(unsigned long error_code);

private:
    /**
     * @brief Check if user is in administrators group
     * @return True if user is admin
     */
    [[nodiscard]] static bool is_user_admin() noexcept;

    /**
     * @brief Get current executable path
     * @return Executable path or error
     */
    [[nodiscard]] static auto get_executable_path()
        -> std::expected<std::string, sak::error_code>;

    /**
     * @brief Get command line arguments
     * @return Arguments string
     */
    [[nodiscard]] static std::string get_command_line_args();
};

} // namespace sak

#endif // _WIN32
