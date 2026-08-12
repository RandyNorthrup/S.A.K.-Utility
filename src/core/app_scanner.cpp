// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_scanner.h"

#include "sak/bundled_tools_manager.h"
#include "sak/chocolatey_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sak {

namespace {
constexpr int kRegistrySubKeyNameChars = 256;
constexpr int kRegistryValueBufferChars = 1024;
constexpr int kChocolateyListFieldCount = 2;

// Mark a scan source incomplete when a hive open / enum / subkey-open / AppX /
// choco step fails, so a denied or failed source is surfaced to the caller
// rather than silently read as an empty inventory. Only ever clears the flag;
// the aggregating caller initializes it to true.
void markSourceIncomplete(bool* scanOk) {
    if (scanOk != nullptr) {
        *scanOk = false;
    }
}

#ifdef _WIN32
// Short hive label so a registry_key identifier disambiguates HKLM vs HKCU
// entries that share the same Uninstall subkey path.
QString hiveDisplayPrefix(HKEY hive) {
    if (hive == HKEY_LOCAL_MACHINE) {
        return QStringLiteral("HKLM");
    }
    if (hive == HKEY_CURRENT_USER) {
        return QStringLiteral("HKCU");
    }
    return QStringLiteral("HKEY");
}
#endif
}  // namespace

// Registry paths for installed applications
static const wchar_t* REGISTRY_UNINSTALL_HKLM =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
static const wchar_t* REGISTRY_UNINSTALL_HKCU =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
static const wchar_t* REGISTRY_UNINSTALL_WOW64 =
    L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

AppScanner::AppScanner() = default;
AppScanner::~AppScanner() = default;

std::vector<AppScanner::AppInfo> AppScanner::scanAll(bool* scanOk) {
    std::vector<AppInfo> all_apps;

    // Scan registry (HKLM + HKCU)
    bool registry_ok = true;
    auto registry_apps = scanRegistry(&registry_ok);
    sak::logDebug("AppScanner: Registry scan found {} apps", registry_apps.size());
    all_apps.insert(all_apps.end(), registry_apps.begin(), registry_apps.end());

    // Scan AppX packages
    bool appx_ok = true;
    auto appx_apps = scanAppX(&appx_ok);
    sak::logDebug("AppScanner: AppX scan found {} apps", appx_apps.size());
    all_apps.insert(all_apps.end(), appx_apps.begin(), appx_apps.end());

    // Scan Chocolatey packages
    bool choco_ok = true;
    auto choco_apps = scanChocolatey(&choco_ok);
    sak::logDebug("AppScanner: Chocolatey scan found {} apps", choco_apps.size());
    all_apps.insert(all_apps.end(), choco_apps.begin(), choco_apps.end());

    if (scanOk != nullptr) {
        *scanOk = registry_ok && appx_ok && choco_ok;
    }
    sak::logDebug("AppScanner: Total apps found: {}", all_apps.size());
    return all_apps;
}

std::vector<AppScanner::AppInfo> AppScanner::scanRegistry(bool* scanOk) {
    // Initialize once here (the aggregate over all hives); scanRegistryHive only
    // ever clears the flag, so a failure in any hive is not undone by a later one.
    if (scanOk != nullptr) {
        *scanOk = true;
    }
    std::vector<AppInfo> apps;

    // Scan HKEY_LOCAL_MACHINE (system-wide apps)
    auto hklm_apps = scanRegistryHive(HKEY_LOCAL_MACHINE,
                                      QString::fromWCharArray(REGISTRY_UNINSTALL_HKLM),
                                      scanOk);
    apps.insert(apps.end(), hklm_apps.begin(), hklm_apps.end());

    // Scan HKEY_LOCAL_MACHINE WOW6432Node (32-bit apps on 64-bit Windows)
    auto wow64_apps = scanRegistryHive(HKEY_LOCAL_MACHINE,
                                       QString::fromWCharArray(REGISTRY_UNINSTALL_WOW64),
                                       scanOk);
    apps.insert(apps.end(), wow64_apps.begin(), wow64_apps.end());

    // Scan HKEY_CURRENT_USER (user-specific apps)
    auto hkcu_apps = scanRegistryHive(HKEY_CURRENT_USER,
                                      QString::fromWCharArray(REGISTRY_UNINSTALL_HKCU),
                                      scanOk);
    apps.insert(apps.end(), hkcu_apps.begin(), hkcu_apps.end());

    return apps;
}

