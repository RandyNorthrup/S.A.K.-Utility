// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_data_manager.h"

#include "sak/encryption.h"
#include "sak/layout_constants.h"
#include "sak/leftover_cleanup_guard.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtGlobal>

#include <algorithm>
#include <optional>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

/// @brief True if the path is an NTFS reparse point (junction / directory symlink).
///
/// Recursive backup must not descend into these: a junction can point outside
/// the source root (copying arbitrary external data into the backup) or back to
/// an ancestor (infinite recursion). On non-Windows this maps to a symlink.
bool isReparsePoint(const QString& path) {
#ifdef SAK_PLATFORM_WINDOWS
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()));
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return QFileInfo(path).isSymLink();
#endif
}

/// @brief Case-insensitive, trailing-dot/space-normalized identity of a path.
///
/// Mirrors how Win32 resolves a path so two spellings of the SAME object compare
/// equal (and a crafted "C:\\Windows." cannot masquerade as a different target).
QString backupPathIdentity(const QString& path) {
    return sak::cleanupCanonicalLower(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

/// @brief True when @p a and @p b name the same on-disk object.
bool sameBackupObject(const QString& a, const QString& b) {
    const QString ia = backupPathIdentity(a);
    return !ia.isEmpty() && ia == backupPathIdentity(b);
}

/// @brief Append paths that exist for the given name variants in a base directory.
void appendExistingVariants(QStringList& paths,
                            const QString& base_dir,
                            const QStringList& variants) {
    for (const auto& variant : variants) {
        const QString path = QDir(base_dir).filePath(variant);
        if (QDir(path).exists()) {
            paths.append(path);
        }
    }
}

/// @brief Populate a BackupEntry from a metadata object, failing closed on wrong types.
///
/// A tampered sidecar must not coerce a wrong-typed field to a silent default (notably
/// a numeric or absent checksum collapsing to "" which would disable restore verification).
/// The load-bearing fields (app_name, backup_path, checksum) are type-checked; a mismatch
/// rejects the whole sidecar rather than proceeding with a fabricated value.
bool parseMetadataObject(const QJsonObject& json, sak::UserDataManager::BackupEntry& entry) {
    const QJsonValue name = json.value(QStringLiteral("app_name"));
    if (!name.isString() || name.toString().isEmpty()) {
        return false;
    }
    const QJsonValue path = json.value(QStringLiteral("backup_path"));
    if (!path.isString()) {
        return false;
    }
    const QJsonValue checksum = json.value(QStringLiteral("checksum"));
    if (!checksum.isString()) {
        return false;  // absent or wrong-typed checksum must not become "" (disables verify)
    }
    entry.app_name = name.toString();
    entry.app_version = json.value(QStringLiteral("app_version")).toString();
    for (const auto& p : json.value(QStringLiteral("source_paths")).toArray()) {
        entry.source_paths.append(p.toString());
    }
    entry.backup_path = path.toString();
    entry.backup_date = QDateTime::fromString(json.value(QStringLiteral("backup_date")).toString(),
                                              Qt::ISODate);
    entry.total_size_bytes = json.value(QStringLiteral("total_size")).toInteger();
    entry.compressed_size_bytes = json.value(QStringLiteral("compressed_size")).toInteger();
    entry.checksum = checksum.toString();
    entry.encrypted = json.value(QStringLiteral("encrypted")).toBool();
    for (const auto& pat : json.value(QStringLiteral("excluded_patterns")).toArray()) {
        entry.excluded_patterns.append(pat.toString());
    }
    return true;
}

}  // namespace

namespace sak {

namespace {
constexpr int kFastCompressionMaxLevel = 3;
}

UserDataManager::UserDataManager(QObject* parent) : QObject(parent) {}

std::optional<UserDataManager::BackupEntry> UserDataManager::backupAppData(
    const QString& app_name,
    const QStringList& source_paths,
    const QString& backup_dir,
    const BackupConfig& config) {
    Q_EMIT operationStarted(app_name, "backup");

    // Empty app_name / backup_dir are enforced fail-closed inside validateBackupRequest
    // (a runtime check, not a debug-only Q_ASSERT that is compiled out of release).
    if (!validateBackupRequest(app_name, source_paths, backup_dir, config)) {
        return std::nullopt;
    }

    // Generate a collision-free backup path (payload plus a ".json" sidecar).
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString safe_name = app_name;
    safe_name.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    QString base_name = QString("%1_%2").arg(safe_name, timestamp);
    QString backup_path = uniqueBackupPath(backup_dir, base_name, config.compress);

    // Calculate total size
    const qint64 total_size = calculateSize(source_paths);
    Q_EMIT progressUpdate(0, total_size, "Calculating size...");

    // Create archive (compressed) or copy tree (uncompressed) at backup_path.
    if (!writeBackupPayload(app_name, source_paths, backup_path, config)) {
        return std::nullopt;
    }

    // Verify checksum
    QString checksum;
    if (config.verify_checksum && config.compress) {
        Q_EMIT progressUpdate(total_size, total_size, "Verifying checksum...");
        checksum = generateChecksum(backup_path);
        if (checksum.isEmpty()) {
            Q_EMIT operationError(app_name, "Failed to generate checksum");
            return std::nullopt;
        }
    }

    // Persist metadata that already carries the checksum. Fail closed on a
    // metadata write failure so a backup is never reported successful while its
    // required .json sidecar is missing (which would make restore skip integrity
    // checks or fail outright).
    BackupEntry entry = buildBackupResult(app_name, source_paths, backup_path, total_size, config);
    entry.checksum = checksum;
    if (!writeMetadata(entry, backup_path + ".json")) {
        removeBackupPayload(backup_path, config.compress);
        Q_EMIT operationError(app_name, "Failed to write backup metadata");
        return std::nullopt;
    }

    Q_EMIT operationCompleted(app_name, true, "Backup completed successfully");
    return entry;
}

namespace {

/// @brief Error describing why a BackupConfig is rejected, or empty when it is accepted.
///
/// Encryption is only applied to compressed archives, and needs a password. Reject either
/// mismatch up front so a requested encryption can never silently produce a readable plaintext
/// archive or copy directory. Registry backup is not implemented; completing a backup that was
/// asked to include registry data would report incomplete work as success, so fail closed until
/// the feature lands (the flag itself is kept, not dropped).
QString backupConfigError(const UserDataManager::BackupConfig& config) {
    if (config.encrypt && !config.compress) {
        return QStringLiteral("Encryption requires compression");
    }
    if (config.encrypt && config.password.isEmpty()) {
        return QStringLiteral("Encryption requires a password");
    }
    if (config.include_registry) {
        return QStringLiteral("Registry backup is not implemented");
    }
    return QString();
}

}  // namespace

bool UserDataManager::validateBackupRequest(const QString& app_name,
                                            const QStringList& source_paths,
                                            const QString& backup_dir,
                                            const BackupConfig& config) {
    // Runtime (not just Q_ASSERT) guards: assertions are compiled out of a release
    // build, so re-check the required inputs here and fail closed.
    if (app_name.isEmpty()) {
        Q_EMIT operationError(app_name, "App name must not be empty");
        return false;
    }
    if (backup_dir.isEmpty()) {
        Q_EMIT operationError(app_name, "Backup directory must not be empty");
        return false;
    }
    if (source_paths.isEmpty()) {
        Q_EMIT operationError(app_name, "No source paths specified");
        return false;
    }
    const QDir dir(backup_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        Q_EMIT operationError(app_name, "Failed to create backup directory");
        return false;
    }
    const QString config_error = backupConfigError(config);
    if (!config_error.isEmpty()) {
        Q_EMIT operationError(app_name, config_error);
        return false;
    }
    return true;
}

bool UserDataManager::writeBackupPayload(const QString& app_name,
                                         const QStringList& source_paths,
                                         const QString& backup_path,
                                         const BackupConfig& config) {
    if (config.compress) {
        if (!createArchive(source_paths, backup_path, config)) {
            Q_EMIT operationError(app_name, "Failed to create archive");
            return false;
        }
        return true;
    }
    if (!copySourcesToDest(source_paths, backup_path, config.exclude_patterns)) {
        Q_EMIT operationError(app_name, "Failed to copy directory");
        return false;
    }
    return true;
}

QString UserDataManager::uniqueBackupPath(const QString& backup_dir,
                                          const QString& base_name,
                                          bool compress) {
    const QDir dir(backup_dir);
    const QString suffix = compress ? QStringLiteral(".zip") : QString();
    QString candidate = dir.filePath(base_name + suffix);
    int counter = 1;
    // A backup is its payload plus a ".json" sidecar; treat either already
    // existing as a collision so a prior backup that shares this one-second
    // timestamp is never overwritten.
    while (QFileInfo::exists(candidate) || QFileInfo::exists(candidate + ".json")) {
        candidate = dir.filePath(QString("%1_%2%3").arg(base_name).arg(counter).arg(suffix));
        ++counter;
    }
    return candidate;
}

void UserDataManager::removeBackupPayload(const QString& backup_path, bool compress) {
    if (compress) {
        QFile::remove(backup_path);
    } else {
        QDir(backup_path).removeRecursively();
    }
}

UserDataManager::BackupEntry UserDataManager::buildBackupResult(const QString& app_name,
                                                                const QStringList& source_paths,
                                                                const QString& archive_path,
                                                                qint64 total_size,
                                                                const BackupConfig& config) {
    BackupEntry entry;
    entry.app_name = app_name;
    entry.source_paths = source_paths;
    entry.backup_path = archive_path;
    entry.backup_date = QDateTime::currentDateTime();
    entry.total_size_bytes = total_size;
    entry.compressed_size_bytes = config.compress ? QFileInfo(archive_path).size() : total_size;
    entry.encrypted = config.encrypt;
    entry.excluded_patterns = config.exclude_patterns;
    return entry;
}

bool UserDataManager::allPathsPresent(std::initializer_list<QString> paths) {
    // An empty list is vacuously "all present" under none_of -- reject it so a caller
    // that assembled no paths cannot pass the guard.
    if (paths.size() == 0) {
        return false;
    }
    // A whitespace-only path is not empty but QDir still treats it as the CWD, so trim
    // before the emptiness test and fail closed on either.
    return std::none_of(paths.begin(), paths.end(), [](const QString& p) {
        return p.trimmed().isEmpty();
    });
}

bool UserDataManager::encryptedArchiveSizeOk(qint64 size) {
    constexpr qint64 kMaxEncryptedArchiveBytes = 4LL * 1024 * 1024 * 1024;
    return size >= 0 && size <= kMaxEncryptedArchiveBytes;
}

bool UserDataManager::backupMultipleApps(const QStringList& app_names,
                                         const QString& backup_dir,
                                         const BackupConfig& config) {
    if (!allPathsPresent({backup_dir}) || app_names.isEmpty()) {
        Q_EMIT operationError(QStringLiteral("Unknown"),
                              QStringLiteral("Empty app list or backup directory"));
        return false;
    }
    bool all_success = true;

    for (const auto& app_name : app_names) {
        // Discover data paths
        auto paths = discoverAppDataPaths(app_name);
        if (paths.isEmpty()) {
            sak::logWarning("[UserDataManager] No data paths found for {}", app_name.toStdString());
            Q_EMIT operationError(app_name, "No data paths found");
            all_success = false;
            continue;
        }

        // Backup each app
        if (!backupAppData(app_name, paths, backup_dir, config)) {
            all_success = false;
        }
    }

    return all_success;
}

bool UserDataManager::restoreAppData(const QString& backup_path,
                                     const QString& restore_dir,
                                     const RestoreConfig& config) {
    if (!allPathsPresent({backup_path, restore_dir})) {
        Q_EMIT operationError(QStringLiteral("Unknown"),
                              QStringLiteral("Empty backup path or restore directory"));
        return false;
    }
    // Read metadata
    auto entry_opt = readMetadata(backup_path + ".json");
    if (!entry_opt.has_value()) {
        Q_EMIT operationError("Unknown", "Failed to read backup metadata");
        return false;
    }

    auto entry = entry_opt.value();
    Q_EMIT operationStarted(entry.app_name, "restore");

    // Verify integrity. A compressed archive MUST carry a checksum when verification
    // is requested; an emptied sidecar cannot be used to silently skip the check.
    if (!verifyRestoreIntegrity(backup_path, entry, config)) {
        return false;
    }

    // Create backup of existing data. Fail closed: if a complete safety copy
    // cannot be made, abort before the -Force extraction below overwrites the
    // only intact original with an incomplete/absent backup.
    if (config.create_backup) {
        const QDir restore(restore_dir);
        if (restore.exists()) {
            const QString backup_name = "backup_" +
                                        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            const QString backup_existing = restore.filePath("../" + backup_name);
            if (!copyDirectory(restore_dir, backup_existing, {})) {
                Q_EMIT operationError(entry.app_name,
                                      "Failed to back up existing data before restore");
                return false;
            }
        }
    }

    if (!restorePayload(backup_path, restore_dir, entry, config)) {
        return false;
    }

    Q_EMIT operationCompleted(entry.app_name, true, "Restore completed successfully");
    return true;
}

bool UserDataManager::restorePayload(const QString& backup_path,
                                     const QString& restore_dir,
                                     const BackupEntry& entry,
                                     const RestoreConfig& config) {
    if (backup_path.endsWith(".zip")) {
        Q_EMIT progressUpdate(0, entry.compressed_size_bytes, "Extracting archive...");
        if (!extractArchive(backup_path, restore_dir, config)) {
            Q_EMIT operationError(entry.app_name, "Failed to extract archive");
            return false;
        }
        return true;
    }
    // Uncompressed backups are a directory payload: copy the tree back in, honoring the
    // overwrite_existing flag (the old copy always refused to replace existing files). An existing
    // file is replaced only when overwrite is requested, otherwise it is left in place (Skip).
    Q_EMIT progressUpdate(0, entry.total_size_bytes, "Copying backup...");
    const ExistingFilePolicy restore_policy =
        config.overwrite_existing ? ExistingFilePolicy::Overwrite : ExistingFilePolicy::Skip;
    if (!copyDirectory(backup_path, restore_dir, {}, restore_policy)) {
        Q_EMIT operationError(entry.app_name, "Failed to copy backup contents");
        return false;
    }
    return true;
}

bool UserDataManager::verifyRestoreIntegrity(const QString& backup_path,
                                             const BackupEntry& entry,
                                             const RestoreConfig& config) {
    if (!config.verify_checksum) {
        return true;
    }
    // A compressed archive is always checksummed when integrity is requested at backup
    // time, so an empty checksum on a .zip means the sidecar was tampered (emptied to
    // disable verification) or made without integrity. We were ASKED to verify and
    // cannot -- fail closed rather than restore an unverified/swapped payload.
    if (backup_path.endsWith(QStringLiteral(".zip")) && entry.checksum.isEmpty()) {
        Q_EMIT operationError(entry.app_name,
                              "Compressed backup has no checksum to verify - refusing restore");
        return false;
    }
    if (entry.checksum.isEmpty()) {
        return true;  // Directory payload: no per-archive checksum is recorded.
    }
    Q_EMIT progressUpdate(0, kPercentMax, "Verifying backup integrity...");
    if (generateChecksum(backup_path) != entry.checksum) {
        Q_EMIT operationError(entry.app_name, "Checksum mismatch - backup may be corrupted");
        return false;
    }
    return true;
}

bool UserDataManager::restoreMultipleApps(const QStringList& backup_paths,
                                          const QString& restore_dir,
                                          const RestoreConfig& config) {
    if (!allPathsPresent({restore_dir}) || backup_paths.isEmpty()) {
        Q_EMIT operationError(QStringLiteral("Unknown"),
                              QStringLiteral("Empty backup list or restore directory"));
        return false;
    }
    bool all_success = true;

    for (const auto& backup_path : backup_paths) {
        if (!restoreAppData(backup_path, restore_dir, config)) {
            all_success = false;
        }
    }

    return all_success;
}

QStringList UserDataManager::discoverAppDataPaths(const QString& app_name) const {
    // Fail closed on an empty name: every name variant would then be "", and
    // QDir(base).filePath("") is the BASE directory itself -- which exists -- so the standard
    // data roots would be reported as this app's data. A const method has no signal to emit,
    // so the refusal is logged.
    if (app_name.isEmpty()) {
        sak::logWarning("[UserDataManager] discoverAppDataPaths refused: empty app name");
        return {};
    }
    QStringList paths;
    const QStringList base_dirs = getStandardDataPaths();

    // Common name variations
    QString nospace = app_name;
    nospace.replace(" ", "");
    QString underscore = app_name;
    underscore.replace(" ", "_");
    const QStringList name_variants = {app_name, app_name.toLower(), nospace, underscore};

    // Search in standard locations
    for (const auto& base : base_dirs) {
        appendExistingVariants(paths, base, name_variants);
    }

    return paths;
}

std::vector<UserDataManager::DataLocation> UserDataManager::getCommonDataLocations() const {
    std::vector<DataLocation> locations;

    const QString appdata_local =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString appdata_roaming =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    // Chrome
    locations.push_back({.pattern = "Google Chrome",
                         .paths = {appdata_local + "/../Google/Chrome/User Data"},
                         .description = "Browser profile, history, bookmarks, extensions"});

    // Firefox
    locations.push_back({.pattern = "Mozilla Firefox",
                         .paths = {appdata_roaming + "/../Mozilla/Firefox/Profiles"},
                         .description = "Browser profile, history, bookmarks, extensions"});

    // VS Code
    locations.push_back({.pattern = "Visual Studio Code",
                         .paths = {appdata_roaming + "/../Code/User"},
                         .description = "Settings, keybindings, extensions, snippets"});

    // BitLocker Recovery Keys (sentinel path -- handled specially by backup wizard)
    locations.push_back({.pattern = "BitLocker Recovery Keys",
                         .paths = {"bitlocker://recovery-keys"},
                         .description = "BitLocker recovery keys for all encrypted volumes"});

    return locations;
}

QStringList UserDataManager::scanForAppData(const QString& app_name) const {
    // Fail closed on an empty name: QString::contains("") is true for every path, so the scan
    // would report EVERY directory under the standard data roots as this app's data.
    if (app_name.isEmpty()) {
        sak::logWarning("[UserDataManager] scanForAppData refused: empty app name");
        return {};
    }
    QStringList found_paths;
    const QStringList search_dirs = getStandardDataPaths();

    for (const auto& dir : search_dirs) {
        QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
        while (it.hasNext()) {
            const QString path = it.next();
            if (path.contains(app_name, Qt::CaseInsensitive)) {
                found_paths.append(path);
            }
        }
    }

    return found_paths;
}

std::vector<UserDataManager::BackupEntry> UserDataManager::listBackups(
    const QString& backup_dir) const {
    // Fail closed on an empty directory: QDir("") is the process working directory, so the
    // listing would advertise whatever happens to sit next to the running executable as backups.
    if (backup_dir.isEmpty()) {
        sak::logWarning("[UserDataManager] listBackups refused: empty backup directory");
        return {};
    }
    std::vector<BackupEntry> backups;

    const QDir dir(backup_dir);
    // Compressed backups are *.zip files; uncompressed backups are directories.
    // Both carry a "<name>.json" sidecar, so entries without one are skipped
    // below and unrelated files/directories are ignored.
    auto files = dir.entryList({"*.zip"}, QDir::Files, QDir::Time);
    files += dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);

    for (const auto& file : files) {
        const QString metadata_path = dir.filePath(file) + ".json";
        if (!QFile::exists(metadata_path)) {
            continue;
        }

        auto entry = readMetadata(metadata_path);
        if (entry.has_value()) {
            backups.push_back(entry.value());
        }
    }

    return backups;
}

UserDataManager::BackupEntry UserDataManager::getBackupInfo(const QString& backup_path) const {
    // Fail closed on an empty path: the sidecar lookup would otherwise read a bare ".json"
    // relative to the working directory. An empty entry is this method's "not found" value.
    if (backup_path.isEmpty()) {
        sak::logWarning("[UserDataManager] getBackupInfo refused: empty backup path");
        return BackupEntry{};
    }
    auto entry = readMetadata(backup_path + ".json");
    return entry.value_or(BackupEntry{});
}

QString UserDataManager::backupDeletionRefusal(const QString& backup_path,
                                               const std::optional<QString>& recorded_backup_path) {
    // Layer 1: shared root/UNC/device/reparse/system-tree screens (the same fail-closed guard
    // the leftover-cleanup deleter uses). Refuses drive roots, the Windows tree, shared user/
    // system roots, UNC/device paths, and symlink/junction paths (leaf OR ancestor) so an
    // ancestor-junction swap cannot redirect the recursive delete outside the intended target.
    const QString screen = sak::filePathDeletionRefusal(backup_path);
    if (!screen.isEmpty()) {
        return screen;
    }
    // Layer 2: metadata identity. Only a directory the manager itself created as a backup carries
    // a sidecar whose recorded backup_path names this exact object. No sidecar (or one that points
    // elsewhere) => an unmanaged tree, never deleted.
    if (!recorded_backup_path.has_value()) {
        return QStringLiteral("no backup metadata sidecar identifies this path");
    }
    if (!sameBackupObject(backup_path, recorded_backup_path.value())) {
        return QStringLiteral("backup metadata does not identify this target");
    }
    return {};
}

bool UserDataManager::deleteBackup(const QString& backup_path) {
    const QString metadata = backup_path + ".json";
    std::optional<QString> recorded;
    // Only read the sidecar once the raw string clears the screens: reading it is a stat/open on
    // the target, and a UNC/device path must be rejected LEXICALLY first (a bare open on
    // \\host\share leaks an NTLM hash). filePathDeletionRefusal is pure/deterministic, so the seam
    // re-checks it below without a second stat mattering.
    if (sak::filePathDeletionRefusal(backup_path).isEmpty()) {
        if (auto entry = readMetadata(metadata); entry.has_value()) {
            recorded = entry->backup_path;
        }
    }

    const QString refusal = backupDeletionRefusal(backup_path, recorded);
    if (!refusal.isEmpty()) {
        Q_EMIT operationError(QFileInfo(backup_path).fileName(),
                              QStringLiteral("Refusing to delete backup: ") + refusal);
        return false;
    }

    bool success = true;
    // Delete the payload. An uncompressed backup is a DIRECTORY, which QFile::remove cannot
    // delete -- it would leave the backup on disk forever; remove such payloads recursively.
    const QFileInfo payload(backup_path);
    if (payload.isDir()) {
        success &= QDir(backup_path).removeRecursively();
    } else if (payload.exists()) {
        success &= QFile::remove(backup_path);
    }

    if (QFile::exists(metadata)) {
        success &= QFile::remove(metadata);
    }

    return success;
}

bool UserDataManager::verifyBackup(const QString& backup_path) {
    auto entry = readMetadata(backup_path + ".json");
    if (!entry.has_value()) {
        return false;
    }

    // Metadata alone is not a valid backup: the payload must actually exist. Otherwise a stray
    // .json (payload deleted/never written) verifies as good and a restore then finds nothing.
    if (!QFileInfo::exists(backup_path)) {
        return false;
    }

    if (entry->checksum.isEmpty()) {
        return true;  // Payload present but no checksum recorded (e.g. directory payload).
    }

    const QString current = generateChecksum(backup_path);
    return !current.isEmpty() && current == entry->checksum;
}

qint64 UserDataManager::calculateSize(const QStringList& paths) const {
    qint64 total = 0;

    for (const auto& path : paths) {
        const QFileInfo info(path);
        if (info.isFile()) {
            total += info.size();
        } else if (info.isDir()) {
            QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                total += it.fileInfo().size();
            }
        }
    }

    return total;
}

