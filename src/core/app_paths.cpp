// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_paths.h"

#include <QCoreApplication>
#include <QDir>

namespace sak::app_paths {

QString applicationDirectory() {
    QString app_dir = QCoreApplication::applicationDirPath();
    if (app_dir.trimmed().isEmpty()) {
        app_dir = QDir::currentPath();
    }
    return app_dir;
}

QString dataRoot() {
    return QDir(applicationDirectory()).filePath(QStringLiteral("data"));
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
