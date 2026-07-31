// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_profile_manager.cpp
/// @brief Discovers email client profiles via registry/filesystem

#include "sak/email_profile_manager.h"

#include "sak/email_constants.h"
#include "sak/io_write_utils.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <vector>

#include <windows.h>
#endif

namespace {

/// Stable manifest label for an email client type.
QString clientTypeManifestName(sak::EmailClientType type) {
    switch (type) {
    case sak::EmailClientType::Outlook:
        return QStringLiteral("Outlook");
    case sak::EmailClientType::Thunderbird:
        return QStringLiteral("Thunderbird");
    case sak::EmailClientType::WindowsMail:
        return QStringLiteral("WindowsMail");
    case sak::EmailClientType::Other:
        break;
    }
    return QStringLiteral("Other");
}

/// RAII single-flight guard: clears the active flag when it goes out of scope, so
/// every return path from a guarded operation releases the lock (B7-16).
class ScopedActiveFlag {
public:
    explicit ScopedActiveFlag(std::atomic<bool>& flag) : m_flag(flag) {}
    ~ScopedActiveFlag() { m_flag.store(false); }
    ScopedActiveFlag(const ScopedActiveFlag&) = delete;
    ScopedActiveFlag& operator=(const ScopedActiveFlag&) = delete;
    ScopedActiveFlag(ScopedActiveFlag&&) = delete;
    ScopedActiveFlag& operator=(ScopedActiveFlag&&) = delete;

private:
    std::atomic<bool>& m_flag;
};

#ifdef Q_OS_WIN
/// @brief Fully resolve an EXISTING path through every reparse point (junctions AND symlinks) to
/// its real on-disk location, using GetFinalPathNameByHandleW. QFileInfo::canonicalFilePath does
/// NOT follow directory junctions on Windows, so it cannot detect a junction-based escape. Returns
/// a forward-slash path with the \\?\ prefix stripped, or empty if the path cannot be opened.
QString realCanonicalPath(const QString& path) {
    HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,  // required to open a directory handle
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<wchar_t> buffer(4096);
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    QString result = QString::fromWCharArray(buffer.data(), static_cast<int>(written));
    if (result.startsWith(QStringLiteral("\\\\?\\"))) {
        result = result.mid(4);
    }
    return QDir::fromNativeSeparators(result);
}
#else
QString realCanonicalPath(const QString& path) {
    return QFileInfo(path).canonicalFilePath();
}
#endif

constexpr int kRegExportTimeoutMs = 10'000;
constexpr int kRegImportTimeoutMs = 10'000;
constexpr int kRegistryPathMinimumBytes = 4;
constexpr qsizetype kMapiPropertyLeafNameLength = 8;
constexpr qsizetype kMapiPropertyTypePrefixLength = 4;

/// Resolve a manifest-supplied file name strictly inside `dir`. Returns the
/// absolute path only when `name` is a bare basename (no directory component,
/// not absolute, not "."/".."); otherwise returns an empty string so the caller
/// rejects it. This confines both the backup source read and the registry
/// import to the backup directory, blocking "../../payload.reg" style escapes.
QString resolveWithinDirectory(const QString& dir, const QString& name) {
    if (name.isEmpty()) {
        return {};
    }
    const QString bare = QFileInfo(name).fileName();
    if (bare != name || bare == QStringLiteral(".") || bare == QStringLiteral("..")) {
        return {};
    }
    return QDir(dir).absoluteFilePath(bare);
}

/// @brief Deepest existing ancestor directory of @p abs_path (which itself may not exist yet),
/// or empty if none of its ancestors exist. A junction/symlink can only redirect through an
/// EXISTING reparse point, so this is the part that must be canonicalized for a real containment
/// check.
QString deepestExistingAncestor(const QString& abs_path) {
    QString existing = abs_path;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).path();
        if (parent == existing) {
            return {};  // reached the filesystem root without finding an existing component
        }
        existing = parent;
    }
    return existing;
}

