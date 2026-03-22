// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file leftover_scanner.cpp
/// @brief Multi-level leftover scanning for orphaned files, folders, and
///        registry entries after program uninstallation

#include "sak/leftover_scanner.h"

#include "sak/layout_constants.h"
#include "sak/registry_snapshot_engine.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace sak {

namespace {
constexpr int kPowerShellTimeoutMs = 15'000;
constexpr int kMinConcatNameLen = 4;

#ifdef Q_OS_WIN
constexpr DWORD kMaxRegistryValueNameLen = 256;
constexpr DWORD kMaxRegistryValueDataLen = 1024;
#endif

const QSet<QString> kGenericDirNames = {
    "bin",
    "app",
    "application",
    "program",
    "data",
    "files",
    "lib",
    "x64",
    "x86",
    "release",
    "debug",
    "build",
    "common",
    "shared",
    "cache",
    "logs",
    "config",
};

template <typename ReportFn>
void appendAndReportItems(QVector<LeftoverItem>& out,
                          const QVector<LeftoverItem>& items,
                          ReportFn report) {
    for (const auto& item : items) {
        report(item.path);
    }
    out.append(items);
}

void appendEnvDir(QStringList& dirs, const char* env_var) {
    QString value = QDir::toNativeSeparators(qEnvironmentVariable(env_var));
    if (!value.isEmpty()) {
        dirs.append(value);
    }
}

void appendStandardDir(QStringList& dirs, QStandardPaths::StandardLocation loc) {
    QString path = QStandardPaths::writableLocation(loc);
    if (!path.isEmpty()) {
        dirs.append(path);
    }
}

bool isSignificantParentDir(const QString& parent) {
    return !parent.isEmpty() && !kGenericDirNames.contains(parent) && parent != "program files" &&
           parent != "program files (x86)" && parent != "programdata";
}

}  // namespace

// Protected system paths that should NEVER be auto-selected for deletion
const QStringList LeftoverScanner::kProtectedPaths = {
    "C:\\Windows",
    "C:\\Windows\\System32",
    "C:\\Windows\\SysWOW64",
    "C:\\Windows\\WinSxS",
    "C:\\Program Files\\Common Files\\Microsoft Shared",
    "C:\\Program Files\\Windows",
    "C:\\ProgramData\\Microsoft",
    "C:\\Users\\Default",
    "C:\\Users\\Public",
    "HKLM\\SOFTWARE\\Microsoft\\Windows",
    "HKLM\\SOFTWARE\\Microsoft\\Windows NT",
    "HKLM\\SYSTEM\\CurrentControlSet\\Control",
    "HKLM\\SYSTEM\\CurrentControlSet\\Enum",
    "HKCR\\CLSID\\{00000000-",
};

// ======================================================================
// Construction
// ======================================================================

LeftoverScanner::LeftoverScanner(const ProgramInfo& program,
                                 ScanLevel level,
                                 const QSet<QString>& registryBefore)
    : m_program(program), m_level(level), m_registryBefore(registryBefore) {
    buildExactNames();
}

void LeftoverScanner::extractInstallDirNames() {
    if (m_program.installLocation.isEmpty()) {
        return;
    }
    QDir install_dir(m_program.installLocation);
    QString dir_name = install_dir.dirName().toLower();
    if (!dir_name.isEmpty() && !kGenericDirNames.contains(dir_name)) {
        m_installDirName = dir_name;
        if (!m_exactNames.contains(dir_name)) {
            m_exactNames.append(dir_name);
        }
    }

    if (install_dir.cdUp()) {
        QString parent = install_dir.dirName().toLower();
        if (isSignificantParentDir(parent)) {
            m_installParentName = parent;
        }
    }
}

void LeftoverScanner::buildExactNames() {
    if (m_program.displayName.isEmpty()) {
        return;
    }

    m_exactNames.append(m_program.displayName.toLower());
    extractInstallDirNames();

    if (!m_program.packageFamilyName.isEmpty()) {
        QString pkg = m_program.packageFamilyName.toLower();
        if (!m_exactNames.contains(pkg)) {
            m_exactNames.append(pkg);
        }
    }

    QString concat = m_program.displayName;
    concat.remove(QRegularExpression("[\\s\\-_]+"));
    if (concat.length() >= kMinConcatNameLen) {
        m_concatenatedName = concat.toLower();
    }

    m_exactNames.removeDuplicates();
}

