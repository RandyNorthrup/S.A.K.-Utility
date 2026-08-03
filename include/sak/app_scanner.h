// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <expected>
#include <vector>

namespace sak {

/**
 * @brief Scans installed applications from Windows Registry and AppX packages
 *
 * Enumerates all installed software on the system by querying:
 * - Win32 registry (HKLM/HKCU Uninstall keys, both native and WoW64)
 * - AppX/UWP packages via PowerShell
 * - Chocolatey package manager (when available)
 */
class AppScanner {
public:
    /// @brief Information about a discovered installed application
    struct AppInfo {
        QString name;              // Application name
        QString version;           // Installed version
        QString publisher;         // Publisher/vendor
        QString install_date;      // Installation date
        QString install_location;  // Install path
        QString uninstall_string;  // Uninstall command
        QString registry_key;      // Registry location

        // To be populated later by PackageMatcher
        QString choco_package;        // Matched Chocolatey package name
        bool choco_available{false};  // Is available in Chocolatey?

        enum class Confidence {
            High,
            Medium,
            Low,
            Manual,
            Unknown
        };
        Confidence match_confidence{Confidence::Unknown};

        enum class Source {
            Registry,
            AppX,
            Chocolatey
        };
        Source source{Source::Registry};

        // Version locking (for migration)
        bool version_locked{false};  // Lock to specific version for restore
        QString locked_version;      // The version to install (defaults to current version)

        // User data (populated by UserDataManager)
        bool has_user_data{false};
        qint64 estimated_data_size{0};
    };

    AppScanner();
    ~AppScanner();

    /**
     * @brief Scan installed applications from all sources
     * @return List of discovered applications
     */
    std::vector<AppInfo> scanAll();

    /**
     * @brief Scan from Windows Registry (HKLM and HKCU)
     * @return List of applications from registry
     */
    std::vector<AppInfo> scanRegistry();

    /**
     * @brief Scan Windows Store (AppX) packages
     * @return List of AppX packages
     */
    static std::vector<AppInfo> scanAppX();

    /**
     * @brief Scan already installed Chocolatey packages
     * @return List of Chocolatey packages
     */
    static std::vector<AppInfo> scanChocolatey();

    /// @brief Resolve the absolute Windows PowerShell path under @p systemRoot's
    ///        System32 so a PATH/CWD-planted powershell.exe can never run in our
    ///        stead. Returns empty (fail closed) when @p systemRoot is empty.
    ///        Pure; unit-testable.
    [[nodiscard]] static QString composePowerShellPath(const QString& systemRoot);

private:
    /**
     * @brief Scan specific registry hive
     * @param hive HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER
     * @param subkey Registry subkey path
     * @return List of applications from this hive
     */
    std::vector<AppInfo> scanRegistryHive(void* hive, const QString& subkey);

    /**
     * @brief Read registry value
     * @param key Registry key handle
     * @param valueName Name of value to read
     * @return Value as QString, or empty if error
     */
    static QString readRegistryValue(void* key, const QString& valueName);

    /**
     * @brief Convert a raw REG_SZ/REG_EXPAND_SZ buffer to a QString.
     *
     * Bounds the read to the byte count RegQueryValueExW reported and strips any
     * trailing NUL, so a value that fills the buffer with no terminator cannot
     * cause an out-of-bounds read. Exposed for headless unit testing.
     * @param data Wide-char buffer as written by RegQueryValueExW
     * @param byteLength Byte count RegQueryValueExW returned
     * @param capacityChars Buffer capacity in wchar_t units
     * @return Decoded string, length-bounded and NUL-trimmed
     */
    static QString registryStringFromBuffer(const wchar_t* data,
                                            unsigned long byteLength,
                                            int capacityChars);

    /// @brief Check if app name indicates a system component to filter
    static bool isSystemComponent(const QString& name);
};

}  // namespace sak