/// Outlook version keys in order of preference (newest first)
const QStringList kOutlookVersions = {
    QStringLiteral("16.0"),  // Office 2016/2019/2021/365
    QStringLiteral("15.0"),  // Office 2013
    QStringLiteral("14.0"),  // Office 2010
};

/// Outlook profiles registry path template
const QString kOutlookProfilesPath = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\%1"
    "\\Outlook\\Profiles");

/// Outlook data file registry path
const QString kOutlookDefaultProfilePath = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\%1"
    "\\Outlook");

/// Windows Messaging Subsystem path
const QString kWmsProfilesPath = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT"
    "\\CurrentVersion\\Windows Messaging Subsystem"
    "\\Profiles");

/// Parse a registry QVariant value into a file path string
QString parseRegistryPathValue(const QVariant& raw, bool is_unicode) {
    if (!raw.isValid()) {
        return {};
    }

    if (raw.typeId() != QMetaType::QByteArray) {
        return raw.toString();
    }

    QByteArray bytes = raw.toByteArray();
    if (bytes.size() < kRegistryPathMinimumBytes) {
        return {};
    }

    QString path_value;
    if (is_unicode) {
        path_value = QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData()),
                                        bytes.size() / 2);
    } else {
        path_value = QString::fromLocal8Bit(bytes);
    }
    while (path_value.endsWith(QChar(0))) {
        path_value.chop(1);
    }
    return path_value;
}

/// Create an EmailDataFile from a QFileInfo, typed by suffix
sak::EmailDataFile classifyDataFileType(const QFileInfo& fi) {
    sak::EmailDataFile data_file;
    data_file.path = fi.absoluteFilePath();
    data_file.size_bytes = fi.size();
    data_file.is_linked = true;

    QString suffix = fi.suffix().toLower();
    if (suffix == QStringLiteral("pst")) {
        data_file.type = QStringLiteral("PST");
    } else if (suffix == QStringLiteral("ost")) {
        data_file.type = QStringLiteral("OST");
    } else {
        data_file.type = QStringLiteral("Unknown");
    }
    return data_file;
}

