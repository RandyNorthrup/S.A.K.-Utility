// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file path_utils.cpp
/// @brief Implementation of path manipulation utilities

#include "sak/path_utils.h"

#include "sak/logger.h"

#include <QtGlobal>

#include <algorithm>
#include <cctype>
#include <limits>
#include <system_error>
#include <vector>

namespace sak {

namespace {

/// @brief Normalize path separators and case for comparison
std::string normalize_for_compare(std::string value) {
    std::ranges::replace(value, '\\', '/');
#ifdef _WIN32
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return value;
}

/// @brief Saturating unsigned add of a file size onto a running byte total: clamps to
///        UINTMAX instead of wrapping. file_size() reports the LOGICAL size, so a handful
///        of attacker-created sparse files can each claim exabytes; a wrapped total would
///        under-report the space a set needs and let an oversized one be judged to fit
///        (fail open). Clamping keeps a downstream capacity check failing closed.
std::uintmax_t saturatingAddSize(std::uintmax_t total, std::uintmax_t size) {
    constexpr auto kUintMax = std::numeric_limits<std::uintmax_t>::max();
    return (total > kUintMax - size) ? kUintMax : total + size;
}

/// @brief Accumulate size information for a single directory entry
/// @return false only when a regular file's size cannot be read -- an unreadable real file must
///         fail the whole scan closed rather than be silently skipped, because an under-counted
///         total would let an oversized set be judged to fit (fail open).
bool accumulateFileEntry(const std::filesystem::directory_entry& entry,
                         path_utils::DirectorySizeInfo& info) {
    if (!entry.is_regular_file()) {
        return true;
    }
    std::error_code ec;
    auto size = entry.file_size(ec);
    if (ec) {
        return false;
    }
    // Saturate instead of wrapping (see saturatingAddSize).
    info.total_bytes = saturatingAddSize(info.total_bytes, size);
    constexpr auto kUintMax = std::numeric_limits<std::uintmax_t>::max();
    if (info.file_count < kUintMax) {
        ++info.file_count;
    }
    return true;
}

/// @brief Outcome of trying to open a directory for enumeration.
enum class OpenDirResult {
    opened,
    skipped_denied,
    failed
};

/// @brief Open dir_path for iteration, distinguishing a permission-denied skip
///        (kept non-fatal so a scan does not crash on a system tree) from a
///        genuine read error (which must fail the whole scan closed).
OpenDirResult openDirectory(const std::filesystem::path& dir_path,
                            std::filesystem::directory_iterator& out) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir_path, ec);
    if (!ec) {
        out = std::move(it);
        return OpenDirResult::opened;
    }
    if (ec == std::errc::permission_denied) {
        return OpenDirResult::skipped_denied;
    }
    return OpenDirResult::failed;
}

/// @brief True when entry is a real subdirectory to descend into. Symlinked and
///        junction directories are NOT followed (matches the previous
///        recursive_directory_iterator with no follow_directory_symlink), which
///        also keeps the walk free of symlink cycles.
bool shouldRecurse(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_symlink(ec) || ec) {
        return false;
    }
    return entry.is_directory(ec) && !ec;
}

/// @brief Push child_path's iterator onto stack. A permission-denied directory
///        is skipped and recorded on info (info.complete=false) instead of
///        aborting; only a genuine read error returns false (fail closed).
bool pushDirectory(const std::filesystem::path& child_path,
                   std::vector<std::filesystem::directory_iterator>& stack,
                   path_utils::DirectorySizeInfo& info) {
    std::filesystem::directory_iterator child;
    switch (openDirectory(child_path, child)) {
    case OpenDirResult::opened:
        stack.push_back(std::move(child));
        return true;
    case OpenDirResult::skipped_denied:
        info.complete = false;
        ++info.skipped_dirs;
        return true;
    case OpenDirResult::failed:
        return false;
    }
    return false;  // unreachable: all enum cases return above
}

/// @brief Size a directory tree with an explicit stack of directory_iterators.
///        Permission-denied subtrees are skipped and flagged on info; an
///        unreadable regular file or any other iteration error fails closed.
/// @return error_code::success on a completed (possibly partial) walk.
error_code walkDirectoryTree(const std::filesystem::path& root,
                             path_utils::DirectorySizeInfo& info) {
    std::vector<std::filesystem::directory_iterator> stack;
    if (!pushDirectory(root, stack, info)) {
        return error_code::read_error;
    }
    const std::filesystem::directory_iterator end;
    while (!stack.empty()) {
        auto& level = stack.back();
        if (level == end) {
            stack.pop_back();
            continue;
        }
        const std::filesystem::directory_entry entry = *level;
        std::error_code ec;
        level.increment(ec);
        if (ec || !accumulateFileEntry(entry, info)) {
            return error_code::read_error;
        }
        if (shouldRecurse(entry) && !pushDirectory(entry.path(), stack, info)) {
            return error_code::read_error;
        }
    }
    return error_code::success;
}

}  // anonymous namespace