std::vector<AppScanner::AppInfo> AppScanner::scanRegistryHive(void* hive,
                                                              const QString& subkey,
                                                              bool* scanOk) {
    // scanRegistry is the only caller; it passes a predefined HKEY root and one of the
    // REGISTRY_UNINSTALL_* string constants.
    Q_ASSERT(hive);
    Q_ASSERT(!subkey.isEmpty());
    std::vector<AppInfo> apps;

    HKEY hKey;
    LONG const result =
        RegOpenKeyExW(static_cast<HKEY>(hive), subkey.toStdWString().c_str(), 0, KEY_READ, &hKey);

    if (result != ERROR_SUCCESS) {
        sak::logWarning("AppScanner: Failed to open registry key: {}", subkey.toStdString());
        markSourceIncomplete(scanOk);
        return apps;
    }

    enumerateHiveApps(hKey, hive, subkey, apps, scanOk);

    RegCloseKey(hKey);
    return apps;
}

void AppScanner::enumerateHiveApps(
    void* openKey, void* hive, const QString& subkey, std::vector<AppInfo>& apps, bool* scanOk) {
    HKEY const hKey = static_cast<HKEY>(openKey);
    // Enumerate subkeys (each represents an app). Classify the return code
    // explicitly: an ERROR_MORE_DATA (a single over-long subkey name) or a
    // transient error must SKIP that one entry, not silently halt the entire
    // enumeration -- the old "while(... == ERROR_SUCCESS)" stopped on the first
    // non-success and dropped every remaining app.
    DWORD index = 0;
    wchar_t subKeyName[kRegistrySubKeyNameChars];

    while (true) {
        DWORD subKeyNameSize = kRegistrySubKeyNameChars;
        const LONG enumResult = RegEnumKeyExW(
            hKey, index, subKeyName, &subKeyNameSize, nullptr, nullptr, nullptr, nullptr);
        if (enumResult == ERROR_NO_MORE_ITEMS) {
            break;  // enumeration complete
        }
        ++index;
        if (enumResult != ERROR_SUCCESS) {
            // ERROR_MORE_DATA (name > buffer) or a transient error: skip this one
            // subkey and keep going so a single bad entry never truncates the scan.
            sak::logWarning("AppScanner: skipping registry subkey (enum status {})",
                            static_cast<long>(enumResult));
            markSourceIncomplete(scanOk);
            continue;
        }

        // Open this application's registry key
        HKEY appKey;
        if (RegOpenKeyExW(hKey, subKeyName, 0, KEY_READ, &appKey) != ERROR_SUCCESS) {
            markSourceIncomplete(scanOk);
            continue;
        }

        AppInfo app;
        app.source = AppInfo::Source::Registry;
        app.registry_key = hiveDisplayPrefix(static_cast<HKEY>(hive)) + "\\" + subkey + "\\" +
                           QString::fromWCharArray(subKeyName);

        // Read application details
        app.name = readRegistryValue(appKey, "DisplayName");
        app.version = readRegistryValue(appKey, "DisplayVersion");
        app.publisher = readRegistryValue(appKey, "Publisher");
        app.install_date = readRegistryValue(appKey, "InstallDate");
        app.install_location = readRegistryValue(appKey, "InstallLocation");
        app.uninstall_string = readRegistryValue(appKey, "UninstallString");

        // Only add if we have a display name and not a system component
        if (!app.name.isEmpty() && !isSystemComponent(app.name)) {
            apps.push_back(app);
        }

        RegCloseKey(appKey);
    }
}

