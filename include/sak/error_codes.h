/// @file error_codes.h
/// @brief Error code definitions for SAK Utility
/// @note Using std::expected pattern for type-safe error handling

#ifndef SAK_ERROR_CODES_H
#define SAK_ERROR_CODES_H

#include <string_view>
#include <system_error>

namespace sak {

/// @brief General error codes for file system operations
enum class error_code {
    success = 0,
    
    // File system errors (1-99)
    file_not_found = 1,
    permission_denied = 2,
    path_too_long = 3,
    invalid_path = 4,
    disk_full = 5,
    file_already_exists = 6,
    directory_not_empty = 7,
    is_directory = 8,
    not_a_directory = 9,
    file_too_large = 10,
    invalid_filename = 11,
    circular_reference = 12,
    symlink_loop = 13,
    
    // I/O errors (100-199)
    read_error = 100,
    write_error = 101,
    file_write_error = 101,  // Alias for compatibility
    seek_error = 102,
    truncate_error = 103,
    flush_error = 104,
    lock_error = 105,
    invalid_argument = 106,
    
    // Hash/verification errors (200-299)
    hash_calculation_failed = 200,
    hash_mismatch = 201,
    verification_failed = 202,
    corrupted_data = 203,
    
    // Configuration errors (300-399)
    invalid_configuration = 300,
    missing_required_field = 301,
    parse_error = 302,
    unsupported_version = 303,
    
    // Platform errors (400-499)
    platform_not_supported = 400,
    permission_update_failed = 401,
    registry_access_denied = 402,
    plist_parse_error = 403,
    elevation_required = 404,
    elevation_failed = 405,
    environment_error = 406,
    execution_failed = 407,
    not_found = 408,
    
    // Threading errors (500-599)
    thread_creation_failed = 500,
    operation_cancelled = 501,
    timeout = 502,
    deadlock_detected = 503,
    
    // Memory errors (600-699)
    out_of_memory = 600,
    allocation_failed = 601,
    buffer_overflow = 602,
    
    // Scanner/organizer errors (700-799)
    scan_failed = 700,
    organization_failed = 701,
    duplicate_resolution_failed = 702,
    license_scan_failed = 703,
    backup_failed = 704,
    
    // Network errors (800-899)
    network_unavailable = 800,
    connection_failed = 801,
    transfer_failed = 802,
    network_timeout = 803,
    protocol_error = 804,
    authentication_failed = 805,
    
    // Security/validation errors (850-899)
    validation_failed = 850,
    path_traversal_attempt = 851,
    invalid_file = 852,
    integer_overflow = 853,
    insufficient_disk_space = 854,
    insufficient_memory = 855,
    resource_limit_reached = 856,
    filesystem_error = 857,
    crypto_error = 858,
    decrypt_failed = 859,
    invalid_format = 860,
    