QString UserDataManager::generateChecksum(const QString& file_path) const {
    return calculateSHA256(file_path);
}

bool UserDataManager::compareChecksums(const QString& file1, const QString& file2) const {
    const QString hash1 = generateChecksum(file1);
    const QString hash2 = generateChecksum(file2);
    // Two UNREADABLE files both hash to "" -- that must NOT be reported as "equal". Require a real
    // (non-empty) digest from both before comparing.
    return !hash1.isEmpty() && hash1 == hash2;
}

QString UserDataManager::mapCompressionLevel(int level) {
    if (level == 0) {
        return QStringLiteral("NoCompression");
    }
    if (level <= kFastCompressionMaxLevel) {
        return QStringLiteral("Fastest");
    }
    return QStringLiteral("Optimal");  // PowerShell doesn't have higher levels
}

bool UserDataManager::encryptArchiveInPlace(const QString& archive_path,
                                            const BackupConfig& config) {
    // Read original archive
    QFile archive(archive_path);
    if (!archive.open(QIODevice::ReadOnly)) {
        sak::logError("[UserDataManager] Failed to open archive for reading: {}",
                      archive_path.toStdString());
        return false;
    }
    const QByteArray data = archive.readAll();
    archive.close();

    // Encrypt data
    auto encrypted = sak::encryptData(data, config.password);
    if (!encrypted) {
        sak::logWarning("[UserDataManager] Encryption failed: {}",
                        static_cast<int>(encrypted.error()));
        if (!QFile::remove(archive_path)) {
            sak::logError("[UserDataManager] Failed to remove archive after encryption failure: {}",
                          archive_path.toStdString());
        }
        return false;
    }

    // Write to a sibling temp, then atomically replace the plaintext archive. Truncating
    // the only archive in place would, on a crash mid-write, leave an unrecoverable partial
    // file; staging keeps the plaintext intact until the encrypted copy is fully written.
    // On any failure remove BOTH temp and plaintext so a readable archive is never left
    // behind when encryption was requested.
    const QString tmp = archive_path + QStringLiteral(".enc_tmp");
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sak::logWarning("[UserDataManager] Failed to open temp for encrypted file");
        QFile::remove(archive_path);
        return false;
    }
    if (out.write(*encrypted) != encrypted->size()) {
        sak::logWarning("[UserDataManager] Incomplete write of encrypted file");
        out.close();
        QFile::remove(tmp);
        QFile::remove(archive_path);
        return false;
    }
    out.close();
    if (!atomicReplaceFile(tmp, archive_path)) {
        QFile::remove(archive_path);
        return false;
    }
    return true;
}

