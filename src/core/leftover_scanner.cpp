// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file leftover_scanner.cpp
/// @brief Multi-level leftover scanning for orphaned files, folders, and
///        registry entries after program uninstallation

#include "sak/leftover_scanner.h"

#include "sak/layout_constants.h"
#include "sak/process_runner.h"
#include "sak/registry_snapshot_engine.h"
#include "sak/windows_path_policy.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

namespace sak {

namespace {
constexpr int kPowerShellTimeoutMs = 15'000;
// Hard backstop on how many files calculateSize will enumerate for one folder's size, so even a
// pathological install tree (millions of files) cannot make the walk unbounded when nothing
// cancels.
constexpr int kMaxSizeWalkEntries = 200'000;
constexpr int kMinConcatNameLen = 4;
constexpr int kRegistryHivePrefixLength = 5;

#ifdef Q_OS_WIN
constexpr DWORD kMaxRegistryValueNameLen = 256;
constexpr DWORD kMaxRegistryValueDataLen = 1024;

// Resolve a System32 executable to its absolute path so an elevated leftover scan
// cannot be hijacked by a PATH/CWD-planted binary of the same name. Fail closed
// (empty) if the Windows root cannot be trusted.
QString system32Exe(const QString& exe) {
    wchar_t buffer[MAX_PATH];
    const UINT len = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return QDir::cleanPath(QString::fromWCharArray(buffer, static_cast<int>(len)) +
                           QStringLiteral("/System32/") + exe);
}
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
    const QString value = QDir::toNativeSeparators(qEnvironmentVariable(env_var));
    if (!value.isEmpty()) {
        dirs.append(value);
    }
}

void appendStandardDir(QStringList& dirs, QStandardPaths::StandardLocation loc) {
    const QString path = QStandardPaths::writableLocation(loc);
    if (!path.isEmpty()) {
        dirs.append(path);
    }
}

bool isSignificantParentDir(const QString& parent) {
    return !parent.isEmpty() && !kGenericDirNames.contains(parent) && parent != "program files" &&
           parent != "program files (x86)" && parent != "programdata";
}

// Directory or registry-subkey leaf names that are shared OS or vendor containers, never a
// single product's private location. Any path ending in one of these must never be
// auto-selected for deletion.
const QSet<QString> kSharedRootNames = {
    "windows",
    "system32",
    "syswow64",
    "winsxs",
    "program files",
    "program files (x86)",
    "programdata",
    "common files",
    "microsoft",
    "microsoft shared",
    "windowsapps",
};

bool isSharedContainerPath(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    return kSharedRootNames.contains(QDir(path).dirName().toLower());
}

/// A leftover is pre-selected only when it may actually be deleted AND is classified Safe:
/// a report-only item (one whose path could not be tied to the program) is never checked.
bool leftoverPreSelected(const LeftoverItem& item) {
    return item.deletable && item.risk == LeftoverItem::RiskLevel::Safe;
}

/// Reduce a name to its comparable core: lower-cased, non-alphanumerics dropped. Lets
/// "Vendor App 2.0" and "VendorApp" compare equal without a substring free-for-all.
QString foldedName(const QString& value) {
    static const QRegularExpression kNoise(QStringLiteral("[^a-z0-9]+"));
    QString folded = value.toLower();
    folded.remove(kNoise);
    return folded;
}

/// Two folded names match when they are equal, or one is a prefix of the other and the
/// shorter side is long enough to be distinctive. Installer display names routinely carry
/// a version/architecture suffix the install directory omits ("Notepad++ (64-bit)" vs
/// "Notepad++"), and vendor directories routinely omit the vendor ("Google Chrome" vs
/// "Google\\Chrome"), so an exact-only rule would reject real installs. The length floor
/// stops a two-letter leaf from matching every program on the machine.
bool foldedNamesMatch(const QString& a, const QString& b) {
    constexpr int kMinDistinctiveFoldedLen = 4;
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    if (a == b) {
        return true;
    }
    if (std::min(a.size(), b.size()) < kMinDistinctiveFoldedLen) {
        return false;
    }
    return a.startsWith(b) || b.startsWith(a);
}

/// Append the profiles container (e.g. C:\\Users) and EVERY user profile root under it.
/// Deleting another user's whole profile is as catastrophic as deleting our own, and a
/// registry value can name any of them.
void appendProfileRoots(QStringList& roots) {
    const QString user_profile = qEnvironmentVariable("USERPROFILE");
    if (user_profile.isEmpty()) {
        return;
    }
    QDir profiles(user_profile);
    if (!profiles.cdUp()) {
        return;
    }
    roots.append(QDir::toNativeSeparators(profiles.absolutePath()));
    const auto children = profiles.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& child : children) {
        roots.append(QDir::toNativeSeparators(profiles.absoluteFilePath(child)));
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

// ======================================================================
// Construction
// ======================================================================

LeftoverScanner::LeftoverScanner(const ProgramInfo& program,
                                 ScanLevel level,
                                 const QSet<QString>& registryBefore)
    : m_program(program), m_level(level), m_registryBefore(registryBefore) {
    // Screen the registry-supplied InstallLocation ONCE, before anything derives names or
    // deletion targets from it. Everything downstream reads the screened/trusted copies;
    // m_program.installLocation is never used as authority again.
    const ScreenedInstallLocation screened = screenInstallLocation(program);
    m_screenedInstallLocation = screened.syntactic;
    m_trustedInstallLocation = screened.trusted;
    m_installLocationRefusal = screened.refusal;
    buildExactNames();
}

QStringList LeftoverScanner::criticalInstallRoots() {
    QStringList roots;
    const auto addEnv = [&roots](const char* variable) {
        const QString value = qEnvironmentVariable(variable);
        if (!value.isEmpty()) {
            roots.append(QDir::toNativeSeparators(value));
        }
    };
    addEnv("SystemRoot");
    addEnv("windir");
    addEnv("ProgramFiles");
    addEnv("ProgramFiles(x86)");
    addEnv("ProgramW6432");
    addEnv("ProgramData");
    addEnv("CommonProgramFiles");
    addEnv("CommonProgramFiles(x86)");
    addEnv("ALLUSERSPROFILE");
    addEnv("APPDATA");
    addEnv("LOCALAPPDATA");
    addEnv("PUBLIC");
    addEnv("TEMP");
    addEnv("TMP");
    addEnv("USERPROFILE");
    roots.append(QDir::toNativeSeparators(QDir::homePath()));
    appendProfileRoots(roots);
    return roots;
}

QString LeftoverScanner::installLocationSyntaxRefusal(const QString& rawLocation,
                                                      const QStringList& criticalRoots) {
    if (rawLocation.trimmed().isEmpty()) {
        return QStringLiteral("no install location is recorded");
    }
    // Same dialect as the UninstallString screen: refuses relative, drive-relative,
    // root-relative, UNC, traversal-bearing, "%VAR%"-carrying and volume-root values.
    if (!literalLocalPathTrusted(rawLocation)) {
        return QStringLiteral(
            "it is not a literal local directory path (relative, UNC, traversal, "
            "unexpanded variable or a whole volume)");
    }
    const QString screened = screeningPathForm(rawLocation);
    for (const QString& root : criticalRoots) {
        if (screeningPathForm(root).compare(screened, Qt::CaseInsensitive) == 0) {
            return QStringLiteral("it is a system, shared or user-profile root directory");
        }
    }
    return {};
}

bool LeftoverScanner::installLocationMatchesProgram(const QString& canonicalLocation,
                                                    const QString& displayName,
                                                    const QString& packageFamilyName) {
    QDir dir(canonicalLocation);
    const QString leaf = foldedName(dir.dirName());
    QString parent_leaf;
    if (dir.cdUp()) {
        parent_leaf = foldedName(dir.dirName());
    }
    // "App" and the "Publisher\App" pair are the two shapes a real install uses.
    const QStringList location_names{leaf, parent_leaf + leaf};
    const QStringList program_names{foldedName(displayName), foldedName(packageFamilyName)};
    for (const QString& location_name : location_names) {
        for (const QString& program_name : program_names) {
            if (foldedNamesMatch(location_name, program_name)) {
                return true;
            }
        }
    }
    return false;
}

LeftoverScanner::ScreenedInstallLocation LeftoverScanner::screenInstallLocation(
    const ProgramInfo& program) {
    ScreenedInstallLocation out;
    if (program.installLocation.trimmed().isEmpty()) {
        return out;  // nothing recorded: no candidate, and nothing to refuse
    }
    const QString refusal = installLocationSyntaxRefusal(program.installLocation,
                                                         criticalInstallRoots());
    if (!refusal.isEmpty()) {
        // Fail closed and do NOT touch the filesystem: a UNC or traversal value must not
        // even be stat'ed, let alone become a deletion candidate.
        out.refusal = refusal;
        return out;
    }
    out.syntactic = QDir::toNativeSeparators(screeningPathForm(program.installLocation));
    out.trusted = provenInstallLocation(out.syntactic, program, out.refusal);
    return out;
}

QString LeftoverScanner::provenInstallLocation(const QString& screenedLocation,
                                               const ProgramInfo& program,
                                               QString& refusalOut) {
    const QFileInfo info(screenedLocation);
    if (!info.exists() || !info.isDir()) {
        refusalOut = QStringLiteral("it is not an existing directory");
        return {};
    }
    // A junction/symlink lets the registry value aim a recursive delete at a target it
    // does not name. Refuse the reparse point itself; canonicalFilePath() additionally
    // resolves one in any PARENT component, and the resolved path is screened again so it
    // cannot land on a UNC share or a critical root by redirection.
    if (info.isSymLink()) {
        refusalOut = QStringLiteral("it is a reparse point (junction or symbolic link)");
        return {};
    }
    const QString canonical = QDir::toNativeSeparators(info.canonicalFilePath());
    if (canonical.isEmpty() ||
        !installLocationSyntaxRefusal(canonical, criticalInstallRoots()).isEmpty()) {
        refusalOut = QStringLiteral("it does not resolve to a literal local directory");
        return {};
    }
    if (!installLocationMatchesProgram(canonical, program.displayName, program.packageFamilyName)) {
        refusalOut = QStringLiteral("it cannot be tied to this program by name");
        return {};
    }
    return canonical;
}

void LeftoverScanner::extractInstallDirNames() {
    // Names are derived from the SYNTACTICALLY screened value, not the raw registry
    // string: a "%VAR%"/UNC/volume-root value must not seed the exact-name set that
    // decides which unrelated directories get matched elsewhere. The full trust tier is
    // deliberately NOT required here -- after an uninstall the directory is usually gone,
    // and its name is still the best signal for finding what it left behind.
    if (m_screenedInstallLocation.isEmpty()) {
        return;
    }
    QDir install_dir(m_screenedInstallLocation);
    const QString dir_name = install_dir.dirName().toLower();
    if (!dir_name.isEmpty() && !kGenericDirNames.contains(dir_name)) {
        m_installDirName = dir_name;
        if (!m_exactNames.contains(dir_name)) {
            m_exactNames.append(dir_name);
        }
    }

    if (install_dir.cdUp()) {
        const QString parent = install_dir.dirName().toLower();
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
        const QString pkg = m_program.packageFamilyName.toLower();
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
    std::function<void(const QString&, int)> progressCallback,
    LeftoverScanReliability* reliability) {
    QVector<LeftoverItem> all_items;
    int found_count = 0;
    LeftoverScanReliability rel;  // per-phase reliability, copied to the out-param at the end

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
        runPhase([&] { return scanRegistryDiff(stopRequested, rel.registry_ok); });
        runPhase([&] { return scanKnownRegistryPaths(stopRequested); });
#endif
    }

    // Phase 4: System objects (Advanced only)
    if (m_level == ScanLevel::Advanced) {
        runPhase([&] { return scanServices(stopRequested, rel.services_ok); });
        runPhase([&] { return scanScheduledTasks(stopRequested, rel.scheduled_tasks_ok); });
        runPhase([&] { return scanFirewallRules(stopRequested, rel.firewall_ok); });
        runPhase([&] { return scanStartupEntries(stopRequested, rel.startup_ok); });
    }

    // Surface per-phase reliability (a shell-out phase that failed returns empty, which without
    // this would read as an honest "nothing found"). runPhase skips a phase entirely when a stop
    // was already requested; leaving its flag true is correct -- the op reports cancellation
    // separately.
    if (reliability != nullptr) {
        *reliability = rel;
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
        item.selected = leftoverPreSelected(item);
        result.append(std::move(item));
    }

    return result;
}

// ======================================================================
// Phase 1: Install Location Scan
// ======================================================================

QVector<LeftoverItem> LeftoverScanner::scanInstallLocation(const std::atomic<bool>& stopRequested) {
    if (stopRequested.load()) {
        return {};
    }
    // Only the TRUSTED location may become a deletion candidate. Anything less is either
    // reported without deletion authority or dropped entirely (see reportOnlyInstallLocation).
    if (m_trustedInstallLocation.isEmpty()) {
        return reportOnlyInstallLocation();
    }

    LeftoverItem item;
    item.type = LeftoverItem::Type::Folder;
    item.path = m_trustedInstallLocation;
    item.description = "Program install directory still exists";
    item.sizeBytes = calculateSize(item.path, stopRequested);
    // Route through classifyRisk so a shared publisher/OS install location (e.g. a program
    // whose InstallLocation is Program Files\Common Files) is Risky, not auto-selected.
    item.risk = classifyRisk(item.path, item.type);

    return {item};
}

QVector<LeftoverItem> LeftoverScanner::reportOnlyInstallLocation() const {
    // A value that failed the SYNTAX screen yields nothing at all -- it is never stat'ed
    // and never becomes a candidate, with or without a warning attached.
    if (m_screenedInstallLocation.isEmpty() || m_installLocationRefusal.isEmpty()) {
        return {};
    }
    if (!QFileInfo(m_screenedInstallLocation).exists()) {
        return {};
    }

    // Syntactically clean and present, but not provably this program's directory. Surface
    // it so a technician can see and judge it, with deletion authority withheld: the size
    // walk is skipped too, since we will not be deleting this tree.
    LeftoverItem item;
    item.type = LeftoverItem::Type::Folder;
    item.path = m_screenedInstallLocation;
    item.description = QStringLiteral(
                           "Registered install directory -- REVIEW ONLY, not "
                           "deletable because %1")
                           .arg(m_installLocationRefusal);
    item.risk = LeftoverItem::RiskLevel::Risky;
    item.deletable = false;
    return {item};
}

// ======================================================================
// Phase 2: Known Application Paths
// ======================================================================

QVector<LeftoverItem> LeftoverScanner::scanKnownPaths(const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;

    // Scan roots come from environment variables (there is no OS-agnostic other source), while
    // kProtectedPaths hard-codes C:. This is safe by design: classifyRisk -> isSharedContainerPath
    // matches the shared-container LEAF name (windows/system32/programdata/...) case-insensitively
    // on ANY drive, so a non-C: OS directory is still Risky. A poisoned APPDATA/ProgramFiles only
    // redirects WHERE we look; anything found is still classified before it can be auto-selected.
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
    const QDir base(basePath);
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
            item.sizeBytes = calculateSize(entry.absoluteFilePath(), stopRequested);
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
            continue;
        }

        // Check publisher\product directory structure
        if (!isPublisherDir(dir_lower)) {
            continue;
        }
        const QDir pub_dir(entry.absoluteFilePath());
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
            item.sizeBytes = calculateSize(sub.absoluteFilePath(), stopRequested);
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
        }
    }

    return items;
}

