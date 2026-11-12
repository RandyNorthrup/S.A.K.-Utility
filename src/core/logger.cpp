// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file logger.cpp
/// @brief Implementation of thread-safe logging system

#include "sak/logger.h"

#include "sak/path_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace sak {

namespace {

// C0 control characters and DEL, escaped so an attacker-controlled substring
// (filename, disk path, PST field, model tool argument) cannot embed a raw
// newline and forge a separate, well-formed log record (CWE-117). Any byte at
// or above this ceiling, other than DEL, passes through untouched -- including
// every UTF-8 lead/continuation byte.
constexpr unsigned char kControlCeiling = 0x20;
constexpr unsigned char kDelByte = 0x7F;

// Render one C0/DEL control byte as a printable escape.
void appendEscapedControl(std::string& out, unsigned char byte) {
    switch (byte) {
    case '\n':
        out += "\\n";
        return;
    case '\r':
        out += "\\r";
        return;
    case '\t':
        out += "\\t";
        return;
    default:
        break;
    }
    static constexpr char kHexDigits[] = "0123456789abcdef";
    constexpr unsigned kNibbleBits = 4;
    constexpr unsigned kNibbleMask = 0x0FU;
    out += "\\x";
    out.push_back(kHexDigits[(byte >> kNibbleBits) & kNibbleMask]);
    out.push_back(kHexDigits[byte & kNibbleMask]);
}

// Return a copy of text with CR/LF and every other C0/DEL control character
// escaped, so a message can never inject a line break into the log stream.
std::string sanitizeLogText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte >= kControlCeiling && byte != kDelByte) {
            out.push_back(raw);
            continue;
        }
        appendEscapedControl(out, byte);
    }
    return out;
}

}  // namespace

logger& logger::instance() noexcept {
    static logger instance;
    return instance;
}

logger::~logger() {
    flush();
}

auto logger::initialize(std::unique_ptr<LogSink> sink) -> std::expected<void, error_code> {
    if (!sink) {
        return std::unexpected(error_code::invalid_argument);
    }
    std::string where;
    {
        const std::lock_guard lock(m_mutex);
        // The new sink is complete before the old one is let go, so a failed re-initialize
        // (which never gets here) leaves the log exactly where it was.
        m_sink = std::move(sink);
        m_initialized.store(true, std::memory_order_release);
        m_bytes_written.store(0, std::memory_order_relaxed);
        where = pathAsUtf8(m_sink->location());
    }
    // Outside the lock: writeEntryToFile takes it.
    logInfo("Logger initialized: {}", where);
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

void logger::flush() const noexcept {
    // Every entry goes straight to the sink (WriteFile for the file); no buffer of ours to empty.
    const std::lock_guard lock(m_mutex);
}

std::filesystem::path logger::getLogFile() const noexcept {
    const std::lock_guard lock(m_mutex);
    return m_sink ? m_sink->location() : std::filesystem::path();
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
                std::println(std::cerr, "[{}] {}", to_string(level), sanitizeLogText(message));
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
        // Neutralize CR/LF and other control characters in the (potentially
        // attacker-controlled) message so it cannot forge a separate log
        // record (CWE-117). The trailing '\n' below is the ONE intended break.
        auto safe_message = sanitizeLogText(message);

        auto log_entry = std::format("[{}] [{}] [{}:{}:{}] {}\n",
                                     timestamp,
                                     level_str,
                                     filename,
                                     loc.line(),
                                     loc.function_name(),
                                     safe_message);

        writeEntryToFile(log_entry);
        writeEntryToConsole(log_entry, level);

    } catch (const std::exception& e) {
        // Logger itself cannot recurse -- fall back to stderr
        (void)std::fprintf(stderr, "SAK Logger: logInternal failed: %s\n", e.what());
    } catch (...) {
        // Intentional: final safety net in noexcept function
        (void)std::fprintf(stderr, "SAK Logger: logInternal failed with unknown exception\n");
    }
}

void logger::writeEntryToFile(std::string_view log_entry) noexcept {
    const std::lock_guard lock(m_mutex);

    if (needsRotation()) {
        rotateLog();
    }

    if (!m_sink) {
        return;
    }

    // Every line reaches the file as it is written (WriteFile, no buffer of ours). Flushing only
    // errors kept a session's log at 0 bytes until exit on testpc1 and the VM (review 3), so a
    // crash or a kill lost it all.
    // Fail closed: only charge bytes against the rotation counter when the whole entry landed.
    // A failed write (full disk, revoked handle) must not advance m_bytes_written -- otherwise
    // rotation math drifts and the loss is invisible -- and must surface via a one-shot notice.
    const bool stream_good = m_sink->write(log_entry);
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
    // A NEW, uniquely named file, never the one just closed (FileLogSink::rotate). Fail loud, not
    // silent: if the new file could not be made, every later entry would be dropped with no trace,
    // so surface it once.
    const bool rotated = m_sink->rotate();
    m_bytes_written.store(0, std::memory_order_relaxed);
    if (!rotated) {
        noteWriteFailure();
    }
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