// ======================================================================
// Scan Orchestrator
// ======================================================================

QVector<LeftoverItem> LeftoverScanner::scan(
    const std::atomic<bool>& stopRequested,
    std::function<void(const QString&, int)> progressCallback) {
    QVector<LeftoverItem> all_items;
    int found_count = 0;

    auto report = [&](const QString& path) {
        ++found_count;
        if (progressCallback) {
            progressCallback(path, found_count);
        }
    };

    auto runPhase = [&](auto scanner_fn) {
        if (!stopRequested.load()) {
            appendAndReportItems(all_items, scanner_fn(), report);
        }
    };

    // Phase 1: Known install location
    runPhase([&] { return scanInstallLocation(stopRequested); });

    // Phase 2: Standard application directories
    runPhase([&] { return scanKnownPaths(stopRequested); });

    // Phase 3: Registry (snapshot diff + known paths)
    if (m_level == ScanLevel::Moderate || m_level == ScanLevel::Advanced) {
#ifdef Q_OS_WIN
        runPhase([&] { return scanRegistryDiff(stopRequested); });
        runPhase([&] { return scanKnownRegistryPaths(stopRequested); });
#endif
    }

    // Phase 4: System objects (Advanced only)
    if (m_level == ScanLevel::Advanced) {
        runPhase([&] { return scanServices(stopRequested); });
        runPhase([&] { return scanScheduledTasks(stopRequested); });
        runPhase([&] { return scanFirewallRules(stopRequested); });
        runPhase([&] { return scanStartupEntries(stopRequested); });
    }

    // Deduplicate by path and pre-select safe items
    QSet<QString> seen;
    QVector<LeftoverItem> result;
    result.reserve(all_items.size());
    for (auto& item : all_items) {
        if (seen.contains(item.path)) {
            continue;
        }
        seen.insert(item.path);
        item.selected = (item.risk == LeftoverItem::RiskLevel::Safe);
        result.append(std::move(item));
    }

    return result;
}

// ======================================================================
// Phase 1: Install Location Scan
// ======================================================================

QVector<LeftoverItem> LeftoverScanner::scanInstallLocation(const std::atomic<bool>& stopRequested) {
    if (m_program.installLocation.isEmpty() || stopRequested.load()) {
        return {};
    }

    QDir install_dir(m_program.installLocation);
    if (!install_dir.exists()) {
        return {};
    }

    LeftoverItem item;
    item.type = LeftoverItem::Type::Folder;
    item.path = QDir::toNativeSeparators(install_dir.absolutePath());
    item.description = "Program install directory still exists";
    item.sizeBytes = calculateSize(install_dir.absolutePath());
    item.risk = LeftoverItem::RiskLevel::Safe;

    return {item};
}

// ======================================================================
// Phase 2: Known Application Paths
// ======================================================================