QVector<LeftoverItem> LeftoverScanner::scanStandardFiles(const QString& basePath,
                                                         const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;
    const QDir base(basePath);
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

QVector<LeftoverItem> LeftoverScanner::scanRegistryDiff(const std::atomic<bool>& stopRequested,
                                                        bool& ok) {
    if (m_registryBefore.isEmpty() || stopRequested.load()) {
        return {};
    }

    // A PARTIAL "after" snapshot silently drops keys, so a survived leftover would be
    // missed and read as an honest "clean" -- surface that as a reliability failure.
    bool snapshot_ok = true;
    auto registry_after = RegistrySnapshotEngine::captureSnapshot(&snapshot_ok);
    if (!snapshot_ok) {
        ok = false;
    }
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
            LONG const rc =
                RegOpenKeyExW(hive, reinterpret_cast<LPCWSTR>(subkey.utf16()), 0, KEY_READ, &key);
            if (rc != ERROR_SUCCESS) {
                return;
            }
            RegCloseKey(key);
            LeftoverItem item;
            item.type = LeftoverItem::Type::RegistryKey;
            item.path = hiveName + "\\" + subkey;
            item.description = description;
            // Route through classifyRisk so a heuristically-guessed shared vendor subkey (e.g.
            // SOFTWARE\Microsoft) is Risky rather than blindly Safe/auto-selected.
            item.risk = classifyRisk(item.path, item.type);
            items.append(item);
        };

    // Check the program's own uninstall registry key
    if (!m_program.registryKeyPath.isEmpty()) {
        if (m_program.registryKeyPath.startsWith("HKLM\\")) {
            checkKey(HKEY_LOCAL_MACHINE,
                     m_program.registryKeyPath.mid(kRegistryHivePrefixLength),
                     "HKLM",
                     "Program uninstall key still exists");
        } else if (m_program.registryKeyPath.startsWith("HKCU\\")) {
            checkKey(HKEY_CURRENT_USER,
                     m_program.registryKeyPath.mid(kRegistryHivePrefixLength),
                     "HKCU",
                     "Program uninstall key still exists");
        }
    }

    // Check for product keys under standard hives
    if (!m_installDirName.isEmpty()) {
        const QString dir_proper = QDir(m_screenedInstallLocation).dirName();
        checkKey(
            HKEY_CURRENT_USER, "Software\\" + dir_proper, "HKCU", "Leftover product registry key");
        checkKey(
            HKEY_LOCAL_MACHINE, "SOFTWARE\\" + dir_proper, "HKLM", "Leftover product registry key");
    }

    return items;
}

