// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file leftover_scanner.cpp
/// @brief Multi-level leftover scanning for orphaned files, folders, and
///        registry entries after program uninstallation

#include "sak/leftover_scanner.h"
#include "sak/registry_snapshot_engine.h"

#include <QRegularExpression>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace sak {

namespace {
constexpr int kPowerShellTimeoutMs       = 15000;
constexpr DWORD kMaxRegistryKeyNameLen   = 256;
constexpr DWORD kMaxRegistryValueNameLen = 256;
constexpr DWORD kMaxRegistryValueDataLen = 1024;
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

LeftoverScanner::LeftoverScanner(const ProgramInfo& program, ScanLevel level)
    : m_program(program)
    , m_level(level)
{
    buildSearchPatterns();
}

void LeftoverScanner::buildSearchPatterns()
{
    // Common words to exclude from pattern matching to reduce false positives
    static const QSet<QString> kExcludedWords = {
        "the", "for", "and", "pro", "app", "new", "all", "one", "free",
        "media", "player", "viewer", "editor", "manager", "service",
        "system", "update", "setup", "install", "windows", "microsoft",
        "tools", "tool", "software", "portable", "lite", "plus", "studio",
        "home", "server", "client", "web", "desktop", "data", "file",
    };

    // Build name patterns from display name
    if (!m_program.displayName.isEmpty()) {
        m_namePatterns.append(m_program.displayName.toLower());

        // Split on spaces and add individual words (>= 3 chars)
        // Skip common English words that cause too many false matches
        const QStringList words = m_program.displayName.split(
            QRegularExpression("[\\s\\-_]+"), Qt::SkipEmptyParts);
        for (const auto& word : words) {
            if (word.length() >= 3
                && !kExcludedWords.contains(word.toLower())) {
                m_namePatterns.append(word.toLower());
            }
        }

        // Add concatenated name (no spaces)
        QString concat = m_program.displayName;
        concat.remove(QRegularExpression("[\\s\\-_]+"));
        if (concat.length() >= 3) {
            m_namePatterns.append(concat.toLower());
        }
    }

    // Build publisher patterns
    if (!m_program.publisher.isEmpty()) {
        m_publisherPatterns.append(m_program.publisher.toLower());

        // Split publisher name
        const QStringList words = m_program.publisher.split(
            QRegularExpression("[\\s\\-_,\\.]+"), Qt::SkipEmptyParts);
        for (const auto& word : words) {
            if (word.length() >= 3
                && word.toLower() != "inc" && word.toLower() != "ltd"
                && word.toLower() != "llc" && word.toLower() != "corp"
                && word.toLower() != "gmbh" && word.toLower() != "the") {
                m_publisherPatterns.append(word.toLower());
            }
        }
    }

    // Extract install directory name
    if (!m_program.installLocation.isEmpty()) {
        QDir dir(m_program.installLocation);
        m_installDirName = dir.dirName().toLower();
        if (!m_installDirName.isEmpty()) {
            m_namePatterns.append(m_installDirName);
        }
    }

    // Remove duplicates
    m_namePatterns.removeDuplicates();
    m_publisherPatterns.removeDuplicates();
}

QVector<LeftoverItem> LeftoverScanner::scan(
    const std::atomic<bool>& stopRequested,
    std::function<void(const QString&, int)> progressCallback)
{
    QVector<LeftoverItem> all_leftovers;
    int found_count = 0;

    auto report = [&](const QString& path) {
        ++found_count;
        if (progressCallback) {
            progressCallback(path, found_count);
        }
    };

    // Phase 1: File system scanning (all levels)
    if (!stopRequested.load()) {
        auto fs_items = scanFileSystem(stopRequested);
        for (const auto& item : fs_items) {
            report(item.path);
        }
        all_leftovers.append(fs_items);
    }

    // Phase 2: Registry scanning (Moderate and Advanced)
    if (!stopRequested.load()
        && (m_level == ScanLevel::Moderate || m_level == ScanLevel::Advanced)) {
#ifdef Q_OS_WIN
        auto reg_items = scanRegistry(stopRequested);
        for (const auto& item : reg_items) {
            report(item.path);
        }
        all_leftovers.append(reg_items);
#endif
    }

    // Phase 3: Registry snapshot diff is handled by UninstallWorker, which owns
    // the snapshot lifecycle. The scanner only does pattern-based registry search
    // in Phase 2 above. This avoids duplicate snapshot captures and diffs.

    // Phase 4: System objects (Advanced only)
    if (!stopRequested.load() && m_level == ScanLevel::Advanced) {
        auto service_items = scanServices(stopRequested);
        for (const auto& item : service_items) {
            report(item.path);
        }
        all_leftovers.append(service_items);

        if (!stopRequested.load()) {
            auto task_items = scanScheduledTasks(stopRequested);
            for (const auto& item : task_items) {
                report(item.path);
            }
            all_leftovers.append(task_items);
        }

        if (!stopRequested.load()) {
            auto fw_items = scanFirewallRules(stopRequested);
            for (const auto& item : fw_items) {
                report(item.path);
            }
            all_leftovers.append(fw_items);
        }

        if (!stopRequested.load()) {
            auto startup_items = scanStartupEntries(stopRequested);
            for (const auto& item : startup_items) {
                report(item.path);
            }
            all_leftovers.append(startup_items);
        }
    }

    // Pre-select safe items
    for (auto& item : all_leftovers) {
        item.selected = (item.risk == LeftoverItem::RiskLevel::Safe);
    }

    return all_leftovers;
}

QVector<LeftoverItem> LeftoverScanner::scanFileSystem(
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

    // Common scan locations
    QStringList scan_dirs;

    // Program Files directories (all levels)
    QString program_files = QDir::toNativeSeparators(
        qEnvironmentVariable("ProgramFiles"));
    QString program_files_x86 = QDir::toNativeSeparators(
        qEnvironmentVariable("ProgramFiles(x86)"));

    if (!program_files.isEmpty()) scan_dirs.append(program_files);
    if (!program_files_x86.isEmpty()) scan_dirs.append(program_files_x86);

    // AppData directories (all levels)
    // AppData paths from environment variables (more reliable than QStandardPaths)
    QString roaming = QDir::toNativeSeparators(
        qEnvironmentVariable("APPDATA"));
    QString local = QDir::toNativeSeparators(
        qEnvironmentVariable("LOCALAPPDATA"));
    QString program_data = QDir::toNativeSeparators(
        qEnvironmentVariable("ProgramData"));

    if (!roaming.isEmpty()) scan_dirs.append(roaming);
    if (!local.isEmpty()) scan_dirs.append(local);
    if (!program_data.isEmpty()) scan_dirs.append(program_data);

    // Temp directories (all levels)
    QString temp = QDir::toNativeSeparators(qEnvironmentVariable("TEMP"));
    if (!temp.isEmpty()) scan_dirs.append(temp);

    // Start Menu and Desktop (all levels)
    QString start_menu = QStandardPaths::writableLocation(
        QStandardPaths::ApplicationsLocation);
    QString desktop = QStandardPaths::writableLocation(
        QStandardPaths::DesktopLocation);
    if (!start_menu.isEmpty()) scan_dirs.append(start_menu);
    if (!desktop.isEmpty()) scan_dirs.append(desktop);

    // Common Files (Advanced only)
    if (m_level == ScanLevel::Advanced) {
        QString common = QDir::toNativeSeparators(
            qEnvironmentVariable("CommonProgramFiles"));
        if (!common.isEmpty()) scan_dirs.append(common);
    }

    // Scan each directory
    for (const auto& dir : scan_dirs) {
        if (stopRequested.load()) break;

        auto dir_items = scanDirectory(dir, stopRequested);
        items.append(dir_items);
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanDirectory(
    const QString& basePath,
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

    QDir base(basePath);
    if (!base.exists()) {
        return items;
    }

    // Scan top-level directories for matching names
    const auto entries = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (stopRequested.load()) break;

        QString dir_name = entry.fileName();
        if (matchesProgram(dir_name)) {
            LeftoverItem item;
            item.type = LeftoverItem::Type::Folder;
            item.path = QDir::toNativeSeparators(entry.absoluteFilePath());
            item.description = QString("Leftover folder in %1").arg(basePath);
            item.sizeBytes = calculateSize(entry.absoluteFilePath());
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
        }
    }

    // Also check for matching files (shortcuts, configs) in the directory
    const auto files = base.entryInfoList(QDir::Files);
    for (const auto& file : files) {
        if (stopRequested.load()) break;

        QString file_name = file.fileName();
        if (matchesProgram(file_name)) {
            LeftoverItem item;
            item.type = LeftoverItem::Type::File;
            item.path = QDir::toNativeSeparators(file.absoluteFilePath());
            item.description = QString("Leftover file in %1").arg(basePath);
            item.sizeBytes = file.size();
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
        }
    }

    return items;
}

#ifdef Q_OS_WIN

QVector<LeftoverItem> LeftoverScanner::scanRegistry(
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

    // Scan HKCU\Software for matching keys
    auto hkcu = scanRegistryHive(HKEY_CURRENT_USER, "Software",
                                  "HKCU", stopRequested);
    items.append(hkcu);

    if (stopRequested.load()) return items;

    // Scan HKLM\SOFTWARE for matching keys
    auto hklm = scanRegistryHive(HKEY_LOCAL_MACHINE, "SOFTWARE",
                                  "HKLM", stopRequested);
    items.append(hklm);

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanRegistryHive(
    HKEY hive, const QString& subkey, const QString& hiveName,
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(hive,
                            reinterpret_cast<LPCWSTR>(subkey.utf16()),
                            0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        return items;
    }

    DWORD subkey_count = 0;
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkey_count,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    wchar_t subkey_name[kMaxRegistryKeyNameLen];
    for (DWORD i = 0; i < subkey_count; ++i) {
        if (stopRequested.load()) break;

        DWORD name_len = kMaxRegistryKeyNameLen;
        rc = RegEnumKeyExW(key, i, subkey_name, &name_len,
                           nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        QString child_name = QString::fromWCharArray(subkey_name, name_len);

        // Skip Microsoft's own keys
        if (child_name.startsWith("Microsoft", Qt::CaseInsensitive)
            || child_name.startsWith("Windows", Qt::CaseInsensitive)
            || child_name.startsWith("Classes", Qt::CaseInsensitive)) {
            continue;
        }

        if (matchesProgram(child_name)) {
            LeftoverItem item;
            item.type = LeftoverItem::Type::RegistryKey;
            item.path = QString("%1\\%2\\%3")
                .arg(hiveName, subkey, child_name);
            item.description = "Leftover registry key";
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
        }
    }

    RegCloseKey(key);
    return items;
}

#endif // Q_OS_WIN

QVector<LeftoverItem> LeftoverScanner::scanServices(
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

    QProcess proc;
    proc.setProgram("sc.exe");
    proc.setArguments({"query", "type=", "service", "state=", "all"});
    proc.start();

    if (!proc.waitForFinished(kPowerShellTimeoutMs)) {
        return items;
    }

    QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QStringList lines = output.split('\n');

    QString current_service;
    QString current_display;

    for (const auto& line : lines) {
        if (stopRequested.load()) break;

        QString trimmed = line.trimmed();

        if (trimmed.startsWith("SERVICE_NAME:")) {
            current_service = trimmed.mid(14).trimmed();
        } else if (trimmed.startsWith("DISPLAY_NAME:")) {
            current_display = trimmed.mid(14).trimmed();

            // Check if service matches our program
            if (matchesProgram(current_service)
                || matchesProgram(current_display)) {
                LeftoverItem item;
                item.type = LeftoverItem::Type::Service;
                item.path = current_service;
                item.description = QString("Windows service: %1")
                    .arg(current_display);
                item.risk = LeftoverItem::RiskLevel::Risky;
                items.append(item);
            }
        }
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanScheduledTasks(
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

    QProcess proc;
    proc.setProgram("schtasks.exe");
    proc.setArguments({"/query", "/fo", "CSV", "/nh"});
    proc.start();

    if (!proc.waitForFinished(kPowerShellTimeoutMs)) {
        return items;
    }

    QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QStringList lines = output.split('\n');

    for (const auto& line : lines) {
        if (stopRequested.load()) break;

        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;

        // CSV format: "TaskName","Next Run Time","Status"
        QStringList fields = trimmed.split(',');
        if (fields.isEmpty()) continue;

        QString task_name = fields[0];
        task_name.remove('"');

        if (matchesProgram(task_name)) {
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

QVector<LeftoverItem> LeftoverScanner::scanFirewallRules(
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

    QProcess proc;
    proc.setProgram("netsh.exe");
    proc.setArguments({"advfirewall", "firewall", "show", "rule",
                       "name=all", "dir=in"});
    proc.start();

    if (!proc.waitForFinished(kPowerShellTimeoutMs)) {
        return items;
    }

    QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QStringList lines = output.split('\n');

    QString current_rule;

    for (const auto& line : lines) {
        if (stopRequested.load()) break;

        QString trimmed = line.trimmed();

        if (trimmed.startsWith("Rule Name:", Qt::CaseInsensitive)) {
            current_rule = trimmed.mid(10).trimmed();

            if (matchesProgram(current_rule)) {
                LeftoverItem item;
                item.type = LeftoverItem::Type::FirewallRule;
                item.path = current_rule;
                item.description = "Windows Firewall rule";
                item.risk = LeftoverItem::RiskLevel::Review;
                items.append(item);
            }
        }
    }

    // Also scan outbound rules
    QProcess proc2;
    proc2.setProgram("netsh.exe");
    proc2.setArguments({"advfirewall", "firewall", "show", "rule",
                        "name=all", "dir=out"});
    proc2.start();

    if (proc2.waitForFinished(kPowerShellTimeoutMs)) {
        output = QString::fromLocal8Bit(proc2.readAllStandardOutput());
        lines = output.split('\n');

        for (const auto& line : lines) {
            if (stopRequested.load()) break;

            QString trimmed = line.trimmed();
            if (trimmed.startsWith("Rule Name:", Qt::CaseInsensitive)) {
                current_rule = trimmed.mid(10).trimmed();

                if (matchesProgram(current_rule)) {
                    LeftoverItem item;
                    item.type = LeftoverItem::Type::FirewallRule;
                    item.path = current_rule;
                    item.description = "Windows Firewall rule (outbound)";
                    item.risk = LeftoverItem::RiskLevel::Review;
                    items.append(item);
                }
            }
        }
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanStartupEntries(
    const std::atomic<bool>& stopRequested)
{
    QVector<LeftoverItem> items;

#ifdef Q_OS_WIN
    // Scan HKCU\...\Run
    auto scanRunKey = [&](HKEY hive, const wchar_t* subkey, const QString& hiveName) {
        HKEY key = nullptr;
        LONG rc = RegOpenKeyExW(hive, subkey, 0, KEY_READ, &key);
        if (rc != ERROR_SUCCESS) return;

        DWORD value_count = 0;
        RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &value_count, nullptr, nullptr, nullptr, nullptr);

        wchar_t value_name[kMaxRegistryValueNameLen];
        BYTE value_data[kMaxRegistryValueDataLen];

        for (DWORD i = 0; i < value_count; ++i) {
            if (stopRequested.load()) break;

            DWORD name_len = kMaxRegistryValueNameLen;
            DWORD data_len = kMaxRegistryValueDataLen;
            DWORD type = 0;

            rc = RegEnumValueW(key, i, value_name, &name_len, nullptr,
                               &type, value_data, &data_len);
            if (rc != ERROR_SUCCESS) continue;

            QString name = QString::fromWCharArray(value_name, name_len);
            QString data;
            if ((type == REG_SZ || type == REG_EXPAND_SZ)
                && data_len >= sizeof(wchar_t)) {
                data = QString::fromWCharArray(
                    reinterpret_cast<wchar_t*>(value_data),
                    data_len / sizeof(wchar_t) - 1);
            }

            if (matchesProgram(name) || matchesProgram(data)) {
                LeftoverItem item;
                item.type = LeftoverItem::Type::StartupEntry;
                item.path = QString("%1\\%2")
                    .arg(hiveName,
                         QString::fromWCharArray(subkey));
                item.registryValueName = name;
                item.registryValueData = data;
                item.description = QString("Startup entry: %1").arg(name);
                item.risk = LeftoverItem::RiskLevel::Review;
                items.append(item);
            }
        }

        RegCloseKey(key);
    };

    scanRunKey(HKEY_CURRENT_USER,
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", "HKCU");
    scanRunKey(HKEY_CURRENT_USER,
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "HKCU");

    if (!stopRequested.load()) {
        scanRunKey(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", "HKLM");
        scanRunKey(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "HKLM");
    }
#endif

    // Also check Startup folder
    if (!stopRequested.load()) {
        QString startup_folder = QStandardPaths::writableLocation(
            QStandardPaths::ApplicationsLocation) + "/Startup";
        if (QDir(startup_folder).exists()) {
            QDir dir(startup_folder);
            const auto files = dir.entryInfoList(QDir::Files);
            for (const auto& file : files) {
                if (stopRequested.load()) break;
                if (matchesProgram(file.fileName())) {
                    LeftoverItem item;
                    item.type = LeftoverItem::Type::StartupEntry;
                    item.path = QDir::toNativeSeparators(
                        file.absoluteFilePath());
                    item.description = "Startup shortcut";
                    item.sizeBytes = file.size();
                    item.risk = LeftoverItem::RiskLevel::Safe;
                    items.append(item);
                }
            }
        }
    }

    return items;
}

LeftoverItem::RiskLevel LeftoverScanner::classifyRisk(
    const QString& path, LeftoverItem::Type type) const
{
    // Protected paths are always Risky
    if (isProtectedPath(path)) {
        return LeftoverItem::RiskLevel::Risky;
    }

    // Exact name match in expected location → Safe
    // Partial match or shared publisher dir → Review
    // System path proximity → Risky

    const QString path_lower = path.toLower();

    // File/Folder: check if in Program Files with exact match
    if (type == LeftoverItem::Type::File || type == LeftoverItem::Type::Folder) {
        // Check for exact install directory match
        if (!m_installDirName.isEmpty()
            && path_lower.contains(m_installDirName)) {
            return LeftoverItem::RiskLevel::Safe;
        }

        // Check for program name match
        for (const auto& pattern : m_namePatterns) {
            if (path_lower.contains(pattern)) {
                // If it's in AppData or ProgramData, likely safe
                if (path_lower.contains("appdata") || path_lower.contains("programdata")) {
                    return LeftoverItem::RiskLevel::Safe;
                }
                // In Program Files, likely safe
                if (path_lower.contains("program files")) {
                    return LeftoverItem::RiskLevel::Safe;
                }
                // Desktop shortcuts are safe
                if (path_lower.contains("desktop")) {
                    return LeftoverItem::RiskLevel::Safe;
                }
                // Start menu items are safe
                if (path_lower.contains("start menu")) {
                    return LeftoverItem::RiskLevel::Safe;
                }
                // Temp files are safe
                if (path_lower.contains("\\temp\\") || path_lower.contains("\\tmp\\")) {
                    return LeftoverItem::RiskLevel::Safe;
                }
                return LeftoverItem::RiskLevel::Review;
            }
        }

        // Publisher match only → Review
        for (const auto& pattern : m_publisherPatterns) {
            if (path_lower.contains(pattern)) {
                return LeftoverItem::RiskLevel::Review;
            }
        }
    }

    // Registry keys
    if (type == LeftoverItem::Type::RegistryKey
        || type == LeftoverItem::Type::RegistryValue) {
        for (const auto& pattern : m_namePatterns) {
            if (path_lower.contains(pattern)) {
                return LeftoverItem::RiskLevel::Safe;
            }
        }
        for (const auto& pattern : m_publisherPatterns) {
            if (path_lower.contains(pattern)) {
                return LeftoverItem::RiskLevel::Review;
            }
        }
    }

    // Services and tasks are always at least Review
    if (type == LeftoverItem::Type::Service) {
        return LeftoverItem::RiskLevel::Risky;
    }

    if (type == LeftoverItem::Type::ScheduledTask
        || type == LeftoverItem::Type::FirewallRule
        || type == LeftoverItem::Type::StartupEntry) {
        return LeftoverItem::RiskLevel::Review;
    }

    return LeftoverItem::RiskLevel::Review;
}

bool LeftoverScanner::isProtectedPath(const QString& path) const
{
    const QString path_native = QDir::toNativeSeparators(path);
    for (const auto& protected_path : kProtectedPaths) {
        if (path_native.startsWith(protected_path, Qt::CaseInsensitive)) {
            // Allow paths that go deeper than the protected path
            // but block direct matches
            if (path_native.length() == protected_path.length()
                || (path_native.length() > protected_path.length()
                    && path_native[protected_path.length()] == '\\')) {
                // Check if part of our install — if install location IS
                // under a protected path, allow it
                if (!m_program.installLocation.isEmpty()
                    && path_native.startsWith(
                        QDir::toNativeSeparators(m_program.installLocation),
                        Qt::CaseInsensitive)) {
                    return false;
                }
                return true;
            }
        }
    }
    return false;
}

bool LeftoverScanner::matchesProgram(const QString& name) const
{
    if (name.isEmpty()) {
        return false;
    }

    const QString name_lower = name.toLower();

    // Check against name patterns
    for (const auto& pattern : m_namePatterns) {
        if (name_lower.contains(pattern)) {
            return true;
        }
    }

    // Check against publisher patterns (less strict — only for full word match)
    for (const auto& pattern : m_publisherPatterns) {
        if (name_lower == pattern) {
            return true;
        }
    }

    return false;
}

qint64 LeftoverScanner::calculateSize(const QString& path)
{
    QFileInfo info(path);
    if (info.isFile()) {
        return info.size();
    }

    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

} // namespace sak