/// Collect PST/OST files from an Outlook profile registry key
QVector<sak::EmailDataFile> findOutlookDataFiles(const QSettings& profile_key) {
    QVector<sak::EmailDataFile> files;

    static const QStringList kPathPropertyIds = {
        QStringLiteral("6610"),
        QStringLiteral("6600"),
    };
    static const QStringList kValidTypePrefixes = {
        QStringLiteral("001f"),
        QStringLiteral("001e"),
    };

    QStringList groups = profile_key.childGroups();
    for (const auto& group : groups) {
        QString prefix = group + QStringLiteral("/");
        QStringList keys = profile_key.allKeys();

        for (const auto& key : keys) {
            if (!key.startsWith(prefix)) {
                continue;
            }
            QString value_name = key.mid(prefix.size());

            int last_slash = value_name.lastIndexOf(QLatin1Char('/'));
            QString leaf_name = (last_slash >= 0) ? value_name.mid(last_slash + 1) : value_name;

            if (leaf_name.size() != kMapiPropertyLeafNameLength) {
                continue;
            }
            QString prop_id = leaf_name.mid(kMapiPropertyTypePrefixLength);
            if (!kPathPropertyIds.contains(prop_id)) {
                continue;
            }

            QString type_prefix = leaf_name.left(kMapiPropertyTypePrefixLength);
            if (!kValidTypePrefixes.contains(type_prefix)) {
                continue;
            }
            bool is_unicode = (type_prefix == QStringLiteral("001f"));

            QString path_value = parseRegistryPathValue(profile_key.value(key), is_unicode);
            if (path_value.isEmpty()) {
                continue;
            }

            QFileInfo fi(path_value);
            if (!fi.exists()) {
                continue;
            }

            files.append(classifyDataFileType(fi));
        }
    }
    return files;
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

EmailProfileManager::EmailProfileManager(QObject* parent) : QObject(parent) {}

// ============================================================================
// Public API
// ============================================================================

void EmailProfileManager::noteSettingsStatus(const QSettings& settings) {
    if (settings.status() != QSettings::NoError) {
        m_discovery_reliable = false;
    }
}

void EmailProfileManager::discoverProfiles() {
    if (m_operation_active.exchange(true)) {
        Q_EMIT errorOccurred(QStringLiteral("An email-profile operation is already in progress"));
        return;
    }
    const ScopedActiveFlag active_guard(m_operation_active);

    m_cancelled.store(false);
    m_discovery_reliable = true;
    m_profiles.clear();

    auto outlook = discoverOutlookProfiles();
    m_profiles.append(outlook);

    if (!m_cancelled.load()) {
        auto thunderbird = discoverThunderbirdProfiles();
        m_profiles.append(thunderbird);
    }

    if (!m_cancelled.load()) {
        auto windows_mail = discoverWindowsMailProfiles();
        m_profiles.append(windows_mail);
    }

    Q_EMIT profilesDiscovered(m_profiles);
}

void EmailProfileManager::backupProfiles(const QVector<int>& profile_indices,
                                         const QString& backup_path) {
    if (m_operation_active.exchange(true)) {
        Q_EMIT errorOccurred(QStringLiteral("An email-profile operation is already in progress"));
        return;
    }
    const ScopedActiveFlag active_guard(m_operation_active);

    // Fail closed instead of asserting: an empty path is caller/UI error but must
    // not abort the process in a release build or crash the debug test harness.
    if (backup_path.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Backup path is empty"));
        return;
    }
    m_cancelled.store(false);
    m_backup_dest_names.clear();

    QDir dir(backup_path);
    if (!dir.mkpath(QStringLiteral("."))) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to create backup directory"));
        return;
    }

    const int total_files = countTotalDataFiles(profile_indices);

    int files_done = 0;
    qint64 bytes_copied = 0;
    QVector<sak::EmailClientProfile> backed_up_profiles;

    for (int idx : profile_indices) {
        if (m_cancelled.load()) {
            break;
        }
        if (idx < 0 || idx >= m_profiles.size()) {
            continue;
        }

        backed_up_profiles.append(m_profiles[idx]);
        backupSingleProfile(m_profiles[idx], backup_path, files_done, total_files, bytes_copied);
    }

    if (!createBackupManifest(backup_path, backed_up_profiles)) {
        sak::logWarning("Failed to create backup manifest in: {}", backup_path.toStdString());
    }
    Q_EMIT backupComplete(backup_path, files_done, bytes_copied);
}

int EmailProfileManager::countTotalDataFiles(const QVector<int>& profile_indices) const {
    int total = 0;
    for (int idx : profile_indices) {
        if (idx >= 0 && idx < m_profiles.size()) {
            total += m_profiles[idx].data_files.size();
        }
    }
    return total;
}

void EmailProfileManager::backupSingleProfile(const sak::EmailClientProfile& profile,
                                              const QString& backup_path,
                                              int& files_done,
                                              int total_files,
                                              qint64& bytes_copied) {
    if (profile.client_type == sak::EmailClientType::Outlook && !profile.profile_path.isEmpty()) {
        QString reg_file = backup_path + QLatin1Char('/') + QStringLiteral("registry_") +
                           profile.profile_name + QStringLiteral(".reg");
        if (!exportRegistryKey(profile.profile_path, reg_file)) {
            sak::logWarning("Failed to export registry key: {}",
                            profile.profile_path.toStdString());
        }
    }

    for (const auto& data_file : profile.data_files) {
        if (m_cancelled.load()) {
            break;
        }

        QFileInfo fi(data_file.path);
        if (!fi.exists()) {
            continue;
        }

        const QString dest = uniqueBackupDestination(backup_path, fi);
        if (!QFile::copy(data_file.path, dest)) {
            // A failed required copy is a backup failure, not silent success:
            // surface it and do not count it toward files_done or the manifest.
            Q_EMIT errorOccurred(QStringLiteral("Failed to back up: %1").arg(data_file.path));
            continue;
        }

        // Record the ACTUAL on-disk name (which may carry a collision suffix) so
        // the manifest points restore at the real file, not the original basename.
        m_backup_dest_names.insert(data_file.path, QFileInfo(dest).fileName());
        bytes_copied += fi.size();
        files_done++;
        Q_EMIT backupProgress(files_done, total_files, bytes_copied);
    }
}