QString AppScanner::readRegistryValue(void* key,
                                      const QString& valueName) {  // NOLINT - static member
    // scanRegistryHive is the only caller; it passes a key from a RegOpenKeyExW that returned
    // ERROR_SUCCESS and a literal value name.
    Q_ASSERT(key);
    Q_ASSERT(!valueName.isEmpty());
    HKEY hKey = static_cast<HKEY>(key);
    wchar_t buffer[kRegistryValueBufferChars];
    DWORD bufferSize = sizeof(buffer);
    DWORD type;

    LONG const result = RegQueryValueExW(hKey,
                                         valueName.toStdWString().c_str(),
                                         nullptr,
                                         &type,
                                         reinterpret_cast<LPBYTE>(buffer),
                                         &bufferSize);

    if (result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        return registryStringFromBuffer(buffer, bufferSize, kRegistryValueBufferChars);
    }

    return QString();
}

QString AppScanner::registryStringFromBuffer(const wchar_t* data,
                                             unsigned long byteLength,
                                             int capacityChars) {
    if (data == nullptr || byteLength == 0 || capacityChars <= 0) {
        return QString();
    }
    int lengthChars = static_cast<int>(byteLength / sizeof(wchar_t));
    lengthChars = std::min(lengthChars, capacityChars);  // never read past the caller's buffer

    while (lengthChars > 0 && data[lengthChars - 1] == L'\0') {
        --lengthChars;  // drop terminator(s); a full buffer may carry none
    }
    return QString::fromWCharArray(data, lengthChars);
}

bool AppScanner::isSystemComponent(const QString& name) {
    // The only call site short-circuits on !app.name.isEmpty() before calling.
    Q_ASSERT(!name.isEmpty());
    static constexpr const char* kPrefixes[] = {
        "KB",
        "Security Update",
        "Update for",
        "Hotfix",
        "vs_",
        "Microsoft DCF",
        "Microsoft Help",
    };
    for (const auto* prefix : kPrefixes) {
        if (name.startsWith(QLatin1String(prefix))) {
            return true;
        }
    }

    static constexpr const char* kContains[] = {
        "(KB",
        "Redistributable",
        "Microsoft .NET",
        "Windows SDK",
        "Windows Driver Kit",
        "Windows Assessment",
        "Microsoft Visual C++",
    };
    for (const auto* substr : kContains) {
        if (name.contains(QLatin1String(substr))) {
            return true;
        }
    }

    return name.startsWith("Microsoft SQL Server") && !name.contains("Management Studio");
}

QString AppScanner::authoritativeWindowsRoot() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const UINT len = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};  // fail closed: cannot trust an unresolved Windows root
    }
    return QString::fromWCharArray(buffer, static_cast<int>(len));
#else
    return {};
#endif
}

QString AppScanner::composePowerShellPath(const QString& systemRoot) {
    // Fail closed: with no known Windows root we cannot resolve a trusted
    // PowerShell, and invoking a bare "powershell.exe" would let a PATH/CWD-planted
    // binary run in our stead. Callers treat empty as "do not run".
    if (systemRoot.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(systemRoot +
                           QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe"));
}

std::vector<AppScanner::AppInfo> AppScanner::scanAppX(bool* scanOk) {
    if (scanOk != nullptr) {
        *scanOk = true;
    }
    std::vector<AppInfo> apps;

    // Resolve PowerShell under the OS-authoritative Windows directory, never the
    // attacker-influenceable %SystemRoot% env var, so a poisoned env cannot point
    // us at a planted powershell.exe.
    const QString powershell = composePowerShellPath(authoritativeWindowsRoot());
    if (powershell.isEmpty()) {
        sak::logWarning("AppScanner: cannot resolve System32 PowerShell; skipping AppX scan");
        markSourceIncomplete(scanOk);
        return apps;
    }

    const auto result = sak::runProcess(
        powershell,
        {QStringLiteral("-NoProfile"),
         QStringLiteral("-Command"),
         QStringLiteral("Get-AppxPackage | Select-Object Name,Version,Publisher,InstallLocation "
                        "| ConvertTo-Json")},
        sak::kTimeoutProcessLongMs);
    if (!result.succeeded()) {
        sak::logWarning("AppScanner: PowerShell failed/timed out while scanning AppX packages");
        markSourceIncomplete(scanOk);
        return apps;
    }

    const QString output = result.std_out;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        sak::logWarning("AppScanner: Failed to parse AppX JSON {}",
                        error.errorString().toStdString());
        markSourceIncomplete(scanOk);
        return apps;
    }

    QJsonArray packages;
    if (doc.isArray()) {
        packages = doc.array();
    } else if (doc.isObject()) {
        packages.append(doc.object());
    }

    for (const auto& value : packages) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        AppInfo app;
        app.source = AppInfo::Source::AppX;
        app.name = obj.value("Name").toString();
        app.version = obj.value("Version").toString();
        app.publisher = obj.value("Publisher").toString();
        app.install_location = obj.value("InstallLocation").toString();

        if (!app.name.isEmpty()) {
            apps.push_back(app);
        }
    }

    return apps;
}

