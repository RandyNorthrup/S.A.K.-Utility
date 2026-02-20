/// @file file_scanner.h
/// @brief Recursive directory scanning with filtering
/// @details Provides high-performance directory traversal with pattern matching and callbacks

#pragma once

#include "error_codes.h"
#include "path_utils.h"
#include <filesystem>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <stop_token>
#include <atomic>

namespace sak {

/// @brief File type filter options
enum class file_type_filter {
    all,           ///< All file system entries
    files_only,    ///< Regular files only
    directories_only ///< Directories only
};

/// @brief Scan result statistics
struct scan_statistics {
    std::size_t files_found = 0;        ///< Number of files found
    std::size_t directories_found = 0;  ///< Number of directories found
    std::size_t errors_encountered = 0; ///< Number of errors during scan
    std::uintmax_t total_size = 0;      ///< Total size of all files (bytes)
    std::size_t skipped_by_filter = 0;  ///< Items skipped by filters
};

/// @brief Callback function for each found entry
/// @param path Path of the found entry
/// @param is_directory True if entry is a directory
/// @return True to continue scanning, false to stop
using scan_callback = std::function<bool(const std::filesystem::path& path, bool is_directory)>;

/// @brief Progress callback for scan operations
/// @param files_processed Number of files processed so far
/// @param total_size_processed Total size processed in bytes
using scan_progress_callback = std::function<void(std::size_t files_processed, std::uintmax_t total_size_processed)>;

/// @brief Scan options for file scanner
struct scan_options {
    bool recursive = true;                      ///< Recurse into subdirectories
    bool follow_symlinks = false;               ///< Follow symbolic links
    file_type_filter type_filter = file_type_filter::all; ///< Filter by type
    std::vector<std::string> include_patterns;  ///< Patterns to include (e.g., "*.txt")
    std::vector<std::string> exclude_patterns;  ///< Patterns to exclude
    std::vector<std::string> exclude_dirs;      ///< Directory names to exclude (e.g., ".git")
    std::size_t max_depth = 0;                  ///< Maximum recursion depth (0 = unlimited)
    std::uintmax_t min_file_size = 0;          ///< Minimum file size to include (bytes)
    std::uintmax_t max_file_size = 0;          ///< Maximum file size to include (0 = unlimited)
    bool skip_hidden = false;                   ///< Skip hidden files/directories
    bool calculate_sizes = true;                ///< Calculate file sizes during scan
    scan_callback callback;                     ///< Callback for each found entry
    scan_progress_callback progress_callback;   ///< Progress callback
};

/// @brief High-performance recursive directory scanner
/// @details Thread-safe scanner with filtering, pattern matching, and cancellation support
class file_scanner {
public:
    /// @brief Default constructor
    file_scanner() = default;
    
    /// @brief Scan directory with options
    /// @param root_path Root directory to scan
    /// @param options Scan options
    /// @param stop_token Cancellation token
    /// @return Expected containing statistics or error code
    [[nodiscard]] auto scan(
        const std::filesystem::path& root_path,
        const scan_options& options,
        std::stop_token stop_token = {}) -> std::expected<scan_statistics, error_code>;
    
    /// @brief Scan directory and collect all matching paths
    /// @param root_path Root directory to scan
    /// @param options Scan options
    /// @param stop_token Cancellation token
    /// @return Expected containing list of paths or error code
    [[nodiscard]] auto scan_and_collect(
        const std::filesystem::path& root_path,
        const scan_options& options,
        std::stop_token stop_token = {}) -> std::expected<std::vector<std::filesystem::path>, error_code>;
    
    /// @brief Simple recursive file listing
    /// @param root_path Root directory to scan
    /// @param recursive Whether to recurse into subdirectories
    /// @return Expected containing list of file paths or error code
    [[nodiscard]] static auto list_files(
        const std::filesystem::path& root_path,
        bool recursive = true) -> std::expected<std::vector<std::filesystem::path>, error_code>;
    
    /// @brief Find files matching patterns
    /// @param root_path Root directory to search
    /// @param patterns Filename patterns (e.g., "*.txt", "test_*")
    /// @param recursive Whether to recurse into subdirectories
    /// @return Expected containing list of matching paths or error code
    [[nodiscard]] static auto find_files(
        const std::filesystem::path& root_path,
        const std::vector<std::string>& patterns,
        bool recursive = true) -> std::expected<std::vector<std::filesystem::path>, error_code>;
    
private:
    /// @brief Check if entry should be processed based on filters
    /// @param entry Directory entry
    /// @param options Scan options
    /// @param current_depth Current recursion depth
    /// @return True if should process, false if should skip
    [[nodiscard]] bool should_process_entry(
        const std::filesystem::directory_entry& entry,
        const scan_options& options,
        std::size_t current_depth) const noexcept;
    
    /// @brief Check if path is hidden
    /// @param path Path to check
    /// @return True if hidden
    [[nodiscard]] static bool is_hidden(const std::filesystem::path& path) noexcept;
    
    /// @brief Recursive scan implementation
    /// @param current_path Current directory path
    /// @param options Scan options
    /// @param stats Statistics accumulator
    /// @param current_depth Current recursion depth
    /// @param stop_token Cancellation token
    /// @return Error code if error occurred, success otherwise
    auto scan_directory_recursive(
        const std::filesystem::path& current_path,
        const scan_options& options,
        scan_statistics& stats,
        std::size_t current_depth,
        std::stop_token stop_token) -> std::expected<void, error_code>;
    
    std::atomic<std::size_t> m_files_processed{0};  ///< Files processed counter
    std::atomic<std::uintmax_t> m_size_processed{0}; ///< Size processed counter
};

} // namespace sak

