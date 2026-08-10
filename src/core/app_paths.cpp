// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_paths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <atomic>

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

QString dataRoot() {
    if (isPackaged()) {
        const QString local_data = writableAppLocalDataLocation();
        if (!local_data.trimmed().isEmpty()) {
            return local_data;
        }
    }

    const QString app_dir = applicationDirectory();
    if (!app_dir.trimmed().isEmpty()) {
        const QString portable_data = QDir(app_dir).filePath(QStringLiteral("data"));
        if (isWritableDirectory(portable_data)) {
            return portable_data;
        }
    }

    // Portable storage is unavailable or not writable. Use the OS per-user writable
    // location. Never return a current-working-directory-relative path, and never return
    // the portable path that just failed its writability probe.
    const QString local_data = writableAppLocalDataLocation();
    if (!local_data.trimmed().isEmpty()) {
        return local_data;
    }
    return QString();
}

QString configDirectory() {
    return QDir(dataRoot()).filePath(QStringLiteral("config"));
}

QString configFilePath() {
    return QDir(configDirectory()).filePath(QStringLiteral("Utility.ini"));
}

QString logsDirectory() {
    return QDir(dataRoot()).filePath(QStringLiteral("logs"));
}

QString tempDirectory() {
    return QDir(dataRoot()).filePath(QStringLiteral("temp"));
}

bool ensureDirectory(const QString& path) {
    return QDir().mkpath(path);
}

}  // namespace sak::app_paths