    // Generic errors (900-999)
    unknown_error = 900,
    not_implemented = 901,
    internal_error = 902,
    assertion_failed = 903,
    invalid_operation = 904,
    partial_failure = 905,
};

/// @brief Convert error code to human-readable message
/// @param ec Error code to convert
/// @return String view describing the error
[[nodiscard]] constexpr std::string_view to_string(error_code ec) noexcept {
    switch (ec) {
        case error_code::success: return "Success";
        
        // File system errors
        case error_code::file_not_found: return "File not found";
        case error_code::permission_denied: return "Permission denied";
        case error_code::path_too_long: return "Path too long";
        case error_code::invalid_path: return "Invalid path";
        case error_code::disk_full: return "Disk full";
        case error_code::file_already_exists: return "File already exists";
        case error_code::directory_not_empty: return "Directory not empty";
        case error_code::is_directory: return "Path is a directory";
        case error_code::not_a_directory: return "Path is not a directory";
        case error_code::file_too_large: return "File too large";
        case error_code::invalid_filename: return "Invalid filename";
        case error_code::circular_reference: return "Circular reference detected";
        case error_code::symlink_loop: return "Symlink loop detected";
        
        // I/O errors
        case error_code::read_error: return "Read error";
        case error_code::write_error: return "Write error";
        case error_code::seek_error: return "Seek error";
        case error_code::truncate_error: return "Truncate error";
        case error_code::flush_error: return "Flush error";
        case error_code::lock_error: return "Lock error";
        
        // Hash/verification errors
        case error_code::hash_calculation_failed: return "Hash calculation failed";
        case error_code::hash_mismatch: return "Hash mismatch";
        case error_code::verification_failed: return "Verification failed";
        case error_code::corrupted_data: return "Corrupted data";
        
        // Configuration errors
        case error_code::invalid_configuration: return "Invalid configuration";
        case error_code::missing_required_field: return "Missing required field";
        case error_code::parse_error: return "Parse error";
        case error_code::unsupported_version: return "Unsupported version";
        
        // Platform errors
        case error_code::platform_not_supported: return "Platform not supported";
        case error_code::permission_update_failed: return "Permission update failed";
        case error_code::registry_access_denied: return "Registry access denied";
        case error_code::plist_parse_error: return "Plist parse error";
        case error_code::elevation_required: return "Elevation required";
        case error_code::elevation_failed: return "Elevation failed";
        case error_code::environment_error: return "Environment error";
        case error_code::execution_failed: return "Execution failed";
        case error_code::not_found: return "Not found";
        
        // Threading errors
        case error_code::thread_creation_failed: return "Thread creation failed";
        case error_code::operation_cancelled: return "Operation cancelled";
        case error_code::timeout: return "Operation timed out";
        case error_code::deadlock_detected: return "Deadlock detected";
        
        // Memory errors
        case error_code::out_of_memory: return "Out of memory";
        case error_code::allocation_failed: return "Allocation failed";
        case error_code::buffer_overflow: return "Buffer overflow";
        
        // Scanner/organizer errors
        case error_code::scan_failed: return "Scan failed";
        case error_code::organization_failed: return "Organization failed";
        case error_code::duplicate_resolution_failed: return "Duplicate resolution failed";
        case error_code::license_scan_failed: return "License scan failed";
        case error_code::backup_failed: return "Backup failed";
        
        // Network errors
        case error_code::network_unavailable: return "Network unavailable";
        case error_code::connection_failed: return "Connection failed";
        case error_code::transfer_failed: return "Transfer failed";
        case error_code::network_timeout: return "Network timeout";
        case error_code::protocol_error: return "Protocol error";
        case error_code::authentication_failed: return "Authentication failed";
        
        // Security/validation errors
        case error_code::validation_failed: return "Validation failed";
        case error_code::path_traversal_attempt: return "Path traversal attempt detected";
        case error_code::invalid_file: return "Invalid file";
        case error_code::integer_overflow: return "Integer overflow";
        case error_code::insufficient_disk_space: return "Insufficient disk space";
        case error_code::insufficient_memory: return "Insufficient memory";
        case error_code::resource_limit_reached: return "Resource limit reached";
        case error_code::filesystem_error: return "Filesystem error";
        
        // Generic errors
        case error_code::unknown_error: return "Unknown error";
        case error_code::not_implemented: return "Not implemented";
        case error_code::internal_error: return "Internal error";
        case error_code::assertion_failed: return "Assertion failed";
        
        default: return "Undefined error";
    }
}

/// @brief Exception base class for SAK Utility
/// @note Only used for unrecoverable errors; prefer std::expected for expected failures
class sak_exception : public std::exception {
public:
    explicit sak_exception(std::string_view message) noexcept
        : m_message(message) {}
    
    [[nodiscard]] const char* what() const noexcept override {
        return m_message.c_str();
    }

private:
    std::string m_message;
};

/// @brief File system exception
class file_system_exception : public sak_exception {
public:
    explicit file_system_exception(std::string_view message) noexcept
        : sak_exception(message) {}
};

/// @brief Permission exception
class permission_exception : public sak_exception {
public:
    explicit permission_exception(std::string_view message) noexcept
        : sak_exception(message) {}
};

/// @brief Hash calculation exception
class hash_calculation_exception : public sak_exception {
public:
    explicit hash_calculation_exception(std::string_view message) noexcept
        : sak_exception(message) {}
};

/// @brief Configuration exception
class configuration_exception : public sak_exception {
public:
    explicit configuration_exception(std::string_view message) noexcept
        : sak_exception(message) {}
};

/// @brief Platform exception
class platform_exception : public sak_exception {
public:
    explicit platform_exception(std::string_view message) noexcept
        : sak_exception(message) {}
};

} // namespace sak

#endif // SAK_ERROR_CODES_H
