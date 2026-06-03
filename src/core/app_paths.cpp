// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>

#include <appmodel.h>
#endif

namespace sak::app_paths {

namespace {

QString writableAppLocalDataLocation() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

bool isWritableDirectory(const QString& path) {
    if (path.trimmed().isEmpty()) {
        return false;
    }
    if (!QDir().mkpath(path)) {
        return false;
    }

    const QString probe_path = QDir(path).filePath(QStringLiteral(".sak_write_probe"));
    QFile probe(probe_path);
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    probe.close();
    QFile::remove(probe_path);
    return true;
}

}  // namespace

QString applicationDirectory() {
    QString app_dir = QCoreApplication::applicationDirPath();
    if (app_dir.trimmed().isEmpty()) {
        app_dir = QDir::currentPath();
    }
    return app_dir;
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

    const QString portable_data = QDir(applicationDirectory()).filePath(QStringLiteral("data"));
    if (isWritableDirectory(portable_data)) {
        return portable_data;
    }

    const QString local_data = writableAppLocalDataLocation();
    if (!local_data.trimmed().isEmpty()) {
        return local_data;
    }
    return portable_data;
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