namespace {
// Parse `choco list --limit-output` rows (name|version|...) into AppInfo entries.
std::vector<AppScanner::AppInfo> parseChocolateyListOutput(const QString& output) {
    std::vector<AppScanner::AppInfo> apps;
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        if (line.trimmed().isEmpty() || line.startsWith("Chocolatey")) {
            continue;
        }
        const QStringList parts = line.split('|');
        if (parts.size() >= kChocolateyListFieldCount) {
            AppScanner::AppInfo app;
            app.source = AppScanner::AppInfo::Source::Chocolatey;
            app.name = parts[0].trimmed();
            app.version = parts[1].trimmed();
            app.publisher = "Chocolatey";
            app.choco_package = app.name;
            app.choco_available = true;
            apps.push_back(app);
        }
    }
    return apps;
}
}  // namespace

std::vector<AppScanner::AppInfo> AppScanner::scanChocolatey(bool* scanOk) {
    if (scanOk != nullptr) {
        *scanOk = true;
    }
    std::vector<AppInfo> apps;

    // Bundled portable choco ONLY. A portable technician tool must never depend on
    // (or require the install of) a Chocolatey on the serviced machine, and must
    // never read the system %ProgramData%\chocolatey store. If the bundled choco is
    // absent the scan simply reports no Chocolatey packages -- it never falls back
    // to a PATH choco.
    const auto& tools = BundledToolsManager::instance();
    if (!tools.toolExists(QStringLiteral("chocolatey"), QStringLiteral("choco.exe"))) {
        // A designed, legitimate "no Chocolatey source" state (e.g. Thin Bundle),
        // NOT a scan failure -- leave scanOk true so the common no-choco machine
        // does not read as a perpetually incomplete inventory.
        sak::logWarning("Bundled Chocolatey not present; skipping Chocolatey package scan");
        return apps;
    }
    const QString choco_path = tools.toolPath(QStringLiteral("chocolatey"),
                                              QStringLiteral("choco.exe"));

    // Fail closed: never launch the bundled choco.exe for inventory unless it is a
    // genuine, Chocolatey-signed binary. Mirrors ChocolateyManager::executeChoco so
    // a tampered bundled choco cannot run on the scan path either.
    if (!ChocolateyManager::isAuthenticChocoBinary(choco_path)) {
        sak::logWarning(
            "Bundled choco.exe failed authenticity verification; skipping Chocolatey "
            "package scan");
        markSourceIncomplete(scanOk);
        return apps;
    }

    // Pin ChocolateyInstall to the bundled portable root so `choco list` reads OUR
    // store, not the system one (matches OfflineDeploymentWorker/ChocolateyManager).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ChocolateyInstall"), QFileInfo(choco_path).absolutePath());

    const auto result = sak::runProcessWithEnvironment(
        choco_path,
        {QStringLiteral("list"), QStringLiteral("--local-only"), QStringLiteral("--limit-output")},
        sak::kTimeoutProcessMediumMs,
        env);
    if (!result.succeeded()) {
        sak::logWarning(
            "Chocolatey package scan failed/timed out after 10s -- choco may not be installed "
            "or is unresponsive");
        markSourceIncomplete(scanOk);
        return apps;
    }

    return parseChocolateyListOutput(result.std_out);
}

}  // namespace sak
