// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_paths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>

#include <atomic>
#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>

#include <appmodel.h>
#endif

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
#ifdef Q_OS_WIN
    UINT32 length = 0;
    const LONG result = GetCurrentPackageFullName(&length, nullptr);
    return result == ERROR_INSUFFICIENT_BUFFER;
#else
    return false;
#endif
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

QString sessionTempDirectory() {
    return joinDataPath(tempDirectory(),
                        QStringLiteral("session-%1").arg(QCoreApplication::applicationPid()));
}

QProcessEnvironment withSessionTemp(QProcessEnvironment environment) {
    const QString dir = sessionTempDirectory();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        return environment;  // the child keeps the normal temp rather than an unusable one
    }
    const QString native = QDir::toNativeSeparators(dir);
    environment.insert(QStringLiteral("TMP"), native);
    environment.insert(QStringLiteral("TEMP"), native);
    return environment;
}

void removeSessionTempDirectory() {
    const QString dir = sessionTempDirectory();
    if (!dir.isEmpty()) {
        QDir(dir).removeRecursively();
    }
}

QStringList staleSessionTempDirectories(const QStringList& entries,
                                        qint64 own_pid,
                                        const std::function<bool(qint64)>& alive) {
    static const QRegularExpression session(QStringLiteral("^session-(\\d+)$"));
    QStringList stale;
    for (const QString& entry : entries) {
        const auto match = session.match(entry);
        if (!match.hasMatch()) {
            continue;  // not a session folder: never ours to remove
        }
        const qint64 pid = match.captured(1).toLongLong();
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
    const QString base = tempDirectory();
    if (base.isEmpty()) {
        return 0;
    }
    const QDir dir(base);
    const QStringList stale =
        staleSessionTempDirectories(dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot),
                                    QCoreApplication::applicationPid(),
                                    processIsRunning);
    int removed = 0;
    for (const QString& entry : stale) {
        if (QDir(dir.filePath(entry)).removeRecursively()) {
            ++removed;
        }
    }
    return removed;
}
bool ensureDirectory(const QString& path) {
    return QDir().mkpath(path);
}

}  // namespace sak::app_paths