QString EmailProfileManager::uniqueBackupDestination(const QString& backup_path,
                                                     const QFileInfo& source) {
    QString dest = backup_path + QLatin1Char('/') + source.fileName();
    int attempt = 1;
    while (QFile::exists(dest)) {
        const QString suffix_index = attempt > 1 ? QString::number(attempt) : QString();
        dest = backup_path + QLatin1Char('/') + source.completeBaseName() +
               QStringLiteral("_backup") + suffix_index + QLatin1Char('.') + source.suffix();
        ++attempt;
    }
    return dest;
}

void EmailProfileManager::restoreProfiles(const QString& backup_manifest_path) {
    if (m_operation_active.exchange(true)) {
        Q_EMIT errorOccurred(QStringLiteral("An email-profile operation is already in progress"));
        return;
    }
    const ScopedActiveFlag active_guard(m_operation_active);

    m_cancelled.store(false);

    QFile file(backup_manifest_path);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to open backup manifest"));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject()) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid backup manifest format"));
        return;
    }

    QJsonObject root = doc.object();
    QJsonArray profiles = root[QStringLiteral("profiles")].toArray();
    QString backup_dir = QFileInfo(backup_manifest_path).absolutePath();

    int restored = 0;
    for (const auto& prof_val : profiles) {
        if (m_cancelled.load()) {
            break;
        }
        // Count a profile only when it restored with no rejected/missing/registry/
        // copy failures, so "profiles restored" reflects reality (B7-29).
        if (restoreSingleProfile(prof_val.toObject(), backup_dir)) {
            restored++;
        }
    }

    Q_EMIT restoreComplete(restored);
}

bool EmailProfileManager::restoreSingleProfile(const QJsonObject& prof, const QString& backup_dir) {
    bool all_ok = restoreRegistryFromManifest(prof, backup_dir);

    const QString home_root = QDir::homePath();
    const QJsonArray files = prof[QStringLiteral("data_files")].toArray();
    for (const auto& file_val : files) {
        // Evaluate every file (don't short-circuit) so all failures are logged.
        if (!restoreOneDataFile(file_val.toObject(), backup_dir, home_root)) {
            all_ok = false;
        }
    }
    return all_ok;
}

bool EmailProfileManager::restoreRegistryFromManifest(const QJsonObject& prof,
                                                      const QString& backup_dir) {
    const QString reg_name = prof[QStringLiteral("registry_file")].toString();
    if (reg_name.isEmpty()) {
        return true;  // no registry component -> nothing to fail
    }
    // Confine the .reg to the backup directory: a manifest is untrusted input and
    // "../../payload.reg" would otherwise import an arbitrary registry file.
    const QString full_reg = resolveWithinDirectory(backup_dir, reg_name);
    if (full_reg.isEmpty()) {
        sak::logWarning("Rejected registry file escaping backup dir: {}", reg_name.toStdString());
        return false;
    }
    if (!QFile::exists(full_reg)) {
        // The manifest referenced a registry export that is missing from the backup.
        sak::logWarning("Registry file missing from backup: {}", full_reg.toStdString());
        return false;
    }
    if (!importRegistryKey(full_reg)) {
        sak::logWarning("Failed to import registry key: {}", full_reg.toStdString());
        return false;
    }
    return true;
}

