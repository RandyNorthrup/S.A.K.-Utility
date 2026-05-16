// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_paths.h"

#include "sak/app_paths.h"

#include <QDir>

namespace sak::ai {

QString credentialDirectory() {
    return QDir(sak::app_paths::dataRoot()).filePath(QStringLiteral("credentials"));
}

QString sessionRootDirectory() {
    return QDir(sak::app_paths::dataRoot()).filePath(QStringLiteral("ai_sessions"));
}

QString workflowLibraryDirectory() {
    return QDir(sak::app_paths::dataRoot()).filePath(QStringLiteral("ai/workflows"));
}

}  // namespace sak::ai