QVector<LeftoverItem> LeftoverScanner::scanKnownPaths(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    QStringList base_dirs;
    appendEnvDir(base_dirs, "APPDATA");
    appendEnvDir(base_dirs, "LOCALAPPDATA");
    appendEnvDir(base_dirs, "ProgramData");

    if (m_level == ScanLevel::Moderate || m_level == ScanLevel::Advanced) {
        appendEnvDir(base_dirs, "ProgramFiles");
        appendEnvDir(base_dirs, "ProgramFiles(x86)");
        appendEnvDir(base_dirs, "TEMP");
    }

    if (m_level == ScanLevel::Advanced) {
        appendEnvDir(base_dirs, "CommonProgramFiles");
        appendStandardDir(base_dirs, QStandardPaths::DesktopLocation);
        appendStandardDir(base_dirs, QStandardPaths::ApplicationsLocation);
    }

    for (const auto& base : base_dirs) {
        if (stopRequested.load()) {
            break;
        }
        items.append(scanStandardDirs(base, stopRequested));
        items.append(scanStandardFiles(base, stopRequested));
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanStandardDirs(const QString& basePath,
                                                        const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;
    QDir base(basePath);
    if (!base.exists()) {
        return items;
    }

    const auto entries = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (stopRequested.load()) {
            break;
        }

        const QString dir_lower = entry.fileName().toLower();

        // Direct match: directory name matches program or install dir
        if (matchesProgramExact(dir_lower)) {
            LeftoverItem item;
            item.type = LeftoverItem::Type::Folder;
            item.path = QDir::toNativeSeparators(entry.absoluteFilePath());
            item.description = QString("Leftover folder in %1").arg(basePath);
            item.sizeBytes = calculateSize(entry.absoluteFilePath());
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
            continue;
        }

        // Check publisher\product directory structure
        if (!isPublisherDir(dir_lower)) {
            continue;
        }
        QDir pub_dir(entry.absoluteFilePath());
        const auto sub_entries = pub_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& sub : sub_entries) {
            if (stopRequested.load()) {
                break;
            }
            if (!matchesProgramExact(sub.fileName().toLower())) {
                continue;
            }
            LeftoverItem item;
            item.type = LeftoverItem::Type::Folder;
            item.path = QDir::toNativeSeparators(sub.absoluteFilePath());
            item.description = "Leftover folder under publisher directory";
            item.sizeBytes = calculateSize(sub.absoluteFilePath());
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
        }
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanStandardFiles(const QString& basePath,
                                                         const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;
    QDir base(basePath);
    if (!base.exists()) {
        return items;
    }

    const auto files = base.entryInfoList(QDir::Files);
    for (const auto& file : files) {
        if (stopRequested.load()) {
            break;
        }
        if (!matchesProgramExact(file.completeBaseName().toLower())) {
            continue;
        }
        LeftoverItem item;
        item.type = LeftoverItem::Type::File;
        item.path = QDir::toNativeSeparators(file.absoluteFilePath());
        item.description = QString("Leftover file in %1").arg(basePath);
        item.sizeBytes = file.size();
        item.risk = classifyRisk(item.path, item.type);
        items.append(item);
    }

    return items;
}

// ======================================================================
// Phase 3: Registry
// ======================================================================

#ifdef Q_OS_WIN

QVector<LeftoverItem> LeftoverScanner::scanRegistryDiff(const std::atomic<bool>& stopRequested) {
    if (m_registryBefore.isEmpty() || stopRequested.load()) {
        return {};
    }

    auto registry_after = RegistrySnapshotEngine::captureSnapshot();
    QVector<LeftoverItem> items;

    for (const auto& key : registry_after) {
        if (stopRequested.load()) {
            break;
        }
        // Only report keys that existed before uninstall (survived)
        if (!m_registryBefore.contains(key)) {
            continue;
        }
        if (!registryKeyMatchesProgram(key)) {
            continue;
        }
        LeftoverItem item;
        item.type = LeftoverItem::Type::RegistryKey;
        item.path = key;
        item.description = "Registry key survived uninstallation";
        item.risk = classifyRisk(key, LeftoverItem::Type::RegistryKey);
        items.append(item);
    }

    return items;
}

bool LeftoverScanner::registryKeyMatchesProgram(const QString& keyPath) const {
    const QString key_lower = keyPath.toLower();

    // Skip Windows system keys
    if (key_lower.contains("\\microsoft\\windows\\") ||
        key_lower.contains("\\microsoft\\windows nt\\") ||
        key_lower.contains("\\currentcontrolset\\control\\") ||
        key_lower.contains("\\currentcontrolset\\enum\\")) {
        return false;
    }

    static const QSet<QString> kRootComponents = {
        "hklm",
        "hkcu",
        "software",
        "wow6432node",
        "system",
        "currentcontrolset",
        "services",
    };

    const QStringList components = keyPath.split('\\', Qt::SkipEmptyParts);
    for (const auto& component : components) {
        const QString comp_lower = component.toLower();
        if (kRootComponents.contains(comp_lower)) {
            continue;
        }
        if (matchesProgramExact(comp_lower)) {
            return true;
        }
    }

    return false;
}

QVector<LeftoverItem> LeftoverScanner::scanKnownRegistryPaths(
    const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;
    if (stopRequested.load()) {
        return items;
    }

    auto checkKey =
        [&](HKEY hive, const QString& subkey, const QString& hiveName, const QString& description) {
            HKEY key = nullptr;
            LONG rc =
                RegOpenKeyExW(hive, reinterpret_cast<LPCWSTR>(subkey.utf16()), 0, KEY_READ, &key);
            if (rc != ERROR_SUCCESS) {
                return;
            }
            RegCloseKey(key);
            LeftoverItem item;
            item.type = LeftoverItem::Type::RegistryKey;
            item.path = hiveName + "\\" + subkey;
            item.description = description;
            item.risk = LeftoverItem::RiskLevel::Safe;
            items.append(item);
        };

    // Check the program's own uninstall registry key
    if (!m_program.registryKeyPath.isEmpty()) {
        if (m_program.registryKeyPath.startsWith("HKLM\\")) {
            checkKey(HKEY_LOCAL_MACHINE,
                     m_program.registryKeyPath.mid(5),
                     "HKLM",
                     "Program uninstall key still exists");
        } else if (m_program.registryKeyPath.startsWith("HKCU\\")) {
            checkKey(HKEY_CURRENT_USER,
                     m_program.registryKeyPath.mid(5),
                     "HKCU",
                     "Program uninstall key still exists");
        }
    }

    // Check for product keys under standard hives
    if (!m_installDirName.isEmpty()) {
        QString dir_proper = QDir(m_program.installLocation).dirName();
        checkKey(
            HKEY_CURRENT_USER, "Software\\" + dir_proper, "HKCU", "Leftover product registry key");
        checkKey(
            HKEY_LOCAL_MACHINE, "SOFTWARE\\" + dir_proper, "HKLM", "Leftover product registry key");
    }

    return items;
}

#endif  // Q_OS_WIN

QVector<LeftoverItem> LeftoverScanner::scanServices(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    QProcess proc;
    proc.setProgram("sc.exe");
    proc.setArguments({"query", "type=", "service", "state=", "all"});
    proc.start();

    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs) ||
        !proc.waitForFinished(kPowerShellTimeoutMs)) {
        return items;
    }

    QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QStringList lines = output.split('\n');

    QString current_service;
    QString current_display;

    for (const auto& line : lines) {
        if (stopRequested.load()) {
            break;
        }

        QString trimmed = line.trimmed();

        if (trimmed.startsWith("SERVICE_NAME:")) {
            current_service = trimmed.mid(14).trimmed();
        } else if (trimmed.startsWith("DISPLAY_NAME:")) {
            current_display = trimmed.mid(14).trimmed();

            if (matchesProgramStrict(current_service) || matchesProgramStrict(current_display)) {
                LeftoverItem item;
                item.type = LeftoverItem::Type::Service;
                item.path = current_service;
                item.description = QString("Windows service: %1").arg(current_display);
                item.risk = LeftoverItem::RiskLevel::Risky;
                items.append(item);
            }
        }
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanScheduledTasks(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    QProcess proc;
    proc.setProgram("schtasks.exe");
    proc.setArguments({"/query", "/fo", "CSV", "/nh"});
    proc.start();

    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs) ||
        !proc.waitForFinished(kPowerShellTimeoutMs)) {
        return items;
    }

    QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QStringList lines = output.split('\n');

    for (const auto& line : lines) {
        if (stopRequested.load()) {
            break;
        }

        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        // CSV format: "TaskName","Next Run Time","Status"
        QStringList fields = trimmed.split(',');
        if (fields.isEmpty()) {
            continue;
        }

        QString task_name = fields[0];
        task_name.remove('"');

        if (matchesProgramStrict(task_name)) {
            LeftoverItem item;
            item.type = LeftoverItem::Type::ScheduledTask;
            item.path = task_name;
            item.description = "Scheduled task";
            item.risk = LeftoverItem::RiskLevel::Review;
            items.append(item);
        }
    }

    return items;
}

