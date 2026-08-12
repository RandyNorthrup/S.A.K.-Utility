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
     * @param scanOk Optional out-param; set false if ANY source (registry hive,
     *        AppX, or Chocolatey) could not be fully read, so a denied/failed
     *        source is surfaced rather than read as an empty inventory. Left
     *        untouched other than the aggregate result. Pass nullptr to ignore.
     * @return List of discovered applications
     */
    std::vector<AppInfo> scanAll(bool* scanOk = nullptr);

    /**
     * @brief Scan from Windows Registry (HKLM and HKCU)
     * @param scanOk Optional out-param; initialized to true, set false if any
     *        hive open/enum/subkey-open failed. Pass nullptr to ignore.
     * @return List of applications from registry
     */
    std::vector<AppInfo> scanRegistry(bool* scanOk = nullptr);

    /**
     * @brief Scan Windows Store (AppX) packages
     * @param scanOk Optional out-param; initialized to true, set false if the
     *        AppX source could not be enumerated (PowerShell unresolved, the
     *        query failed/timed out, or its output failed to parse). Pass
     *        nullptr to ignore.
     * @return List of AppX packages
     */
    static std::vector<AppInfo> scanAppX(bool* scanOk = nullptr);

    /**
     * @brief Scan already installed Chocolatey packages
     * @param scanOk Optional out-param; initialized to true, set false only when
     *        a PRESENT bundled choco could not be read (tampered binary or a
     *        failed/timed-out list). An absent bundled choco is a designed,
     *        legitimate "no Chocolatey source" state and leaves scanOk true.
     *        Pass nullptr to ignore.
     * @return List of Chocolatey packages
     */
    static std::vector<AppInfo> scanChocolatey(bool* scanOk = nullptr);

    /// @brief Resolve the absolute Windows PowerShell path under @p systemRoot's
    ///        System32 so a PATH/CWD-planted powershell.exe can never run in our
    ///        stead. Returns empty (fail closed) when @p systemRoot is empty.
    ///        Pure; unit-testable.
    [[nodiscard]] static QString composePowerShellPath(const QString& systemRoot);

    /// @brief The Windows directory as reported by the OS (GetWindowsDirectoryW),
    ///        NOT the attacker-influenceable %SystemRoot% environment variable, so
    ///        a poisoned env cannot redirect PowerShell resolution to a writable
    ///        directory. Returns empty (fail closed) if the API fails or off
    ///        Windows.
    [[nodiscard]] static QString authoritativeWindowsRoot();

private:
    /**
     * @brief Scan specific registry hive
     * @param hive HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER
     * @param subkey Registry subkey path
     * @param scanOk Optional out-param; set false (never back to true) if the
     *        hive open or any per-subkey enum/open failed. Pass nullptr to ignore.
     * @return List of applications from this hive
     */
    std::vector<AppInfo> scanRegistryHive(void* hive,
                                          const QString& subkey,
                                          bool* scanOk = nullptr);

    /**
     * @brief Enumerate every app subkey under an already-open hive key, appending
     *        matching entries to @p apps. Extracted from scanRegistryHive to keep
     *        that function within the length limit once completeness reporting was
     *        threaded through.
     * @param openKey Handle from a successful RegOpenKeyExW (an HKEY as void*)
     * @param hive The root hive (an HKEY as void*), used only to label the
     *        registry_key identifier HKLM/HKCU
     * @param subkey The uninstall subkey path being enumerated (for the label)
     * @param apps Destination list; matching, non-system apps are appended
     * @param scanOk Optional out-param; set false on any per-subkey enum/open
     *        failure so a partial enumeration is surfaced. Pass nullptr to ignore.
     */
    static void enumerateHiveApps(
        void* openKey, void* hive, const QString& subkey, std::vector<AppInfo>& apps, bool* scanOk);

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
