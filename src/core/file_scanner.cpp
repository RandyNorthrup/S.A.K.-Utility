// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_scanner.cpp
/// @brief Implementation of recursive directory scanner

#include "sak/file_scanner.h"

#include "sak/logger.h"

#include <QtGlobal>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots
#else
#include <sys/stat.h>
#endif

namespace sak {

auto file_scanner::scan(const std::filesystem::path& root_path,
                        const scan_options& options,
                        std::stop_token stop_token) -> std::expected<scan_statistics, error_code> {
    // Validate root path. An empty root_path never exists, so it is reported
    // here as file_not_found.
    if (!std::filesystem::exists(root_path)) {
        logError("Scan root path does not exist: {}", root_path.string());
        return std::unexpected(error_code::file_not_found);
    }

    if (!std::filesystem::is_directory(root_path)) {
        logError("Scan root path is not a directory: {}", root_path.string());
        return std::unexpected(error_code::not_a_directory);
    }

    // Reset counters
    m_files_processed.store(0, std::memory_order_relaxed);
    m_size_processed.store(0, std::memory_order_relaxed);

    // Reset the symlink-cycle guard and seed it with the root so a link pointing
    // back at the root is caught. Only meaningful when following symlinks.
    m_visited_dirs.clear();
    if (options.follow_symlinks) {
        std::error_code root_ec;
        const auto canonical_root = std::filesystem::canonical(root_path, root_ec);
        if (!root_ec) {
            m_visited_dirs.insert(canonical_root.string());
        }
    }

    scan_statistics stats;

    logInfo("Starting directory scan: {}", root_path.string());

    // Start recursive scan
    auto result = scanDirectoryRecursive(root_path, options, stats, 0, stop_token);

    if (!result) {
        return std::unexpected(result.error());
    }

    if (stop_token.stop_requested()) {
        logWarning("Directory scan cancelled");
        return std::unexpected(error_code::operation_cancelled);
    }

    logInfo("Directory scan complete: {} files, {} dirs, {} errors",
            stats.files_found,
            stats.directories_found,
            stats.errors_encountered);

    // Postcondition: stats counters must be consistent. Every counter is an
    // unsigned member of the local scan_statistics that this scan only ever
    // increments, so a zero sum implies files_found == 0.
    Q_ASSERT_X(stats.files_found + stats.directories_found + stats.skipped_by_filter +
                           stats.errors_encountered >
                       0 ||
                   stats.files_found == 0,
               "file_scanner::scan",
               "scan stats must be consistent");

    // Incomplete-scan contract (R5-P9-20): this is a count-and-continue bulk scan.
    // A non-zero stats.errors_encountered means one or more directories/entries could
    // not be enumerated, so the returned statistics describe a PARTIAL scan. A caller
    // that requires completeness must treat errors_encountered > 0 as incomplete and
    // must not trust a successful expected<> alone. errors_encountered is the explicit
    // incomplete indicator surfaced in the returned value.
    return stats;
}

auto file_scanner::scanAndCollect(const std::filesystem::path& root_path,
                                  const scan_options& options,
                                  std::stop_token stop_token)
    -> std::expected<std::vector<std::filesystem::path>, error_code> {
    std::vector<std::filesystem::path> collected_paths;

    // Create modified options with collection callback
    scan_options modified_options = options;
    modified_options.callback = [&collected_paths, &options](const std::filesystem::path& path,
                                                             bool is_directory) -> bool {
        // Add to collection
        collected_paths.push_back(path);

        // Call user callback if provided
        if (options.callback) {
            return options.callback(path, is_directory);
        }

        return true;
    };

    auto result = scan(root_path, modified_options, stop_token);

    if (!result) {
        return std::unexpected(result.error());
    }

    return collected_paths;
}

auto file_scanner::listFiles(const std::filesystem::path& root_path, bool recursive)
    -> std::expected<std::vector<std::filesystem::path>, error_code> {
    // An empty root_path is rejected by scan(), which scanAndCollect() reaches
    // unconditionally, and reported as file_not_found.
    scan_options options;
    options.recursive = recursive;
    options.type_filter = file_type_filter::files_only;

    file_scanner scanner;
    return scanner.scanAndCollect(root_path, options);
}

auto file_scanner::findFiles(const std::filesystem::path& root_path,
                             const std::vector<std::string>& patterns,
                             bool recursive)
    -> std::expected<std::vector<std::filesystem::path>, error_code> {
    // An empty root_path is rejected by scan() as file_not_found. An empty
    // pattern list is not: it would leave include_patterns empty, which the scan
    // reads as "no filter" and answers a filtered query with every file. Fail
    // closed instead.
    if (patterns.empty()) {
        logError("findFiles requires at least one pattern");
        return std::unexpected(error_code::invalid_argument);
    }

    scan_options options;
    options.recursive = recursive;
    options.type_filter = file_type_filter::files_only;
    options.include_patterns = patterns;

    file_scanner scanner;
    return scanner.scanAndCollect(root_path, options);
}

namespace {

bool passesTypeFilter(file_type_filter filter, bool is_dir, bool is_file) {
    switch (filter) {
    case file_type_filter::files_only:
        return is_file;
    case file_type_filter::directories_only:
        return is_dir;
    case file_type_filter::all:
        return true;
    }
    return true;
}

bool isExcludedDirectory(const std::filesystem::path& path,
                         const std::vector<std::string>& exclude_dirs) {
    const auto dir_name = path.filename().string();
    return std::ranges::any_of(exclude_dirs,
                               [&dir_name](const auto& excl) { return dir_name == excl; });
}

bool checkSizeFilter(const std::filesystem::directory_entry& entry, const scan_options& options) {
    const bool has_size_limit = options.min_file_size > 0 || options.max_file_size > 0;
    std::error_code ec;
    auto size = entry.file_size(ec);
    if (ec) {
        // Fail closed (R5-P9-32): a size that cannot be stat'd cannot be shown to
        // satisfy a configured min/max limit, so exclude the file rather than letting
        // it bypass the limit. With no limit configured there is nothing to enforce,
        // so the stat failure alone must not drop the file from an unfiltered scan.
        return !has_size_limit;
    }
    if (options.min_file_size > 0 && size < options.min_file_size) {
        return false;
    }
    if (options.max_file_size > 0 && size > options.max_file_size) {
        return false;
    }
    return true;
}

// Non-following test for a reparse point (symbolic link OR junction/mount point). directory_entry::
// is_symlink() alone is insufficient on Windows: MSVC maps a junction to file_type::junction (not
// file_type::symlink), so is_symlink() returns false for junctions. We therefore read the reparse
// attribute directly -- GetFileAttributesW returns the reparse point's OWN attributes and does NOT
// follow the link, so it never resolves a (possibly UNC) target. Mirrors the leaf guard
// sak::pathIsReparsePoint used across the file ops. On non-Windows, is_symlink() is authoritative.
bool isReparsePointEntry(const std::filesystem::directory_entry& entry) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(entry.path().wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
#endif
    return entry.is_symlink();
}

}  // namespace

bool file_scanner::passesDepthAndVisibility(const std::filesystem::path& path,
                                            const scan_options& options,
                                            std::size_t current_depth) {
    if (options.max_depth > 0 && current_depth >= options.max_depth) {
        return false;
    }
    return !(options.skip_hidden && isHidden(path));
}

bool file_scanner::shouldProcessEntry(const std::filesystem::directory_entry& entry,
                                      const scan_options& options,
                                      std::size_t current_depth) noexcept {
    try {
        const auto& path = entry.path();

        if (!passesDepthAndVisibility(path, options, current_depth)) {
            return false;
        }

        const bool is_dir = entry.is_directory();
        const bool is_file = entry.is_regular_file();

        if (!passesTypeFilter(options.type_filter, is_dir, is_file)) {
            return false;
        }

        if (is_dir && isExcludedDirectory(path, options.exclude_dirs)) {
            return false;
        }

        if (is_file && !shouldIncludeFile(entry, path, options)) {
            return false;
        }

        return true;

    } catch (const std::exception& e) {
        logDebug("Exception in shouldProcessEntry(): {}", e.what());
        return false;
    } catch (...) {
        // Intentional: final safety net in noexcept function
        logDebug("Non-standard exception in shouldProcessEntry()");
        return false;
    }
}

bool file_scanner::shouldIncludeFile(const std::filesystem::directory_entry& entry,
                                     const std::filesystem::path& path,
                                     const scan_options& options) noexcept {
    try {
        if (!options.exclude_patterns.empty() &&
            path_utils::matchesPattern(path, options.exclude_patterns)) {
            return false;
        }

        if (!options.include_patterns.empty() &&
            !path_utils::matchesPattern(path, options.include_patterns)) {
            return false;
        }

        if (options.calculate_sizes && !checkSizeFilter(entry, options)) {
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        logDebug("Exception in shouldIncludeFile(): {}", e.what());
        return false;
    } catch (...) {
        // Intentional: final safety net in noexcept function
        logDebug("Non-standard exception in shouldIncludeFile()");
        return false;
    }
}

bool file_scanner::isHidden(const std::filesystem::path& path) noexcept {
    // Reached only through passesDepthAndVisibility(), whose two callers
    // (shouldProcessEntry, canDescendInto) both pass directory_entry::path(),
    // which is never empty.
    Q_ASSERT(!path.empty());
    try {
        auto filename = path.filename().string();

        // Unix-style hidden files (start with .)
        if (!filename.empty() && filename[0] == '.') {
            return true;
        }

#ifdef _WIN32
        // Windows hidden attribute
        DWORD const attrs = GetFileAttributesW(path.wstring().c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return (attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
        }
#endif

        return false;

    } catch (const std::exception& e) {
        logDebug("Exception in isHidden(): {}", e.what());
        return false;
    } catch (...) {
        // Intentional: final safety net in noexcept function
        logDebug("Non-standard exception in isHidden()");
        return false;
    }
}

auto file_scanner::recurseIntoDirectory(const std::filesystem::path& dir_path,
                                        const scan_options& options,
                                        scan_statistics& stats,
                                        std::size_t current_depth,
                                        std::stop_token stop_token)
    -> std::expected<void, error_code> {
    auto recurse_result =
        scanDirectoryRecursive(dir_path, options, stats, current_depth + 1, stop_token);

    if (!recurse_result && recurse_result.error() == error_code::operation_cancelled) {
        return recurse_result;
    }
    if (!recurse_result) {
        stats.errors_encountered++;
    }
    return {};
}

bool file_scanner::canDescendInto(const std::filesystem::directory_entry& entry,
                                  const scan_options& options,
                                  scan_statistics& stats,
                                  std::size_t current_depth) {
    const auto& path = entry.path();
    if (!passesDepthAndVisibility(path, options, current_depth)) {
        return false;
    }
    if (isExcludedDirectory(path, options.exclude_dirs)) {
        return false;
    }
    // Never follow a symlinked/junction directory unless explicitly enabled: a
    // link can escape the scan root, and one pointing at an ancestor recurses
    // without bound until the path length errors out (an effective hang).
    // isReparsePointEntry (not is_symlink()) so a Windows JUNCTION -- which
    // directory_entry maps to file_type::junction and is_symlink() misses -- is also
    // refused; otherwise a planted junction steers the default walk out of the scan
    // root (B8-16). This is the recursion gate even when skip_symlinks is off; the
    // skip_symlinks path already drops reparse points wholesale before this runs.
    if (isReparsePointEntry(entry) && !options.follow_symlinks) {
        return false;
    }
    // When following is enabled, break cycles by canonical identity. A canonicalization
    // FAILURE (unreadable junction/symlink target, access denied, IO error) leaves the entry
    // with no identity to register, so descending would silently drop the cycle/confinement
    // guard for that subtree -- a planted link could then loop or steer the walk out of the
    // scan root. Fail closed: refuse the descent and count it as an error rather than
    // traversing blind (a skip that is never reported would read as "nothing was there").
    if (options.follow_symlinks) {
        std::error_code ec;
        const auto canonical = std::filesystem::canonical(path, ec);
        if (ec) {
            logWarning("Refusing to follow {}: canonical path unresolved ({})",
                       path.string(),
                       ec.message());
            stats.errors_encountered++;
            return false;
        }
        if (!m_visited_dirs.insert(canonical.string()).second) {
            return false;  // already descended into this directory this scan
        }
    }
    return true;
}

auto file_scanner::processScannedEntry(const std::filesystem::directory_entry& entry,
                                       const scan_options& options,
                                       scan_statistics& stats,
                                       std::size_t current_depth,
                                       std::stop_token stop_token)
    -> std::expected<void, error_code> {
    const bool is_dir = entry.is_directory();
    const bool is_file = entry.is_regular_file();

    // Emission is gated by the output type filter (files_only/directories_only)
    // and the file-specific filters. This is kept separate from the traversal
    // decision so a files_only scan still recurses into subdirectories.
    if (shouldProcessEntry(entry, options, current_depth)) {
        if (is_file) {
            updateFileStats(entry, options, stats);
        } else if (is_dir) {
            stats.directories_found++;
        }
        if (options.callback && !options.callback(entry.path(), is_dir)) {
            return std::unexpected(error_code::operation_cancelled);
        }
    } else {
        stats.skipped_by_filter++;
    }

    // Traversal is decided independently of the output type filter.
    if (is_dir && options.recursive && canDescendInto(entry, options, stats, current_depth)) {
        return recurseIntoDirectory(entry.path(), options, stats, current_depth, stop_token);
    }

    return {};
}

void file_scanner::updateFileStats(const std::filesystem::directory_entry& entry,
                                   const scan_options& options,
                                   scan_statistics& stats) {
    stats.files_found++;

    if (options.calculate_sizes) {
        std::error_code size_ec;
        auto size = entry.file_size(size_ec);
        if (!size_ec) {
            stats.total_size += size;
            m_size_processed.fetch_add(size, std::memory_order_relaxed);
        }
    }

    m_files_processed.fetch_add(1, std::memory_order_relaxed);

    if (options.progress_callback) {
        options.progress_callback(m_files_processed.load(std::memory_order_relaxed),
                                  m_size_processed.load(std::memory_order_relaxed));
    }
}

auto file_scanner::processEntryWithErrorHandling(const std::filesystem::directory_entry& entry,
                                                 const scan_options& options,
                                                 scan_statistics& stats,
                                                 std::size_t current_depth,
                                                 std::stop_token stop_token)
    -> std::expected<void, error_code> {
    try {
        // R2: a reparse-point (symlink/junction) entry is dropped BEFORE processScannedEntry runs
        // any type/size query when skip_symlinks is set. isReparsePointEntry reads the reparse
        // attribute non-following, so unlike is_regular_file()/file_size() -- which resolve the
        // target -- it never triggers the SMB/NTLM handshake a UNC-targeted symlink would. Skipping
        // here means the entry is neither emitted NOR descended, so a planted junction also cannot
        // steer the headless walk into an arbitrary local tree. Kept inside the try so the
        // non-following stat's own errors are absorbed like any other per-entry failure.
        if (options.skip_symlinks && isReparsePointEntry(entry)) {
            stats.skipped_by_filter++;
            return {};
        }
        return processScannedEntry(entry, options, stats, current_depth, stop_token);
    } catch (const std::filesystem::filesystem_error& e) {
        logWarning("Error processing entry: {} - {}", entry.path().string(), e.what());
        stats.errors_encountered++;
        return {};
    } catch (const std::exception& e) {
        logWarning("Unexpected error processing entry: {}", e.what());
        stats.errors_encountered++;
        return {};
    }
}

auto file_scanner::scanDirectoryRecursive(const std::filesystem::path& current_path,
                                          const scan_options& options,
                                          scan_statistics& stats,
                                          std::size_t current_depth,
                                          std::stop_token stop_token)
    -> std::expected<void, error_code> {
    // scan() rejects a root that does not exist (an empty path never does) before
    // the first call, and recurseIntoDirectory() passes directory_entry::path().
    Q_ASSERT_X(!current_path.empty(), "scanDirectoryRecursive", "current_path must not be empty");

    if (stop_token.stop_requested()) {
        return std::unexpected(error_code::operation_cancelled);
    }

    try {
        auto dir_options = std::filesystem::directory_options::skip_permission_denied;
        if (options.follow_symlinks) {
            dir_options |= std::filesystem::directory_options::follow_directory_symlink;
        }

        std::error_code ec;
        const std::filesystem::directory_iterator dir_it(current_path, dir_options, ec);

        if (ec) {
            logWarning("Failed to open directory: {} - {}", current_path.string(), ec.message());
            stats.errors_encountered++;
            return {};  // Continue with other directories
        }

        for (const auto& entry : dir_it) {
            if (stop_token.stop_requested()) {
                return std::unexpected(error_code::operation_cancelled);
            }

            auto result =
                processEntryWithErrorHandling(entry, options, stats, current_depth, stop_token);
            if (!result) {
                return result;
            }
        }

        return {};

    } catch (const std::filesystem::filesystem_error& e) {
        logError("Filesystem error during scan: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (const std::exception& e) {
        logError("Unexpected error during scan: {}", e.what());
        return std::unexpected(error_code::unknown_error);
    }
}

}  // namespace sak