bool EmailProfileManager::restoreOneDataFile(const QJsonObject& file_obj,
                                             const QString& backup_dir,
                                             const QString& home_root) {
    const QString original = file_obj[QStringLiteral("original_path")].toString();
    const QString backed_up = file_obj[QStringLiteral("backed_up_name")].toString();

    // Source must be a bare name inside the backup dir; destination must be an
    // absolute path confined to the user's home tree.
    const QString source = resolveWithinDirectory(backup_dir, backed_up);
    if (source.isEmpty()) {
        sak::logWarning("Rejected backup source escaping backup dir: {}", backed_up.toStdString());
        return false;
    }
    if (!destinationWithinRoot(home_root, original)) {
        sak::logWarning("Rejected restore destination outside user home: {}",
                        original.toStdString());
        return false;
    }
    if (!QFile::exists(source)) {
        // A file the manifest says was backed up is gone from the backup.
        sak::logWarning("Backup source missing: {}", source.toStdString());
        return false;
    }
    if (QFile::exists(original)) {
        return true;  // already present -> intentional non-overwrite skip, not a failure
    }
    QDir().mkpath(QFileInfo(original).absolutePath());
    if (!QFile::copy(source, original)) {
        sak::logWarning("Failed to restore file: {}", original.toStdString());
        return false;
    }
    return true;
}

bool EmailProfileManager::destinationWithinRoot(const QString& root, const QString& candidate) {
    if (candidate.isEmpty()) {
        return false;
    }
    // Real root, resolving any junction/symlink in the home path itself.
    QString root_real = realCanonicalPath(root);
    if (root_real.isEmpty()) {
        root_real = QDir::cleanPath(QDir(root).absolutePath());
    }

    // A lexical cleanPath collapses "..", but a JUNCTION under the root would still pass a purely
    // textual prefix check while redirecting the write elsewhere. So fully resolve the deepest
    // EXISTING ancestor (the leaf may not exist yet) through every reparse point -- that is the
    // component a junction redirects -- and require IT to sit inside the real root.
    const QString abs = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    const QString existing = deepestExistingAncestor(abs);
    if (existing.isEmpty()) {
        return false;
    }
    const QString existing_real = realCanonicalPath(existing);
    if (existing_real.isEmpty()) {
        return false;
    }

    QString root_slash = root_real;
    if (!root_slash.endsWith(QLatin1Char('/'))) {
        root_slash += QLatin1Char('/');
    }
    return existing_real.compare(root_real, Qt::CaseInsensitive) == 0 ||
           (existing_real + QLatin1Char('/')).startsWith(root_slash, Qt::CaseInsensitive);
}

QSet<QString> EmailProfileManager::linkedFilePaths() const {
    QSet<QString> paths;
    for (const auto& profile : m_profiles) {
        for (const auto& data_file : profile.data_files) {
            paths.insert(
                QDir::toNativeSeparators(QFileInfo(data_file.path).absoluteFilePath().toLower()));
        }
    }
    return paths;
}

void EmailProfileManager::cancel() {
    m_cancelled.store(true);
}

// ============================================================================
// Outlook Discovery
// ============================================================================

QVector<sak::EmailClientProfile> EmailProfileManager::discoverOutlookProfiles() {
    QVector<sak::EmailClientProfile> results;

    for (const auto& version : kOutlookVersions) {
        if (m_cancelled.load()) {
            break;
        }

        QString reg_path = kOutlookProfilesPath.arg(version);

        // Use QSettings to read the registry
        QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft"
                                          "\\Office\\%1\\Outlook\\Profiles")
                               .arg(version),
                           QSettings::NativeFormat);

        QStringList profile_names = registry.childGroups();
        noteSettingsStatus(registry);  // a failed key read must not read as "no profiles"
        if (profile_names.isEmpty()) {
            continue;
        }

        sak::logInfo("ProfileManager: Found {} Outlook {} profiles",
                     profile_names.size(),
                     version.toStdString());

        // Determine which is the default profile
        QSettings outlook_key(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft"
                                             "\\Office\\%1\\Outlook")
                                  .arg(version),
                              QSettings::NativeFormat);
        QString default_profile =
            outlook_key.value(QStringLiteral("DefaultProfile"), QString()).toString();

        for (const auto& name : profile_names) {
            sak::EmailClientProfile profile;
            profile.client_type = sak::EmailClientType::Outlook;
            profile.client_name = QStringLiteral("Microsoft Outlook ") + version;
            profile.client_version = version;
            profile.profile_name = name;
            profile.profile_path = reg_path + QLatin1Char('\\') + name;

            // Dig into profile subkeys for data files
            registry.beginGroup(name);
            profile.data_files = findOutlookDataFiles(registry);
            registry.endGroup();

            sak::logInfo("ProfileManager: Profile '{}' has {} data files",
                         name.toStdString(),
                         profile.data_files.size());

            // Calculate total size
            for (const auto& df : profile.data_files) {
                profile.total_size_bytes += df.size_bytes;
            }

            results.append(profile);
        }
    }

    discoverWmsProfiles(results);

    return results;
}