bool UserDataManager::compressSourcePaths(const QStringList& source_paths,
                                          const QString& archive_path,
                                          int compression_level) {
    // System32-qualified interpreter, never a bare "powershell.exe": a backup can be
    // taken from an elevated session, so a PATH/CWD-planted powershell must not be able
    // to satisfy the launch. Unresolvable -> refuse the archive (fail closed).
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        sak::logError("[UserDataManager] Cannot resolve the System32 PowerShell path");
        return false;
    }

    const QString compressionLevel = mapCompressionLevel(compression_level);

    // Use PowerShell's Compress-Archive for Windows.
    QStringList args;
    args << "-NoProfile" << "-Command";

    // Escape single quotes in paths to prevent PowerShell injection.
    QStringList escaped_sources;
    for (const auto& path : source_paths) {
        escaped_sources << QString(path).replace("'", "''");
    }
    const QString sources = escaped_sources.join("','");
    const QString safe_archive = QString(archive_path).replace("'", "''");
    const QString command = QString(
                                "Compress-Archive -Path '%1' -DestinationPath '%2' "
                                "-CompressionLevel %3 -Force")
                                .arg(sources, safe_archive, compressionLevel);
    args << command;

    const auto result = sak::runProcess(powershell,
                                        args,
                                        sak::kTimeoutArchiveMs);  // 5 minute timeout
    if (result.timed_out) {
        sak::logError("Archive compression timed out after 5 minutes -- killing process");
        return false;
    }
    if (result.exit_code != 0 || !QFile::exists(archive_path)) {
        sak::logError("Archive compression failed: exit code {}, archive exists: {}",
                      result.exit_code,
                      QFile::exists(archive_path));
        return false;
    }
    return true;
}

