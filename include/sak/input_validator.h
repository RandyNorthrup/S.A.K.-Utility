// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file input_validator.h
/// @brief Input validation utilities for security-critical operations
/// @details Comprehensive validation framework following OWASP guidelines

#pragma once

#include "error_codes.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sak {

constexpr std::size_t kWindowsDefaultMaxPathLength = 260;
constexpr std::size_t kDefaultStringMaxLength = 1024;

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
    std::size_t max_path_length = kWindowsDefaultMaxPathLength;  // Windows MAX_PATH default
    std::filesystem::path base_directory;                        // For path traversal checks
};

/// @brief String validation configuration
struct string_validation_config {
    std::size_t min_length = 0;
    std::size_t max_length = kDefaultStringMaxLength;
    bool allow_null_bytes = false;
    bool allow_control_chars = false;
    bool require_printable = false;
    bool require_ascii = false;
    bool require_utf8 = false;
};

/// @brief Numeric validation configuration
template <typename T>
struct numeric_validation_config {
    T min_value = (std::numeric_limits<T>::min)();
    T max_value = (std::numeric_limits<T>::max)();
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
    [[nodiscard]] static validation_result validatePath(const std::filesystem::path& path,
                                                        const path_validation_config& config = {});

    [[nodiscard]] static validation_result validatePathSyntax(const std::filesystem::path& path,
                                                              const path_validation_config& config);

    /// @brief Check if path contains traversal sequences (../, ..\, etc.)
    /// @param path Path to check
    /// @return True if path contains potentially dangerous traversal sequences
    [[nodiscard]] static bool containsTraversalSequences(
        const std::filesystem::path& path) noexcept;

    /// @brief Ensure path is within allowed base directory
    /// @param path Path to validate
    /// @param base_dir Base directory that path must be within
    /// @return Validation result
    [[nodiscard]] static validation_result validatePathWithinBase(
        const std::filesystem::path& path, const std::filesystem::path& base_dir);

    /// @brief Check if path contains suspicious patterns
    /// @param path Path to check
    /// @return True if path contains suspicious patterns (UNC paths, device names, etc.)
    [[nodiscard]] static bool containsSuspiciousPatterns(
        const std::filesystem::path& path) noexcept;

    /// @brief Decompose a path into every cumulative prefix, root-to-leaf.
    /// @param path Path to decompose.
    /// @return e.g. "C:/a/b" -> {"C:", "C:/", "C:/a", "C:/a/b"}.
    /// @note Pure + static for unit testing. Used to check EVERY ancestor for a
    ///       reparse point when symlinks are disallowed -- the final-component-only
    ///       check could be bypassed by an ancestor junction/symlink.
    [[nodiscard]] static std::vector<std::filesystem::path> pathPrefixes(
        const std::filesystem::path& path);

    // ============================================
    // String Validation (Injection Prevention)
    // ============================================

    /// @brief Validate a string against security criteria
    /// @param str String to validate
    /// @param config Validation configuration
    /// @return Validation result
    [[nodiscard]] static validation_result validateString(
        std::string_view str, const string_validation_config& config = {});

    /// @brief Check for null bytes in string (common injection technique)
    /// @param str String to check
    /// @return True if null bytes found
    [[nodiscard]] static bool containsNullBytes(std::string_view str) noexcept;

    /// @brief Check for control characters in string
    /// @param str String to check
    /// @return True if control characters found
    [[nodiscard]] static bool containsControlChars(std::string_view str) noexcept;

    /// @brief Validate UTF-8 encoding
    /// @param str String to validate
    /// @return True if valid UTF-8
    [[nodiscard]] static bool isValidUtf8(std::string_view str) noexcept;