void EmailProfileManager::discoverWmsProfiles(QVector<sak::EmailClientProfile>& results) {
    QSettings wms(kWmsProfilesPath, QSettings::NativeFormat);
    QStringList wms_profiles = wms.childGroups();
    noteSettingsStatus(wms);
    for (const auto& name : wms_profiles) {
        if (m_cancelled.load()) {
            break;
        }

        bool already_found = false;
        for (const auto& existing : results) {
            if (existing.profile_name == name) {
                already_found = true;
                break;
            }
        }
        if (already_found) {
            continue;
        }

        sak::EmailClientProfile profile;
        profile.client_type = sak::EmailClientType::Outlook;
        profile.client_name = QStringLiteral("Windows Messaging Subsystem");
        profile.profile_name = name;
        profile.profile_path = kWmsProfilesPath + QLatin1Char('\\') + name;

        wms.beginGroup(name);
        profile.data_files = findOutlookDataFiles(wms);
        wms.endGroup();

        for (const auto& df : profile.data_files) {
            profile.total_size_bytes += df.size_bytes;
        }

        if (!profile.data_files.isEmpty()) {
            results.append(profile);
        }
    }
}

// ============================================================================
// Thunderbird Discovery
// ============================================================================

QVector<sak::EmailClientProfile> EmailProfileManager::discoverThunderbirdProfiles() {
    QVector<sak::EmailClientProfile> results;

    // Thunderbird stores profiles in %APPDATA%\Thunderbird\profiles.ini
    QString appdata = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString tb_dir = QDir::homePath() + QStringLiteral("/AppData/Roaming/Thunderbird");
    QString profiles_ini = tb_dir + QStringLiteral("/profiles.ini");

    if (!QFile::exists(profiles_ini)) {
        return results;
    }

    QSettings ini(profiles_ini, QSettings::IniFormat);
    QStringList groups = ini.childGroups();
    noteSettingsStatus(ini);  // a present-but-corrupt profiles.ini must not read as "no profiles"

    for (const auto& group : groups) {
        if (m_cancelled.load()) {
            break;
        }
        if (!group.startsWith(QStringLiteral("Profile"), Qt::CaseInsensitive)) {
            continue;
        }

        ini.beginGroup(group);
        QString name = ini.value(QStringLiteral("Name"), QStringLiteral("Default")).toString();
        QString path = ini.value(QStringLiteral("Path")).toString();
        bool is_relative = ini.value(QStringLiteral("IsRelative"), 1).toInt();
        ini.endGroup();

        QString full_path = is_relative ? (tb_dir + QLatin1Char('/') + path) : path;

        sak::EmailClientProfile profile;
        profile.client_type = sak::EmailClientType::Thunderbird;
        profile.client_name = QStringLiteral("Mozilla Thunderbird");
        profile.profile_name = name;
        profile.profile_path = full_path;

        // Scan profile directory for MBOX files (no extension in TB)
        // and also look for .msf (index) files alongside mbox
        QDir profile_dir(full_path);
        if (profile_dir.exists()) {
            scanThunderbirdDir(profile_dir, profile.data_files);
        }

        for (const auto& df : profile.data_files) {
            profile.total_size_bytes += df.size_bytes;
        }

        if (!profile.data_files.isEmpty()) {
            results.append(profile);
        }
    }

    return results;
}

// ============================================================================
// Windows Mail Discovery
// ============================================================================

