// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/bundled_tools_manager.h"
#include <QFileInfo>

namespace sak {

BundledToolsManager& BundledToolsManager::instance() {
    static BundledToolsManager instance;
    return instance;
}

BundledToolsManager::BundledToolsManager() {
    // Get application directory
    m_base_path = QCoreApplication::applicationDirPath();
}

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
    return QFileInfo::exists(toolPath(category, exeName));
}

bool BundledToolsManager::scriptExists(const QString& scriptName) const {
    return QFileInfo::exists(scriptPath(scriptName));
}

bool BundledToolsManager::moduleExists(const QString& moduleName) const {
    QDir moduleDir(psModulePath(moduleName));
    return moduleDir.exists();
}

QString BundledToolsManager::getModuleImportCommand(const QString& moduleName) const {
    QString modulePath = psModulePath(moduleName);
    // Escape backslashes for PowerShell
    modulePath.replace("\\", "\\\\");
    return QString("Import-Module '%1' -Force").arg(modulePath);
}

} // namespace sak
