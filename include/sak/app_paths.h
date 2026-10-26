// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <expected>
#include <functional>

namespace sak::app_paths {

[[nodiscard]] QString applicationDirectory();
[[nodiscard]] bool isPackaged();
/// The data root, decided once per process by initializeDataRoot(); empty if it failed.
[[nodiscard]] QString dataRoot();
/// Decide the data root once (thread-safe; later calls return the same answer): data/ beside
/// the program, created and write-probed once -- or, for a packaged (MSIX) install, the
/// per-user location. There is no fallback: an unwritable portable folder is an error whose
/// text tells the user what to do. main() calls this before anything touches data/.
[[nodiscard]] std::expected<QString, QString> initializeDataRoot();
/// Pure decision behind initializeDataRoot(), testable without touching the real folders.
[[nodiscard]] std::expected<QString, QString> decideDataRoot(
    bool packaged,
    const QString& app_dir,
    const QString& per_user_location,
    const std::function<bool(const QString&)>& writable);
[[nodiscard]] QString configDirectory();
[[nodiscard]] QString configFilePath();
/// UI state (see sak::PortableSettings): data/config/ui_state.ini, never the registry.
[[nodiscard]] QString uiStateFilePath();
[[nodiscard]] QString logsDirectory();
/// Directory for crash minidumps + summaries (a sibling of logs under the data root).
[[nodiscard]] QString crashesDirectory();
[[nodiscard]] QString tempDirectory();
/// This process's own scratch folder for child processes: data/temp/session-<pid>.
/// The app no longer points TMP/TEMP at data/temp for the whole process; only the children
/// that need a portable temp get one (withSessionTemp), and the folder is removed at exit.
[[nodiscard]] QString sessionTempDirectory();
/// @p environment with TMP and TEMP set to sessionTempDirectory() (created on demand).
[[nodiscard]] QProcessEnvironment withSessionTemp(QProcessEnvironment environment);
/// Remove this process's session temp folder (at exit).
void removeSessionTempDirectory();
/// Pure: the "session-<pid>" names in @p entries whose process is gone (never @p own_pid).
[[nodiscard]] QStringList staleSessionTempDirectories(const QStringList& entries,
                                                      qint64 own_pid,
                                                      const std::function<bool(qint64)>& alive);
/// Remove the session temp folders earlier runs left behind (a crash skips the exit cleanup).
/// Only "session-<pid>" folders of processes that are gone; nothing else in data/temp.
int removeStaleSessionTempDirectories();
[[nodiscard]] bool ensureDirectory(const QString& path);

}  // namespace sak::app_paths