bool UserDataManager::buildArchivePayload(const QStringList& source_paths,
                                          const QString& archive_path,
                                          const BackupConfig& config) {
    if (config.exclude_patterns.isEmpty()) {
        return compressSourcePaths(source_paths, archive_path, config.compression_level);
    }
    // Compress-Archive has no exclusion support, so it would archive the raw sources
    // verbatim -- a user excluding secrets would still ship them. Stage a filtered copy
    // (same exclusion + collision rules as an uncompressed backup) and compress THAT.
    // Fail closed if the staging copy cannot be completed.
    const QTemporaryDir staging;
    if (!staging.isValid()) {
        sak::logError("[UserDataManager] Failed to create staging dir for filtered archive");
        return false;
    }
    if (!copySourcesToDest(source_paths, staging.path(), config.exclude_patterns)) {
        return false;
    }
    return compressSourcePaths({QDir(staging.path()).filePath("*")},
                               archive_path,
                               config.compression_level);
}

bool UserDataManager::createArchive(const QStringList& source_paths,
                                    const QString& archive_path,
                                    const BackupConfig& config) {
    // Invariants of the single call path (writeBackupPayload, from backupAppData):
    // validateBackupRequest already rejected an empty source_paths list, and archive_path is a
    // uniqueBackupPath() result -- a QDir::filePath() join, which is never empty.
    Q_ASSERT_X(!source_paths.isEmpty(), "createArchive", "source_paths must not be empty");
    Q_ASSERT_X(!archive_path.isEmpty(), "createArchive", "archive_path must not be empty");

    if (!buildArchivePayload(source_paths, archive_path, config)) {
        return false;
    }

    // Encrypt archive if requested.
    if (config.encrypt && !config.password.isEmpty()) {
        return encryptArchiveInPlace(archive_path, config);
    }
    return true;
}

