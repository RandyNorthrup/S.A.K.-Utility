// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/bundled_tools_manager.h"

#include <QFileInfo>

namespace sak {

BundledToolsManager& BundledToolsManager::instance() {
    static BundledToolsManager instance;
    return instance;
}

BundledToolsManager::BundledToolsManager() : m_base_path(QCoreApplication::applicationDirPath()) {}

QString BundledToolsManager::toolsPath() const {
    return m_base_path + "/tools";
}

QString BundledToolsManager::scriptsPath() const {
    return m_base_path + "/scripts";
}

QString BundledToolsManager::psModulePath(const QString& moduleName) const {
    return QString("%1/tools/ps_modules/%2").arg(m_base_path, moduleName);
}

QString BundledToolsManager::scriptPath(const QString& scriptName) const {
    return QString("%1/scripts/%2").arg(m_base_path, scriptName);
}

QString BundledToolsManager::toolPath(const QString& category, const QString& exeName) const {
    return QString("%1/tools/%2/%3").arg(m_base_path, category, exeName);
}

bool BundledToolsManager::toolExists(const QString& category, const QString& exeName) const {
    // Fail closed: a resolved DIRECTORY (an empty/"." exeName collapses the path onto a parent
    // folder that exists) is not a runnable tool -- require a regular file, not mere presence.
    const QFileInfo info(toolPath(category, exeName));
    return info.exists() && info.isFile();
}

bool BundledToolsManager::scriptExists(const QString& scriptName) const {
    const QFileInfo info(scriptPath(scriptName));
    return info.exists() && info.isFile();
}

bool BundledToolsManager::moduleExists(const QString& moduleName) const {
    // A PowerShell module is its own named subdirectory. An empty/"."/".." name resolves to the
    // ps_modules root (or its parent), which exists -- reject it so only a real module matches.
    const QString trimmed = moduleName.trimmed();
    if (trimmed.isEmpty() || trimmed == QStringLiteral(".") || trimmed == QStringLiteral("..")) {
        return false;
    }
    const QDir moduleDir(psModulePath(moduleName));
    return moduleDir.exists();
}

QString BundledToolsManager::getModuleImportCommand(const QString& moduleName) const {
    QString modulePath = psModulePath(moduleName);
    // Escape for a PowerShell single-quoted string literal: a literal single quote is doubled
    // ('') so an attacker-supplied module name cannot close the quote and inject a command. Do
    // this FIRST, before any other substitution, so no earlier edit can reintroduce a bare quote.
    modulePath.replace(QLatin1Char('\''), QStringLiteral("''"));
    // Escape backslashes for PowerShell
    modulePath.replace("\\", "\\\\");
    return QString("Import-Module '%1' -Force").arg(modulePath);
}

}  // namespace sak
