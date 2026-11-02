// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file app_data_sources.h
/// @brief The application data a profile backup knows how to find, and what is true of it.
///
/// Both wizards and the backup worker read this. A source is a path relative to a profile; the
/// worker turns each selected one into a FolderType::ApplicationData folder for every user who has
/// it, so it is copied, compressed, encrypted, sealed and restored like any other folder.

#include "sak/smart_file_filter.h"
#include "sak/user_profile_types.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

namespace sak {

/// Every source the wizard looks for. All are selected by default except SSH keys, which are
/// secrets a person should choose to copy, not have copied because they clicked Scan.
[[nodiscard]] QVector<AppDataSourceInfo> commonAppDataSources();

/// The lower-case executable names that hold this source's files open while the program runs.
/// Empty for a source that has no single program behind it.
[[nodiscard]] QStringList appDataSourceProcessNames(const QString& relative_path);

/// The lower-case executable name of every process running now.
[[nodiscard]] QStringList runningProcessNames();

/// The display names of the selected @p sources whose program is in @p running_processes, one each.
/// Pure, so the decision can be tested without starting a browser.
[[nodiscard]] QStringList appDataSourcesHeldOpen(const QVector<AppDataSourceInfo>& sources,
                                                 const QStringList& running_processes);

/// What a source holds once @p filter has left out what it would not copy.
struct AppDataMeasure {
    bool exists{false};
    qint64 size_bytes{0};
    int file_count{0};
};

/// Size one file or folder as a backup would store it. Returns early, with what it has counted so
/// far, once @p cancel is set.
[[nodiscard]] AppDataMeasure measureAppDataSource(const QString& absolute_path,
                                                  const SmartFileFilter& filter,
                                                  const QString& profile_path,
                                                  const std::atomic_bool* cancel = nullptr);

/// The sources present for at least one selected user, one row per source, with sizes added up
/// across those users.
[[nodiscard]] QVector<AppDataSourceInfo> scanAppDataSources(const QVector<UserProfile>& users,
                                                            const SmartFilter& rules,
                                                            const std::atomic_bool* cancel);

/// The manifest's sources that this backup really holds, with the size actually stored. A backup
/// from a version that recorded the choice and copied nothing lists sources with no data behind
/// them, and those must not be offered.
[[nodiscard]] QVector<AppDataSourceInfo> appDataStoredInBackup(const BackupManifest& manifest);

}  // namespace sak
