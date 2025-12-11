#include "sak/app_scanner.h"
#include <QProcess>
#include <QDebug>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sak {

// Registry paths for installed applications
static const wchar_t* REGISTRY_UNINSTALL_HKLM = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
static const wchar_t* REGISTRY_UNINSTALL_HKCU = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
static const wchar_t* REGISTRY_UNINSTALL_WOW64 = L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

AppScanner::AppScanner() = default;
AppScanner::~AppScanner() = default;

std::vector<AppScanner::AppInfo> AppScanner::scanAll() {
    std::vector<AppInfo> all_apps;
    
    // Scan registry (HKLM + HKCU)
    auto registry_apps = scanRegistry();
    all_apps.insert(all_apps.end(), registry_apps.begin(), registry_apps.end());
    
    // Scan AppX packages
    auto appx_apps = scanAppX();
    all_apps.insert(all_apps.end(), appx_apps.begin(), appx_apps.end());
    
    // Scan Chocolatey packages
    auto choco_apps = scanChocolatey();
    all_apps.insert(all_apps.end(), choco_apps.begin(), choco_apps.end());
    
    qDebug() << "AppScanner: Found" << all_apps.size() << "total applications";
    
    return all_apps;
}

std::vector<AppScanner::AppInfo> AppScanner::scanRegistry() {
    std::vector<AppInfo> apps;
    
    // Scan HKEY_LOCAL_MACHINE (system-wide apps)
    auto hklm_apps = scanRegistryHive(HKEY_LOCAL_MACHINE, QString::fromWCharArray(REGISTRY_UNINSTALL_HKLM));
    apps.insert(apps.end(), hklm_apps.begin(), hklm_apps.end());
    
    // Scan HKEY_LOCAL_MACHINE WOW6432Node (32-bit apps on 64-bit Windows)
    auto wow64_apps = scanRegistryHive(HKEY_LOCAL_MACHINE, QString::fromWCharArray(REGISTRY_UNINSTALL_WOW64));
    apps.insert(apps.end(), wow64_apps.begin(), wow64_apps.end());
    
    // Scan HKEY_CURRENT_USER (user-specific apps)
    auto hkcu_apps = scanRegistryHive(HKEY_CURRENT_USER, QString::fromWCharArray(REGISTRY_UNINSTALL_HKCU));
    apps.insert(apps.end(), hkcu_apps.begin(), hkcu_apps.end());
    
    qDebug() << "AppScanner: Found" << apps.size() << "applications from registry";
    
    return apps;
}

std::vector<AppScanner::AppInfo> AppScanner::scanRegistryHive(void* hive, const QString& subkey) {
    std::vector<AppInfo> apps;
    
    HKEY hKey;
    LONG result = RegOpenKeyExW(
        static_cast<HKEY>(hive),
        subkey.toStdWString().c_str(),
        0,
        KEY_READ,
        &hKey
    );
    
    if (result != ERROR_SUCCESS) {
        qWarning() << "AppScanner: Failed to open registry key:" << subkey;
        return apps;
    }
    
    // Enumerate subkeys (each represents an app)
    DWORD index = 0;
    wchar_t subKeyName[256];
    DWORD subKeyNameSize = 256;
    
    while (RegEnumKeyExW(hKey, index, subKeyName, &subKeyNameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        // Open this application's registry key
        HKEY appKey;
        if (RegOpenKeyExW(hKey, subKeyName, 0, KEY_READ, &appKey) == ERROR_SUCCESS) {
            AppInfo app;
            app.source = AppInfo::Source::Registry;
            app.registry_key = subkey + "\\" + QString::fromWCharArray(subKeyName);
            
            // Read application details
            app.name = readRegistryValue(appKey, "DisplayName");
            app.version = readRegistryValue(appKey, "DisplayVersion");
            app.publisher = readRegistryValue(appKey, "Publisher");
            app.install_date = readRegistryValue(appKey, "InstallDate");
            app.install_location = readRegistryValue(appKey, "InstallLocation");
            app.uninstall_string = readRegistryValue(appKey, "UninstallString");
            
            // Only add if we have a display name
            if (!app.name.isEmpty()) {
                // Filter out Windows components and system apps
                if (!app.name.startsWith("KB") &&  // Windows updates
                    !app.name.startsWith("Security Update") &&
                    !app.name.contains("(KB") &&
                    !app.publisher.contains("Microsoft Corporation") || app.name.contains("Visual Studio")) {
                    
                    qDebug() << "  Found:" << app.name << "v" << app.version;
                    apps.push_back(app);
                }
            }
            
            RegCloseKey(appKey);
        }
        
        index++;
        subKeyNameSize = 256;
    }
    
    RegCloseKey(hKey);
    return apps;
}

QString AppScanner::readRegistryValue(void* key, const QString& valueName) {
    HKEY hKey = static_cast<HKEY>(key);
    wchar_t buffer[1024];
    DWORD bufferSize = sizeof(buffer);
    DWORD type;
    
    LONG result = RegQueryValueExW(
        hKey,
        valueName.toStdWString().c_str(),
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(buffer),
        &bufferSize
    );
    
    if (result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        return QString::fromWCharArray(buffer);
    }
    
    return QString();
}

std::vector<AppScanner::AppInfo> AppScanner::scanAppX() {
    std::vector<AppInfo> apps;
    
    // Use PowerShell to enumerate AppX packages
    QProcess process;
    process.setProgram("powershell.exe");
    process.setArguments({
        "-NoProfile",
        "-Command",
        "Get-AppxPackage | Select-Object Name,Version,Publisher,InstallLocation | ConvertTo-Json"
    });
    
    process.start();
    if (!process.waitForFinished(30000)) {
        qWarning() << "AppScanner: PowerShell timeout while scanning AppX packages";
        return apps;
    }
    
    QString output = QString::fromUtf8(process.readAllStandardOutput());
    
    // Parse JSON output (simplified for now - full implementation would use QJsonDocument)
    // For now, just log that we found AppX packages
    if (!output.isEmpty()) {
        qDebug() << "AppScanner: AppX packages found (parsing not yet implemented)";
    }
    
    return apps;
}

std::vector<AppScanner::AppInfo> AppScanner::scanChocolatey() {
    std::vector<AppInfo> apps;
    
    // Check if chocolatey is installed
    QProcess process;
    process.setProgram("choco");
    process.setArguments({"list", "--local-only"});
    
    process.start();
    if (!process.waitForFinished(10000)) {
        qDebug() << "AppScanner: Chocolatey not available or timeout";
        return apps;
    }
    
    QString output = QString::fromUtf8(process.readAllStandardOutput());
    
    // Parse choco list output (simplified)
    if (!output.isEmpty()) {
        qDebug() << "AppScanner: Chocolatey packages found (parsing not yet implemented)";
    }
    
    return apps;
}

} // namespace sak
