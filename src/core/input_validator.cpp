/// @file input_validator.cpp
/// @brief Implementation of input validation utilities

#include "sak/input_validator.h"
#include "sak/logger.h"
#include "sak/path_utils.h"
#include <algorithm>
#include <cctype>
#include <regex>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots
#else
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace sak {

// ============================================
// Path Validation
// ============================================

validation_result input_validator::validate_path(
    const std::filesystem::path& path,
    const path_validation_config& config) {
    
    // Check path length
    const auto path_str = path.string();
    if (path_str.length() > config.max_path_length) {
        return failure(error_code::path_too_long,
                      "Path exceeds maximum allowed length");
    }
    
    // Check for null bytes
    if (path_str.find('\0') != std::string::npos) {
        return failure(error_code::invalid_path,
                      "Path contains null bytes");
    }
    
    // Check for suspicious patterns
    if (contains_suspicious_patterns(path)) {
        return failure(error_code::invalid_path,
                      "Path contains suspicious patterns");
    }
    
    // Check for traversal sequences
    if (contains_traversal_sequences(path)) {
        return failure(error_code::path_traversal_attempt,
                      "Path contains directory traversal sequences");
    }
    
    // Check relative path requirement
    if (!config.allow_relative_paths && path.is_relative()) {
        return failure(error_code::invalid_path,
                      "Relative paths are not allowed");
    }
    
    // Check existence
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    
    if (config.must_exist && !exists) {
        return failure(error_code::file_not_found,
                      "Path must exist but does not");
    }
    
    if (exists) {
        // Check symlink requirement
        const bool is_symlink = std::filesystem::is_symlink(path, ec);
        if (is_symlink && !config.allow_symlinks) {
            return failure(error_code::invalid_path,
                          "Symbolic links are not allowed");
        }
        
        // Check directory requirement
        const bool is_dir = std::filesystem::is_directory(path, ec);
        if (config.must_be_directory && !is_dir) {
            return failure(error_code::not_a_directory,
                          "Path must be a directory");
        }
        
        // Check file requirement
        const bool is_file = std::filesystem::is_regular_file(path, ec);
        if (config.must_be_file && !is_file) {
            return failure(error_code::invalid_file,
                          "Path must be a regular file");
        }
        
        // Check permissions (platform-specific)
#ifdef _WIN32
        // Windows permission checks via GetFileAttributes
        DWORD attrs = GetFileAttributesW(path.wstring().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (config.check_read_permission || config.check_write_permission) {
                return failure(error_code::permission_denied,
                              "Cannot check file permissions");
            }
        }
        
        if (config.check_write_permission && (attrs & FILE_ATTRIBUTE_READONLY)) {
            return failure(error_code::permission_denied,
                          "Path is read-only");
        }
#else
        // Unix permission checks via access()
        if (config.check_read_permission && access(path.c_str(), R_OK) != 0) {
            return failure(error_code::permission_denied,
                          "Path is not readable");
        }
        
        if (config.check_write_permission && access(path.c_str(), W_OK) != 0) {
            return failure(error_code::permission_denied,
                          "Path is not writable");
        }
#endif
    }
    
    // Check base directory constraint
    if (!config.base_directory.empty()) {
        auto within_base = validate_path_within_base(path, config.base_directory);
        if (!within_base) {
            return within_base;
        }
    }
    
    return success();
}

bool input_validator::contains_traversal_sequences(
    const std::filesystem::path& path) noexcept {
    
    const auto path_str = path.string();
    
    // Check for common traversal patterns
    if (path_str.find("..") != std::string::npos) {
        return true;
    }
    
    // Check for encoded traversal attempts
    if (path_str.find("%2e%2e") != std::string::npos ||  // URL encoded ..
        path_str.find("%252e%252e") != std::string::npos) {  // Double encoded
        return true;
    }
    
    // Check each path component
    for (const auto& component : path) {
        const auto comp_str = component.string();
        if (comp_str == ".." || comp_str == ".") {
            return true;
        }
    }
    
    return false;
}

validation_result input_validator::validate_path_within_base(
    const std::filesystem::path& path,
    const std::filesystem::path& base_dir) {
    
    try {
        // Canonicalize both paths
        std::error_code ec;
        auto canonical_path = std::filesystem::weakly_canonical(path, ec);
        if (ec) {
            return failure(error_code::invalid_path,
                          "Cannot canonicalize path");
        }
        
        auto canonical_base = std::filesystem::weakly_canonical(base_dir, ec);
        if (ec) {
            return failure(error_code::invalid_path,
                          "Cannot canonicalize base directory");
        }
        
        // Check if path starts with base (case-insensitive on Windows)
        auto path_str = canonical_path.string();
        auto base_str = canonical_base.string();
        
#ifdef _WIN32
        // Windows filesystem is case-insensitive
        std::transform(path_str.begin(), path_str.end(), path_str.begin(), ::tolower);
        std::transform(base_str.begin(), base_str.end(), base_str.begin(), ::tolower);
#endif
        
        if (path_str.find(base_str) != 0) {
            return failure(error_code::path_traversal_attempt,
                          "Path is outside allowed base directory");
        }
        
        return success();
        
    } catch (...) {
        return failure(error_code::unknown_error,
                      "Exception during path validation");
    }
}

bool input_validator::contains_suspicious_patterns(
    const std::filesystem::path& path) noexcept {
    
    const auto path_str = path.string();
    
    // Windows device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9)
    static const std::regex device_pattern(
        R"((^|[/\\])(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$))",
        std::regex::icase);
    
    if (std::regex_search(path_str, device_pattern)) {
        return true;
    }
    
    // UNC paths (Windows network paths)
#ifdef _WIN32
    if (path_str.starts_with("\\\\") || path_str.starts_with("//")) {
        return true;
    }
#endif
    
    // Unusual characters that might indicate injection
    if (path_str.find('\0') != std::string::npos ||
        path_str.find('\n') != std::string::npos ||
        path_str.find('\r') != std::string::npos) {
        return true;
    }
    
    return false;
}

// ============================================
// String Validation
// ============================================

validation_result input_validator::validate_string(
    std::string_view str,
    const string_validation_config& config) {
    
    // Check length
    if (str.length() < config.min_length) {
        return failure(error_code::validation_failed,
                      "String is too short");
    }
    
    if (str.length() > config.max_length) {
        return failure(error_code::validation_failed,
                      "String is too long");
    }
    
    // Check for null bytes
    if (!config.allow_null_bytes && contains_null_bytes(str)) {
        return failure(error_code::validation_failed,
                      "String contains null bytes");
    }
    
    // Check for control characters
    if (!config.allow_control_chars && contains_control_chars(str)) {
        return failure(error_code::validation_failed,
                      "String contains control characters");
    }
    
    // Check printability
    if (config.require_printable) {
        if (!std::all_of(str.begin(), str.end(), 
                        [](unsigned char c) { return std::isprint(c); })) {
            return failure(error_code::validation_failed,
                          "String contains non-printable characters");
        }
    }
    
    // Check ASCII
    if (config.require_ascii) {
        if (!std::all_of(str.begin(), str.end(),
                        [](unsigned char c) { return c < 128; })) {
            return failure(error_code::validation_failed,
                          "String contains non-ASCII characters");
        }
    }
    
    // Check UTF-8
    if (config.require_utf8 && !is_valid_utf8(str)) {
        return failure(error_code::validation_failed,
                      "String is not valid UTF-8");
    }
    
    return success();
}

bool input_validator::contains_null_bytes(std::string_view str) noexcept {
    return str.find('\0') != std::string_view::npos;
}

bool input_validator::contains_control_chars(std::string_view str) noexcept {
    return std::any_of(str.begin(), str.end(),
                      [](unsigned char c) {
                          return std::iscntrl(c) && c != '\n' && c != '\r' && c != '\t';
                      });
}

bool input_validator::is_valid_utf8(std::string_view str) noexcept {
    std::size_t i = 0;
    
    while (i < str.length()) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        
        // ASCII
        if (c < 0x80) {
            i++;
            continue;
        }
        
        // 2-byte sequence
        if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= str.length()) return false;
            if ((static_cast<unsigned char>(str[i + 1]) & 0xC0) != 0x80) return false;
            i += 2;
            continue;
        }
        
        // 3-byte sequence
        if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= str.length()) return false;
            if ((static_cast<unsigned char>(str[i + 1]) & 0xC0) != 0x80) return false;
            if ((static_cast<unsigned char>(str[i + 2]) & 0xC0) != 0x80) return false;
            i += 3;
            continue;
        }
        
        // 4-byte sequence
        if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= str.length()) return false;
            if ((static_cast<unsigned char>(str[i + 1]) & 0xC0) != 0x80) return false;
            if ((static_cast<unsigned char>(str[i + 2]) & 0xC0) != 0x80) return false;
            if ((static_cast<unsigned char>(str[i + 3]) & 0xC0) != 0x80) return false;
            i += 4;
            continue;
        }
        
        // Invalid UTF-8
        return false;
    }
    
    return true;
}