#endif  // Q_OS_WIN

QVector<LeftoverItem> buildServiceLeftoverItems(const QVector<QPair<QString, QString>>& services,
                                                const std::function<bool(const QString&)>& matches,
                                                const std::atomic<bool>& stopRequested) {
    QVector<LeftoverItem> items;
    for (const auto& service : services) {
        if (stopRequested.load()) {
            break;
        }
        const QString& name = service.first;
        const QString& display = service.second;
        if (matches(name) || matches(display)) {
            LeftoverItem item;
            item.type = LeftoverItem::Type::Service;
            item.path = name;
            item.description = QString("Windows service: %1").arg(display);
            item.risk = LeftoverItem::RiskLevel::Risky;
            items.append(item);
        }
    }
    return items;
}

#ifdef Q_OS_WIN
namespace {
// Enumerate installed Win32 services as (keyName, displayName) pairs straight from the Service
// Control Manager (EnumServicesStatusExW). Both fields are language-neutral, so the leftover match
// no longer depends on English sc.exe console labels ("SERVICE_NAME:"/"DISPLAY_NAME:") that were
// empty on localized Windows. Sets @p ok false on a real SCM/enumeration failure (an empty result
// is then a FAILED read, not an honest "no services"); leaves it true on clean completion or a
// cooperative cancel. Fails closed: a hard error discards any partial list.
QVector<QPair<QString, QString>> enumerateWin32Services(const std::atomic<bool>& stopRequested,
                                                        bool& ok) {
    QVector<QPair<QString, QString>> services;

    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scm == nullptr) {
        ok = false;  // cannot even open the SCM -> a failed read, not "none found"
        return services;
    }