QString UserDataManager::decryptArchiveToTempFile(const QString& archive_path,
                                                  const QString& password) {
    // Read encrypted archive
    QFile archive(archive_path);
    if (!archive.open(QIODevice::ReadOnly)) {
        sak::logError("[UserDataManager] Failed to open encrypted archive for reading: {}",
                      archive_path.toStdString());
        return {};
    }
    // archive_path is an attacker-controlled backup file; readAll() would pull the whole file
    // into memory BEFORE the zip-bomb preflight (which only runs on the decrypted .zip) ever
    // sees it. Refuse an oversized/unreadable ciphertext outright -- fail closed rather than
    // risk OOM. 4 GiB comfortably covers a legitimate compressed backup while bounding the read.
    if (!encryptedArchiveSizeOk(archive.size())) {
        sak::logError("[UserDataManager] Encrypted archive too large or unreadable: {} bytes",
                      static_cast<long long>(archive.size()));
        return {};
    }
    const QByteArray encrypted_data = archive.readAll();
    archive.close();

    // Decrypt data
    auto decrypted = sak::decryptData(encrypted_data, password);
    if (!decrypted) {
        sak::logWarning("[UserDataManager] Decryption failed: {}",
                        static_cast<int>(decrypted.error()));
        return {};
    }

    // QTemporaryFile gives us an OS-generated unique name, preventing adversaries from predicting
    // the path and racing to read the plaintext (TOCTOU mitigation). The name MUST end in .zip:
    // Expand-Archive (below) refuses any source whose extension is not .zip, so an extensionless
    // temp made encrypted backups impossible to restore. The XXXXXX keeps the random unique part.
    QTemporaryFile temp(QDir::tempPath() + QStringLiteral("/sak_udm_XXXXXX.zip"));
    // AutoRemove is disabled because we must keep the file alive until
    // PowerShell finishes reading it -- cleanup is handled manually on
    // every exit path in extractArchive().
    temp.setAutoRemove(false);
    if (!temp.open()) {
        sak::logError("[UserDataManager] Failed to create temporary file for decryption");
        return {};
    }
    QString temp_path = temp.fileName();
    if (temp.write(*decrypted) != decrypted->size()) {
        sak::logError("[UserDataManager] Incomplete write of decrypted temporary file");
        // AutoRemove is off, so a partial plaintext would otherwise linger in
        // TEMP; delete whatever bytes were written before returning.
        temp.close();
        QFile::remove(temp_path);
        return {};
    }
    temp.close();

    return temp_path;
}

