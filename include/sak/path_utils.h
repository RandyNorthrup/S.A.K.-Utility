// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file path_utils.h
/// @brief Path manipulation and validation utilities
/// @details Provides cross-platform path operations with proper error handling

#pragma once

#include "error_codes.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sak {

/// @brief Path utility functions for file system operations
/// @details Thread-safe utilities for path manipulation, validation, and normalization
class path_utils {
public:
    /// @brief Make a path relative to a base path
    /// @param path Path to make relative
    /// @param base Base path
    /// @return Expected containing relative path or error code
    [[nodiscard]] static auto makeRelative(const std::filesystem::path& path,
                                           const std::filesystem::path& base)
        -> std::expected<std::filesystem::path, error_code>;

    /// @brief Check if a path is safe (not trying to escape base directory)
    /// @param path Path to check
    /// @param base_dir Base directory
    /// @return Expected containing true if safe, false if potentially dangerous, or error code
    [[nodiscard]] static auto isSafePath(const std::filesystem::path& path,
                                         const std::filesystem::path& base_dir)
        -> std::expected<bool, error_code>;

    /// @brief Check if path matches any of the given patterns (wildcards supported)
    /// @param path Path to check
    /// @param patterns List of patterns (e.g., "*.txt", "test_*")
    /// @return True if path matches any pattern
    [[nodiscard]] static bool matchesPattern(const std::filesystem::path& path,
                                             const std::vector<std::string>& patterns) noexcept;

    /// @brief Result type for directory size + file count queries
    struct DirectorySizeInfo {
        std::uintmax_t total_bytes{0};
        std::uintmax_t file_count{0};
    };

    /// @brief Calculate total size and file count of a directory (recursive)
    /// @param dir_path Directory path
    /// @return Expected containing size info or error code
    [[nodiscard]] static auto getDirectorySizeAndCount(const std::filesystem::path& dir_path)
        -> std::expected<DirectorySizeInfo, error_code>;

    /// @brief Get available disk space at path
    /// @param path Path to check
    /// @return Expected containing available bytes or error code
    [[nodiscard]] static auto getAvailableSpace(const std::filesystem::path& path)
        -> std::expected<std::uintmax_t, error_code>;

private:
    /// @brief Normalize a path (resolve .., ., remove redundant separators)
    /// @param path Path to normalize
    /// @return Expected containing normalized path or error code
    /// @note Internal only — used by isSafePath()
    [[nodiscard]] static auto normalize(const std::filesystem::path& path)
        -> std::expected<std::filesystem::path, error_code>;

    /// @brief Simple wildcard matching (*, ?)
    /// @param str String to match
    /// @param pattern Pattern with wildcards
    /// @return True if matches
    [[nodiscard]] static bool wildcardMatch(std::string_view str,
                                            std::string_view pattern) noexcept;
};

}  // namespace sak