    constexpr DWORD kInitialBufferBytes = 64u * 1024u;
    std::vector<BYTE> buffer(kInitialBufferBytes);
    DWORD resume_handle = 0;
    for (;;) {
        if (stopRequested.load()) {
            break;  // cooperative cancel: ok stays true (a cancel is not a read failure)
        }
        DWORD bytes_needed = 0;
        DWORD services_returned = 0;
        const BOOL rc = EnumServicesStatusExW(scm,
                                              SC_ENUM_PROCESS_INFO,
                                              SERVICE_WIN32,
                                              SERVICE_STATE_ALL,
                                              buffer.data(),
                                              static_cast<DWORD>(buffer.size()),
                                              &bytes_needed,
                                              &services_returned,
                                              &resume_handle,
                                              nullptr);
        const DWORD err = rc ? ERROR_SUCCESS : GetLastError();
        if (rc == FALSE && err != ERROR_MORE_DATA) {
            ok = false;  // genuine enumeration failure -> fail closed, discard the partial list
            CloseServiceHandle(scm);
            return {};
        }

        const auto* entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD i = 0; i < services_returned; ++i) {
            services.append({QString::fromWCharArray(entries[i].lpServiceName),
                             QString::fromWCharArray(entries[i].lpDisplayName)});
        }