bool UserDataManager::archiveWithinLimits(const QString& archive_path) {
    // System32-qualified interpreter only; an unresolvable path means the preflight
    // cannot run, so the archive is REFUSED rather than extracted unchecked.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        sak::logError("[UserDataManager] Cannot resolve the System32 PowerShell path");
        return false;
    }

    // Zip-bomb guard: a hostile backup .zip can inflate to exhaust the disk. Enumerate
    // the archive first and refuse once the entry count or total DECOMPRESSED size
    // exceeds the cap, BEFORE Expand-Archive writes anything into the live destination.
    constexpr qint64 kMaxEntries = 100'000;
    constexpr qint64 kMaxDecompressedBytes = 8LL * 1024 * 1024 * 1024;  // 8 GiB
    const QString safe = QString(archive_path).replace("'", "''");
    const QString command =
        QString(
            "$ErrorActionPreference='Stop'; "
            "Add-Type -AssemblyName System.IO.Compression.FileSystem; "
            "$zip=[System.IO.Compression.ZipFile]::OpenRead('%1'); "
            "try { $n=0; $sz=0; foreach($e in $zip.Entries){ $n++; $sz=$sz+$e.Length; "
            "if($n -gt %2){ throw 'archive entry count exceeds limit' } "
            "if($sz -gt %3){ throw 'archive decompressed size exceeds limit' } } } "
            "finally { $zip.Dispose() }")
            .arg(safe)
            .arg(kMaxEntries)
            .arg(kMaxDecompressedBytes);
    QStringList args;
    args << "-NoProfile" << "-Command" << command;
    const auto result = sak::runProcess(powershell, args, sak::kTimeoutArchiveMs);
    if (result.timed_out) {
        sak::logError("[UserDataManager] Archive preflight timed out");
        return false;
    }
    if (!result.succeeded()) {
        sak::logError("[UserDataManager] Archive rejected by zip-bomb preflight (exit {})",
                      result.exit_code);
        return false;
    }
    return true;
}

bool UserDataManager::extractArchive(const QString& archive_path,
                                     const QString& destination,
                                     const RestoreConfig& config) {
    // Invariants (sole caller restorePayload): a ".zip" path; restore_dir passed allPathsPresent.
    Q_ASSERT_X(!archive_path.isEmpty(), "extractArchive", "archive_path must not be empty");
    Q_ASSERT_X(!destination.isEmpty(), "extractArchive", "destination must not be empty");
    // System32-qualified interpreter only; refuse before any plaintext temp is written.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        sak::logError("[UserDataManager] Cannot resolve the System32 PowerShell path");
        return false;
    }
    QString file_to_extract = archive_path;
    QString temp_decrypted;

    // Decrypt if password provided
    if (!config.password.isEmpty()) {
        temp_decrypted = decryptArchiveToTempFile(archive_path, config.password);
        if (temp_decrypted.isEmpty()) {
            return false;
        }
        file_to_extract = temp_decrypted;
    }

    // Refuse a zip-bomb before extracting into the live destination. Clean up the
    // decrypted temp (if any) on refusal so no plaintext lingers.
    if (!archiveWithinLimits(file_to_extract)) {
        if (!temp_decrypted.isEmpty()) {
            QFile::remove(temp_decrypted);
        }
        return false;
    }

    // Use PowerShell's Expand-Archive for Windows
    QStringList args;
    // -NoProfile avoids loading the user's PS profile, which could alter
    // execution behaviour or add unwanted delays during restore.
    args << "-NoProfile" << "-Command";

    // Doubling single-quotes is PowerShell's escape for literal quotes
    // inside a single-quoted string; this neutralises paths containing
    // apostrophes without opening a code-injection vector.
    const QString safe_source = QString(file_to_extract).replace("'", "''");
    const QString safe_dest = QString(destination).replace("'", "''");
    // Honor RestoreConfig::overwrite_existing: -Force overwrites existing files, without it
    // Expand-Archive refuses to clobber them. The old code hard-coded -Force, silently overwriting
    // regardless of the flag.
    const QString force = config.overwrite_existing ? QStringLiteral(" -Force") : QString();
    const QString command = QString("Expand-Archive -Path '%1' -DestinationPath '%2'%3")
                                .arg(safe_source, safe_dest, force);

    args << command;

    const auto result = sak::runProcess(powershell,
                                        args,
                                        sak::kTimeoutArchiveMs);  // 5 minute timeout
    if (result.timed_out) {
        if (!temp_decrypted.isEmpty()) {
            QFile::remove(temp_decrypted);
        }
        return false;
    }

    // Clean up temporary decrypted file
    if (!temp_decrypted.isEmpty()) {
        QFile::remove(temp_decrypted);
    }

    return result.succeeded();
}