auto path_utils::normalize(const std::filesystem::path& path)
    -> std::expected<std::filesystem::path, error_code> {
    // isSafePath() is the only caller and rejects an empty path and an empty
    // base_dir before either normalize() call below it.
    Q_ASSERT_X(!path.empty(), "path_utils::normalize", "path must not be empty");

    try {
        auto normalized = std::filesystem::weakly_canonical(path);
        if (!normalized.is_absolute()) {
            // weakly_canonical leaves a path it cannot anchor to an existing
            // prefix relative. isSafePath's containment comparison only means
            // anything between two absolute paths, so fail closed rather than
            // answer a security question from a half-resolved path.
            logError("Normalized path is not absolute: {}", normalized.string());
            return std::unexpected(error_code::invalid_path);
        }
        return normalized;
    } catch (const std::filesystem::filesystem_error& e) {
        logError("Failed to normalize path: {}", e.what());
        return std::unexpected(error_code::invalid_path);
    } catch (const std::exception& e) {
        logError("Unexpected exception in normalize(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::makeRelative(const std::filesystem::path& path, const std::filesystem::path& base)
    -> std::expected<std::filesystem::path, error_code> {
    // Reject the empty inputs explicitly. std::filesystem::relative is specified as
    // weakly_canonical(path).lexically_relative(weakly_canonical(base)), and
    // lexically_relative on two equal paths yields "." rather than an empty path -- so
    // makeRelative("", "") used to succeed and hand back ".", a path that resolves to the
    // caller's working directory. The empty check below never covered that.
    if (path.empty() || base.empty()) {
        logError("makeRelative requires a non-empty path and base");
        return std::unexpected(error_code::invalid_path);
    }
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
    }
}

auto path_utils::isSafePath(const std::filesystem::path& path,
                            const std::filesystem::path& base_dir)
    -> std::expected<bool, error_code> {
    // Containment has no meaning without both operands, and this answer gates
    // path-traversal decisions, so refuse instead of returning a guessed bool.
    if (path.empty() || base_dir.empty()) {
        logError("isSafePath requires a non-empty path and base directory");
        return std::unexpected(error_code::invalid_path);
    }

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

        path_str = normalize_for_compare(std::move(path_str));
        base_str = normalize_for_compare(std::move(base_str));

        // Ensure both end with separator for proper comparison
        if (!base_str.empty() && base_str.back() != '/') {
            base_str += '/';
        }

        return path_str.starts_with(base_str) ||
               path_str == normalize_for_compare(norm_base.string());

    } catch (const std::exception& e) {
        logError("Exception in isSafePath(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

bool path_utils::matchesPattern(const std::filesystem::path& path,
                                const std::vector<std::string>& patterns) noexcept {
    try {
        const auto filename = path.filename().string();
        return std::ranges::any_of(patterns,

                                   [&filename](const std::string& pattern) {
                                       return wildcardMatch(filename, pattern);
                                   });
    } catch (const std::exception& e) {
        logError("Exception in matchesPattern(): {}", e.what());
        return false;
    } catch (...) {
        // Intentional: final safety net in noexcept function
        logError("Non-standard exception in matchesPattern()");
        return false;
    }
}

auto path_utils::getDirectorySizeAndCount(const std::filesystem::path& dir_path)
    -> std::expected<DirectorySizeInfo, error_code> {
    // An empty dir_path fails the exists() check below and is reported as
    // file_not_found.
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec) || ec) {
        return std::unexpected(error_code::file_not_found);
    }

    if (!std::filesystem::is_directory(dir_path, ec) || ec) {
        return std::unexpected(error_code::not_a_directory);
    }

    DirectorySizeInfo info;

    try {
        // A permission-denied subtree is skipped (info.complete is set false and
        // info.skipped_dirs counted) rather than crashing the scan on a system
        // tree; an unreadable regular file or any other read error fails closed.
        if (const auto walk_ec = walkDirectoryTree(dir_path, info);
            walk_ec != error_code::success) {
            logError("Failed to calculate the size of directory: {}", dir_path.string());
            return std::unexpected(walk_ec);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        logError("Failed to calculate directory size: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (const std::exception& e) {
        logError("Unexpected exception in getDirectorySizeAndCount(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }

    return info;
}

std::uintmax_t path_utils::saturatingAddSizeForTesting(std::uintmax_t total, std::uintmax_t size) {
    return saturatingAddSize(total, size);
}

auto path_utils::getAvailableSpace(const std::filesystem::path& path)
    -> std::expected<std::uintmax_t, error_code> {
    // An empty path makes std::filesystem::space() fail; the handler below turns
    // that into read_error.
    try {
        auto space_info = std::filesystem::space(path);
        return space_info.available;
    } catch (const std::filesystem::filesystem_error& e) {
        logError("Failed to get available space: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (const std::exception& e) {
        logError("Unexpected exception in getAvailableSpace(): {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

bool path_utils::wildcardMatch(std::string_view str, std::string_view pattern) noexcept {
    // Empty inputs need no precondition: the loops below already answer them (an
    // empty pattern matches only an empty string). The caller's pattern list and
    // the filename it derives are both outside this function's control.
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

}  // namespace sak