        if (rc != FALSE) {
            break;  // enumeration complete
        }
        // ERROR_MORE_DATA: grow the buffer if even one more entry will not fit, then resume.
        if (bytes_needed > buffer.size()) {
            buffer.resize(bytes_needed);
        }
    }

    CloseServiceHandle(scm);
    return services;
}
}  // namespace
#endif  // Q_OS_WIN

QVector<LeftoverItem> LeftoverScanner::scanServices(const std::atomic<bool>& stopRequested,
                                                    bool& ok) {
#ifdef Q_OS_WIN
    const QVector<QPair<QString, QString>> services = enumerateWin32Services(stopRequested, ok);
    if (!ok) {
        return {};  // enumeration failed -> empty is a FAILED read, honesty preserved by ok=false
    }
    return buildServiceLeftoverItems(
        services,
        [this](const QString& text) { return matchesProgramStrict(text); },
        stopRequested);
#else
    Q_UNUSED(stopRequested);
    ok = false;  // no SCM off Windows -> a failed read, never a dishonest empty-success
    return {};
#endif
}

// Extract the FIRST field of a CSV line, honoring RFC-4180 double-quoting so a task name that
// itself contains a comma (e.g. "Acme, Inc. Updater") is not truncated -- a naive split(',')[0]
// would take only "Acme and removeScheduledTask would then target the wrong/nonexistent task.
// Doubled quotes ("") inside a quoted field are a single literal quote. External linkage so the
// parse is unit-testable without schtasks.
QString parseFirstCsvField(const QString& line) {
    if (line.isEmpty() || line.at(0) != QLatin1Char('"')) {
        const int comma = line.indexOf(QLatin1Char(','));
        return comma < 0 ? line : line.left(comma);
    }
    QString out;
    for (int i = 1; i < line.length(); ++i) {
        const QChar ch = line.at(i);
        if (ch != QLatin1Char('"')) {
            out.append(ch);
        } else if (i + 1 < line.length() && line.at(i + 1) == QLatin1Char('"')) {
            out.append(QLatin1Char('"'));  // escaped quote inside the quoted field
            ++i;
        } else {
            break;  // closing quote of the field
        }
    }
    return out;
}

