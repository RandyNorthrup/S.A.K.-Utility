/// @file logger.cpp
/// @brief Implementation of thread-safe logging system

#include "sak/logger.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace sak {

logger& logger::instance() noexcept {
    static logger instance;
    return instance;
}

logger::~logger() {
    flush();
}

auto logger::initialize(
    const std::filesystem::path& log_dir,
    std::string_view prefix) -> std::expected<void, error_code> {
    
    std::lock_guard lock(m_mutex);
    
    // Ensure log directory exists
    if (auto result = ensure_log_directory(log_dir); !result) {
        return std::unexpected(result.error());
    }
    
    m_log_dir = log_dir;
    m_prefix = prefix;
    
    // Generate log file path with timestamp
    auto timestamp = std::format("{:%Y-%m-%d_%H-%M-%S}",
                                 std::chrono::system_clock::now());
    m_log_file = m_log_dir / std::format("{}_{}.log", m_prefix, timestamp);
    
    // Open log file
    m_file_stream.open(m_log_file, std::ios::out | std::ios::app);
    if (!m_file_stream.is_open()) {
        return std::unexpected(error_code::write_error);
    }
    
    m_initialized.store(true, std::memory_order_release);
    m_bytes_written.store(0, std::memory_order_relaxed);
    
    // Write initialization message
    log_info("Logger initialized: {}", m_log_file.string());
    
    return {};
}

void logger::set_level(log_level level) noexcept {
    m_min_level.store(level, std::memory_order_relaxed);
}

log_level logger::get_level() const noexcept {
    return m_min_level.load(std::memory_order_relaxed);
}

void logger::set_console_output(bool enable) noexcept {
    m_console_output.store(enable, std::memory_order_relaxed);
}

void logger::flush() noexcept {
    std::lock_guard lock(m_mutex);
    if (m_file_stream.is_open()) {
        m_file_stream.flush();
    }
}

std::filesystem::path logger::get_log_file() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_log_file;
}

bool logger::is_initialized() const noexcept {
    return m_initialized.load(std::memory_order_acquire);
}

void logger::log_internal(
    log_level level,
    std::string_view message,
    const std::source_location& loc) noexcept {
    
    if (!is_initialized()) {
        // Fallback to console if not initialized
        if (m_console_output.load(std::memory_order_relaxed)) {
            std::println(std::cerr, "[{}] {}", to_string(level), message);
        }
        return;
    }
    
    try {
        // Format log entry
        auto timestamp = get_timestamp();
        auto level_str = to_string(level);
        auto filename = std::filesystem::path(loc.file_name()).filename().string();
        
        auto log_entry = std::format(
            "[{}] [{}] [{}:{}:{}] {}\n",
            timestamp,
            level_str,
            filename,
            loc.line(),
            loc.function_name(),
            message
        );
        
        {
            std::lock_guard lock(m_mutex);
            
            // Check if rotation is needed
            if (needs_rotation()) {
                rotate_log();
            }
            
            // Write to file
            if (m_file_stream.is_open()) {
                m_file_stream << log_entry;
                m_bytes_written.fetch_add(log_entry.size(), std::memory_order_relaxed);
                
                // Flush immediately for error and critical messages
                if (level >= log_level::error) {
                    m_file_stream.flush();
                }
            }
        }
        
        // Console output
        if (m_console_output.load(std::memory_order_relaxed)) {
            if (level >= log_level::error) {
                std::print(std::cerr, "{}", log_entry);
            } else {
                std::print("{}", log_entry);
            }
        }
        
    } catch (...) {
        // Silently ignore errors - never throw from logger
    }
}

auto logger::ensure_log_directory(
    const std::filesystem::path& dir) -> std::expected<void, error_code> {
    
    try {
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
        
        if (!std::filesystem::is_directory(dir)) {
            return std::unexpected(error_code::not_a_directory);
        }
        
        // Check write permission
        auto test_file = dir / ".test_write";
        std::ofstream test(test_file);
        if (!test.is_open()) {
            return std::unexpected(error_code::permission_denied);
        }
        test.close();
        std::filesystem::remove(test_file);
        
        return {};
        
    } catch (const std::filesystem::filesystem_error&) {
        return std::unexpected(error_code::permission_denied);
    } catch (...) {
        return std::unexpected(error_code::unknown_error);
    }
}

std::string logger::get_timestamp() const noexcept {
    try {
        auto now = std::chrono::system_clock::now();
        return std::format("{:%Y-%m-%d %H:%M:%S}", now);
    } catch (...) {
        return "TIMESTAMP_ERROR";
    }
}

bool logger::needs_rotation() const noexcept {
    return m_bytes_written.load(std::memory_order_relaxed) >= MAX_LOG_SIZE;
}

void logger::rotate_log() noexcept {
    try {
        // Close current file
        if (m_file_stream.is_open()) {
            m_file_stream.close();
        }
        
        // Find existing log files
        std::vector<std::filesystem::path> log_files;
        for (const auto& entry : std::filesystem::directory_iterator(m_log_dir)) {
            if (entry.is_regular_file() &&
                entry.path().extension() == ".log" &&
                entry.path().stem().string().starts_with(m_prefix)) {
                log_files.push_back(entry.path());
            }
        }
        
        // Sort by modification time (oldest first)
        std::sort(log_files.begin(), log_files.end(),
            [](const auto& a, const auto& b) {
                return std::filesystem::last_write_time(a) <
                       std::filesystem::last_write_time(b);
            });
        
        // Remove oldest files if we exceed MAX_LOG_FILES
        while (log_files.size() >= MAX_LOG_FILES) {
            std::filesystem::remove(log_files.front());
            log_files.erase(log_files.begin());
        }
        
        // Create new log file
        auto timestamp = std::format("{:%Y-%m-%d_%H-%M-%S}",
                                     std::chrono::system_clock::now());
        m_log_file = m_log_dir / std::format("{}_{}.log", m_prefix, timestamp);
        
        m_file_stream.open(m_log_file, std::ios::out | std::ios::app);
        m_bytes_written.store(0, std::memory_order_relaxed);
        
    } catch (...) {
        // Best effort - if rotation fails, continue with current file
    }
}

void logger::log(
    log_level level,
    std::string_view format,
    const std::source_location& loc) noexcept {
    
    // Early exit if level is below minimum
    if (level < m_min_level.load(std::memory_order_relaxed)) {
        return;
    }
    
    // For no-args, just log the format string directly
    log_internal(level, std::string(format), loc);
}

} // namespace sak
