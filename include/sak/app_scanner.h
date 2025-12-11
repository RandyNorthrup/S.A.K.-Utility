#pragma once

#include <QString>
#include <QStringList>
#include <vector>
#include <expected>

namespace sak {

/**
 * @brief Scans installed applications from Windows Registry and AppX packages
 * 
 * SCAN-FIRST APPROACH:
 * - Implement this first
 * - Run on dev machine to get real data
 * - Inspect actual values to design AppInfo structure
 * - Validate all fields populate correctly
 */
class AppScanner {
public:
    struct AppInfo {
        QString name;               // Application name
        QString version;            // Installed version
        QString publisher;          // Publisher/vendor
        QString install_date;       // Installation date
        QString install_location;   // Install path
        QString uninstall_string;   // Uninstall command
        QString registry_key;       // Registry location
        
        // To be populated later by PackageMatcher
        QString choco_package;      // Matched Chocolatey package name
        bool choco_available{false}; // Is available in Chocolatey?
        
        enum class Confidence { High, Medium, Low, Manual, Unknown };
        Confidence match_confidence{Confidence::Unknown};
        
        enum class Source { Registry, AppX, Chocolatey };
        Source source{Source::Registry};
        
        // Version locking (for migration)
        bool version_locked{false};        // Lock to specific version for restore
        QString locked_version;            // The version to install (defaults to current version)
        
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
    std::vector<AppInfo> scanAppX();

    /**
     * @brief Scan already installed Chocolatey packages
     * @return List of Chocolatey packages
     */
    std::vector<AppInfo> scanChocolatey();

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
    QString readRegistryValue(void* key, const QString& valueName);

    /**
     * @brief Parse AppX package using PowerShell
     * @return List of AppX packages
     */
    std::vector<AppInfo> parseAppXPackages();
};

} // namespace sak