void LeftoverScanner::scanFirewallDirection(const QStringList& netsh_args,
                                            const QString& description,
                                            const std::atomic<bool>& stopRequested,
                                            QVector<LeftoverItem>& items) {
    QProcess proc;
    proc.setProgram("netsh.exe");
    proc.setArguments(netsh_args);
    proc.start();

    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs) ||
        !proc.waitForFinished(kPowerShellTimeoutMs)) {
        return;
    }

    const QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
    const QStringList lines = output.split('\n');

    for (const auto& line : lines) {
        if (stopRequested.load()) {
            break;
        }
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith("Rule Name:", Qt::CaseInsensitive)) {
            continue;
        }
        const QString rule_name = trimmed.mid(10).trimmed();
        if (!matchesProgramStrict(rule_name)) {
            continue;
        }
        LeftoverItem item;
        item.type = LeftoverItem::Type::FirewallRule;
        item.path = rule_name;
        item.description = description;
        item.risk = LeftoverItem::RiskLevel::Review;
        items.append(item);
    }
}

QVector<LeftoverItem> LeftoverScanner::scanFirewallRules(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    scanFirewallDirection({"advfirewall", "firewall", "show", "rule", "name=all", "dir=in"},
                          "Windows Firewall rule",
                          stopRequested,
                          items);

    if (!stopRequested.load()) {
        scanFirewallDirection({"advfirewall", "firewall", "show", "rule", "name=all", "dir=out"},
                              "Windows Firewall rule (outbound)",
                              stopRequested,
                              items);
    }

    return items;
}