QVector<LeftoverItem> LeftoverScanner::scanScheduledTasks(const std::atomic<bool>& stopRequested,
                                                          bool& ok) {
    QVector<LeftoverItem> items;

    QString schtasks = QStringLiteral("schtasks.exe");
#ifdef Q_OS_WIN
    schtasks = system32Exe(QStringLiteral("schtasks.exe"));
    if (schtasks.isEmpty()) {
        ok = false;  // cannot resolve a trusted System32 schtasks -> fail closed
        return items;
    }
#endif
    const auto result = sak::runProcess(schtasks,
                                        {QStringLiteral("/query"),
                                         QStringLiteral("/fo"),
                                         QStringLiteral("CSV"),
                                         QStringLiteral("/nh")},
                                        kPowerShellTimeoutMs,
                                        [&stopRequested]() { return stopRequested.load(); });
    if (!result.succeeded()) {
        ok =
            false;  // schtasks failed/blocked/timed out -> empty is a FAILED read, not "none found"
        return items;
    }

    const QString output = result.std_out;
    const QStringList lines = output.split('\n');

    for (const auto& line : lines) {
        if (stopRequested.load()) {
            break;
        }

        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        // CSV format: "TaskName","Next Run Time","Status" -- parse the quoted first field so an
        // embedded comma in the task name does not truncate the task identity.
        const QString task_name = parseFirstCsvField(trimmed);
        if (task_name.isEmpty()) {
            continue;
        }

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

namespace {
// A netsh "advfirewall firewall show rule" block is delimited by a locale-neutral run of dashes
// (exactly one separator per rule). Its presence proves rules exist even when the localized
// "Rule Name:" label cannot be matched by this English-keyed parser.
constexpr int kMinFirewallSeparatorDashes = 10;

bool isNetshRuleSeparator(const QString& trimmed) {
    if (trimmed.length() < kMinFirewallSeparatorDashes) {
        return false;
    }
    for (const QChar ch : trimmed) {
        if (ch != QLatin1Char('-')) {
            return false;
        }
    }
    return true;
}
}  // namespace

// A netsh dump whose per-rule dashed separators are present but whose "Rule Name:" headers are all
// absent is a LOCALIZED dump this parser cannot read: the rules exist yet none can be extracted.
// Returns true in exactly that case so the firewall phase is marked UNRELIABLE (fail closed) rather
// than reporting a deceptively empty "no rules". A genuinely empty dump (no separators) returns
// false -- that is an honest "nothing here". External linkage so it is unit-testable without netsh.
bool firewallDumpHeaderMissing(const QStringList& lines) {
    bool saw_block = false;
    bool saw_header = false;
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (isNetshRuleSeparator(trimmed)) {
            saw_block = true;
        } else if (trimmed.startsWith("Rule Name:", Qt::CaseInsensitive)) {
            saw_header = true;
        }
    }
    return saw_block && !saw_header;
}

namespace {
// Lengths of the netsh rule-block label prefixes, each counting through the trailing colon.
constexpr int kDirectionLabelLen = 10;  // "Direction:"
constexpr int kProfilesLabelLen = 9;    // "Profiles:"
// Value that follows a "Label:" prefix in a netsh rule block (labelLen counts the
// prefix including the colon). Trimmed of the padding netsh inserts for alignment.
QString netshFieldValue(const QString& trimmed, int labelLen) {
    return trimmed.mid(labelLen).trimmed();
}
}  // namespace

// Copy the direction/profile/program identity fields off one netsh rule-block line into
// the pending item so cleanup can narrow the delete to this exact rule. An "Any"/empty
// program means the rule is not program-bound, so program= is intentionally omitted.
// External linkage so it is unit-testable without netsh.
void applyFirewallField(const QString& trimmed, LeftoverItem& item) {
    if (trimmed.startsWith("Direction:", Qt::CaseInsensitive)) {
        item.firewallDirection = netshFieldValue(trimmed, kDirectionLabelLen).toLower();
    } else if (trimmed.startsWith("Profiles:", Qt::CaseInsensitive)) {
        item.firewallProfile = netshFieldValue(trimmed, kProfilesLabelLen);
    } else if (trimmed.startsWith("Program:", Qt::CaseInsensitive)) {
        const QString prog = netshFieldValue(trimmed, 8);
        if (prog.compare("Any", Qt::CaseInsensitive) != 0) {
            item.firewallProgram = prog;
        }
    }
}

int LeftoverScanner::appendFirewallRule(const QString& headerLine,
                                        const QString& description,
                                        QVector<LeftoverItem>& items) const {
    const QString rule_name = netshFieldValue(headerLine, 10);
    if (!matchesProgramStrict(rule_name)) {
        return -1;
    }
    LeftoverItem item;
    item.type = LeftoverItem::Type::FirewallRule;
    item.path = rule_name;
    item.description = description;
    item.risk = LeftoverItem::RiskLevel::Review;
    items.append(item);
    return static_cast<int>(items.size()) - 1;
}

void LeftoverScanner::scanFirewallDirection(const QStringList& netsh_args,
                                            const QString& description,
                                            const std::atomic<bool>& stopRequested,
                                            QVector<LeftoverItem>& items,
                                            bool& ok) {
    QString netsh = QStringLiteral("netsh.exe");
#ifdef Q_OS_WIN
    netsh = system32Exe(QStringLiteral("netsh.exe"));
    if (netsh.isEmpty()) {
        ok = false;  // cannot resolve a trusted System32 netsh -> fail closed
        return;
    }
#endif
    const auto result =
        sak::runProcess(netsh, netsh_args, kPowerShellTimeoutMs, [&stopRequested]() {
            return stopRequested.load();
        });
    if (!result.succeeded()) {
        ok = false;  // netsh failed/blocked/timed out -> empty is a FAILED read, not "none found"
        return;
    }

    const QString output = result.std_out;
    const QStringList lines = output.split('\n');

    // Track the pending matched rule by INDEX (append can reallocate items). Every
    // field line after a matched "Rule Name:" feeds that rule until the next header;
    // a non-matching header resets to -1 so its fields are ignored.
    int current = -1;
    for (const auto& line : lines) {
        if (stopRequested.load()) {
            break;
        }
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("Rule Name:", Qt::CaseInsensitive)) {
            current = appendFirewallRule(trimmed, description, items);
        } else if (current >= 0) {
            applyFirewallField(trimmed, items[current]);
        }
    }

    // Rules clearly present (dashed separators) but zero parseable "Rule Name:" headers means the
    // labels are localized and this parse is blind: fail closed so the empty item list is not read
    // as an honest "no firewall rules".
    if (firewallDumpHeaderMissing(lines)) {
        ok = false;
    }
}

QVector<LeftoverItem> LeftoverScanner::scanFirewallRules(const std::atomic<bool>& stopRequested,
                                                         bool& ok) {
    QVector<LeftoverItem> items;

    scanFirewallDirection({"advfirewall", "firewall", "show", "rule", "name=all", "dir=in"},
                          "Windows Firewall rule",
                          stopRequested,
                          items,
                          ok);

    if (!stopRequested.load()) {
        scanFirewallDirection({"advfirewall", "firewall", "show", "rule", "name=all", "dir=out"},
                              "Windows Firewall rule (outbound)",
                              stopRequested,
                              items,
                              ok);
    }

    return items;
}

#ifdef Q_OS_WIN
namespace {
// Sane upper bounds for the ERROR_MORE_DATA retry loop. Genuine Run-key entries are tiny; these
// caps exist only so a misbehaving provider cannot spin the grow loop forever. Past them we fail
// closed (skip the value) rather than loop.
constexpr DWORD kMaxRunValueNameChars = 32u * 1024u;
constexpr std::size_t kMaxRunValueDataBytes = 1024u * 1024u;
// The API never reports the required NAME buffer size, so on ERROR_MORE_DATA the name buffer is
// grown by doubling until the ceiling above is reached.
constexpr std::size_t kNameBufferGrowthFactor = 2;

// Decode a REG_SZ / REG_EXPAND_SZ payload to a QString; empty for any other type or a buffer too
// short to hold even one wide char.
QString decodeRunValueBytes(DWORD type, const BYTE* data, DWORD data_bytes) {
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || data_bytes < sizeof(wchar_t)) {
        return {};
    }
    const auto* wdata = reinterpret_cast<const wchar_t*>(data);
    int chars = static_cast<int>(data_bytes / sizeof(wchar_t));
    // REG_SZ SHOULD carry a NUL terminator, but the registry does not guarantee it. Only drop the
    // final wide char when it actually IS the terminator; a non-terminated value keeps every char
    // (the old unconditional "-1" silently lost the last character of such values).
    if (chars > 0 && wdata[chars - 1] == L'\0') {
        --chars;
    }
    return QString::fromWCharArray(wdata, chars);
}

