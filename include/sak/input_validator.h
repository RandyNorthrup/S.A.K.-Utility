/// @file input_validator.h
/// @brief Input validation utilities for security-critical operations
/// @details Comprehensive validation framework following OWASP guidelines

#pragma once

#include "error_codes.h"
#include <filesystem>
#include <expected>
#include <string>
#include <string_view>
#include <span>
#include <cstddef>
#include <cstdint>

namespace sak {

/// @brief Input validation result with detailed error information
struct validation_result {
    bool is_valid;
    error_code error;
    std::string error_message;
    
    explicit operator bool() const noexcept { return is_valid; }
};

/// @brief Path validation configuration
struct path_validation_config {
    bool allow_relative_paths = false;
    bool allow_symlinks = false;
    bool must_exist = false;
    bool must_be_directory = false;
    bool must_be_file = false;
    bool check_read_permission = false;
    bool check_write_permission = false;
    std::size_t max_path_length = 260;  // Windows MAX_PATH default
    std::filesystem::path base_directory;  // For path traversal checks
};

/// @brief String validation configuration
struct string_validation_config {
    std::size_t min_length = 0;
    std::size_t max_length = 1024;
    bool allow_null_bytes = false;
    bool allow_control_chars = false;
    bool require_printable = false;
    bool require_ascii = false;
    bool require_utf8 = false;
};

/// @brief Numeric validation configuration
template<typename T>
struct numeric_validation_config {
    T min_value = std::numeric_limits<T>::min();
    T max_value = std::numeric_limits<T>::max();
    bool check_overflow = true;
};

/// @brief Input validation utilities
/// @details Provides comprehensive input validation following security best practices
class input_validator {
public:
    // ============================================
    // Path Validation (Path Traversal Prevention)
    // ============================================
    
    /// @brief Validate a filesystem path against security criteria
    /// @param path Path to validate
    /// @param config Validation configuration
    /// @return Validation result with detailed error information
    [[nodiscard]] static validation_result validate_path(
        const std::filesystem::path& path,
        const path_validation_config& config = {});
    
    /// @brief Check if path contains traversal sequences (../, ..\, etc.)
    /// @param path Path to check
    /// @return True if path contains potentially dangerous traversal sequences
    [[nodiscard]] static bool contains_traversal_sequences(
        const std::filesystem::path& path) noexcept;
    
    /// @brief Ensure path is within allowed base directory
    /// @param path Path to validate
    /// @param base_dir Base directory that path must be within
    /// @return Validation result
    [[nodiscard]] static validation_result validate_path_within_base(
        const std::filesystem::path& path,
        const std::filesystem::path& base_dir);
    
    /// @brief Check if path contains suspicious patterns
    /// @param path Path to check
    /// @return True if path contains suspicious patterns (UNC paths, device names, etc.)
    [[nodiscard]] static bool contains_suspicious_patterns(
        const std::filesystem::path& path) noexcept;
    
    // ============================================
    // String Validation (Injection Prevention)
    // ============================================
    
    /// @brief Validate a string against security criteria
    /// @param str String to validate
    /// @param config Validation configuration
    /// @return Validation result
    [[nodiscard]] static validation_result validate_string(
        std::string_view str,
        const string_validation_config& config = {});
    
    /// @brief Check for null bytes in string (common injection technique)
    /// @param str String to check
    /// @return True if null bytes found
    [[nodiscard]] static bool contains_null_bytes(
        std::string_view str) noexcept;
    
    /// @brief Check for control characters in string
    /// @param str String to check
    /// @return True if control characters found
    [[nodiscard]] static bool contains_control_chars(
        std::string_view str) noexcept;
    
    /// @brief Validate UTF-8 encoding
    /// @param str String to validate
    /// @return True if valid UTF-8
    [[nodiscard]] static bool is_valid_utf8(
        std::string_view str) noexcept;
    
    /// @brief Sanitize string by removing dangerous characters
    /// @param str String to sanitize
    /// @param allow_unicode If false, removes non-ASCII characters
    /// @return Sanitized string
    [[nodiscard]] static std::string sanitize_string(
        std::string_view str,
        bool allow_unicode = true);
    
    // ============================================
    // Numeric Validation (Overflow Prevention)
    // ============================================
    
    /// @brief Validate numeric value against range
    /// @tparam T Numeric type
    /// @param value Value to validate
    /// @param config Validation configuration
    /// @return Validation result
    template<typename T>
    [[nodiscard]] static validation_result validate_numeric(
        T value,
        const numeric_validation_config<T>& config = {});
    
    /// @brief Safe addition with overflow check
    /// @tparam T Numeric type
    /// @param a First operand
    /// @param b Second operand
    /// @return Expected containing result or overflow error
    template<typename T>
    [[nodiscard]] static std::expected<T, error_code> safe_add(T a, T b);
    
    /// @brief Safe multiplication with overflow check
    /// @tparam T Numeric type
    /// @param a First operand
    /// @param b Second operand
    /// @return Expected containing result or overflow error
    template<typename T>
    [[nodiscard]] static std::expected<T, error_code> safe_multiply(T a, T b);
    
