// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file path_utils.cpp
/// @brief Implementation of path manipulation utilities

#include "sak/path_utils.h"
#include "sak/logger.h"
#include <QtGlobal>
#include <algorithm>
#include <cctype>

namespace sak {

auto path_utils::normalize(
    const std::filesystem::path& path) -> std::expected<std::filesystem::path, error_code> {
    Q_ASSERT_X(!path.empty(), "path_utils::normalize", "path must not be empty");
    
    try {
        auto normalized = std::filesystem::weakly_canonical(path);
        Q_ASSERT_X(normalized.is_absolute(), "path_utils::normalize",
            "normalized path must be absolute");
        return normalized;
    } catch (const std::filesystem::filesystem_error& e) {
        logError("Failed to normalize path: {}", e.what());
        return std::unexpected(error_code::invalid_path);
    } catch (const std::exception& e) {
        logError("Unexpected exception in normalize(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    } catch (...) {
        logError("Non-standard exception in normalize()");
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::makeRelative(
    const std::filesystem::path& path,
    const std::filesystem::path& base) -> std::expected<std::filesystem::path, error_code> {
    Q_ASSERT_X(!path.empty(), "path_utils::makeRelative", "path must not be empty");
    Q_ASSERT_X(!base.empty(), "path_utils::makeRelative", "base must not be empty");
    
    try {
        auto rel = std::filesystem::relative(path, base);
        if (rel.empty()) {
            return std::unexpected(error_code::invalid_path);
        }
        return rel;
    } catch (const std::filesystem::filesystem_error& e) {
        logError("Failed to make path relative: {}", e.what());
        return std::unexpected(error_code::invalid_path);
    } catch (const std::exception& e) {
        logError("Unexpected exception in makeRelative(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    } catch (...) {
        logError("Non-standard exception in makeRelative()");
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::isSafePath(
    const std::filesystem::path& path,
    const std::filesystem::path& base_dir) -> std::expected<bool, error_code> {
    Q_ASSERT_X(!path.empty(), "path_utils::isSafePath", "path must not be empty");
    Q_ASSERT_X(!base_dir.empty(), "path_utils::isSafePath", "base_dir must not be empty");
    
    try {
        // Normalize both paths
        auto norm_path_result = normalize(path);
        auto norm_base_result = normalize(base_dir);
        
        if (!norm_path_result || !norm_base_result) {
            return std::unexpected(error_code::invalid_path);
        }
        
        auto norm_path = *norm_path_result;
        auto norm_base = *norm_base_result;
        
        // Check if normalized path starts with base directory
        auto path_str = norm_path.string();
        auto base_str = norm_base.string();

        auto normalize_for_compare = [](std::string value) {
            std::replace(value.begin(), value.end(), '\\', '/');
#ifdef _WIN32
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
            return value;
        };

        path_str = normalize_for_compare(std::move(path_str));
        base_str = normalize_for_compare(std::move(base_str));
        
        // Ensure both end with separator for proper comparison
        if (!base_str.empty() && base_str.back() != '/') {
            base_str += '/';
        }
        
        return path_str.starts_with(base_str) || path_str == normalize_for_compare(norm_base.string());
        
    } catch (const std::exception& e) {
        logError("Exception in isSafePath(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    } catch (...) {
        logError("Non-standard exception in isSafePath()");
        return std::unexpected(error_code::unknown_error);
    }
}

bool path_utils::matchesPattern(
    const std::filesystem::path& path,
    const std::vector<std::string>& patterns) noexcept {
    
    try {
        auto filename = path.filename().string();
        
        for (const auto& pattern : patterns) {
            if (wildcardMatch(filename, pattern)) {
                return true;
            }
        }
        
        return false;
    } catch (const std::exception& e) {
        logError("Exception in matchesPattern(): {}", e.what());
        return false;
    } catch (...) {
        logError("Non-standard exception in matchesPattern()");
        return false;
    }
}

auto path_utils::getDirectorySizeAndCount(
    const std::filesystem::path& dir_path) -> std::expected<DirectorySizeInfo, error_code> {
    Q_ASSERT_X(!dir_path.empty(), "path_utils::getDirectorySizeAndCount",
        "dir_path must not be empty");
    
    try {
        if (!std::filesystem::exists(dir_path)) {
            return std::unexpected(error_code::file_not_found);
        }
        
        if (!std::filesystem::is_directory(dir_path)) {
            return std::unexpected(error_code::not_a_directory);
        }
        
        DirectorySizeInfo info;
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                dir_path, std::filesystem::directory_options::skip_permission_denied)) {
            
            if (entry.is_regular_file()) {
                std::error_code ec;
                auto size = entry.file_size(ec);
                if (!ec) {
                    info.total_bytes += size;
                    info.file_count++;
                }
            }
        }
        
        return info;
        
    } catch (const std::filesystem::filesystem_error& e) {
        logError("Failed to calculate directory size: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (const std::exception& e) {
        logError("Unexpected exception in getDirectorySizeAndCount(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    } catch (...) {
        logError("Non-standard exception in getDirectorySizeAndCount()");
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::getAvailableSpace(
    const std::filesystem::path& path) -> std::expected<std::uintmax_t, error_code> {
    Q_ASSERT_X(!path.empty(), "path_utils::getAvailableSpace", "path must not be empty");
    
    try {
        auto space_info = std::filesystem::space(path);
        return space_info.available;
    } catch (const std::filesystem::filesystem_error& e) {
        logError("Failed to get available space: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (const std::exception& e) {
        logError("Unexpected exception in getAvailableSpace(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    } catch (...) {
        logError("Non-standard exception in getAvailableSpace()");
        return std::unexpected(error_code::unknown_error);
    }
}

bool path_utils::wildcardMatch(
    std::string_view str,
    std::string_view pattern) noexcept {
    
    std::size_t s = 0, p = 0;
    std::size_t star_idx = std::string_view::npos;
    std::size_t match_idx = 0;
    
    while (s < str.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == str[s])) {
            ++s;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_idx = p;
            match_idx = s;
            ++p;
        } else if (star_idx != std::string_view::npos) {
            p = star_idx + 1;
            ++match_idx;
            s = match_idx;
        } else {
            return false;
        }
    }
    
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    
    return p == pattern.size();
}

} // namespace sak
