// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file logger.h
/// @brief Thread-safe logging system with structured output
/// @note Enterprise-grade logger with multiple severity levels and automatic rotation

#pragma once

#include "error_codes.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

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
    case log_level::debug:
        return "DEBUG";
    case log_level::info:
        return "INFO";
    case log_level::warning:
        return "WARNING";
    case log_level::error:
        return "ERROR";
    case log_level::critical:
        return "CRITICAL";
    default:
        return "UNKNOWN";
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
    [[nodiscard]] auto initialize(const std::filesystem::path& log_dir,
                                  std::string_view prefix = "sak")
        -> std::expected<void, error_code>;

    /// @brief Set minimum log level
    /// @param level Minimum level to log
    void setLevel(log_level level) noexcept;

    /// @brief Get current minimum log level
    /// @return Current log level
    [[nodiscard]] log_level getLevel() const noexcept;

    /// @brief Enable/disable console output
    /// @param enable True to enable console output
    void setConsoleOutput(bool enable) noexcept;

    /// @brief Log a message (no arguments)
    /// @param level Severity level
    /// @param format Format string
    /// @param loc Source location (auto-captured)
    void log(log_level level,
             std::string_view format,
             const std::source_location& loc = std::source_location::current()) noexcept;

    /// @brief Log a message with arguments
    /// @tparam Args Format argument types
    /// @param level Severity level
    /// @param format Format string
    /// @param arg1 First format argument
    /// @param args Remaining format arguments
    template <typename T, typename... Args>
    void log(log_level level, std::string_view format, const T& arg1, const Args&... args) noexcept;

    /// @brief Flush pending log entries to disk
    void flush() noexcept;

    /// @brief Get current log file path
    /// @return Path to current log file
    [[nodiscard]] std::filesystem::path getLogFile() const noexcept;

    /// @brief Check if logger is initialized
    /// @return True if initialized
    [[nodiscard]] bool isInitialized() const noexcept;

    /// @brief Create the log directory (if needed) and verify it is writable.
    /// @param dir Directory path.
    /// @return Success, or an error code if it cannot be created/written.
    /// @note Public + static so the write-probe hardening (unique, exclusively
    ///       created probe file that never clobbers or follows a link) can be
    ///       unit tested directly.
    [[nodiscard]] static auto ensureLogDirectory(const std::filesystem::path& dir)
        -> std::expected<void, error_code>;

    /// @brief Bytes to charge against the rotation counter for a completed
    ///        write. Fail closed: a write that left the stream in a bad state
    ///        wrote nothing durable, so it must count as zero (not entry_size).
    /// @param stream_good Result of ostream::good() after the write/flush.
    /// @param entry_size Size of the entry that was attempted.
    /// @return entry_size when the write is good, otherwise 0.
    /// @note Public + static so the fail-closed accounting can be unit tested.
    [[nodiscard]] static std::size_t bytesToCommit(bool stream_good,
                                                   std::size_t entry_size) noexcept;

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
    void logInternal(log_level level,
                     std::string_view message,
                     const std::source_location& loc) noexcept;

    /// @brief Generate timestamp string
    /// @return Current timestamp in ISO 8601 format
    [[nodiscard]] static std::string getTimestamp() noexcept;

    /// @brief Check if log rotation is needed
    /// @return True if rotation needed
    [[nodiscard]] bool needsRotation() const noexcept;

    /// @brief Perform log rotation
    void rotateLog() noexcept;

    /// @brief Write a formatted log entry to the log file under lock
    void writeEntryToFile(std::string_view log_entry, log_level level) noexcept;

    /// @brief Emit a one-shot stderr notice on the first file-write failure so a
    ///        persistent disk-write failure is surfaced rather than swallowed.
    void noteWriteFailure() noexcept;

    /// @brief Write a formatted log entry to console (stdout/stderr)
    void writeEntryToConsole(std::string_view log_entry, log_level level) noexcept;

    /// @brief Collect existing log files matching the current prefix
    [[nodiscard]] std::vector<std::filesystem::path> collectRotationCandidates() const;

    mutable std::mutex m_mutex;                              ///< Mutex for thread safety
    std::ofstream m_file_stream;                             ///< Output file stream
    std::filesystem::path m_log_file;                        ///< Current log file path
    std::filesystem::path m_log_dir;                         ///< Log directory
    std::string m_prefix;                                    ///< Log file prefix
    std::atomic<log_level> m_min_level{log_level::info};     ///< Minimum log level
    std::atomic<bool> m_console_output{true};                ///< Console output enabled
    std::atomic<bool> m_initialized{false};                  ///< Initialization flag
    std::atomic<bool> m_write_failed{false};                 ///< One-shot write-failure latch
    std::atomic<std::size_t> m_bytes_written{0};             ///< Bytes written to current file

    static constexpr std::size_t MAX_LOG_SIZE = 10'000'000;  ///< 10MB max log size
    static constexpr std::size_t MAX_LOG_FILES = 5;          ///< Keep last 5 log files
};

