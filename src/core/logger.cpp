// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file logger.cpp
/// @brief Implementation of thread-safe logging system

#include "sak/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

auto logger::initialize(const std::filesystem::path& log_dir, std::string_view prefix)
    -> std::expected<void, error_code> {
    std::string init_log_path;
    {
        const std::lock_guard lock(m_mutex);

        // Ensure log directory exists
        if (auto result = ensureLogDirectory(log_dir); !result) {
            return std::unexpected(result.error());
        }

        m_log_dir = log_dir;
        m_prefix = prefix;

        // Generate log file path with timestamp
        auto timestamp = std::format("{:%Y-%m-%d_%H-%M-%S}", std::chrono::system_clock::now());
        m_log_file = m_log_dir / std::format("{}_{}.log", m_prefix, timestamp);

        // Close any stream from a previous initialize() BEFORE reopening. std::ofstream::open
        // on an already-open stream fails and leaves the stream attached to the old file, so
        // the is_open() check below would still be true: initialize() would report success,
        // and the "Logger initialized: <path>" line would name the NEW file while every
        // subsequent write went to the OLD one. Closing first makes a re-initialize actually
        // move the log, and makes a genuine open failure reachable by the check.
        if (m_file_stream.is_open()) {
            m_file_stream.flush();
            m_file_stream.close();
        }
        m_file_stream.clear();  // drop any failbit from a previous close

        m_file_stream.open(m_log_file, std::ios::out | std::ios::app);
        if (!m_file_stream.is_open()) {
            m_initialized.store(false, std::memory_order_release);
            return std::unexpected(error_code::write_error);
        }

        m_initialized.store(true, std::memory_order_release);
        m_bytes_written.store(0, std::memory_order_relaxed);
        init_log_path = m_log_file.string();
    }

    // Write initialization message (outside lock -- writeEntryToFile acquires it). Use the
    // path captured UNDER the lock, never the shared m_log_file member: reading it unlocked
    // here races a concurrent initialize() that may be mutating it (torn read of a path).
    logInfo("Logger initialized: {}", init_log_path);

    return {};
}

void logger::setLevel(log_level level) noexcept {
    m_min_level.store(level, std::memory_order_relaxed);
}

log_level logger::getLevel() const noexcept {
    return m_min_level.load(std::memory_order_relaxed);
}

void logger::setConsoleOutput(bool enable) noexcept {
    m_console_output.store(enable, std::memory_order_relaxed);
}

void logger::flush() noexcept {
    const std::lock_guard lock(m_mutex);
    if (m_file_stream.is_open()) {
        m_file_stream.flush();
    }
}

std::filesystem::path logger::getLogFile() const noexcept {
    const std::lock_guard lock(m_mutex);
    return m_log_file;
}

bool logger::isInitialized() const noexcept {
    return m_initialized.load(std::memory_order_acquire);
}

void logger::logInternal(log_level level,
                         std::string_view message,
                         const std::source_location& loc) noexcept {
    if (!isInitialized()) {
        // Fallback to console if not initialized. std::println can throw (bad_alloc, I/O
        // failure); this runs BEFORE the try below in a noexcept function, so an escaping
        // exception would terminate the process. Guard it.
        if (m_console_output.load(std::memory_order_relaxed)) {
            try {
                std::println(std::cerr, "[{}] {}", to_string(level), message);
            } catch (...) {
                (void)std::fprintf(stderr, "SAK Logger: console write failed\n");
            }
        }
        return;
    }

    try {
        // Format log entry
        auto timestamp = getTimestamp();
        auto level_str = to_string(level);
        auto filename = std::filesystem::path(loc.file_name()).filename().string();

        auto log_entry = std::format("[{}] [{}] [{}:{}:{}] {}\n",
                                     timestamp,
                                     level_str,
                                     filename,
                                     loc.line(),
                                     loc.function_name(),
                                     message);

        writeEntryToFile(log_entry, level);
        writeEntryToConsole(log_entry, level);

    } catch (const std::exception& e) {
        // Logger itself cannot recurse -- fall back to stderr
        (void)std::fprintf(stderr, "SAK Logger: logInternal failed: %s\n", e.what());
    } catch (...) {
        // Intentional: final safety net in noexcept function
        (void)std::fprintf(stderr, "SAK Logger: logInternal failed with unknown exception\n");
    }
}

void logger::writeEntryToFile(std::string_view log_entry, log_level level) noexcept {
    const std::lock_guard lock(m_mutex);

    if (needsRotation()) {
        rotateLog();
    }

    if (!m_file_stream.is_open()) {
        return;
    }

    m_file_stream << log_entry;

    // Flush immediately for error and critical messages
    if (level >= log_level::error) {
        m_file_stream.flush();
    }

    // Fail closed: only charge bytes against the rotation counter when the
    // stream is still good. A failed write/flush (full disk, revoked handle)
    // must not advance m_bytes_written -- otherwise rotation math drifts and
    // the loss is invisible -- and must surface via a one-shot notice.
    const bool stream_good = m_file_stream.good();
    m_bytes_written.fetch_add(bytesToCommit(stream_good, log_entry.size()),
                              std::memory_order_relaxed);
    if (!stream_good) {
        noteWriteFailure();
    }
}

std::size_t logger::bytesToCommit(bool stream_good, std::size_t entry_size) noexcept {
    return stream_good ? entry_size : 0;
}