struct RunValueRead {
    QString name;
    QString data;
    bool ok = false;
};
}  // namespace

// ERROR_MORE_DATA buffer-growth policy for Run-key value reads, factored out for unit testing.
// @p data_len is RegEnumValueW's authoritative required data size; the required NAME size is never
// reported by the API, so the name buffer is instead doubled. Resizes the buffers in place and
// returns TRUE while still within sane ceilings; returns FALSE past them so the caller FAILS CLOSED
// (skips the value) instead of looping forever on a pathological entry. External linkage for tests.
bool growRunValueBuffers(std::vector<wchar_t>& name_buf,
                         std::vector<BYTE>& data_buf,
                         DWORD data_len) {
    if (static_cast<std::size_t>(data_len) > data_buf.size()) {
        data_buf.resize(data_len);
    } else {
        name_buf.resize(name_buf.size() * kNameBufferGrowthFactor);
    }
    return name_buf.size() <= kMaxRunValueNameChars && data_buf.size() <= kMaxRunValueDataBytes;
}

namespace {
// Read one Run-key value, GROWING both buffers on ERROR_MORE_DATA so a long value name or command
// line is never silently dropped (the old fixed 256/1024 buffers skipped such entries while the
// phase still reported reliable). ok stays false on a genuine error or once the grow ceiling is
// hit -- fail closed rather than emit a truncated value.
RunValueRead readRunKeyValue(HKEY key, DWORD index) {
    std::vector<wchar_t> name_buf(kMaxRegistryValueNameLen);
    std::vector<BYTE> data_buf(kMaxRegistryValueDataLen);
    RunValueRead out;
    for (;;) {
        DWORD name_len = static_cast<DWORD>(name_buf.size());
        DWORD data_len = static_cast<DWORD>(data_buf.size());
        DWORD type = 0;
        const LONG rc = RegEnumValueW(
            key, index, name_buf.data(), &name_len, nullptr, &type, data_buf.data(), &data_len);
        if (rc == ERROR_SUCCESS) {
            out.name = QString::fromWCharArray(name_buf.data(), static_cast<int>(name_len));
            out.data = decodeRunValueBytes(type, data_buf.data(), data_len);
            out.ok = true;
            return out;
        }
        if (rc != ERROR_MORE_DATA || !growRunValueBuffers(name_buf, data_buf, data_len)) {
            return out;  // genuine error or grow ceiling reached -> ok=false (fail closed)
        }
    }
}
}  // namespace

bool LeftoverScanner::appendRunKeyValue(HKEY key,
                                        DWORD index,
                                        const QString& hive_name,
                                        const wchar_t* subkey,
                                        QVector<LeftoverItem>& items) {
    const RunValueRead read = readRunKeyValue(key, index);
    if (!read.ok) {
        // The value could not be read (genuine error or grow ceiling) and was dropped. Signal the
        // failure so scanRunKey marks the startup phase incomplete rather than reporting a
        // deceptively honest "no entries" for a Run key we actually failed to enumerate.
        return false;
    }
    if (!matchesProgramStrict(read.name) && !matchesProgramStrict(read.data)) {
        return true;  // read succeeded; this value simply does not belong to the program
    }

    LeftoverItem item;
    item.type = LeftoverItem::Type::StartupEntry;
    item.path = QString("%1\\%2").arg(hive_name, QString::fromWCharArray(subkey));
    item.registryValueName = read.name;
    item.registryValueData = read.data;
    item.description = QString("Startup entry: %1").arg(read.name);
    item.risk = LeftoverItem::RiskLevel::Review;
    items.append(item);
    return true;
}

bool LeftoverScanner::scanRunKey(HKEY hive,
                                 const wchar_t* subkey,
                                 const QString& hive_name,
                                 const std::atomic<bool>& stopRequested,
                                 QVector<LeftoverItem>& items) {
    HKEY key = nullptr;
    const LONG rc = RegOpenKeyExW(hive, subkey, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        // ERROR_FILE_NOT_FOUND = the Run key simply does not exist -> an honest "no entries here"
        // (reliable). Any OTHER error (ERROR_ACCESS_DENIED under a constrained token, etc.) IS a
        // failed enumeration, so the empty result must not be reported as complete.
        return rc == ERROR_FILE_NOT_FOUND;
    }

    DWORD value_count = 0;
    if (RegQueryInfoKeyW(key,
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
                         nullptr) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return false;  // could not read the value count -> the (empty) enumeration is unreliable
    }

    // A single per-value read failure makes the whole key's enumeration unreliable: some startup
    // entry may have been dropped, so fail closed for the phase rather than report a partial list
    // as complete.
    bool all_reads_ok = true;
    for (DWORD idx = 0; idx < value_count && !stopRequested.load(); ++idx) {
        if (!appendRunKeyValue(key, idx, hive_name, subkey, items)) {
            all_reads_ok = false;
        }
    }

    RegCloseKey(key);
    return all_reads_ok;
}
#endif