#ifdef Q_OS_WIN
void LeftoverScanner::scanRunKey(HKEY hive,
                                 const wchar_t* subkey,
                                 const QString& hive_name,
                                 const std::atomic<bool>& stopRequested,
                                 QVector<LeftoverItem>& items) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(hive, subkey, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        return;
    }

    DWORD value_count = 0;
    RegQueryInfoKeyW(key,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     &value_count,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr);

    wchar_t value_name[kMaxRegistryValueNameLen];
    BYTE value_data[kMaxRegistryValueDataLen];

    for (DWORD idx = 0; idx < value_count; ++idx) {
        if (stopRequested.load()) {
            break;
        }

        DWORD name_len = kMaxRegistryValueNameLen;
        DWORD data_len = kMaxRegistryValueDataLen;
        DWORD type = 0;

        rc = RegEnumValueW(key, idx, value_name, &name_len, nullptr, &type, value_data, &data_len);
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        QString name = QString::fromWCharArray(value_name, name_len);
        QString data;
        if ((type == REG_SZ || type == REG_EXPAND_SZ) && data_len >= sizeof(wchar_t)) {
            data = QString::fromWCharArray(reinterpret_cast<wchar_t*>(value_data),
                                           data_len / sizeof(wchar_t) - 1);
        }

        if (!matchesProgramStrict(name) && !matchesProgramStrict(data)) {
            continue;
        }

        LeftoverItem item;
        item.type = LeftoverItem::Type::StartupEntry;
        item.path = QString("%1\\%2").arg(hive_name, QString::fromWCharArray(subkey));
        item.registryValueName = name;
        item.registryValueData = data;
        item.description = QString("Startup entry: %1").arg(name);
        item.risk = LeftoverItem::RiskLevel::Review;
        items.append(item);
    }

    RegCloseKey(key);
}
#endif

QVector<LeftoverItem> LeftoverScanner::scanStartupEntries(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

#ifdef Q_OS_WIN
    scanRunKey(HKEY_CURRENT_USER,
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
               "HKCU",
               stopRequested,
               items);
    scanRunKey(HKEY_CURRENT_USER,
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
               "HKCU",
               stopRequested,
               items);

    if (!stopRequested.load()) {
        scanRunKey(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                   "HKLM",
                   stopRequested,
                   items);
        scanRunKey(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                   "HKLM",
                   stopRequested,
                   items);
    }
#endif

    if (!stopRequested.load()) {
        scanStartupFolder(stopRequested, items);
    }

    return items;
}

void LeftoverScanner::scanStartupFolder(const std::atomic<bool>& stopRequested,
                                        QVector<LeftoverItem>& items) {
    const QString startup_folder =
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + "/Startup";
    if (!QDir(startup_folder).exists()) {
        return;
    }
    QDir dir(startup_folder);
    const auto files = dir.entryInfoList(QDir::Files);
    for (const auto& file : files) {
        if (stopRequested.load()) {
            break;
        }
        if (!matchesProgramExact(file.completeBaseName().toLower())) {
            continue;
        }
        LeftoverItem item;
        item.type = LeftoverItem::Type::StartupEntry;
        item.path = QDir::toNativeSeparators(file.absoluteFilePath());
        item.description = "Startup shortcut";
        item.sizeBytes = file.size();
        item.risk = LeftoverItem::RiskLevel::Safe;
        items.append(item);
    }
}