/// @brief Log a debug message
/// @param format Format string (no arguments)
inline void logDebug(std::string_view format,
                     const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::debug, format, loc);
}

/// @brief Log a debug message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template <typename T, typename... Args>
void logDebug(std::string_view format, const T& arg1, const Args&... args) noexcept {
    logger::instance().log(log_level::debug, format, arg1, args...);
}

/// @brief Log an info message
/// @param format Format string (no arguments)
inline void logInfo(std::string_view format,
                    const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::info, format, loc);
}

/// @brief Log an info message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template <typename T, typename... Args>
void logInfo(std::string_view format, const T& arg1, const Args&... args) noexcept {
    logger::instance().log(log_level::info, format, arg1, args...);
}

/// @brief Log a warning message
/// @param format Format string (no arguments)
inline void logWarning(std::string_view format,
                       const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::warning, format, loc);
}

/// @brief Log a warning message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template <typename T, typename... Args>
void logWarning(std::string_view format, const T& arg1, const Args&... args) noexcept {
    logger::instance().log(log_level::warning, format, arg1, args...);
}

/// @brief Log an error message
/// @param format Format string (no arguments)
inline void logError(std::string_view format,
                     const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::error, format, loc);
}

/// @brief Log an error message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template <typename T, typename... Args>
void logError(std::string_view format, const T& arg1, const Args&... args) noexcept {
    logger::instance().log(log_level::error, format, arg1, args...);
}

/// @brief Log a critical message
/// @param format Format string (no arguments)
inline void logCritical(
    std::string_view format,
    const std::source_location& loc = std::source_location::current()) noexcept {
    logger::instance().log(log_level::critical, format, loc);
}

/// @brief Log a critical message with arguments
/// @tparam Args Format argument types
/// @param format Format string
/// @param arg1 First format argument (enables overload resolution)
/// @param args Remaining format arguments
template <typename T, typename... Args>
void logCritical(std::string_view format, const T& arg1, const Args&... args) noexcept {
    logger::instance().log(log_level::critical, format, arg1, args...);
}

// Template implementation
template <typename T, typename... Args>
void logger::log(log_level level,
                 std::string_view format,
                 const T& arg1,
                 const Args&... args) noexcept {
    // Early exit if level is below minimum
    if (level < m_min_level.load(std::memory_order_relaxed)) {
        return;
    }

    try {
        auto message = std::vformat(format, std::make_format_args(arg1, args...));
        logInternal(level, message, std::source_location::current());
    } catch (...) {
        // Final safety net in a noexcept function (never throw from the logger). Do not
        // swallow silently: a malformed format string or formatter failure would otherwise
        // erase the entry with no trace, so note it to stderr like logInternal's own catch.
        std::fprintf(stderr, "SAK Logger: log formatting failed; entry dropped\n");
    }
}

}  // namespace sak
