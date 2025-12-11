/// @file path_utils.h
/// @brief Path manipulation and validation utilities
/// @details Provides cross-platform path operations with proper error handling

#ifndef SAK_PATH_UTILS_H
#define SAK_PATH_UTILS_H

#include "error_codes.h"
#include <filesystem>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace sak {

/// @brief Path utility functions for file system operations
/// @details Thread-safe utilities for path manipulation, validation, and normalization
class path_utils {
public:
    /// @brief Normalize a path (resolve .., ., remove redundant separators)
    /// @param path Path to normalize
    /// @return Expected containing normalized path or error code
    [[nodiscard]] static auto normalize(
        const std::filesystem::path& path) -> std::expected<std::filesystem::path, error_code>;
    
    /// @brief Make a path absolute
    /// @param path Path to make absolute
    /// @return Expected containing absolute path or error code
    [[nodiscard]] static auto make_absolute(
        const std::filesystem::path& path) -> std::expected<std::filesystem::path, error_code>;
    
    /// @brief Make a path relative to a base path
    /// @param path Path to make relative
    /// @param base Base path
    /// @return Expected containing relative path or error code
    [[nodiscard]] static auto make_relative(
        const std::filesystem::path& path,
        const std::filesystem::path& base) -> std::expected<std::filesystem::path, error_code>;
    
    /// @brief Check if a path is safe (not trying to escape base directory)
    /// @param path Path to check
    /// @param base_dir Base directory
    /// @return Expected containing true if safe, false if potentially dangerous, or error code
    [[nodiscard]] static auto is_safe_path(
        const std::filesystem::path& path,
        const std::filesystem::path& base_dir) -> std::expected<bool, error_code>;
    
    /// @brief Get file extension in lowercase
    /// @param path Path to extract extension from
    /// @return Lowercase extension (including dot) or empty string if no extension
    [[nodiscard]] static std::string get_extension_lowercase(
        const std::filesystem::path& path) noexcept;
    
    /// @brief Check if path matches any of the given patterns (wildcards supported)
    /// @param path Path to check
    /// @param patterns List of patterns (e.g., "*.txt", "test_*")
    /// @return True if path matches any pattern
    [[nodiscard]] static bool matches_pattern(
        const std::filesystem::path& path,
        const std::vector<std::string>& patterns) noexcept;
    
    /// @brief Get safe filename (remove/replace invalid characters)
    /// @param filename Original filename
    /// @return Safe filename suitable for the current OS
    [[nodiscard]] static std::string get_safe_filename(
        std::string_view filename) noexcept;
    
    /// @brief Calculate total size of directory (recursive)
    /// @param dir_path Directory path
    /// @return Expected containing total size in bytes or error code
    [[nodiscard]] static auto get_directory_size(
        const std::filesystem::path& dir_path) -> std::expected<std::uintmax_t, error_code>;
    
    /// @brief Get available disk space at path
    /// @param path Path to check
    /// @return Expected containing available bytes or error code
    [[nodiscard]] static auto get_available_space(
        const std::filesystem::path& path) -> std::expected<std::uintmax_t, error_code>;
    
    /// @brief Create directory structure (like mkdir -p)
    /// @param dir_path Directory path to create
    /// @return Expected containing void or error code
    [[nodiscard]] static auto create_directories(
        const std::filesystem::path& dir_path) -> std::expected<void, error_code>;
    
    /// @brief Remove file or directory (recursive if directory)
    /// @param path Path to remove
    /// @return Expected containing number of removed items or error code
    [[nodiscard]] static auto remove_all(
        const std::filesystem::path& path) -> std::expected<std::uintmax_t, error_code>;
    
    /// @brief Copy file or directory (recursive if directory)
    /// @param source Source path
    /// @param destination Destination path
    /// @param overwrite Whether to overwrite existing files
    /// @return Expected containing void or error code
    [[nodiscard]] static auto copy(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        bool overwrite = false) -> std::expected<void, error_code>;
    
    /// @brief Move/rename file or directory
    /// @param source Source path
    /// @param destination Destination path
    /// @return Expected containing void or error code
    [[nodiscard]] static auto move(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) -> std::expected<void, error_code>;
    
    /// @brief Check if path exists and is accessible
    /// @param path Path to check
    /// @return Expected containing true if exists and accessible, false otherwise, or error code
    [[nodiscard]] static auto exists_and_accessible(
        const std::filesystem::path& path) -> std::expected<bool, error_code>;
    
    /// @brief Get file/directory creation time
    /// @param path Path to query
    /// @return Expected containing file time or error code
    [[nodiscard]] static auto get_creation_time(
        const std::filesystem::path& path) -> std::expected<std::filesystem::file_time_type, error_code>;
    
    /// @brief Get file/directory last write time
    /// @param path Path to query
    /// @return Expected containing file time or error code
    [[nodiscard]] static auto get_last_write_time(
        const std::filesystem::path& path) -> std::expected<std::filesystem::file_time_type, error_code>;
    
    /// @brief Get temporary directory path
    /// @return Expected containing temp directory path or error code
    [[nodiscard]] static auto get_temp_directory() -> std::expected<std::filesystem::path, error_code>;
    
    /// @brief Create unique temporary directory
    /// @param prefix Directory name prefix
    /// @return Expected containing unique temp directory path or error code
    [[nodiscard]] static auto create_temp_directory(
        std::string_view prefix = "sak_temp") -> std::expected<std::filesystem::path, error_code>;
    
private:
    /// @brief Check if character is valid in filename for current OS
    /// @param c Character to check
    /// @return True if valid
    [[nodiscard]] static bool is_valid_filename_char(char c) noexcept;
    
    /// @brief Simple wildcard matching (*, ?)
    /// @param str String to match
    /// @param pattern Pattern with wildcards
    /// @return True if matches
    [[nodiscard]] static bool wildcard_match(
        std::string_view str,
        std::string_view pattern) noexcept;
};

} // namespace sak

#endif // SAK_PATH_UTILS_H