std::string input_validator::sanitize_string(
    std::string_view str,
    bool allow_unicode) {
    
    std::string result;
    result.reserve(str.length());
    
    for (unsigned char c : str) {
        // Remove null bytes
        if (c == '\0') {
            continue;
        }
        
        // Remove control characters (except newline, carriage return, tab)
        if (std::iscntrl(c) && c != '\n' && c != '\r' && c != '\t') {
            continue;
        }
        
        // Handle non-ASCII
        if (c >= 128) {
            if (allow_unicode) {
                result.push_back(static_cast<char>(c));
            }
            // else skip non-ASCII
        } else {
            result.push_back(static_cast<char>(c));
        }
    }
    
    return result;
}

// ============================================
// Buffer Validation
// ============================================

validation_result input_validator::validate_buffer_size(
    std::size_t buffer_size,
    std::size_t max_size,
    std::size_t required_size) {
    
    if (required_size > 0 && buffer_size < required_size) {
        return failure(error_code::validation_failed,
                      "Buffer is too small");
    }
    
    if (buffer_size > max_size) {
        return failure(error_code::validation_failed,
                      "Buffer exceeds maximum allowed size");
    }
    
    return success();
}

// ============================================
// Resource Validation
// ============================================

validation_result input_validator::validate_disk_space(
    const std::filesystem::path& path,
    std::uintmax_t required_bytes) {
    
    try {
        std::error_code ec;
        auto space_info = std::filesystem::space(path, ec);
        
        if (ec) {
            return failure(error_code::filesystem_error,
                          "Cannot determine available disk space");
        }
        
        if (space_info.available < required_bytes) {
            return failure(error_code::insufficient_disk_space,
                          "Insufficient disk space available");
        }
        
        return success();
        
    } catch (...) {
        return failure(error_code::unknown_error,
                      "Exception during disk space validation");
    }
}