bool UserDataManager::isExcluded(const QString& path, const QStringList& patterns) const {
    const QString normalized = QDir::fromNativeSeparators(path);
    return std::any_of(patterns.begin(), patterns.end(), [&normalized](const auto& pattern) {
        // Match wildcards UNANCHORED against the full path. isExcluded is fed absolute
        // paths, and a DEFAULT (anchored, path-aware) conversion of a glob such as
        // "*.log" or "cache/*" only ever matches a lone path segment -- so against an
        // absolute path it matches NOTHING and silently excludes nothing, letting the
        // very "secret" files a user asked to drop slip into the backup (fail-open).
        // Unanchored conversion lets the glob match the relevant tail/segment of a
        // nested path, so exclusions actually take effect. (Available since Qt 6.0.)
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(
            pattern, QRegularExpression::UnanchoredWildcardConversion));
        return re.match(normalized).hasMatch();
    });
}

QStringList UserDataManager::getStandardDataPaths() const {
    QStringList paths;

    // AppData Local
    paths.append(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/..");

    // AppData Roaming
    paths.append(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/..");

    // ProgramData
    paths.append(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));

    // Documents
    paths.append(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));

    // User home
    paths.append(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));

    return paths;
}

bool UserDataManager::copySourcesToDest(const QStringList& source_paths,
                                        const QString& dest_dir,
                                        const QStringList& exclude_patterns) {
    return std::all_of(source_paths.begin(), source_paths.end(), [&](const auto& source) {
        // Individual ENTRIES arrive unvalidated (validateBackupRequest only rejects an empty
        // LIST), and QDir("") is the process working directory -- an empty entry would copy
        // the CWD into the backup. Refuse here, before copyDirectory relies on it.
        if (!allPathsPresent({source, dest_dir})) {
            sak::logWarning("[UserDataManager] copySourcesToDest refused: empty source or dest");
            return false;
        }
        return copyDirectory(source, dest_dir, exclude_patterns);
    });
}

