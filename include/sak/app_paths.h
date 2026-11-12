// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/no_follow_fs.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <expected>
#include <functional>
#include <utility>

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
/// The folder @p name under the data root ("logs", "temp"), created when missing, opened and held
/// without following a link: the program's own folder is the anchor, and "data" and @p name below
/// it must be ordinary folders. What an elevated process writes into its own data folder goes
/// through this, since that folder can be on a drive other accounts can write to.
[[nodiscard]] std::expected<no_follow::HeldDirectory, QString> openDataSubdirectory(
    const QString& name);
[[nodiscard]] QString configDirectory();
[[nodiscard]] QString configFilePath();
/// UI state (see sak::PortableSettings): data/config/ui_state.ini, never the registry.
[[nodiscard]] QString uiStateFilePath();
[[nodiscard]] QString logsDirectory();
/// Directory for crash minidumps + summaries (a sibling of logs under the data root).
[[nodiscard]] QString crashesDirectory();
[[nodiscard]] QString tempDirectory();
/// This run's temporary folder, data/temp/run-<pid>-<32 random hex>, or empty before
/// establishSessionTempDirectory(). TMP and TEMP point here for the whole process, so Qt's
/// QDir::tempPath() and every child process use it (owner, 2026-11-11: nothing of ours lives on the
/// PC outside the program's folder, not even temporarily).
[[nodiscard]] QString sessionTempDirectory();
/// Create this run's temporary folder (fresh random name, fail-if-exists, opened and held without
/// following a link; on a volume that keeps ACLs, a protected DACL: SYSTEM, Administrators and,
/// when not elevated, the person running it) and point TMP and TEMP at it. Called once at startup
/// by the application and the elevated helper; later calls return the same folder.
[[nodiscard]] std::expected<QString, QString> establishSessionTempDirectory();
/// @p environment with TMP and TEMP set to sessionTempDirectory() (established on demand).
[[nodiscard]] QProcessEnvironment withSessionTemp(QProcessEnvironment environment);
/// A new file @p name in this run's temporary folder holding @p bytes, held read-only
/// (no_follow::writeHeldReadOnlyFile): for a script a tool reads by name, such as diskpart /s.
/// Returns the hold and the file's path; close the hold, then remove the file.
[[nodiscard]] std::expected<std::pair<no_follow::UniqueHandle, QString>, QString>
writeHeldSessionTempFile(const QString& name, const QByteArray& bytes);
/// Remove this run's temporary folder (at exit) without following any link in it.
void removeSessionTempDirectory();
/// Pure: the run folders in @p entries ("run-<pid>-<hex>", and 0.9.2's "session-<pid>") whose
/// process is gone (never @p own_pid).
[[nodiscard]] QStringList staleSessionTempDirectories(const QStringList& entries,
                                                      qint64 own_pid,
                                                      const std::function<bool(qint64)>& alive);
/// Remove the run folders earlier runs left behind (a crash skips the exit cleanup), without
/// following any link. Only run folders of processes that are gone; nothing else in data/temp.
int removeStaleSessionTempDirectories();
[[nodiscard]] bool ensureDirectory(const QString& path);

}  // namespace sak::app_paths