QVector<sak::EmailClientProfile> EmailProfileManager::discoverWindowsMailProfiles() {
    QVector<sak::EmailClientProfile> results;

    // Windows Mail / Windows Live Mail stores data in
    // %LOCALAPPDATA%\Microsoft\Windows Mail
    // or %LOCALAPPDATA%\Microsoft\Windows Live Mail
    QStringList mail_dirs = {
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
            QStringLiteral("/Microsoft/Windows Mail"),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
            QStringLiteral("/Microsoft/Windows Live Mail"),
    };

    for (const auto& mail_dir : mail_dirs) {
        if (m_cancelled.load()) {
            break;
        }

        QDir dir(mail_dir);
        if (!dir.exists()) {
            continue;
        }

        sak::EmailClientProfile profile;
        profile.client_type = sak::EmailClientType::WindowsMail;
        profile.client_name = mail_dir.contains(QStringLiteral("Live"))
                                  ? QStringLiteral("Windows Live Mail")
                                  : QStringLiteral("Windows Mail");
        profile.profile_name = QStringLiteral("Default");
        profile.profile_path = mail_dir;

        // Scan for .eml files
        QStringList filters = {
            QStringLiteral("*.eml"),
            QStringLiteral("*.pst"),
        };
        QFileInfoList file_list = dir.entryInfoList(filters, QDir::Files, QDir::Name);

        for (const auto& fi : file_list) {
            sak::EmailDataFile data_file;
            data_file.path = fi.absoluteFilePath();
            data_file.type = fi.suffix().toUpper();
            data_file.size_bytes = fi.size();
            data_file.is_linked = true;
            profile.data_files.append(data_file);
        }

        for (const auto& df : profile.data_files) {
            profile.total_size_bytes += df.size_bytes;
        }

        if (!profile.data_files.isEmpty()) {
            results.append(profile);
        }
    }

    return results;
}

// ============================================================================
// Registry Export/Import
// ============================================================================

bool EmailProfileManager::exportRegistryKey(const QString& key_path, const QString& output_file) {
    const auto result =
        sak::runProcess(QStringLiteral("reg.exe"),
                        {QStringLiteral("export"), key_path, output_file, QStringLiteral("/y")},
                        kRegExportTimeoutMs);
    return result.succeeded();
}

QString EmailProfileManager::decodeRegFile(const QByteArray& bytes) {
    // reg.exe export writes UTF-16LE with a BOM (0xFF 0xFE). Older REGEDIT4 files are ANSI.
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData() + 2),
                                  (bytes.size() - 2) / 2);
    }
    return QString::fromUtf8(bytes);
}

bool EmailProfileManager::regKeyPathAllowed(const QString& key_path) {
    const QString upper = key_path.trimmed().toUpper();
    // Only the HKCU Outlook profile subtree that a backup legitimately exports. This blocks HKLM
    // entirely and every other HKCU location (Run/RunOnce persistence, shell hooks, etc.).
    return upper.startsWith(QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\MICROSOFT\\OFFICE\\")) &&
           upper.contains(QStringLiteral("\\OUTLOOK\\PROFILES"));
}

bool EmailProfileManager::regContentConfinedToEmailHives(const QString& reg_text) {
    int key_sections = 0;
    const QStringList lines = reg_text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        // Key section headers are the only lines that name a registry path.
        if (!line.startsWith(QLatin1Char('[')) || !line.endsWith(QLatin1Char(']'))) {
            continue;
        }
        QString key = line.mid(1, line.size() - 2).trimmed();
        if (key.startsWith(QLatin1Char('-'))) {
            key = key.mid(1).trimmed();  // [-HKEY...] is a key DELETION -- confine it too
        }
        ++key_sections;
        if (!regKeyPathAllowed(key)) {
            return false;
        }
    }
    // A legitimate export always contains at least one key; zero keys means it is not a real
    // Outlook profile export, so refuse it rather than shelling out to reg.exe on unknown content.
    return key_sections > 0;
}