void logger::noteWriteFailure() noexcept {
    // Announce the first failure only (persistent failures would otherwise spam
    // stderr). Clear the stream error bits so a later recovered write can resume
    // instead of the stream staying wedged in a failed state forever.
    if (!m_write_failed.exchange(true, std::memory_order_relaxed)) {
        (void)std::fprintf(stderr, "SAK Logger: file log write failed (log entries may be lost)\n");
    }
    m_file_stream.clear();
}

void logger::writeEntryToConsole(std::string_view log_entry, log_level level) noexcept {
    if (!m_console_output.load(std::memory_order_relaxed)) {
        return;
    }

    // This function is noexcept but std::print can throw (bad_alloc, I/O failure). Without
    // this guard an escaping exception would cross the noexcept boundary and terminate the
    // process just because a console write failed -- catch and note instead.
    try {
        if (level >= log_level::error) {
            std::print(std::cerr, "{}", log_entry);
        } else {
            std::print("{}", log_entry);
        }
    } catch (...) {
        (void)std::fprintf(stderr, "SAK Logger: console write failed\n");
    }
}

auto logger::ensureLogDirectory(const std::filesystem::path& dir)
    -> std::expected<void, error_code> {
    try {
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }

        if (!std::filesystem::is_directory(dir)) {
            return std::unexpected(error_code::not_a_directory);
        }

        // Check write permission with an unpredictable, exclusively-created probe
        // file. The old fixed ".test_write" name was guessable, so a pre-planted
        // file or symlink at that path would be truncated (default ofstream) and
        // then deleted -- destructive when this runs elevated. A unique name plus
        // std::ios::noreplace (exclusive create: fails if the name already exists,
        // including a planted link) means we never clobber or follow into a
        // victim file.
        static std::atomic<std::uint64_t> probe_counter{0};
        const auto seq = probe_counter.fetch_add(1, std::memory_order_relaxed);
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto probe_name = ".sak_log_probe_" + std::to_string(stamp) + "_" +
                                std::to_string(seq);
        auto test_file = dir / probe_name;
        std::ofstream test(test_file, std::ios::out | std::ios::noreplace);
        if (!test.is_open()) {
            return std::unexpected(error_code::permission_denied);
        }
        test.close();
        std::filesystem::remove(test_file);

        return {};

    } catch (const std::filesystem::filesystem_error&) {
        return std::unexpected(error_code::permission_denied);
    } catch (...) {
        // Intentional: final safety net in noexcept function
        (void)std::fprintf(stderr,
                           "SAK Logger: ensureLogDirectory failed with unknown exception\n");
        return std::unexpected(error_code::unknown_error);
    }
}

std::string logger::getTimestamp() noexcept {
    try {
        auto now = std::chrono::system_clock::now();
        return std::format("{:%Y-%m-%d %H:%M:%S}", now);
    } catch (...) {
        // Intentional: final safety net in noexcept function
        (void)std::fprintf(stderr, "SAK Logger: getTimestamp failed\n");
        return "TIMESTAMP_ERROR";
    }
}

bool logger::needsRotation() const noexcept {
    return m_bytes_written.load(std::memory_order_relaxed) >= MAX_LOG_SIZE;
}

void logger::rotateLog() noexcept {
    try {
        // Close current file
        if (m_file_stream.is_open()) {
            m_file_stream.close();
        }

        // Find existing log files
        auto log_files = collectRotationCandidates();

        // Sort by modification time (oldest first)
        std::sort(log_files.begin(), log_files.end(), [](const auto& a, const auto& b) {
            return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
        });

        // Remove oldest files if we exceed MAX_LOG_FILES
        while (log_files.size() >= MAX_LOG_FILES) {
            std::filesystem::remove(log_files.front());
            log_files.erase(log_files.begin());
        }

        // Create new log file
        auto timestamp = std::format("{:%Y-%m-%d_%H-%M-%S}", std::chrono::system_clock::now());
        m_log_file = m_log_dir / std::format("{}_{}.log", m_prefix, timestamp);

        m_file_stream.open(m_log_file, std::ios::out | std::ios::app);
        m_bytes_written.store(0, std::memory_order_relaxed);
        // Fail loud, not silent: if the post-rotation file could not be reopened, file
        // logging is now disabled while isInitialized() still reports true, and every later
        // entry would be dropped by writeEntryToFile's is_open() guard with no trace. Surface
        // it once via the write-failure notice.
        if (!m_file_stream.is_open()) {
            noteWriteFailure();
        }

    } catch (const std::exception& e) {
        // Logger cannot use logError (infinite recursion), fall back to stderr
        (void)std::fprintf(stderr, "SAK Logger: Log rotation failed: %s\n", e.what());
    } catch (...) {
        // Intentional: final safety net in noexcept function
        (void)std::fprintf(stderr, "SAK Logger: Log rotation failed with unknown exception\n");
    }
}

std::vector<std::filesystem::path> logger::collectRotationCandidates() const {
    std::vector<std::filesystem::path> log_files;
    for (const auto& entry : std::filesystem::directory_iterator(m_log_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".log") {
            continue;
        }
        if (!entry.path().stem().string().starts_with(m_prefix)) {
            continue;
        }
        log_files.push_back(entry.path());
    }
    return log_files;
}

void logger::log(log_level level,
                 std::string_view format,
                 const std::source_location& loc) noexcept {
    // Early exit if level is below minimum
    if (level < m_min_level.load(std::memory_order_relaxed)) {
        return;
    }

    // For no-args, log the format string_view directly. Constructing a std::string here (an
    // allocation) inside this noexcept function, outside any try, would terminate the process
    // on bad_alloc; logInternal reads the view synchronously, so no owning copy is needed.
    logInternal(level, format, loc);
}

}  // namespace sak
