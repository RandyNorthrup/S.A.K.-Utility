// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file log_file_sink.h
/// @brief The log file S.A.K. writes, opened so no link can redirect it (security brief 4a.3).
///
/// The elevated helper and an elevated S.A.K. write their logs into the program's own data
/// folder, which can be on a drive another account writes to. The log used to open a predictable
/// name with ios::app, which follows a link planted there. Each session's file is now created new
/// (CREATE_NEW, never through a name that already exists, never through a link) inside a log
/// folder held without following a link, and kept open shared for reading only: nobody else can
/// write into it while it is ours, and the log window can still read it.

#include "sak/logger.h"
#include "sak/no_follow_fs.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace sak {

class FileLogSink final : public LogSink {
public:
    /// Prune old session logs in @p folder, then open "<prefix>_<timestamp>.log" there.
    [[nodiscard]] static std::expected<std::unique_ptr<FileLogSink>, error_code> open(
        no_follow::HeldDirectory folder, std::string_view prefix);

    ~FileLogSink() override;
    FileLogSink(const FileLogSink&) = delete;
    FileLogSink& operator=(const FileLogSink&) = delete;
    FileLogSink(FileLogSink&&) = delete;
    FileLogSink& operator=(FileLogSink&&) = delete;

    [[nodiscard]] bool write(std::string_view entry) override;
    [[nodiscard]] bool rotate() override;
    [[nodiscard]] std::filesystem::path location() const override;

private:
    FileLogSink(no_follow::HeldDirectory folder, std::string prefix);
    /// A new log file named @p name in the held folder; null if it could not be created.
    [[nodiscard]] void* createFile(const std::filesystem::path& name) const;
    [[nodiscard]] std::vector<std::filesystem::path> logsWithPrefix() const;
    void pruneSessionLogs() const;
    void closeFile() noexcept;

    no_follow::HeldDirectory m_folder;
    std::filesystem::path m_folder_path;
    std::string m_prefix;
    std::filesystem::path m_file;
    void* m_handle{nullptr};
};

/// Start logging into @p folder (app_paths::openDataSubdirectory("logs")): the application's and
/// the elevated helper's way in.
[[nodiscard]] std::expected<void, error_code> startFileLogging(no_follow::HeldDirectory folder,
                                                               std::string_view prefix = "sak");

}  // namespace sak
