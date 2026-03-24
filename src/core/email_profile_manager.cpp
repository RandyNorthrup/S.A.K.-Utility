// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_profile_manager.cpp
/// @brief Discovers email client profiles via registry/filesystem

#include "sak/email_profile_manager.h"

#include "sak/email_constants.h"
#include "sak/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

namespace {

constexpr int kRegExportTimeoutMs = 10'000;
constexpr int kRegImportTimeoutMs = 10'000;

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
    if (bytes.size() < 4) {
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

            if (leaf_name.size() != 8) {
                continue;
            }
            QString prop_id = leaf_name.mid(4);
            if (!kPathPropertyIds.contains(prop_id)) {
                continue;
            }

            QString type_prefix = leaf_name.left(4);
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

void EmailProfileManager::discoverProfiles() {
    m_cancelled.store(false);
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
    Q_ASSERT(!backup_path.isEmpty());
    m_cancelled.store(false);

    QDir dir(backup_path);
    if (!dir.mkpath(QStringLiteral("."))) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to create backup directory"));
        return;
    }

    int total_files = 0;
    for (int idx : profile_indices) {
        if (idx >= 0 && idx < m_profiles.size()) {
            total_files += m_profiles[idx].data_files.size();
        }
    }

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

        QString dest = backup_path + QLatin1Char('/') + fi.fileName();
        if (QFile::exists(dest)) {
            dest = backup_path + QLatin1Char('/') + fi.completeBaseName() +
                   QStringLiteral("_backup.") + fi.suffix();
        }

        if (QFile::copy(data_file.path, dest)) {
            bytes_copied += fi.size();
        }
        files_done++;
        Q_EMIT backupProgress(files_done, total_files, bytes_copied);
    }
}

void EmailProfileManager::restoreProfiles(const QString& backup_manifest_path) {
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
        restoreSingleProfile(prof_val.toObject(), backup_dir);
        restored++;
    }

    Q_EMIT restoreComplete(restored);
}

void EmailProfileManager::restoreSingleProfile(const QJsonObject& prof, const QString& backup_dir) {
    QString reg_file = prof[QStringLiteral("registry_file")].toString();
    if (!reg_file.isEmpty()) {
        QString full_reg = backup_dir + QLatin1Char('/') + reg_file;
        if (QFile::exists(full_reg)) {
            if (!importRegistryKey(full_reg)) {
                sak::logWarning("Failed to import registry key: {}", full_reg.toStdString());
            }
        }
    }

    QJsonArray files = prof[QStringLiteral("data_files")].toArray();
    for (const auto& file_val : files) {
        QJsonObject file_obj = file_val.toObject();
        QString original = file_obj[QStringLiteral("original_path")].toString();
        QString backed_up = file_obj[QStringLiteral("backed_up_name")].toString();
        QString source = backup_dir + QLatin1Char('/') + backed_up;

        if (!QFile::exists(source) || QFile::exists(original)) {
            continue;
        }
        QDir().mkpath(QFileInfo(original).absolutePath());
        if (!QFile::copy(source, original)) {
            sak::logWarning("Failed to restore file: {}", original.toStdString());
        }
    }
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
    QProcess process;
    process.setProgram(QStringLiteral("reg.exe"));
    process.setArguments({QStringLiteral("export"), key_path, output_file, QStringLiteral("/y")});
    process.start();

    if (!process.waitForStarted(kRegExportTimeoutMs)) {
        return false;
    }
    if (!process.waitForFinished(kRegExportTimeoutMs)) {
        process.kill();
        process.waitForFinished(kRegExportTimeoutMs);
        return false;
    }
    return process.exitCode() == 0;
}

bool EmailProfileManager::importRegistryKey(const QString& reg_file) {
    QProcess process;
    process.setProgram(QStringLiteral("reg.exe"));
    process.setArguments({QStringLiteral("import"), reg_file});
    process.start();

    if (!process.waitForStarted(kRegImportTimeoutMs)) {
        return false;
    }
    if (!process.waitForFinished(kRegImportTimeoutMs)) {
        process.kill();
        process.waitForFinished(kRegImportTimeoutMs);
        return false;
    }
    return process.exitCode() == 0;
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

        switch (profile.client_type) {
        case sak::EmailClientType::Outlook:
            prof[QStringLiteral("client_type")] = QStringLiteral("Outlook");
            break;
        case sak::EmailClientType::Thunderbird:
            prof[QStringLiteral("client_type")] = QStringLiteral("Thunderbird");
            break;
        case sak::EmailClientType::WindowsMail:
            prof[QStringLiteral("client_type")] = QStringLiteral("WindowsMail");
            break;
        case sak::EmailClientType::Other:
            prof[QStringLiteral("client_type")] = QStringLiteral("Other");
            break;
        }

        // Registry file reference
        if (profile.client_type == sak::EmailClientType::Outlook) {
            prof[QStringLiteral("registry_file")] = QStringLiteral("registry_") +
                                                    profile.profile_name + QStringLiteral(".reg");
        }

        QJsonArray files_array;
        for (const auto& df : profile.data_files) {
            QJsonObject file_obj;
            file_obj[QStringLiteral("original_path")] = df.path;
            file_obj[QStringLiteral("type")] = df.type;
            file_obj[QStringLiteral("size_bytes")] = df.size_bytes;
            file_obj[QStringLiteral("backed_up_name")] = QFileInfo(df.path).fileName();
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
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
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