    /// @brief Safe cast between numeric types with overflow check
    /// @tparam To Target type
    /// @tparam From Source type
    /// @param value Value to cast
    /// @return Expected containing casted value or overflow error
    template<typename To, typename From>
    [[nodiscard]] static std::expected<To, error_code> safe_cast(From value);
    
    // ============================================
    // Buffer Validation (Buffer Overflow Prevention)
    // ============================================
    
    /// @brief Validate buffer size against limits
    /// @param buffer_size Size of buffer
    /// @param max_size Maximum allowed size
    /// @param required_size Required minimum size (0 = no minimum)
    /// @return Validation result
    [[nodiscard]] static validation_result validate_buffer_size(
        std::size_t buffer_size,
        std::size_t max_size,
        std::size_t required_size = 0);
    
    /// @brief Validate span against expected size
    /// @tparam T Element type
    /// @param data Span to validate
    /// @param expected_size Expected size
    /// @return Validation result
    template<typename T>
    [[nodiscard]] static validation_result validate_span(
        std::span<const T> data,
        std::size_t expected_size);
    
    // ============================================
    // Resource Validation
    // ============================================
    
    /// @brief Check available disk space
    /// @param path Path to check (file or directory)
    /// @param required_bytes Required space in bytes
    /// @return Validation result
    [[nodiscard]] static validation_result validate_disk_space(
        const std::filesystem::path& path,
        std::uintmax_t required_bytes);
    
    /// @brief Check available memory
    /// @param required_bytes Required memory in bytes
    /// @return Validation result
    [[nodiscard]] static validation_result validate_available_memory(
        std::size_t required_bytes);
    
    /// @brief Validate file descriptor/handle limits
    /// @return Validation result (warns if approaching limits)
    [[nodiscard]] static validation_result validate_file_descriptor_limit();
    
    /// @brief Validate thread count against system limits
    /// @param requested_threads Number of threads requested
    /// @return Validation result
    [[nodiscard]] static validation_result validate_thread_count(
        std::size_t requested_threads);
    
    // ============================================
    // Helper Functions
    // ============================================
    
    /// @brief Create a successful validation result
    [[nodiscard]] static validation_result success() noexcept;
    
    /// @brief Create a failed validation result
    /// @param err Error code
    /// @param message Error message
    [[nodiscard]] static validation_result failure(
        error_code err,
        std::string_view message);

private:
    // Platform-specific helpers
    static std::uintmax_t get_available_memory_impl();
    static std::size_t get_file_descriptor_count_impl();
    static std::size_t get_file_descriptor_limit_impl();
};

// ============================================
// Template Implementations
// ============================================

template<typename T>
validation_result input_validator::validate_numeric(
    T value,
    const numeric_validation_config<T>& config) {
    
    if (value < config.min_value) {
        return failure(error_code::validation_failed,
                      "Value below minimum allowed");
    }
    
    if (value > config.max_value) {
        return failure(error_code::validation_failed,
                      "Value exceeds maximum allowed");
    }
    
    return success();
}

template<typename T>
std::expected<T, error_code> input_validator::safe_add(T a, T b) {
    // Check for overflow
    if constexpr (std::is_unsigned_v<T>) {
        if (a > std::numeric_limits<T>::max() - b) {
            return std::unexpected(error_code::integer_overflow);
        }
    } else {
        if (b > 0 && a > std::numeric_limits<T>::max() - b) {
            return std::unexpected(error_code::integer_overflow);
        }
        if (b < 0 && a < std::numeric_limits<T>::min() - b) {
            return std::unexpected(error_code::integer_overflow);
        }
    }
    return a + b;
}

template<typename T>
std::expected<T, error_code> input_validator::safe_multiply(T a, T b) {
    // Check for overflow
    if (a == 0 || b == 0) {
        return T{0};
    }
    
    if constexpr (std::is_unsigned_v<T>) {
        if (a > std::numeric_limits<T>::max() / b) {
            return std::unexpected(error_code::integer_overflow);
        }
    } else {
        if (a > 0) {
            if (b > 0 && a > std::numeric_limits<T>::max() / b) {
                return std::unexpected(error_code::integer_overflow);
            }
            if (b < 0 && b < std::numeric_limits<T>::min() / a) {
                return std::unexpected(error_code::integer_overflow);
            }
        } else {
            if (b > 0 && a < std::numeric_limits<T>::min() / b) {
                return std::unexpected(error_code::integer_overflow);
            }
            if (b < 0 && a != 0 && b < std::numeric_limits<T>::max() / a) {
                return std::unexpected(error_code::integer_overflow);
            }
        }
    }
    return a * b;
}

template<typename To, typename From>
std::expected<To, error_code> input_validator::safe_cast(From value) {
    // Check if cast would overflow or underflow
    if constexpr (std::is_integral_v<To> && std::is_integral_v<From>) {
        if (value < static_cast<From>(std::numeric_limits<To>::min()) ||
            value > static_cast<From>(std::numeric_limits<To>::max())) {
            return std::unexpected(error_code::integer_overflow);
        }
    }
    return static_cast<To>(value);
}

template<typename T>
validation_result input_validator::validate_span(
    std::span<const T> data,
    std::size_t expected_size) {
    
    if (data.size() != expected_size) {
        return failure(error_code::validation_failed,
                      "Span size does not match expected size");
    }
    
    return success();
}

} // namespace sak