// ======================================================================
// Classification and Matching
// ======================================================================

LeftoverItem::RiskLevel LeftoverScanner::classifyFileRisk(const QString& path_lower) const {
    if (!m_program.installLocation.isEmpty() &&
        path_lower.startsWith(m_program.installLocation.toLower())) {
        return LeftoverItem::RiskLevel::Safe;
    }
    if (path_lower.contains("appdata") || path_lower.contains("programdata") ||
        path_lower.contains("program files") || path_lower.contains("\\temp\\")) {
        return LeftoverItem::RiskLevel::Safe;
    }
    return LeftoverItem::RiskLevel::Review;
}

LeftoverItem::RiskLevel LeftoverScanner::classifyTypeRisk(LeftoverItem::Type type) const {
    switch (type) {
    case LeftoverItem::Type::File:
    case LeftoverItem::Type::Folder:
        return LeftoverItem::RiskLevel::Review;

    case LeftoverItem::Type::RegistryKey:
    case LeftoverItem::Type::RegistryValue:
        return LeftoverItem::RiskLevel::Safe;

    case LeftoverItem::Type::Service:
        return LeftoverItem::RiskLevel::Risky;

    case LeftoverItem::Type::ScheduledTask:
    case LeftoverItem::Type::FirewallRule:
    case LeftoverItem::Type::StartupEntry:
    case LeftoverItem::Type::ShellExtension:
        return LeftoverItem::RiskLevel::Review;
    }

    return LeftoverItem::RiskLevel::Review;
}

LeftoverItem::RiskLevel LeftoverScanner::classifyRisk(const QString& path,
                                                      LeftoverItem::Type type) const {
    if (isProtectedPath(path)) {
        return LeftoverItem::RiskLevel::Risky;
    }

    if (type == LeftoverItem::Type::File || type == LeftoverItem::Type::Folder) {
        return classifyFileRisk(path.toLower());
    }

    return classifyTypeRisk(type);
}

bool LeftoverScanner::isProtectedPath(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    const QString path_native = QDir::toNativeSeparators(path);
    const QString install_native = m_program.installLocation.isEmpty()
                                       ? QString{}
                                       : QDir::toNativeSeparators(m_program.installLocation);

    for (const auto& protected_path : kProtectedPaths) {
        if (!path_native.startsWith(protected_path, Qt::CaseInsensitive)) {
            continue;
        }
        const int len = protected_path.length();
        const bool direct = (path_native.length() == len);
        const bool child = (path_native.length() > len && path_native[len] == '\\');
        if (!direct && !child) {
            continue;
        }
        if (!install_native.isEmpty() &&
            path_native.startsWith(install_native, Qt::CaseInsensitive)) {
            return false;
        }
        return true;
    }
    return false;
}

bool LeftoverScanner::matchesProgramExact(const QString& nameLower) const {
    if (nameLower.isEmpty()) {
        return false;
    }
    for (const auto& exact : m_exactNames) {
        if (nameLower == exact) {
            return true;
        }
    }
    return false;
}

bool LeftoverScanner::matchesProgramStrict(const QString& text) const {
    if (text.isEmpty()) {
        return false;
    }
    const QString text_lower = text.toLower();

    if (!m_program.displayName.isEmpty() && text_lower.contains(m_program.displayName.toLower())) {
        return true;
    }
    if (!m_concatenatedName.isEmpty() && text_lower.contains(m_concatenatedName)) {
        return true;
    }
    if (!m_installDirName.isEmpty() && text_lower.contains(m_installDirName)) {
        return true;
    }
    return false;
}

bool LeftoverScanner::isPublisherDir(const QString& dirNameLower) const {
    if (!m_program.publisher.isEmpty() && dirNameLower == m_program.publisher.toLower()) {
        return true;
    }
    if (!m_installParentName.isEmpty() && dirNameLower == m_installParentName) {
        return true;
    }
    return false;
}

qint64 LeftoverScanner::calculateSize(const QString& path) {
    QFileInfo info(path);
    if (info.isFile()) {
        return info.size();
    }

    qint64 total = 0;
    QDirIterator it(path,
                    QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

}  // namespace sak
