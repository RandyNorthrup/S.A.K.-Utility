/// @file platform_interface.cpp
/// @brief Implementation of platform abstraction layer

#define _CRT_SECURE_NO_WARNINGS
#include "sak/platform_interface.h"
#include "sak/logger.h"
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots
#else
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#endif

namespace sak {
namespace platform {

auto get_home_directory() -> std::expected<std::filesystem::path, error_code> {
    try {
#ifdef _WIN32
        // Try USERPROFILE first (preferred on Windows)
        if (auto profile = std::getenv("USERPROFILE")) {
            return std::filesystem::path(profile);
        }
        
        // Fallback to HOMEDRIVE + HOMEPATH
        if (auto drive = std::getenv("HOMEDRIVE")) {
            if (auto path = std::getenv("HOMEPATH")) {
                return std::filesystem::path(std::string(drive) + path);
            }
        }
        
        log_error("Failed to get home directory: environment variables not set");
        return std::unexpected(error_code::environment_error);
#else
        // Try HOME environment variable first
        if (auto home = std::getenv("HOME")) {
            return std::filesystem::path(home);
        }
        
        // Fallback to getpwuid
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir) {
            return std::filesystem::path(pw->pw_dir);
        }
        
        log_error("Failed to get home directory");
        return std::unexpected(error_code::environment_error);
#endif
    } catch (const std::exception& e) {
        log_error("Exception getting home directory: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

auto get_app_data_directory(
    std::string_view app_name) -> std::expected<std::filesystem::path, error_code> {
    
    try {
#ifdef _WIN32
        // Use %APPDATA% on Windows
        wchar_t* app_data_path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app_data_path))) {
            std::filesystem::path result = app_data_path;
            CoTaskMemFree(app_data_path);
            return result / app_name;
        }
        
        log_error("Failed to get AppData directory");
        return std::unexpected(error_code::environment_error);
#elif defined(__APPLE__)
        // Use ~/Library/Application Support on macOS
        auto home = get_home_directory();
        if (!home) {
            return std::unexpected(home.error());
        }
        return *home / "Library" / "Application Support" / app_name;
#else
        // Use ~/.config on Linux (XDG Base Directory)
        if (auto xdg_config = std::getenv("XDG_CONFIG_HOME")) {
            return std::filesystem::path(xdg_config) / app_name;
        }
        
        auto home = get_home_directory();
        if (!home) {
            return std::unexpected(home.error());
        }
        return *home / ".config" / app_name;
#endif
    } catch (const std::exception& e) {
        log_error("Exception getting app data directory: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

auto get_system_config_directory() -> std::expected<std::filesystem::path, error_code> {
    try {
#ifdef _WIN32
        // Use %ProgramData% on Windows
        if (auto program_data = std::getenv("ProgramData")) {
            return std::filesystem::path(program_data);
        }
        // Fallback: use ALLUSERSPROFILE if ProgramData not set
        if (auto all_users = std::getenv("ALLUSERSPROFILE")) {
            return std::filesystem::path(all_users);
        }
        log_error("Neither ProgramData nor ALLUSERSPROFILE environment variables found");
        return std::unexpected(error_code::environment_error);
#elif defined(__APPLE__)
        return std::filesystem::path("/Library/Application Support");
#else
        return std::filesystem::path("/etc");
#endif
    } catch (const std::exception& e) {
        log_error("Exception getting system config directory: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

auto open_in_file_manager(
    const std::filesystem::path& path) -> std::expected<void, error_code> {
    
    try {
#ifdef _WIN32
        // Use Windows Explorer
        auto path_str = path.wstring();
        auto result = reinterpret_cast<std::intptr_t>(
            ShellExecuteW(nullptr, L"explore", path_str.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
        );
        
        if (result <= 32) {
            log_error("Failed to open file manager");
            return std::unexpected(error_code::execution_failed);
        }
        
        return {};
#elif defined(__APPLE__)
        // Use macOS Finder
        auto command = "open \"" + path.string() + "\"";
        if (std::system(command.c_str()) != 0) {
            log_error("Failed to open Finder");
            return std::unexpected(error_code::execution_failed);
        }
        return {};
#else
        // Try xdg-open on Linux
        auto command = "xdg-open \"" + path.string() + "\" 2>/dev/null";
        if (std::system(command.c_str()) != 0) {
            log_error("Failed to open file manager");
            return std::unexpected(error_code::execution_failed);
        }
        return {};
#endif
    } catch (const std::exception& e) {
        log_error("Exception opening file manager: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

auto open_url(std::string_view url) -> std::expected<void, error_code> {
    try {
#ifdef _WIN32
        // Use ShellExecute on Windows
        std::wstring wide_url(url.begin(), url.end());
        auto result = reinterpret_cast<std::intptr_t>(
            ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
        );
        
        if (result <= 32) {
            log_error("Failed to open URL");
            return std::unexpected(error_code::execution_failed);
        }
        
        return {};
#elif defined(__APPLE__)
        // Use open command on macOS
        auto command = "open \"" + std::string(url) + "\"";
        if (std::system(command.c_str()) != 0) {
            log_error("Failed to open URL");
            return std::unexpected(error_code::execution_failed);
        }
        return {};
#else
        // Try xdg-open on Linux
        auto command = "xdg-open \"" + std::string(url) + "\" 2>/dev/null";
        if (std::system(command.c_str()) != 0) {
            log_error("Failed to open URL");
            return std::unexpected(error_code::execution_failed);
        }
        return {};
#endif
    } catch (const std::exception& e) {
        log_error("Exception opening URL: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

auto get_env_variable(
    std::string_view name) -> std::expected<std::string, error_code> {
    
    try {
        auto name_str = std::string(name);
        
#ifdef _WIN32
        // Windows: use _wgetenv for Unicode support and proper conversion
        std::wstring wide_name(name_str.begin(), name_str.end());
        if (auto value = _wgetenv(wide_name.c_str())) {
            // Convert wide string to UTF-8 properly
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
            if (size_needed > 0) {
                std::string result(size_needed - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size_needed, nullptr, nullptr);
                return result;
            }
        }
#else
        if (auto value = std::getenv(name_str.c_str())) {
            return std::string(value);
        }
#endif
        
        return std::unexpected(error_code::not_found);
        
    } catch (const std::exception& e) {
        log_error("Exception getting environment variable: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

auto set_env_variable(
    std::string_view name,
    std::string_view value) -> std::expected<void, error_code> {
    
    try {
        auto name_str = std::string(name);
        auto value_str = std::string(value);
        
#ifdef _WIN32
        if (_putenv_s(name_str.c_str(), value_str.c_str()) != 0) {
            log_error("Failed to set environment variable");
            return std::unexpected(error_code::execution_failed);
        }
#else
        if (setenv(name_str.c_str(), value_str.c_str(), 1) != 0) {
            log_error("Failed to set environment variable");
            return std::unexpected(error_code::execution_failed);
        }
#endif
        
        return {};
        
    } catch (const std::exception& e) {
        log_error("Exception setting environment variable: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

bool is_elevated() noexcept {
    try {
#ifdef _WIN32
        BOOL is_admin = FALSE;
        SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
        PSID admin_group = nullptr;
        
        if (AllocateAndInitializeSid(&nt_authority, 2,
                                     SECURITY_BUILTIN_DOMAIN_RID,
                                     DOMAIN_ALIAS_RID_ADMINS,
                                     0, 0, 0, 0, 0, 0,
                                     &admin_group)) {
            CheckTokenMembership(nullptr, admin_group, &is_admin);
            FreeSid(admin_group);
        }
        
        return is_admin != FALSE;
#else
        return getuid() == 0;
#endif
    } catch (...) {
        return false;
    }
}

int get_process_id() noexcept {
    try {
#ifdef _WIN32
        return static_cast<int>(GetCurrentProcessId());
#else
        return static_cast<int>(getpid());
#endif
    } catch (...) {
        return -1;
    }
}

unsigned int get_cpu_count() noexcept {
    try {
        auto count = std::thread::hardware_concurrency();
        return count > 0 ? count : 1;
    } catch (...) {
        return 1;
    }
}

std::uint64_t get_total_memory() noexcept {
    try {
#ifdef _WIN32
        MEMORYSTATUSEX status;
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            return status.ullTotalPhys;
        }
#elif defined(__APPLE__)
        int mib[2] = { CTL_HW, HW_MEMSIZE };
        std::uint64_t size = 0;
        std::size_t len = sizeof(size);
        if (sysctl(mib, 2, &size, &len, nullptr, 0) == 0) {
            return size;
        }
#elif defined(__linux__)
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            return static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
        }
#endif
    } catch (...) {
    }
    return 0;
}

std::uint64_t get_available_memory() noexcept {
    try {
#ifdef _WIN32
        MEMORYSTATUSEX status;
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            return status.ullAvailPhys;
        }
#elif defined(__linux__)
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            return static_cast<std::uint64_t>(info.freeram) * info.mem_unit;
        }
#elif defined(__APPLE__)
        // macOS available memory calculation is more complex
        // This is a simplified version
        int mib[2] = { CTL_HW, HW_USERMEM };
        std::uint64_t size = 0;
        std::size_t len = sizeof(size);
        if (sysctl(mib, 2, &size, &len, nullptr, 0) == 0) {
            return size;
        }
#endif
    } catch (...) {
    }
    return 0;
}

auto is_filesystem_case_sensitive(
    [[maybe_unused]] const std::filesystem::path& path) -> std::expected<bool, error_code> {
    
    try {
#ifdef _WIN32
        // Windows filesystems are generally case-insensitive
        return false;
#elif defined(__APPLE__)
        // macOS can be either APFS (case-sensitive optional) or HFS+ (usually case-insensitive)
        // Default to case-insensitive for compatibility
        return false;
#else
        // Linux filesystems are typically case-sensitive
        return true;
#endif
    } catch (const std::exception& e) {
        log_error("Exception checking filesystem case sensitivity: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

} // namespace platform
} // namespace sak