validation_result input_validator::validate_available_memory(
    std::size_t required_bytes) {
    
    const auto available = get_available_memory_impl();
    
    if (available == 0) {
        return failure(error_code::unknown_error,
                      "Cannot determine available memory");
    }
    
    if (available < required_bytes) {
        return failure(error_code::insufficient_memory,
                      "Insufficient memory available");
    }
    
    return success();
}

validation_result input_validator::validate_file_descriptor_limit() {
    const auto current_count = get_file_descriptor_count_impl();
    const auto limit = get_file_descriptor_limit_impl();
    
    if (limit == 0) {
        return success();  // Cannot determine, assume OK
    }
    
    // Warn if using more than 80% of limit
    if (current_count > (limit * 4 / 5)) {
        log_warning("Approaching file descriptor limit: {}/{}", current_count, limit);
        return failure(error_code::resource_limit_reached,
                      "Approaching file descriptor limit");
    }
    
    return success();
}

validation_result input_validator::validate_thread_count(
    std::size_t requested_threads) {
    
    const auto hardware_threads = std::thread::hardware_concurrency();
    
    if (hardware_threads == 0) {
        // Cannot determine, allow up to reasonable limit
        if (requested_threads > 64) {
            return failure(error_code::validation_failed,
                          "Thread count exceeds reasonable limit");
        }
        return success();
    }
    
    // Warn if requesting more than 2x hardware threads
    if (requested_threads > hardware_threads * 2) {
        log_warning("Requested threads ({}) exceeds 2x hardware threads ({})",
                   requested_threads, hardware_threads);
    }
    
    // Reject if requesting more than 4x hardware threads
    if (requested_threads > hardware_threads * 4) {
        return failure(error_code::validation_failed,
                      "Thread count exceeds 4x hardware threads");
    }
    
    return success();
}

// ============================================
// Helper Functions
// ============================================

validation_result input_validator::success() noexcept {
    return validation_result{true, error_code::success, ""};
}

validation_result input_validator::failure(
    error_code err,
    std::string_view message) {
    
    return validation_result{false, err, std::string(message)};
}

// ============================================
// Platform-Specific Implementations
// ============================================

std::uintmax_t input_validator::get_available_memory_impl() {
#ifdef _WIN32
    MEMORYSTATUSEX mem_info;
    mem_info.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem_info)) {
        return mem_info.ullAvailPhys;
    }
    return 0;
#else
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<std::uintmax_t>(pages) * static_cast<std::uintmax_t>(page_size);
    }
    return 0;
#endif
}

std::size_t input_validator::get_file_descriptor_count_impl() {
#ifdef _WIN32
    // Windows doesn't have a direct equivalent
    return 0;  // Cannot determine
#else
    // Count open file descriptors in /proc/self/fd
    std::size_t count = 0;
    try {
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)entry;
            ++count;
        }
    } catch (...) {
        return 0;  // Cannot determine
    }
    return count;
#endif
}

std::size_t input_validator::get_file_descriptor_limit_impl() {
#ifdef _WIN32
    // Windows doesn't have RLIMIT_NOFILE
    return 0;  // Cannot determine
#else
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        return limit.rlim_cur;
    }
    return 0;
#endif
}

} // namespace sak