bool UserDataManager::atomicReplaceFile(const QString& tmp, const QString& target) {
#ifdef SAK_PLATFORM_WINDOWS
    // MoveFileExW performs the replace as a single atomic rename: the destination name
    // always resolves to a complete file (the old one or the new one), so unlike the
    // rename-aside dance below there is no crash window in which target is absent. tmp is
    // a sibling of target (same volume), so this is a metadata rename, never a copy.
    // WRITE_THROUGH flushes before returning; on failure target is left untouched, so drop
    // the staged tmp and fail closed.
    if (MoveFileExW(reinterpret_cast<const wchar_t*>(tmp.utf16()),
                    reinterpret_cast<const wchar_t*>(target.utf16()),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        QFile::remove(tmp);
        return false;
    }
    return true;
#else
    // Move the existing target ASIDE instead of deleting it: if renaming the staged replacement
    // onto target fails, roll the original back into place. The original is never destroyed while
    // the swap is incomplete (fail closed, no data-loss window that the old delete-then-rename
    // left when a rename failed between the two steps).
    const QString backup = target + QStringLiteral(".sak_old");
    QFile::remove(backup);
    const bool had_target = QFileInfo::exists(target);
    if (had_target && !QFile::rename(target, backup)) {
        QFile::remove(tmp);
        return false;
    }
    if (!QFile::rename(tmp, target)) {
        if (had_target) {
            QFile::rename(backup, target);  // restore the untouched original
        }
        QFile::remove(tmp);
        return false;
    }
    QFile::remove(backup);
    return true;
#endif
}

bool UserDataManager::overwriteFile(const QString& source_file, const QString& dest_file) {
    // Stage the replacement beside the target first: if this copy fails the original
    // destination is still fully intact (no truncate/remove has happened yet).
    const QString tmp = dest_file + QStringLiteral(".sak_tmp");
    QFile::remove(tmp);
    if (!QFile::copy(source_file, tmp)) {
        return false;
    }
    return atomicReplaceFile(tmp, dest_file);
}

bool UserDataManager::copyPlainFiles(const QDir& source_dir,
                                     const QDir& dest_dir,
                                     const QStringList& exclude_patterns,
                                     ExistingFilePolicy policy) {
    const auto files = source_dir.entryList(QDir::Files);
    for (const auto& file : files) {
        const QString source_file = source_dir.filePath(file);
        if (isExcluded(source_file, exclude_patterns)) {
            continue;
        }

        const QString dest_file = dest_dir.filePath(file);
        // QFile::copy fails if the destination exists, so an existing file is handled per policy:
        // Skip leaves it (restore, overwrite_existing=false); Overwrite replaces it; Fail treats it
        // as a collision (backup creation: two distinct sources must not merge into one name).
        if (QFileInfo::exists(dest_file)) {
            if (policy == ExistingFilePolicy::Skip) {
                continue;
            }
            if (policy == ExistingFilePolicy::Fail) {
                sak::logWarning("[UserDataManager] Destination already exists (collision): {}",
                                dest_file.toStdString());
                return false;
            }
            // Overwrite WITHOUT a data-loss window: overwriteFile copies to a temp and
            // atomically swaps, so a failed copy never destroys the original file.
            if (!overwriteFile(source_file, dest_file)) {
                sak::logWarning("[UserDataManager] Failed to replace {}", dest_file.toStdString());
                return false;
            }
            continue;
        }
        if (!QFile::copy(source_file, dest_file)) {
            // Fail closed: a partial copy must not be reported as a complete
            // backup (callers such as the pre-restore safety copy rely on this).
            sak::logWarning("[UserDataManager] Failed to copy {}", source_file.toStdString());
            return false;
        }
    }
    return true;
}

bool UserDataManager::copyDirectory(const QString& source,
                                    const QString& destination,
                                    const QStringList& exclude_patterns,
                                    ExistingFilePolicy policy) {
    // Invariants of every call site: copySourcesToDest rejects an empty source_paths ENTRY;
    // restoreAppData/restorePayload pass dirs that allPathsPresent already accepted, EXCEPT
    // the backup_existing destination at restoreAppData, which allPathsPresent never sees -
    // that one is a QDir::filePath() join and so is non-empty for the same reason as the
    // recursive call below.
    Q_ASSERT_X(!source.isEmpty(), "copyDirectory", "source must not be empty");
    Q_ASSERT_X(!destination.isEmpty(), "copyDirectory", "destination must not be empty");
    const QDir source_dir(source);
    if (!source_dir.exists()) {
        return false;
    }
    // An unreadable directory enumerates as EMPTY, indistinguishable from a truly empty
    // one, so a copy would silently omit its contents yet report success. Fail closed:
    // if the source cannot be read, the backup/restore is incomplete and must not pass.
    if (!QFileInfo(source).isReadable()) {
        sak::logWarning("[UserDataManager] Source directory not readable: {}",
                        source.toStdString());
        return false;
    }

    const QDir dest_dir(destination);
    if (!dest_dir.exists() && !dest_dir.mkpath(".")) {
        return false;
    }

    if (!copyPlainFiles(source_dir, dest_dir, exclude_patterns, policy)) {
        return false;
    }

    // Copy subdirectories
    auto dirs = source_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& dir : dirs) {
        const QString source_subdir = source_dir.filePath(dir);
        if (isExcluded(source_subdir, exclude_patterns)) {
            continue;
        }

        // Skip directory reparse points (junctions / symlinks). Descending into
        // one would follow it outside the source root -- pulling in arbitrary
        // external data -- or loop forever if it targets an ancestor.
        if (isReparsePoint(source_subdir)) {
            // Record the omission at warning level: a junction/symlink is deliberately
            // NOT followed, so its contents are absent from the backup by design and the
            // operator should be able to see what was left out.
            sak::logWarning("[UserDataManager] Skipping reparse point (not backed up): {}",
                            source_subdir.toStdString());
            continue;
        }

        const QString dest_subdir = dest_dir.filePath(dir);
        if (!copyDirectory(source_subdir, dest_subdir, exclude_patterns, policy)) {
            return false;
        }
    }

    return true;
}

QString UserDataManager::calculateSHA256(const QString& file_path) const {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);

    // Read in chunks to handle large files. A transient read error must NOT yield a
    // hash over partial content (which would then compare unequal in a benign case or,
    // worse, be recorded as the backup's authoritative digest): fail closed instead.
    const qint64 chunk_size = 1024 * 1024;  // 1 MB
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(chunk_size);
        if (file.error() != QFileDevice::NoError) {
            sak::logWarning("[UserDataManager] Read error while hashing {}",
                            file_path.toStdString());
            return QString();
        }
        if (chunk.isEmpty()) {
            break;  // No error and nothing read -> guard against a non-advancing loop.
        }
        hash.addData(chunk);
    }

    return QString(hash.result().toHex());
}

bool UserDataManager::writeMetadata(const BackupEntry& entry, const QString& metadata_path) {
    // Invariant of the single call path (backupAppData): buildBackupResult copies app_name
    // straight from the request that validateBackupRequest already rejected when empty.
    Q_ASSERT_X(!entry.app_name.isEmpty(), "writeMetadata", "app_name must not be empty");
    QJsonObject json;
    json["app_name"] = entry.app_name;
    json["app_version"] = entry.app_version;

    QJsonArray paths;
    for (const auto& path : entry.source_paths) {
        paths.append(path);
    }
    json["source_paths"] = paths;

    json["backup_path"] = entry.backup_path;
    json["backup_date"] = entry.backup_date.toString(Qt::ISODate);
    json["total_size"] = entry.total_size_bytes;
    json["compressed_size"] = entry.compressed_size_bytes;
    json["checksum"] = entry.checksum;
    json["encrypted"] = entry.encrypted;

    QJsonArray excluded;
    for (const auto& pattern : entry.excluded_patterns) {
        excluded.append(pattern);
    }
    json["excluded_patterns"] = excluded;

    const QJsonDocument doc(json);

    QFile file(metadata_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    const QByteArray json_bytes = doc.toJson();
    if (file.write(json_bytes) != json_bytes.size()) {
        return false;
    }
    return true;
}

std::optional<UserDataManager::BackupEntry> UserDataManager::readMetadata(
    const QString& metadata_path) const {
    QFile file(metadata_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    // Cap the read: a metadata sidecar is small app-authored JSON. An oversized file is
    // tampered/garbage and readAll()ing it unbounded is a needless memory DoS.
    constexpr qint64 kMaxMetadataBytes = 1LL * 1024 * 1024;  // 1 MiB
    if (file.size() > kMaxMetadataBytes) {
        sak::logWarning("[UserDataManager] Metadata sidecar too large, refusing: {}",
                        metadata_path.toStdString());
        return std::nullopt;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }

    BackupEntry entry;
    if (!parseMetadataObject(doc.object(), entry)) {
        return std::nullopt;  // absent/wrong-typed load-bearing field -> fail closed
    }
    return entry;
}

}  // namespace sak
