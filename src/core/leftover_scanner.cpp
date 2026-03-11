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
constexpr DWORD kMaxRegistryKeyNameLen = 256;
constexpr DWORD kMaxRegistryValueNameLen = 256;
constexpr DWORD kMaxRegistryValueDataLen = 1024;
constexpr int kMinPatternWordLen = 3;

template <typename ReportFn>
void appendAndReportItems(QVector<LeftoverItem>& out,
                          const QVector<LeftoverItem>& items,
                          ReportFn report) {
    for (const auto& item : items) {
        report(item.path);
    }
    out.append(items);
}

bool isExcludedPublisherSuffix(const QString& word_lower) {
    static const QSet<QString> kSuffixes = {"inc", "ltd", "llc", "corp", "gmbh", "the"};
    return kSuffixes.contains(word_lower);
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
    : m_program(program), m_level(level) {
    buildSearchPatterns();
}

void LeftoverScanner::addFilteredWords(const QString& text,
                                       const QString& split_pattern,
                                       QStringList& target,
                                       const QSet<QString>& excludes) {
    const QStringList words = text.split(QRegularExpression(split_pattern), Qt::SkipEmptyParts);
    for (const auto& word : words) {
        if (word.length() >= kMinPatternWordLen && !excludes.contains(word.toLower())) {
            target.append(word.toLower());
        }
    }
}

void LeftoverScanner::buildNamePatterns(const QSet<QString>& excludedWords) {
    if (m_program.displayName.isEmpty()) {
        return;
    }

    m_namePatterns.append(m_program.displayName.toLower());
    addFilteredWords(m_program.displayName, "[\\s\\-_]+", m_namePatterns, excludedWords);

    QString concat = m_program.displayName;
    concat.remove(QRegularExpression("[\\s\\-_]+"));
    if (concat.length() >= kMinPatternWordLen) {
        m_namePatterns.append(concat.toLower());
    }

    if (!m_program.installLocation.isEmpty()) {
        QString dir_name = QDir(m_program.installLocation).dirName().toLower();
        if (!dir_name.isEmpty()) {
            m_namePatterns.append(dir_name);
            m_installDirName = dir_name;
        }
    }
}

void LeftoverScanner::buildSearchPatterns() {
    static const QSet<QString> kExcludedWords = {
        "the",     "for",    "and",      "pro",      "app",     "new",     "all",
        "one",     "free",   "media",    "player",   "viewer",  "editor",  "manager",
        "service", "system", "update",   "setup",    "install", "windows", "microsoft",
        "tools",   "tool",   "software", "portable", "lite",    "plus",    "studio",
        "home",    "server", "client",   "web",      "desktop", "data",    "file",
    };

    buildNamePatterns(kExcludedWords);

    if (!m_program.publisher.isEmpty()) {
        m_publisherPatterns.append(m_program.publisher.toLower());
        static const QSet<QString> kPublisherExcludes = {
            "inc", "ltd", "llc", "corp", "gmbh", "the"};
        addFilteredWords(
            m_program.publisher, "[\\s\\-_,\\.]+", m_publisherPatterns, kPublisherExcludes);
    }

    m_namePatterns.removeDuplicates();
    m_publisherPatterns.removeDuplicates();

    Q_ASSERT(!m_program.displayName.isEmpty() || m_namePatterns.isEmpty());
    Q_ASSERT(!m_program.publisher.isEmpty() || m_publisherPatterns.isEmpty());
}

QVector<LeftoverItem> LeftoverScanner::scan(
    const std::atomic<bool>& stopRequested,
    std::function<void(const QString&, int)> progressCallback) {
    QVector<LeftoverItem> all_leftovers;
    int found_count = 0;

    auto report = [&](const QString& path) {
        ++found_count;
        if (progressCallback) {
            progressCallback(path, found_count);
        }
    };

    auto runPhase = [&](auto scanner_fn) {
        if (!stopRequested.load()) {
            appendAndReportItems(all_leftovers, scanner_fn(), report);
        }
    };

    runPhase([&] { return scanFileSystem(stopRequested); });

    if (m_level == ScanLevel::Moderate || m_level == ScanLevel::Advanced) {
#ifdef Q_OS_WIN
        runPhase([&] { return scanRegistry(stopRequested); });
#endif
    }

    if (m_level == ScanLevel::Advanced) {
        runPhase([&] { return scanServices(stopRequested); });
        runPhase([&] { return scanScheduledTasks(stopRequested); });
        runPhase([&] { return scanFirewallRules(stopRequested); });
        runPhase([&] { return scanStartupEntries(stopRequested); });
    }

    // Pre-select safe items
    for (auto& item : all_leftovers) {
        item.selected = (item.risk == LeftoverItem::RiskLevel::Safe);
    }

    return all_leftovers;
}

QVector<LeftoverItem> LeftoverScanner::scanFileSystem(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    QStringList scan_dirs;

    appendEnvDir(scan_dirs, "ProgramFiles");
    appendEnvDir(scan_dirs, "ProgramFiles(x86)");
    appendEnvDir(scan_dirs, "APPDATA");
    appendEnvDir(scan_dirs, "LOCALAPPDATA");
    appendEnvDir(scan_dirs, "ProgramData");
    appendEnvDir(scan_dirs, "TEMP");
    appendStandardDir(scan_dirs, QStandardPaths::ApplicationsLocation);
    appendStandardDir(scan_dirs, QStandardPaths::DesktopLocation);

    if (m_level == ScanLevel::Advanced) {
        appendEnvDir(scan_dirs, "CommonProgramFiles");
    }

    for (const auto& dir : scan_dirs) {
        if (stopRequested.load()) {
            break;
        }
        items.append(scanDirectory(dir, stopRequested));
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanDirectory(const QString& basePath,
                                                     const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    QDir base(basePath);
    if (!base.exists()) {
        return items;
    }

    // Scan top-level directories for matching names
    const auto entries = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (stopRequested.load()) {
            break;
        }

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
        if (stopRequested.load()) {
            break;
        }

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

QVector<LeftoverItem> LeftoverScanner::scanRegistry(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    // Scan HKCU\Software for matching keys
    auto hkcu = scanRegistryHive(HKEY_CURRENT_USER, "Software", "HKCU", stopRequested);
    items.append(hkcu);

    if (stopRequested.load()) {
        return items;
    }

    // Scan HKLM\SOFTWARE for matching keys
    auto hklm = scanRegistryHive(HKEY_LOCAL_MACHINE, "SOFTWARE", "HKLM", stopRequested);
    items.append(hklm);

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanRegistryHive(HKEY hive,
                                                        const QString& subkey,
                                                        const QString& hiveName,
                                                        const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(hive, reinterpret_cast<LPCWSTR>(subkey.utf16()), 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        return items;
    }

    DWORD subkey_count = 0;
    RegQueryInfoKeyW(key,
                     nullptr,
                     nullptr,
                     nullptr,
                     &subkey_count,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr);

    wchar_t subkey_name[kMaxRegistryKeyNameLen];
    for (DWORD i = 0; i < subkey_count; ++i) {
        if (stopRequested.load()) {
            break;
        }

        DWORD name_len = kMaxRegistryKeyNameLen;
        rc = RegEnumKeyExW(key, i, subkey_name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        QString child_name = QString::fromWCharArray(subkey_name, name_len);

        // Skip Microsoft's own keys
        if (child_name.startsWith("Microsoft", Qt::CaseInsensitive) ||
            child_name.startsWith("Windows", Qt::CaseInsensitive) ||
            child_name.startsWith("Classes", Qt::CaseInsensitive)) {
            continue;
        }

        if (matchesProgram(child_name)) {
            LeftoverItem item;
            item.type = LeftoverItem::Type::RegistryKey;
            item.path = QString("%1\\%2\\%3").arg(hiveName, subkey, child_name);
            item.description = "Leftover registry key";
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
        }
    }

    RegCloseKey(key);
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

            // Check if service matches our program
            if (matchesProgram(current_service) || matchesProgram(current_display)) {
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
        if (!matchesProgram(rule_name)) {
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
    Q_ASSERT(subkey != nullptr);
    Q_ASSERT(!hive_name.isEmpty());

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

        if (!matchesProgram(name) && !matchesProgram(data)) {
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
        if (!matchesProgram(file.fileName())) {
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

namespace {

bool isSafeFileLocation(const QString& path_lower) {
    return path_lower.contains("appdata") || path_lower.contains("programdata") ||
           path_lower.contains("program files") || path_lower.contains("desktop") ||
           path_lower.contains("start menu") || path_lower.contains("\\temp\\") ||
           path_lower.contains("\\tmp\\");
}

bool matchesAnyPattern(const QString& path_lower, const QStringList& patterns) {
    for (const auto& pattern : patterns) {
        if (path_lower.contains(pattern)) {
            return true;
        }
    }
    return false;
}

LeftoverItem::RiskLevel classifyFileOrFolder(const QString& path_lower,
                                             const QString& install_dir,
                                             const QStringList& name_patterns,
                                             const QStringList& publisher_patterns) {
    if (!install_dir.isEmpty() && path_lower.contains(install_dir)) {
        return LeftoverItem::RiskLevel::Safe;
    }
    if (matchesAnyPattern(path_lower, name_patterns)) {
        if (isSafeFileLocation(path_lower)) {
            return LeftoverItem::RiskLevel::Safe;
        }
        return LeftoverItem::RiskLevel::Review;
    }
    if (matchesAnyPattern(path_lower, publisher_patterns)) {
        return LeftoverItem::RiskLevel::Review;
    }
    return LeftoverItem::RiskLevel::Review;
}

LeftoverItem::RiskLevel classifyRegistryType(const QString& path_lower,
                                             const QStringList& name_patterns,
                                             const QStringList& publisher_patterns) {
    if (matchesAnyPattern(path_lower, name_patterns)) {
        return LeftoverItem::RiskLevel::Safe;
    }
    if (matchesAnyPattern(path_lower, publisher_patterns)) {
        return LeftoverItem::RiskLevel::Review;
    }
    return LeftoverItem::RiskLevel::Review;
}

LeftoverItem::RiskLevel classifySystemType(LeftoverItem::Type type) {
    if (type == LeftoverItem::Type::Service) {
        return LeftoverItem::RiskLevel::Risky;
    }
    return LeftoverItem::RiskLevel::Review;
}

}  // namespace

LeftoverItem::RiskLevel LeftoverScanner::classifyRisk(const QString& path,
                                                      LeftoverItem::Type type) const {
    if (isProtectedPath(path)) {
        return LeftoverItem::RiskLevel::Risky;
    }

    const QString path_lower = path.toLower();

    if (type == LeftoverItem::Type::File || type == LeftoverItem::Type::Folder) {
        return classifyFileOrFolder(
            path_lower, m_installDirName, m_namePatterns, m_publisherPatterns);
    }

    if (type == LeftoverItem::Type::RegistryKey || type == LeftoverItem::Type::RegistryValue) {
        return classifyRegistryType(path_lower, m_namePatterns, m_publisherPatterns);
    }

    return classifySystemType(type);
}

bool LeftoverScanner::isProtectedPath(const QString& path) const {
    Q_ASSERT(!path.isEmpty());
    const QString path_native = QDir::toNativeSeparators(path);
    const QString install_native = m_program.installLocation.isEmpty()
                                       ? QString{}
                                       : QDir::toNativeSeparators(m_program.installLocation);
    for (const auto& protected_path : kProtectedPaths) {
        if (!path_native.startsWith(protected_path, Qt::CaseInsensitive)) {
            continue;
        }

        const int protectedLen = protected_path.length();
        const bool directMatch = (path_native.length() == protectedLen);
        const bool directChild = (path_native.length() > protectedLen &&
                                  path_native[protectedLen] == '\\');
        if (!directMatch && !directChild) {
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

bool LeftoverScanner::matchesProgram(const QString& name) const {
    Q_ASSERT(!name.isEmpty());
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
