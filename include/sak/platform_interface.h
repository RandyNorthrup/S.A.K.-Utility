/// @file platform_interface.h
/// @brief Platform abstraction layer for OS-specific functionality
/// @details Provides unified interface for platform-specific operations

#ifndef SAK_PLATFORM_INTERFACE_H
#define SAK_PLATFORM_INTERFACE_H

#include "error_codes.h"
#include <filesystem>
#include <expected>
#include <string>
#include <string_view>

namespace sak {
namespace platform {

/// @brief Operating system type
enum class os_type {
    windows,
    macos,
    linux_os,
    unknown
};

/// @brief Get current operating system type
/// @return OS type
[[nodiscard]] constexpr os_type get_os_type() noexcept {
#ifdef _WIN32
    return os_type::windows;
#elif defined(__APPLE__) && defined(__MACH__)
    return os_type::macos;
#elif defined(__linux__)
    return os_type::linux_os;
#else
    return os_type::unknown;
#endif
}

/// @brief Get OS name as string
/// @return OS name
[[nodiscard]] constexpr std::string_view get_os_name() noexcept {
    switch (get_os_type()) {
        case os_type::windows: return "Windows";
        case os_type::macos: return "macOS";
        case os_type::linux_os: return "Linux";
        default: return "Unknown";
    }
}

/// @brief Get user's home directory
/// @return Expected containing home directory path or error code
[[nodiscard]] auto get_home_directory() -> std::expected<std::filesystem::path, error_code>;

/// @brief Get application data directory (platform-specific)
/// @param app_name Application name
/// @return Expected containing app data directory or error code
[[nodiscard]] auto get_app_data_directory(
    std::string_view app_name) -> std::expected<std::filesystem::path, error_code>;

/// @brief Get system configuration directory (platform-specific)
/// @return Expected containing config directory or error code
[[nodiscard]] auto get_system_config_directory() -> std::expected<std::filesystem::path, error_code>;

/// @brief Open file or directory in system file manager
/// @param path Path to open
/// @return Expected containing void or error code
[[nodiscard]] auto open_in_file_manager(
    const std::filesystem::path& path) -> std::expected<void, error_code>;

/// @brief Open URL in default browser
/// @param url URL to open
/// @return Expected containing void or error code
[[nodiscard]] auto open_url(std::string_view url) -> std::expected<void, error_code>;

/// @brief Get system environment variable
/// @param name Variable name
/// @return Expected containing variable value or error code
[[nodiscard]] auto get_env_variable(
    std::string_view name) -> std::expected<std::string, error_code>;

/// @brief Set system environment variable (current process)
/// @param name Variable name
/// @param value Variable value
/// @return Expected containing void or error code
[[nodiscard]] auto set_env_variable(
    std::string_view name,
    std::string_view value) -> std::expected<void, error_code>;

/// @brief Check if running with elevated privileges (admin/root)
/// @return True if elevated
[[nodiscard]] bool is_elevated() noexcept;

/// @brief Get current process ID
/// @return Process ID
[[nodiscard]] int get_process_id() noexcept;

/// @brief Get number of logical CPU cores
/// @return Number of cores
[[nodiscard]] unsigned int get_cpu_count() noexcept;

/// @brief Get total physical memory in bytes
/// @return Total memory or 0 if unable to determine
[[nodiscard]] std::uint64_t get_total_memory() noexcept;

/// @brief Get available physical memory in bytes
/// @return Available memory or 0 if unable to determine
[[nodiscard]] std::uint64_t get_available_memory() noexcept;

/// @brief Platform-specific path separator
[[nodiscard]] constexpr char get_path_separator() noexcept {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

/// @brief Check if filesystem is case-sensitive at given path
/// @param path Path to check
/// @return Expected containing true if case-sensitive or error code
[[nodiscard]] auto is_filesystem_case_sensitive(
    const std::filesystem::path& path) -> std::expected<bool, error_code>;

} // namespace platform
} // namespace sak

#endif // SAK_PLATFORM_INTERFACE_H