QVector<LeftoverItem> LeftoverScanner::scanStartupEntries(const std::atomic<bool>& stopRequested,
                                                          bool& ok) {
    QVector<LeftoverItem> items;

#ifdef Q_OS_WIN
    // ok stays true only if EVERY Run-key read was reliable (a real failure -> phase incomplete).
    const auto readRun = [&](HKEY hive, const wchar_t* subkey, const QString& hive_name) {
        if (!scanRunKey(hive, subkey, hive_name, stopRequested, items)) {
            ok = false;
        }
    };
    readRun(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", "HKCU");
    readRun(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "HKCU");
    if (!stopRequested.load()) {
        readRun(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", "HKLM");
        readRun(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                "HKLM");
    }
#else
    Q_UNUSED(ok);
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
    const QDir dir(startup_folder);
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

// By design: only File/Folder items reach here, and every File/Folder is produced by an EXACT
// name match (matchesProgramExact), never the loose matchesProgramStrict substring test (which
// only ever yields Risky services or Review tasks/firewall/startup that are NOT auto-selected). So
// a Safe classification here still requires an exact program-name match -- the substring container
// checks below only pick the risk tier for an already exact-matched path.
LeftoverItem::RiskLevel LeftoverScanner::classifyFileRisk(const QString& path_lower) const {
    // A bare startsWith would classify a sibling like C:\App2 as inside C:\App and
    // exempt it from the protected-root check (eligible for auto-selected recursive
    // deletion). Require an exact match or a real path-segment boundary.
    // The TRUSTED location only: an unverifiable registry value must not be able to
    // declare an arbitrary directory (and everything beneath it) Safe and pre-selected.
    const QString install_lower = m_trustedInstallLocation.toLower();
    if (!install_lower.isEmpty()) {
        const QString install_with_sep = install_lower.endsWith(QLatin1Char('\\'))
                                             ? install_lower
                                             : install_lower + QLatin1Char('\\');
        if (path_lower == install_lower || path_lower.startsWith(install_with_sep)) {
            return LeftoverItem::RiskLevel::Safe;
        }
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
    if (isSharedContainerPath(path) || isProtectedPath(path)) {
        return LeftoverItem::RiskLevel::Risky;
    }

    if (type == LeftoverItem::Type::File || type == LeftoverItem::Type::Folder) {
        return classifyFileRisk(path.toLower());
    }

    return classifyTypeRisk(type);
}

namespace {
// A leftover is exempted from a protected root ONLY when it lives inside the
// program's own install folder AND that folder is a strict subdirectory of the
// protected root (e.g. C:\Program Files\MyApp under C:\Program Files). It is
// never exempted when installLocation IS the protected root itself (e.g.
// installLocation set to C:\Windows), which would classify the entire OS
// directory as Safe and auto-select it for recursive deletion.
bool leftoverInsideOwnInstallSubfolder(const QString& path_native,
                                       const QString& install_native,
                                       const QString& protected_path) {
    const int len = protected_path.length();
    const bool install_below_protected =
        !install_native.isEmpty() && install_native.length() > len &&
        install_native.startsWith(protected_path, Qt::CaseInsensitive) &&
        install_native[len] == '\\';
    return install_below_protected && path_native.startsWith(install_native, Qt::CaseInsensitive);
}
}  // namespace

bool LeftoverScanner::isProtectedPath(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    const QString path_native = QDir::toNativeSeparators(path);
    // The protected-root exemption is granted only to a TRUSTED install location. An
    // unverifiable registry value pointing inside C:\Windows must not be able to exempt
    // that subtree from the protected-path check.
    const QString install_native = m_trustedInstallLocation;

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
        return !leftoverInsideOwnInstallSubfolder(path_native, install_native, protected_path);
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

// Returns a best-effort byte total. On cancel or the kMaxSizeWalkEntries cap it returns the partial
// sum WITHOUT an "incomplete" flag by design: sizeBytes is purely informational UI data (it drives
// no deletion decision), and a qint64 total cannot realistically overflow from file sizes, so a
// capped/cancelled partial is an acceptable display value rather than a correctness hazard.
qint64 LeftoverScanner::calculateSize(const QString& path, const std::atomic<bool>& stopRequested) {
    const QFileInfo info(path);
    if (info.isFile()) {
        return info.size();
    }

    qint64 total = 0;
    int seen = 0;
    QDirIterator it(path,
                    QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        // Poll the shared cancel flag INSIDE the walk: scan() only checks stopRequested between
        // phases/items, so without this a size accounting of a huge install tree (an installer that
        // recorded InstallLocation=C:\ or a very large game/dev folder) would keep enumerating to
        // completion after a DeadlineCanceller already fired -- the deadline would be theater. The
        // entry cap is a hard backstop that bounds the walk even when nothing cancels.
        if (stopRequested.load() || seen >= kMaxSizeWalkEntries) {
            break;
        }
        ++seen;
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

}  // namespace sak
