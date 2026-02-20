/// @file file_scanner.cpp
/// @brief Implementation of recursive directory scanner

#include "sak/file_scanner.h"
#include "sak/logger.h"
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

auto file_scanner::scan(
    const std::filesystem::path& root_path,
    const scan_options& options,
    std::stop_token stop_token) -> std::expected<scan_statistics, error_code> {
    
    // Validate root path
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
             stats.files_found, stats.directories_found, stats.errors_encountered);
    
    return stats;
}

auto file_scanner::scanAndCollect(
    const std::filesystem::path& root_path,
    const scan_options& options,
    std::stop_token stop_token) -> std::expected<std::vector<std::filesystem::path>, error_code> {
    
    std::vector<std::filesystem::path> collected_paths;
    
    // Create modified options with collection callback
    scan_options modified_options = options;
    modified_options.callback = [&collected_paths, &options](
        const std::filesystem::path& path, bool is_directory) -> bool {
        
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

auto file_scanner::listFiles(
    const std::filesystem::path& root_path,
    bool recursive) -> std::expected<std::vector<std::filesystem::path>, error_code> {
    
    scan_options options;
    options.recursive = recursive;
    options.type_filter = file_type_filter::files_only;
    
    file_scanner scanner;
    return scanner.scanAndCollect(root_path, options);
}

auto file_scanner::findFiles(
    const std::filesystem::path& root_path,
    const std::vector<std::string>& patterns,
    bool recursive) -> std::expected<std::vector<std::filesystem::path>, error_code> {
    
    scan_options options;
    options.recursive = recursive;
    options.type_filter = file_type_filter::files_only;
    options.include_patterns = patterns;
    
    file_scanner scanner;
    return scanner.scanAndCollect(root_path, options);
}

bool file_scanner::shouldProcessEntry(
    const std::filesystem::directory_entry& entry,
    const scan_options& options,
    std::size_t current_depth) const noexcept {
    
    try {
        const auto& path = entry.path();
        
        // Check depth limit
        if (options.max_depth > 0 && current_depth >= options.max_depth) {
            return false;
        }
        
        // Check hidden files
        if (options.skip_hidden && isHidden(path)) {
            return false;
        }
        
        bool is_dir = entry.is_directory();
        bool is_file = entry.is_regular_file();
        
        // Check type filter
        switch (options.type_filter) {
            case file_type_filter::files_only:
                if (!is_file) return false;
                break;
            case file_type_filter::directories_only:
                if (!is_dir) return false;
                break;
            case file_type_filter::all:
                break;
        }
        
        // Check excluded directories
        if (is_dir) {
            auto dir_name = path.filename().string();
            for (const auto& excluded : options.exclude_dirs) {
                if (dir_name == excluded) {
                    return false;
                }
            }
        }
        
        // For files, check patterns and size filters
        if (is_file) {
            // Check exclude patterns
            if (!options.exclude_patterns.empty()) {
                if (path_utils::matchesPattern(path, options.exclude_patterns)) {
                    return false;
                }
            }
            
            // Check include patterns (if specified, must match at least one)
            if (!options.include_patterns.empty()) {
                if (!path_utils::matchesPattern(path, options.include_patterns)) {
                    return false;
                }
            }
            
            // Check file size
            if (options.calculate_sizes) {
                std::error_code ec;
                auto size = entry.file_size(ec);
                
                if (!ec) {
                    if (options.min_file_size > 0 && size < options.min_file_size) {
                        return false;
                    }
                    
                    if (options.max_file_size > 0 && size > options.max_file_size) {
                        return false;
                    }
                }
            }
        }
        
        return true;
        
    } catch (...) {
        return false;
    }
}

bool file_scanner::isHidden(const std::filesystem::path& path) noexcept {
    try {
        auto filename = path.filename().string();
        
        // Unix-style hidden files (start with .)
        if (!filename.empty() && filename[0] == '.') {
            return true;
        }
        
#ifdef _WIN32
        // Windows hidden attribute
        DWORD attrs = GetFileAttributesW(path.wstring().c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return (attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
        }
#endif
        
        return false;
        
    } catch (...) {
        return false;
    }
}

auto file_scanner::scanDirectoryRecursive(
    const std::filesystem::path& current_path,
    const scan_options& options,
    scan_statistics& stats,
    std::size_t current_depth,
    std::stop_token stop_token) -> std::expected<void, error_code> {
    
    // Check for cancellation
    if (stop_token.stop_requested()) {
        return std::unexpected(error_code::operation_cancelled);
    }
    
    try {
        // Choose directory iterator based on options
        auto dir_options = std::filesystem::directory_options::skip_permission_denied;
        
        if (options.follow_symlinks) {
            dir_options |= std::filesystem::directory_options::follow_directory_symlink;
        }
        
        std::error_code ec;
        std::filesystem::directory_iterator dir_it(current_path, dir_options, ec);
        
        if (ec) {
            logWarning("Failed to open directory: {} - {}", current_path.string(), ec.message());
            stats.errors_encountered++;
            return {}; // Continue with other directories
        }
        
        for (const auto& entry : dir_it) {
            // Check cancellation
            if (stop_token.stop_requested()) {
                return std::unexpected(error_code::operation_cancelled);
            }
            
            try {
                // Check if entry should be processed
                if (!shouldProcessEntry(entry, options, current_depth)) {
                    stats.skipped_by_filter++;
                    continue;
                }
                
                bool is_dir = entry.is_directory();
                bool is_file = entry.is_regular_file();
                
                // Update statistics
                if (is_file) {
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
                    
                    // Call progress callback
                    if (options.progress_callback) {
                        options.progress_callback(
                            m_files_processed.load(std::memory_order_relaxed),
                            m_size_processed.load(std::memory_order_relaxed)
                        );
                    }
                } else if (is_dir) {
                    stats.directories_found++;
                }
                
                // Call user callback
                if (options.callback) {
                    if (!options.callback(entry.path(), is_dir)) {
                        // User requested stop
                        return std::unexpected(error_code::operation_cancelled);
                    }
                }
                
                // Recurse into subdirectories
                if (is_dir && options.recursive) {
                    auto recurse_result = scanDirectoryRecursive(
                        entry.path(),
                        options,
                        stats,
                        current_depth + 1,
                        stop_token
                    );
                    
                    if (!recurse_result) {
                        if (recurse_result.error() == error_code::operation_cancelled) {
                            return recurse_result;
                        }
                        // For other errors, log and continue
                        stats.errors_encountered++;
                    }
                }
                
            } catch (const std::filesystem::filesystem_error& e) {
                logWarning("Error processing entry: {} - {}", entry.path().string(), e.what());
                stats.errors_encountered++;
                // Continue with next entry
            } catch (const std::exception& e) {
                logWarning("Unexpected error processing entry: {}", e.what());
                stats.errors_encountered++;
                // Continue with next entry
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

} // namespace sak
