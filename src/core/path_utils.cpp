/// @file path_utils.cpp
/// @brief Implementation of path manipulation utilities

#include "sak/path_utils.h"
#include "sak/logger.h"
#include <algorithm>
#include <cctype>
#include <random>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sak {

auto path_utils::normalize(
    const std::filesystem::path& path) -> std::expected<std::filesystem::path, error_code> {
    
    try {
        auto normalized = std::filesystem::weakly_canonical(path);
        return normalized;
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to normalize path: {}", e.what());
        return std::unexpected(error_code::invalid_path);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::make_absolute(
    const std::filesystem::path& path) -> std::expected<std::filesystem::path, error_code> {
    
    try {
        if (path.is_absolute()) {
            return path;
        }
        return std::filesystem::absolute(path);
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to make path absolute: {}", e.what());
        return std::unexpected(error_code::invalid_path);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::make_relative(
    const std::filesystem::path& path,
    const std::filesystem::path& base) -> std::expected<std::filesystem::path, error_code> {
    
    try {
        auto rel = std::filesystem::relative(path, base);
        if (rel.empty()) {
            return std::unexpected(error_code::invalid_path);
        }
        return rel;
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to make path relative: {}", e.what());
        return std::unexpected(error_code::invalid_path);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::is_safe_path(
    const std::filesystem::path& path,
    const std::filesystem::path& base_dir) -> std::expected<bool, error_code> {
    
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
        
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

std::string path_utils::get_extension_lowercase(
    const std::filesystem::path& path) noexcept {
    
    try {
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        return ext;
    } catch (...) {
        return "";
    }
}

bool path_utils::matches_pattern(
    const std::filesystem::path& path,
    const std::vector<std::string>& patterns) noexcept {
    
    try {
        auto filename = path.filename().string();
        
        for (const auto& pattern : patterns) {
            if (wildcard_match(filename, pattern)) {
                return true;
            }
        }
        
        return false;
    } catch (...) {
        return false;
    }
}

std::string path_utils::get_safe_filename(std::string_view filename) noexcept {
    std::string safe;
    safe.reserve(filename.size());
    
    for (char c : filename) {
        if (is_valid_filename_char(c)) {
            safe.push_back(c);
        } else {
            safe.push_back('_');
        }
    }
    
    // Remove leading/trailing spaces and dots (Windows restriction)
    while (!safe.empty() && (safe.front() == ' ' || safe.front() == '.')) {
        safe.erase(safe.begin());
    }
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) {
        safe.pop_back();
    }
    
    // Ensure not empty
    if (safe.empty()) {
        safe = "unnamed";
    }
    
    return safe;
}

auto path_utils::get_directory_size(
    const std::filesystem::path& dir_path) -> std::expected<std::uintmax_t, error_code> {
    
    try {
        if (!std::filesystem::exists(dir_path)) {
            return std::unexpected(error_code::file_not_found);
        }
        
        if (!std::filesystem::is_directory(dir_path)) {
            return std::unexpected(error_code::not_a_directory);
        }
        
        std::uintmax_t total_size = 0;
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                dir_path, std::filesystem::directory_options::skip_permission_denied)) {
            
            if (entry.is_regular_file()) {
                std::error_code ec;
                auto size = entry.file_size(ec);
                if (!ec) {
                    total_size += size;
                }
            }
        }
        
        return total_size;
        
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to calculate directory size: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::get_available_space(
    const std::filesystem::path& path) -> std::expected<std::uintmax_t, error_code> {
    
    try {
        auto space_info = std::filesystem::space(path);
        return space_info.available;
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to get available space: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::create_directories(
    const std::filesystem::path& dir_path) -> std::expected<void, error_code> {
    
    try {
        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);
        
        if (ec) {
            log_error("Failed to create directories: {}", ec.message());
            return std::unexpected(error_code::write_error);
        }
        
        return {};
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to create directories: {}", e.what());
        return std::unexpected(error_code::write_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::remove_all(
    const std::filesystem::path& path) -> std::expected<std::uintmax_t, error_code> {
    
    try {
        std::error_code ec;
        auto removed = std::filesystem::remove_all(path, ec);
        
        if (ec) {
            log_error("Failed to remove path: {}", ec.message());
            return std::unexpected(error_code::write_error);
        }
        
        return removed;
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to remove path: {}", e.what());
        return std::unexpected(error_code::write_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::copy(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool overwrite) -> std::expected<void, error_code> {
    
    try {
        auto options = std::filesystem::copy_options::recursive;
        if (overwrite) {
            options |= std::filesystem::copy_options::overwrite_existing;
        }
        
        std::error_code ec;
        std::filesystem::copy(source, destination, options, ec);
        
        if (ec) {
            log_error("Failed to copy: {}", ec.message());
            return std::unexpected(error_code::write_error);
        }
        
        return {};
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to copy: {}", e.what());
        return std::unexpected(error_code::write_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::move(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) -> std::expected<void, error_code> {
    
    try {
        std::error_code ec;
        std::filesystem::rename(source, destination, ec);
        
        if (ec) {
            log_error("Failed to move: {}", ec.message());
            return std::unexpected(error_code::write_error);
        }
        
        return {};
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to move: {}", e.what());
        return std::unexpected(error_code::write_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::exists_and_accessible(
    const std::filesystem::path& path) -> std::expected<bool, error_code> {
    
    try {
        std::error_code ec;
        bool exists = std::filesystem::exists(path, ec);
        
        if (ec) {
            return false; // Path doesn't exist or not accessible
        }
        
        return exists;
    } catch (...) {
        return false;
    }
}

auto path_utils::get_creation_time(
    const std::filesystem::path& path) -> std::expected<std::filesystem::file_time_type, error_code> {
    
    try {
        // Note: C++20 doesn't have creation time in std::filesystem
        // Using last_write_time as fallback
        // Platform-specific implementation would be needed for true creation time
        return get_last_write_time(path);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::get_last_write_time(
    const std::filesystem::path& path) -> std::expected<std::filesystem::file_time_type, error_code> {
    
    try {
        std::error_code ec;
        auto time = std::filesystem::last_write_time(path, ec);
        
        if (ec) {
            log_error("Failed to get last write time: {}", ec.message());
            return std::unexpected(error_code::read_error);
        }
        
        return time;
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to get last write time: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::get_temp_directory() -> std::expected<std::filesystem::path, error_code> {
    try {
        std::error_code ec;
        auto temp_dir = std::filesystem::temp_directory_path(ec);
        
        if (ec) {
            log_error("Failed to get temp directory: {}", ec.message());
            return std::unexpected(error_code::read_error);
        }
        
        return temp_dir;
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to get temp directory: {}", e.what());
        return std::unexpected(error_code::read_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

auto path_utils::create_temp_directory(
    std::string_view prefix) -> std::expected<std::filesystem::path, error_code> {
    
    try {
        auto temp_dir_result = get_temp_directory();
        if (!temp_dir_result) {
            return std::unexpected(temp_dir_result.error());
        }
        
        // Generate unique directory name
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dis;
        
        for (int attempts = 0; attempts < 100; ++attempts) {
            auto unique_name = std::format("{}_{:016x}", prefix, dis(gen));
            auto temp_path = *temp_dir_result / unique_name;
            
            std::error_code ec;
            if (std::filesystem::create_directory(temp_path, ec) && !ec) {
                return temp_path;
            }
        }
        
        log_error("Failed to create unique temp directory after 100 attempts");
        return std::unexpected(error_code::write_error);
        
    } catch (const std::filesystem::filesystem_error& e) {
        log_error("Failed to create temp directory: {}", e.what());
        return std::unexpected(error_code::write_error);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

bool path_utils::is_valid_filename_char(char c) noexcept {
#ifdef _WIN32
    // Windows invalid characters: < > : " / \ | ? *
    // Also control characters (0-31)
    if (c < 32) return false;
    switch (c) {
        case '<': case '>': case ':': case '"':
        case '/': case '\\': case '|': case '?':
        case '*':
            return false;
        default:
            return true;
    }
#else
    // Unix/Linux: only / and null are invalid
    return c != '/' && c != '\0';
#endif
}

bool path_utils::wildcard_match(
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
