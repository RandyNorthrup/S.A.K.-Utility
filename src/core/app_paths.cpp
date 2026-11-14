// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_paths.h"

#include "sak/elevation_manager.h"
#include "sak/logger.h"
#include "sak/no_follow_fs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include <windows.h>

#include <appmodel.h>
#include <sddl.h>

namespace sak::app_paths {

namespace {

QString writableAppLocalDataLocation() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

// Build an unpredictable, per-call write-probe file name. A fixed name (the old
// ".sak_write_probe") is guessable, so a pre-planted file or symlink at that path
// could be truncated (and then deleted) by the probe -- destructive when this
// check runs from the elevated helper. pid+timestamp+counter makes the name
// impractical to pre-plant.
QString makeWriteProbeName() {
    static std::atomic<quint64> counter{0};
    const quint64 seq = counter.fetch_add(1, std::memory_order_relaxed);
    return QStringLiteral(".sak_write_probe_%1_%2_%3")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(seq);
}

bool isWritableDirectory(const QString& path) {
    if (path.trimmed().isEmpty()) {
        return false;
    }
    if (!QDir().mkpath(path)) {
        return false;
    }

    const QString probe_path = QDir(path).filePath(makeWriteProbeName());
    QFile probe(probe_path);
    // NewOnly is an exclusive create: it fails if ANYTHING already exists at the
    // name (a real file, or a symlink/reparse point planted to redirect us), so
    // the probe never truncates or follows a link into a victim file. Combined
    // with the unpredictable name, there is nothing to clobber.
    if (!probe.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return false;
    }
    probe.close();
    QFile::remove(probe_path);
    return true;
}

// Join a child onto a data root, propagating the fail-closed empty sentinel. When
// dataRoot() (or a derived directory) is empty, QDir(QString()).filePath(child)
// would resolve to a current-working-directory-relative path (e.g. "./config"),
// silently reintroducing the CWD-derived path this module refuses to produce. An
// empty root must stay empty so ConfigManager's mkpath fails and startup fails closed.
QString joinDataPath(const QString& root, const QString& child) {
    if (root.trimmed().isEmpty()) {
        return QString();
    }
    return QDir(root).filePath(child);
}

}  // namespace

QString applicationDirectory() {
    // Return the real executable directory only. When it is unavailable (no
    // QCoreApplication instance yet), fail closed with an empty string rather than
    // substituting the attacker-influenceable current working directory. dataRoot()
    // skips the portable branch when this is empty, so no data path is ever derived
    // from the current directory.
    return QCoreApplication::applicationDirPath();
}

bool isPackaged() {
    UINT32 length = 0;
    const LONG result = GetCurrentPackageFullName(&length, nullptr);
    return result == ERROR_INSUFFICIENT_BUFFER;
}

std::expected<QString, QString> decideDataRoot(
    bool packaged,
    const QString& app_dir,
    const QString& per_user_location,
    const std::function<bool(const QString&)>& writable) {
    if (packaged) {
        // An MSIX install's own folder is read-only by design; its per-user location IS its home.
        if (per_user_location.trimmed().isEmpty()) {
            return std::unexpected(
                QStringLiteral("Windows did not provide a data folder for this packaged install."));
        }
        return per_user_location;
    }
    if (app_dir.trimmed().isEmpty()) {
        return std::unexpected(
            QStringLiteral("The program folder could not be determined, so there is no portable "
                           "data folder to use."));
    }
    const QString portable_data = QDir(app_dir).filePath(QStringLiteral("data"));
    if (!writable || !writable(portable_data)) {
        // No fallback (owner rule): settings and logs silently moving to %LOCALAPPDATA% meant a
        // "portable" tool left data on the host and the user never knew where it went.
        return std::unexpected(
            QStringLiteral("S.A.K. Utility keeps its settings, logs and temporary files in the "
                           "\"data\" folder next to the program, and that folder is not writable:\n"
                           "%1\n\nMove the program to a folder you can write to (for example a "
                           "USB drive or your Documents folder, not Program Files or a read-only "
                           "drive), or run it with permission to write there.")
                .arg(QDir::toNativeSeparators(portable_data)));
    }
    return portable_data;
}

std::expected<QString, QString> initializeDataRoot() {
    // Decided once per process: the probe used to create data/ and write a file on EVERY path
    // lookup, and a later lookup could land somewhere else than an earlier one.
    static std::once_flag once;
    static std::expected<QString, QString> decision = std::unexpected(QString());
    std::call_once(once, []() {
        decision = decideDataRoot(isPackaged(),
                                  applicationDirectory(),
                                  writableAppLocalDataLocation(),
                                  isWritableDirectory);
    });
    return decision;
}

QString dataRoot() {
    const auto decided = initializeDataRoot();
    // Empty when undecidable: every derived path is then empty too, so nothing is written to a
    // working-directory-relative location (joinDataPath keeps the empty sentinel).
    return decided ? *decided : QString();
}
std::expected<no_follow::HeldDirectory, QString> openDataSubdirectory(const QString& name) {
    const QString root = dataRoot();
    if (root.isEmpty()) {
        return std::unexpected(QStringLiteral("There is no data folder"));
    }
    // The program's own folder (or, packaged, the per-user location) is the anchor: whatever was
    // launched is trusted wherever links put it. "data" and the folder below it are checked.
    const bool packaged = isPackaged();
    auto anchor = no_follow::HeldDirectory::openAnchor(packaged ? root : applicationDirectory());
    if (!anchor) {
        return std::unexpected(anchor.error());
    }
    return packaged ? anchor->child(name, true)
                    : anchor->descend(QStringLiteral("data/") + name, true);
}

QString configDirectory() {
    return joinDataPath(dataRoot(), QStringLiteral("config"));
}

QString configFilePath() {
    return joinDataPath(configDirectory(), QStringLiteral("Utility.ini"));
}

QString uiStateFilePath() {
    return joinDataPath(configDirectory(), QStringLiteral("ui_state.ini"));
}

QString logsDirectory() {
    return joinDataPath(dataRoot(), QStringLiteral("logs"));
}

QString crashesDirectory() {
    return joinDataPath(dataRoot(), QStringLiteral("crashes"));
}

QString tempDirectory() {
    return joinDataPath(dataRoot(), QStringLiteral("temp"));
}

// ======================================================================
// This run's temporary folder (data\temp\run-<pid>-<random>)
// ======================================================================

namespace {

// Elevated: SYSTEM and Administrators only. Not elevated: the person running it as well. Either
// way no other account can create a link or a file in it on a volume that keeps ACLs (owner,
// 2026-11-11: nothing of ours lives outside the program's folder, so this is where TEMP goes).
const QString kElevatedRunSddl = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
const QString kRunFolderPrefix = QStringLiteral("run-%1-");

struct RunTempState {
    std::optional<no_follow::HeldDirectory> temp_root;  ///< data\temp, held for the whole run
    std::optional<no_follow::HeldDirectory> run;        ///< this run's folder, held until exit
};

RunTempState& runTempState() {
    static RunTempState state;
    return state;
}

std::mutex& runTempMutex() {
    static std::mutex mutex;
    return mutex;
}

QString currentUserSidString() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<std::byte> buffer(needed);
    QString sid_text;
    if (needed != 0 && GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) != 0) {
        LPWSTR text = nullptr;
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        if (ConvertSidToStringSidW(user->User.Sid, &text) != 0) {
            sid_text = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return sid_text;
}

std::expected<QString, QString> runFolderSddl() {
    if (ElevationManager::isElevated()) {
        return kElevatedRunSddl;
    }
    const QString user = currentUserSidString();
    if (user.isEmpty()) {
        return std::unexpected(QStringLiteral("Cannot read this account's identity"));
    }
    return kElevatedRunSddl + QStringLiteral("(A;OICI;FA;;;%1)").arg(user);
}

/// Under runTempMutex(): hold data\temp once.
std::expected<void, QString> holdTempRootLocked() {
    RunTempState& state = runTempState();
    if (state.temp_root) {
        return {};
    }
    auto root = openDataSubdirectory(QStringLiteral("temp"));
    if (!root) {
        return std::unexpected(root.error());
    }
    state.temp_root = std::move(*root);
    return {};
}

bool pointTempVariablesAt(const QString& folder) {
    const QByteArray native = QDir::toNativeSeparators(folder).toLocal8Bit();
    // Both must land: half a redirect leaves children choosing between two folders.
    const bool tmp_set = qputenv("TMP", native);
    const bool temp_set = qputenv("TEMP", native);
    return tmp_set && temp_set;
}

}  // namespace

std::expected<QString, QString> establishSessionTempDirectory() {
    const std::lock_guard lock(runTempMutex());
    RunTempState& state = runTempState();
    if (state.run) {
        return state.run->path();
    }
    if (auto held = holdTempRootLocked(); !held) {
        return std::unexpected(held.error());
    }
    const auto sddl = runFolderSddl();
    if (!sddl) {
        return std::unexpected(sddl.error());
    }
    auto run = state.temp_root->createUniqueChild(
        kRunFolderPrefix.arg(QCoreApplication::applicationPid()), *sddl);
    if (!run) {
        return std::unexpected(run.error());
    }
    if (!pointTempVariablesAt(run->path())) {
        const QString failed = run->path();
        run = std::unexpected(QString());
        (void)no_follow::removeTree(failed);
        return std::unexpected(QStringLiteral("Cannot point TMP and TEMP at %1").arg(failed));
    }
    state.run = std::move(*run);
    return state.run->path();
}

std::expected<std::pair<no_follow::UniqueHandle, QString>, QString> writeHeldSessionTempFile(
    const QString& name, const QByteArray& bytes) {
    if (auto established = establishSessionTempDirectory(); !established) {
        return std::unexpected(established.error());
    }
    const std::lock_guard lock(runTempMutex());
    const RunTempState& state = runTempState();
    if (!state.run) {
        return std::unexpected(QStringLiteral("There is no temporary folder"));
    }
    auto held = no_follow::writeHeldReadOnlyFile(*state.run, name, bytes);
    if (!held) {
        return std::unexpected(held.error());
    }
    return std::pair<no_follow::UniqueHandle, QString>(std::move(*held), state.run->filePath(name));
}

QString sessionTempDirectory() {
    const std::lock_guard lock(runTempMutex());
    const RunTempState& state = runTempState();
    return state.run ? state.run->path() : QString();
}

QProcessEnvironment withSessionTemp(QProcessEnvironment environment) {
    const auto dir = establishSessionTempDirectory();
    if (!dir) {
        return environment;  // TMP and TEMP then still name what this process already uses
    }
    const QString native = QDir::toNativeSeparators(*dir);
    environment.insert(QStringLiteral("TMP"), native);
    environment.insert(QStringLiteral("TEMP"), native);
    return environment;
}

void removeSessionTempDirectory() {
    const std::lock_guard lock(runTempMutex());
    RunTempState& state = runTempState();
    if (!state.run) {
        return;
    }
    const QString folder = state.run->path();
    state.run.reset();  // let go of it; data\temp stays held, so the path still lands there
    if (auto removed = no_follow::removeTree(folder); !removed) {
        // Something still holds a file there (a program started from S.A.K. that outlived it):
        // the next start removes what is left.
        sak::logWarning("Could not remove this run's temporary folder: {}",
                        removed.error().toStdString());
    }
}

QStringList staleSessionTempDirectories(const QStringList& entries,
                                        qint64 own_pid,
                                        const std::function<bool(qint64)>& alive) {
    // run-<pid>-<32 hex> is a run folder; session-<pid> is the folder 0.9.2 used.
    static const QRegularExpression kRunFolder(
        QStringLiteral("^(?:session-(\\d+)|run-(\\d+)-[0-9a-f]{32})$"));
    constexpr int kSessionPidGroup = 1;  // 0.9.2's session-<pid>
    constexpr int kRunPidGroup = 2;      // run-<pid>-<random>
    QStringList stale;
    for (const QString& entry : entries) {
        const auto match = kRunFolder.match(entry);
        if (!match.hasMatch()) {
            continue;  // not a run folder: never ours to remove
        }
        const QString pid_text = match.captured(kSessionPidGroup).isEmpty()
                                     ? match.captured(kRunPidGroup)
                                     : match.captured(kSessionPidGroup);
        const qint64 pid = pid_text.toLongLong();
        if (pid != own_pid && !(alive && alive(pid))) {
            stale.append(entry);
        }
    }
    return stale;
}

namespace {
bool processIsRunning(qint64 pid) {
    HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    const bool running = ::GetExitCodeProcess(process, &exit_code) != 0 &&
                         exit_code == STILL_ACTIVE;
    ::CloseHandle(process);
    return running;
}
}  // namespace

int removeStaleSessionTempDirectories() {
    const std::lock_guard lock(runTempMutex());
    if (auto held = holdTempRootLocked(); !held) {
        sak::logWarning("Could not open the temporary folder to tidy it: {}",
                        held.error().toStdString());
        return 0;
    }
    const no_follow::HeldDirectory& root = *runTempState().temp_root;
    // Junctions list as folders here; removeTree removes a junction itself, never its target.
    const QStringList entries =
        QDir(root.path())
            .entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    int removed = 0;
    for (const QString& entry : staleSessionTempDirectories(
             entries, QCoreApplication::applicationPid(), processIsRunning)) {
        if (auto outcome = no_follow::removeTree(root.filePath(entry)); outcome) {
            ++removed;
        } else {
            // An unelevated run cannot remove an elevated run's folder; the next elevated run can.
            sak::logInfo("Left an old temporary folder: {}", outcome.error().toStdString());
        }
    }
    return removed;
}

bool ensureDirectory(const QString& path) {
    return QDir().mkpath(path);
}

}  // namespace sak::app_paths