    /// @brief Sanitize string by removing dangerous characters
    /// @param str String to sanitize
    /// @param allow_unicode If false, removes non-ASCII characters
    /// @return Sanitized string
    /// @warning This is a lossy, transforming sanitizer BY DESIGN: it drops NUL,
    ///          control, and (optionally) non-ASCII bytes rather than rejecting the
    ///          input. A caller that must REJECT malformed input (to avoid silent
    ///          semantic changes or collisions) must call validateString() instead
    ///          of relying on this transform.
    [[nodiscard]] static std::string sanitizeString(std::string_view str,
                                                    bool allow_unicode = true);

    // ============================================
    // Numeric Validation (Overflow Prevention)
    // ============================================

    /// @brief Validate numeric value against range
    /// @tparam T Numeric type
    /// @param value Value to validate
    /// @param config Validation configuration
    /// @return Validation result
    template <typename T>
    [[nodiscard]] static validation_result validateNumeric(
        T value, const numeric_validation_config<T>& config = {});

    /// @brief Safe addition with overflow check
    /// @tparam T Numeric type
    /// @param a First operand
    /// @param b Second operand
    /// @return Expected containing result or overflow error
    template <typename T>
    [[nodiscard]] static std::expected<T, error_code> safeAdd(T a, T b);

    /// @brief Safe multiplication with overflow check
    /// @tparam T Numeric type
    /// @param a First operand
    /// @param b Second operand
    /// @return Expected containing result or overflow error
    template <typename T>
    [[nodiscard]] static std::expected<T, error_code> safeMultiply(T a, T b);

    /// @brief Safe cast between numeric types with overflow check
    /// @tparam To Target type
    /// @tparam From Source type
    /// @param value Value to cast
    /// @return Expected containing casted value or overflow error
    template <typename To, typename From>
    [[nodiscard]] static std::expected<To, error_code> safeCast(From value);

    // ============================================
    // Buffer Validation (Buffer Overflow Prevention)
    // ============================================

    /// @brief Validate buffer size against limits
    /// @param buffer_size Size of buffer
    /// @param max_size Maximum allowed size
    /// @param required_size Required minimum size (0 = no minimum)
    /// @return Validation result
    [[nodiscard]] static validation_result validateBufferSize(std::size_t buffer_size,
                                                              std::size_t max_size,
                                                              std::size_t required_size = 0);

    /// @brief Validate span against expected size
    /// @tparam T Element type
    /// @param data Span to validate
    /// @param expected_size Expected size
    /// @return Validation result
    template <typename T>
    [[nodiscard]] static validation_result validateSpan(std::span<const T> data,
                                                        std::size_t expected_size);

    // ============================================
    // Resource Validation
    // ============================================

    /// @brief Check available disk space
    /// @param path Path to check (file or directory)
    /// @param required_bytes Required space in bytes
    /// @return Validation result
    [[nodiscard]] static validation_result validate_disk_space(const std::filesystem::path& path,
                                                               std::uintmax_t required_bytes);

    /// @brief Check available memory
    /// @param required_bytes Required memory in bytes
    /// @return Validation result
    [[nodiscard]] static validation_result validate_available_memory(std::size_t required_bytes);

    /// @brief Validate file descriptor/handle limits
    /// @return Validation result (warns if approaching limits)
    [[nodiscard]] static validation_result validate_file_descriptor_limit();

    /// @brief Validate thread count against system limits
    /// @param requested_threads Number of threads requested
    /// @return Validation result
    [[nodiscard]] static validation_result validate_thread_count(std::size_t requested_threads);

    // ============================================
    // Helper Functions
    // ============================================

    /// @brief Create a successful validation result
    [[nodiscard]] static validation_result success() noexcept;

    /// @brief Create a failed validation result
    /// @param err Error code
    /// @param message Error message
    [[nodiscard]] static validation_result failure(error_code err, std::string_view message);

private:
    /// @brief Validate file/directory existence and type constraints
    [[nodiscard]] static validation_result validatePathExistence(
        const std::filesystem::path& path, const path_validation_config& config);