bool EmailProfileManager::importRegistryKey(const QString& reg_file) {
    QFile f(reg_file);
    if (!f.open(QIODevice::ReadOnly)) {
        sak::logWarning("Registry import: cannot read {}", reg_file.toStdString());
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    // A backup is untrusted input. reg.exe import will write EVERY key in the file, so before we
    // run it, confirm the .reg touches only the Outlook profile subtree. Otherwise a crafted backup
    // could import Run-key persistence or overwrite arbitrary HKLM/HKCU settings.
    if (!regContentConfinedToEmailHives(decodeRegFile(bytes))) {
        sak::logWarning("Refused .reg import (keys outside the Outlook profile subtree): {}",
                        reg_file.toStdString());
        return false;
    }

    const auto result = sak::runProcess(QStringLiteral("reg.exe"),
                                        {QStringLiteral("import"), reg_file},
                                        kRegImportTimeoutMs);
    return result.succeeded();
}

// ============================================================================
// Backup Manifest
// ============================================================================

bool EmailProfileManager::createBackupManifest(const QString& backup_path,
                                               const QVector<sak::EmailClientProfile>& profiles) {
    QJsonArray profiles_array;
    for (const auto& profile : profiles) {
        QJsonObject prof;
        prof[QStringLiteral("client_name")] = profile.client_name;
        prof[QStringLiteral("profile_name")] = profile.profile_name;
        prof[QStringLiteral("profile_path")] = profile.profile_path;

        prof[QStringLiteral("client_type")] = clientTypeManifestName(profile.client_type);

        // Registry file reference
        if (profile.client_type == sak::EmailClientType::Outlook) {
            prof[QStringLiteral("registry_file")] = QStringLiteral("registry_") +
                                                    profile.profile_name + QStringLiteral(".reg");
        }

        QJsonArray files_array;
        for (const auto& df : profile.data_files) {
            // Only manifest files that were actually copied; the recorded name is
            // the real on-disk basename (with any collision suffix), so restore
            // never points at a missing file or another profile's copy.
            const auto dest_it = m_backup_dest_names.constFind(df.path);
            if (dest_it == m_backup_dest_names.constEnd()) {
                continue;
            }
            QJsonObject file_obj;
            file_obj[QStringLiteral("original_path")] = df.path;
            file_obj[QStringLiteral("type")] = df.type;
            file_obj[QStringLiteral("size_bytes")] = df.size_bytes;
            file_obj[QStringLiteral("backed_up_name")] = dest_it.value();
            files_array.append(file_obj);
        }
        prof[QStringLiteral("data_files")] = files_array;

        profiles_array.append(prof);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("created")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("tool")] = QStringLiteral("SAK Utility");
    root[QStringLiteral("profiles")] = profiles_array;

    QFile file(backup_path + QStringLiteral("/backup_manifest.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!sak::writeFully(file, QJsonDocument(root).toJson(QJsonDocument::Indented))) {
        // A truncated manifest cannot be trusted to restore; fail closed.
        file.close();
        return false;
    }
    file.close();
    return true;
}

// ============================================================================
// Helper: Scan Thunderbird directory for MBOX files
// ============================================================================

void EmailProfileManager::scanThunderbirdDir(const QDir& dir, QVector<sak::EmailDataFile>& files) {
    constexpr int kMaxRecursionDepth = 20;
    scanThunderbirdDirRecursive(dir, files, 0, kMaxRecursionDepth);
}

void EmailProfileManager::scanThunderbirdDirRecursive(const QDir& dir,
                                                      QVector<sak::EmailDataFile>& files,
                                                      int depth,
                                                      int max_depth) {
    if (depth >= max_depth || m_cancelled.load()) {
        return;
    }

    // Thunderbird MBOX files have no extension and sit alongside .msf files
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

    for (const auto& entry : entries) {
        if (m_cancelled.load()) {
            break;
        }

        if (entry.isDir()) {
            scanThunderbirdDirRecursive(
                QDir(entry.absoluteFilePath()), files, depth + 1, max_depth);
            continue;
        }

        // An MBOX file is identified by having a companion .msf file
        QString path = entry.absoluteFilePath();
        if (entry.suffix().isEmpty() && QFile::exists(path + QStringLiteral(".msf"))) {
            sak::EmailDataFile data_file;
            data_file.path = path;
            data_file.type = QStringLiteral("MBOX");
            data_file.size_bytes = entry.size();
            data_file.is_linked = true;
            files.append(data_file);
        }
    }
}
