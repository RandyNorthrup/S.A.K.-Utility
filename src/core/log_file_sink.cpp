// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file log_file_sink.cpp
/// @brief The log file S.A.K. writes, opened so no link can redirect it (security brief 4a.3).

#include "sak/log_file_sink.h"

#include "sak/path_utils.h"

#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

#include <windows.h>

namespace sak {

namespace {

// Build a uniquely named rotated log path. A bare second-resolution timestamp
// collides when two rotations (or an init immediately followed by a rotation)
// land in the same wall-clock second: reopening the SAME file in append mode
// while resetting the byte counter lets it grow past MAX_LOG_SIZE. A sub-second
// millisecond field plus a process-monotonic sequence keeps every name distinct
// so rotation never appends onto an existing log.
std::filesystem::path buildRotatedLogName(std::string_view prefix) {
    static std::atomic<std::uint64_t> rotate_seq{0};
    const auto seq = rotate_seq.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch - secs).count();
    const auto stamp = std::format("{:%Y-%m-%d_%H-%M-%S}", now);
    return std::format("{}_{}-{:03}-{}.log", prefix, stamp, millis, seq);
}

}  // namespace

// ======================================================================
// logger's file-system half
// ======================================================================

auto logger::initialize(const std::filesystem::path& log_dir, std::string_view prefix)
    -> std::expected<void, error_code> {
    if (auto result = ensureLogDirectory(log_dir); !result) {
        return std::unexpected(result.error());
    }
    // Held without following a link: a junction as (or above) the log folder is refused.
    auto folder = no_follow::HeldDirectory::open(QString::fromStdWString(log_dir.wstring()));
    if (!folder) {
        (void)std::fprintf(stderr,
                           "SAK Logger: log folder refused: %s\n",
                           folder.error().toUtf8().constData());
        return std::unexpected(error_code::permission_denied);
    }
    auto sink = FileLogSink::open(std::move(*folder), prefix);
    if (!sink) {
        return std::unexpected(sink.error());
    }
    return initialize(std::move(*sink));
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

std::vector<std::filesystem::path> logger::sessionLogsToPrune(std::vector<SessionLogFile> logs,
                                                              std::size_t keep_newest,
                                                              std::uintmax_t max_total_bytes) {
    std::ranges::sort(logs, [](const SessionLogFile& a, const SessionLogFile& b) {
        return a.modified > b.modified;  // newest first
    });
    // One slot is the session about to start.
    const std::size_t keep_existing = keep_newest > 0 ? keep_newest - 1 : 0;
    std::vector<std::filesystem::path> prune;
    std::size_t kept = 0;
    std::uintmax_t kept_bytes = 0;
    for (const SessionLogFile& entry : logs) {
        if (kept < keep_existing && kept_bytes + entry.size_bytes <= max_total_bytes) {
            ++kept;
            kept_bytes += entry.size_bytes;
        } else {
            prune.push_back(entry.path);
        }
    }
    return prune;
}

// ======================================================================
// FileLogSink
// ======================================================================

FileLogSink::FileLogSink(no_follow::HeldDirectory folder, std::string prefix)
    : m_folder(std::move(folder))
    , m_folder_path(m_folder.path().toStdWString())
    , m_prefix(std::move(prefix)) {}

FileLogSink::~FileLogSink() {
    closeFile();
}

std::expected<std::unique_ptr<FileLogSink>, error_code> FileLogSink::open(
    no_follow::HeldDirectory folder, std::string_view prefix) {
    std::unique_ptr<FileLogSink> sink(new FileLogSink(std::move(folder), std::string(prefix)));
    // A log per launch, forever: prune old sessions before this one adds its file.
    sink->pruneSessionLogs();
    // system_clock's %S carries a fraction of a second, so the name is new; CREATE_NEW refuses
    // it otherwise rather than write into a file that was already there.
    const auto timestamp = std::format("{:%Y-%m-%d_%H-%M-%S}", std::chrono::system_clock::now());
    const std::filesystem::path name(std::format("{}_{}.log", sink->m_prefix, timestamp));
    sink->m_handle = sink->createFile(name);
    if (sink->m_handle == nullptr) {
        return std::unexpected(error_code::write_error);
    }
    sink->m_file = sink->m_folder_path / name;
    return sink;
}

void* FileLogSink::createFile(const std::filesystem::path& name) const {
    auto opened = no_follow::createNewFile(m_folder, QString::fromStdWString(name.wstring()), true);
    if (!opened) {
        (void)std::fprintf(stderr,
                           "SAK Logger: cannot create log file: %s\n",
                           opened.error().toUtf8().constData());
        return nullptr;
    }
    return opened->release();
}

bool FileLogSink::write(std::string_view entry) {
    if (m_handle == nullptr) {
        return false;
    }
    std::size_t done = 0;
    while (done < entry.size()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(entry.size() - done, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (WriteFile(m_handle, entry.data() + done, chunk, &written, nullptr) == 0 ||
            written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

bool FileLogSink::rotate() {
    try {
        closeFile();
        auto log_files = logsWithPrefix();
        std::ranges::sort(log_files, [](const auto& a, const auto& b) {
            return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
        });
        while (log_files.size() >= logger::MAX_LOG_FILES) {
            std::filesystem::remove(log_files.front());  // the name itself, never a link's target
            log_files.erase(log_files.begin());
        }
        // A NEW, uniquely named file -- never the just-closed one (buildRotatedLogName).
        const std::filesystem::path name = buildRotatedLogName(m_prefix);
        m_handle = createFile(name);
        m_file = m_folder_path / name;
        return m_handle != nullptr;
    } catch (const std::exception& e) {
        // Logger cannot use logError (infinite recursion), fall back to stderr
        (void)std::fprintf(stderr, "SAK Logger: Log rotation failed: %s\n", e.what());
        return false;
    }
}

std::filesystem::path FileLogSink::location() const {
    return m_file;
}

std::vector<std::filesystem::path> FileLogSink::logsWithPrefix() const {
    std::vector<std::filesystem::path> log_files;
    for (const auto& entry : std::filesystem::directory_iterator(m_folder_path)) {
        // The entry itself, not what a link there points at: a link is never pruned through.
        if (!std::filesystem::is_regular_file(entry.symlink_status())) {
            continue;
        }
        if (entry.path().extension() != ".log") {
            continue;
        }
        if (!pathAsUtf8(entry.path().stem()).starts_with(m_prefix)) {
            continue;
        }
        log_files.push_back(entry.path());
    }
    return log_files;
}

void FileLogSink::pruneSessionLogs() const {
    // Runs before the logger has this sink: report problems on stderr, never through log().
    try {
        std::vector<logger::SessionLogFile> logs;
        for (const auto& path : logsWithPrefix()) {
            std::error_code ec;
            const auto modified = std::filesystem::last_write_time(path, ec);
            const auto size = std::filesystem::file_size(path, ec);
            if (!ec) {
                logs.push_back({.path = path, .modified = modified, .size_bytes = size});
            }
        }
        for (const auto& path : logger::sessionLogsToPrune(
                 logs, logger::kMaxSessionLogs, logger::kMaxSessionLogBytes)) {
            std::error_code ec;
            std::filesystem::remove(path, ec);  // a log another instance holds open stays
        }
    } catch (const std::exception& e) {
        (void)std::fprintf(stderr, "SAK Logger: pruning old session logs failed: %s\n", e.what());
    }
}

void FileLogSink::closeFile() noexcept {
    if (m_handle != nullptr) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
}

std::expected<void, error_code> startFileLogging(no_follow::HeldDirectory folder,
                                                 std::string_view prefix) {
    auto sink = FileLogSink::open(std::move(folder), prefix);
    if (!sink) {
        return std::unexpected(sink.error());
    }
    return logger::instance().initialize(std::move(*sink));
}

}  // namespace sak