    /// @brief Platform-specific permission checks
    [[nodiscard]] static validation_result validatePathPermissions(
        const std::filesystem::path& path, const path_validation_config& config);

    // Platform-specific helpers
    static std::uintmax_t get_available_memory_impl();
#ifndef _WIN32
    static std::size_t get_file_descriptor_count_impl();
    static std::size_t get_file_descriptor_limit_impl();
#endif
};

// ============================================
// Template Implementations
// ============================================

template <typename T>
validation_result input_validator::validateNumeric(T value,
                                                   const numeric_validation_config<T>& config) {
    if (value < config.min_value) {
        return failure(error_code::validation_failed, "Value below minimum allowed");
    }

    if (value > config.max_value) {
        return failure(error_code::validation_failed, "Value exceeds maximum allowed");
    }

    return success();
}

template <typename T>
std::expected<T, error_code> input_validator::safeAdd(T a, T b) {
    // Check for overflow
    if constexpr (std::is_unsigned_v<T>) {
        if (a > (std::numeric_limits<T>::max)() - b) {
            return std::unexpected(error_code::integer_overflow);
        }
    } else {
        if (b > 0 && a > (std::numeric_limits<T>::max)() - b) {
            return std::unexpected(error_code::integer_overflow);
        }
        if (b < 0 && a < (std::numeric_limits<T>::min)() - b) {
            return std::unexpected(error_code::integer_overflow);
        }
    }
    return a + b;
}

template <typename T>
bool checkPositiveMultiplierOverflow(T positive_a, T b) {
    if (b > 0 && positive_a > (std::numeric_limits<T>::max)() / b) {
        return true;
    }
    return b < 0 && b < (std::numeric_limits<T>::min)() / positive_a;
}

template <typename T>
bool checkNegativeMultiplierOverflow(T negative_a, T b) {
    if (b > 0 && negative_a < (std::numeric_limits<T>::min)() / b) {
        return true;
    }
    return b < 0 && negative_a != 0 && b < (std::numeric_limits<T>::max)() / negative_a;
}

template <typename T>
bool checkSignedMultiplyOverflow(T a, T b) {
    if (a > 0) {
        return checkPositiveMultiplierOverflow(a, b);
    }
    return checkNegativeMultiplierOverflow(a, b);
}

template <typename T>
std::expected<T, error_code> input_validator::safeMultiply(T a, T b) {
    if (a == 0 || b == 0) {
        return T{0};
    }

    if constexpr (std::is_unsigned_v<T>) {
        if (a > (std::numeric_limits<T>::max)() / b) {
            return std::unexpected(error_code::integer_overflow);
        }
    } else {
        if (checkSignedMultiplyOverflow(a, b)) {
            return std::unexpected(error_code::integer_overflow);
        }
    }
    return a * b;
}

template <typename To, typename From>
std::expected<To, error_code> input_validator::safeCast(From value) {
    // Use std::in_range for the range test. Casting To's limits into From (the
    // previous approach) is wrong for signed->unsigned widening: e.g. with
    // To=uint64_t, static_cast<int64_t>(UINT64_MAX) == -1, so every positive
    // int64_t was compared against -1 and wrongly rejected. std::in_range handles
    // all signed/unsigned width combinations correctly. bool is excluded because
    // std::in_range is not defined for it.
    if constexpr (std::is_integral_v<To> && std::is_integral_v<From> && !std::is_same_v<To, bool> &&
                  !std::is_same_v<From, bool>) {
        if (!std::in_range<To>(value)) {
            return std::unexpected(error_code::integer_overflow);
        }
    }
    return static_cast<To>(value);
}

template <typename T>
validation_result input_validator::validateSpan(std::span<const T> data,
                                                std::size_t expected_size) {
    if (data.size() != expected_size) {
        return failure(error_code::validation_failed, "Span size does not match expected size");
    }

    return success();
}

}  // namespace sak
