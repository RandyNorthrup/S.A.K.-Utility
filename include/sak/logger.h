/// @file logger.h
/// @brief Thread-safe logging system with structured output
/// @note Enterprise-grade logger with multiple severity levels and automatic rotation

#ifndef SAK_LOGGER_H
#define SAK_LOGGER_H

#include "error_codes.h"
#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <print>
#include <source_location>
#include <string>
#include <string_view>

namespace sak {

/// @brief Log severity levels
enum class log_level {
    debug = 0,    ///< Detailed information for diagnosing problems
    info = 1,     ///< General informational messages
    warning = 2,  ///< Warning messages for potentially harmful situations
    error = 3,    ///< Error messages for serious problems
    critical = 4  ///< Critical messages for fatal errors
};

/// @brief Convert log level to string
/// @param level Log level to convert
/// @return String representation
[[nodiscard]] constexpr std::string_view to_string(log_level level) noexcept {
    switch (level) {
        case log_level::debug: return "DEBUG";
        case log_level::info: return "INFO";
        case log_level::warning: return "WARNING";
        case log_level::error: return "ERROR";
        case log_level::critical: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

/// @brief Thread-safe logger with structured output and rotation support
/// @note Singleton pattern with thread-local buffers for performance
class logger {
public:
    /// @brief Get logger instance (singleton)
    /// @return Reference to the global logger instance
    [[nodiscard]] static logger& instance() noexcept;
    
    /// @brief Initialize logger with log directory
    /// @param log_dir Directory to store log files
    /// @param prefix Prefix for log file names
    /// @return Expected containing success or error code
    [[nodiscard]] auto initialize(
        const std::filesystem::path& log_dir,
        std::string_view prefix = "sak") -> std::expected<void, error_code>;
    
    /// @brief Set minimum log level
    /// @param level Minimum level to log
    void set_level(log_level level) noexcept;
    
    /// @brief Get current minimum log level
    /// @return Current log level
    [[nodiscard]] log_level get_level() const noexcept;
    
    /// @brief Enable/disable console output
    /// @param enable True to enable console output
    void set_console_output(bool enable) noexcept;
    
    /// @brief Log a message (no arguments)
    /// @param level Severity level
    /// @param format Format string
    /// @param loc Source location (auto-captured)
    void log(
        log_level level,
        std::string_view format,
        const std::source_location& loc = std::source_location::current()) noexcept;
    
    /// @brief Log a message with arguments
    /// @tparam Args Format argument types
    /// @param level Severity level
    /// @param format Format string
    /// @param arg1 First format argument
    /// @param args Remaining format arguments
    template<typename T, typename... Args>
    void log(
        log_level level,
        std::string_view format,
        const T& arg1,
        const Args&... args) noexcept;
    
    /// @brief Flush pending log entries to disk
    void flush() noexcept;
    
    /// @brief Get current log file path
    /// @return Path to current log file
    [[nodiscard]] std::filesystem::path get_log_file() const noexcept;
    
    /// @brief Check if logger is initialized
    /// @return True if initialized
    [[nodiscard]] bool is_initialized() const noexcept;
    
    // Prevent copying and moving
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;
    logger(logger&&) = delete;
    logger& operator=(logger&&) = delete;
    
private:
    logger() = default;
    ~logger();
    
    /// @brief Internal logging implementation
    /// @param level Severity level
    /// @param message Formatted message
    /// @param loc Source location
    void log_internal(
        log_level level,
        std::string_view message,
        const std::source_location& loc) noexcept;
    
    /// @brief Create log directory if it doesn't exist
    /// @param dir Directory path
    /// @return Expected containing success or error code
    [[nodiscard]] auto ensure_log_directory(
        const std::filesystem::path& dir) -> std::expected<void, error_code>;
    
    /// @brief Generate timestamp string
    /// @return Current timestamp in ISO 8601 format
    [[nodiscard]] std::string get_timestamp() const noexcept;
    
    /// @brief Check if log rotation is needed
    /// @return True if rotation needed
    [[nodiscard]] bool needs_rotation() const noexcept;
    
    /// @brief Perform log rotation
    void rotate_log() noexcept;
    
    mutable std::mutex m_mutex;                    ///< Mutex for thread safety
    std::ofstream m_file_stream;                   ///< Output file stream
    std::filesystem::path m_log_file;              ///< Current log file path
    std::filesystem::path m_log_dir;               ///< Log directory
    std::string m_prefix;                          ///< Log file prefix
    std::atomic<log_level> m_min_level{log_level::info};  ///< Minimum log level
    std::atomic<bool> m_console_output{true};      ///< Console output enabled
    std::atomic<bool> m_initialized{false};        ///< Initialization flag
    std::atomic<std::size_t> m_bytes_written{0};   ///< Bytes written to current file
    
    static constexpr std::size_t MAX_LOG_SIZE = 10'000'000;  ///< 10MB max log size
    static constexpr std::size_t MAX_LOG_FILES = 5;          ///< Keep last 5 log files
};

/// @brief Log a debug message
/// @param format Format string (no arguments)
inline void log_debug(
    std::string_view format,
    const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::debug, format, loc);
}

/// @brief Log a debug message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template<typename T, typename... Args>
void log_debug(
    std::string_view format,
    const T& arg1,
    const Args&... args) noexcept {
    logger::instance().log(log_level::debug, format, arg1, args...);
}

/// @brief Log an info message
/// @param format Format string (no arguments)
inline void log_info(
    std::string_view format,
    const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::info, format, loc);
}

/// @brief Log an info message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template<typename T, typename... Args>
void log_info(
    std::string_view format,
    const T& arg1,
    const Args&... args) noexcept {
    logger::instance().log(log_level::info, format, arg1, args...);
}

/// @brief Log a warning message
/// @param format Format string (no arguments)
inline void log_warning(
    std::string_view format,
    const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::warning, format, loc);
}

/// @brief Log a warning message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template<typename T, typename... Args>
void log_warning(
    std::string_view format,
    const T& arg1,
    const Args&... args) noexcept {
    logger::instance().log(log_level::warning, format, arg1, args...);
}

/// @brief Log an error message
/// @param format Format string (no arguments)
inline void log_error(
    std::string_view format,
    const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::error, format, loc);
}

/// @brief Log an error message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template<typename T, typename... Args>
void log_error(
    std::string_view format,
    const T& arg1,
    const Args&... args) noexcept {
    logger::instance().log(log_level::error, format, arg1, args...);
}

/// @brief Log a critical message
/// @param format Format string (no arguments)
inline void log_critical(
    std::string_view format,
    const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::critical, format, loc);
}

/// @brief Log a critical message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template<typename T, typename... Args>
void log_critical(
    std::string_view format,
    const T& arg1,
    const Args&... args) noexcept {
    logger::instance().log(log_level::critical, format, arg1, args...);
}

// Template implementation
template<typename T, typename... Args>
void logger::log(
    log_level level,
    std::string_view format,
    const T& arg1,
    const Args&... args) noexcept {
    
    // Early exit if level is below minimum
    if (level < m_min_level.load(std::memory_order_relaxed)) {
        return;
    }
    
    try {
        auto message = std::vformat(format, std::make_format_args(arg1, args...));
        log_internal(level, message, std::source_location::current());
    } catch (...) {
        // Never throw from logger - best effort only
    }
}

} // namespace sak

#endif // SAK_LOGGER_H
