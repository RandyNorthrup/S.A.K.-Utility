// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QString>

namespace sak {

/**
 * @brief Manages access to bundled tools and scripts
 *
 * Provides paths to bundled PowerShell modules, scripts, and executables
 * that are included in the application distribution.
 */
class BundledToolsManager {
public:
    /**
     * @brief Get singleton instance
     */
    static BundledToolsManager& instance();

    /**
     * @brief Get path to bundled tools directory
     */
    QString toolsPath() const;

    /**
     * @brief Get path to bundled scripts directory
     */
    QString scriptsPath() const;

    /**
     * @brief Get path to a specific PowerShell module
     * @param moduleName Name of the module (e.g., "PSWindowsUpdate")
     */
    QString psModulePath(const QString& moduleName) const;

    /**
     * @brief Get path to a specific script
     * @param scriptName Name of the script (e.g., "browser_cache_clear.ps1")
     */
    QString scriptPath(const QString& scriptName) const;

    /**
     * @brief Get path to a specific tool executable
     * @param category Tool category (e.g., "sysinternals")
     * @param exeName Executable name (e.g., "PsExec.exe")
     */
    QString toolPath(const QString& category, const QString& exeName) const;

    /**
     * @brief Check if a bundled tool exists
     */
    bool toolExists(const QString& category, const QString& exeName) const;

    /**
     * @brief True when @p path may be run as a bundled tool: a real regular file, not a link
     *        itself, whose real path (every junction and symbolic link on the way resolved,
     *        no_follow::pathWithinRoot) lies inside @p root resolved the same way.
     *
     * A junction in a folder between @p root and the file, pointing outside, refuses: the
     * QFileInfo::canonicalFilePath comparison this replaces let it through, because Qt 6 does
     * not resolve a junction there. A @p root that is itself reached through a junction is fine.
     */
    [[nodiscard]] static bool isTrustedToolFile(const QString& path, const QString& root);

    /**
     * @brief Check if a bundled script exists
     */
    bool scriptExists(const QString& scriptName) const;

    /**
     * @brief Check if a bundled PowerShell module exists
     */
    bool moduleExists(const QString& moduleName) const;

    /**
     * @brief Get PowerShell command to import a bundled module
     * @param moduleName Name of the module
     * @return PowerShell command string
     */
    QString getModuleImportCommand(const QString& moduleName) const;

private:
    BundledToolsManager();
    ~BundledToolsManager() = default;

    BundledToolsManager(const BundledToolsManager&) = delete;
    BundledToolsManager& operator=(const BundledToolsManager&) = delete;

    QString m_base_path;
};

}  // namespace sak
