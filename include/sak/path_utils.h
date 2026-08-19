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
#include <system_error>
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
        /// @brief False when one or more subtrees were skipped (permission
        ///        denied) so total_bytes/file_count under-report. A caller
        ///        gating a capacity/fit decision MUST treat an incomplete
        ///        result as "unknown", never as "fits".
        bool complete{true};
        /// @brief Count of directory subtrees skipped due to permission denied.
        std::uintmax_t skipped_dirs{0};
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

    /// @brief Test seam for the saturating byte-accumulate guard in the recursive size
    ///        walk: total + size clamped to UINTMAX, never wrapping. file_size() reports
    ///        the LOGICAL size, so attacker-created sparse files can each claim exabytes; a
    ///        wrapped total would under-report and let an oversized set be judged to fit
    ///        (fail open), so the accumulate clamps to keep a capacity check failing closed.
    [[nodiscard]] static std::uintmax_t saturatingAddSizeForTesting(std::uintmax_t total,
                                                                    std::uintmax_t size);

    /// @brief Observable outcome of the recursive size walk's directory-open step, for the
    ///        permission-denied test seam below.
    struct DirectoryOpenStepForTesting {
        /// @brief False means the whole scan fails closed (a genuine read error); true means
        ///        the walk continues (an opened directory or a permission-denied skip).
        bool continued{false};
        /// @brief DirectorySizeInfo::complete after the step -- false when a skipped subtree
        ///        flagged the result partial.
        bool complete_after{true};
        /// @brief DirectorySizeInfo::skipped_dirs after the step.
        std::uintmax_t skipped_dirs_after{0};
    };

    /// @brief Test seam for the permission-denied subtree handling in the recursive size walk.
    ///        A directory that denies enumeration is not a deterministic unit fixture on Windows
    ///        (a self-DENY ACL is bypassable by an elevated runner and breaks temp-dir cleanup),
    ///        so this runs the REAL classify + record chain the walk uses on the std::error_code
    ///        that constructing a directory_iterator on such a directory yields: a
    ///        permission_denied error is a NON-FATAL skip that flags the result incomplete
    ///        (continued=true, complete_after=false, one skipped dir), while any OTHER error fails
    ///        the whole scan closed (continued=false). This proves the flag-setting chain, not
    ///        just the error classification.
    [[nodiscard]] static DirectoryOpenStepForTesting applyDirectoryOpenErrorForTesting(
        const std::error_code& ec);

private:
    /// @brief Normalize a path (resolve .., ., remove redundant separators)
    /// @param path Path to normalize
    /// @return Expected containing normalized path or error code
    /// @note Internal only -- used by isSafePath()
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
