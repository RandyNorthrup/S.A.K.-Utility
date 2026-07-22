// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_data_manager.h"

#include "sak/encryption.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
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

/// @brief Append paths that exist for the given name variants in a base directory.
void appendExistingVariants(QStringList& paths,
                            const QString& base_dir,
                            const QStringList& variants) {
    for (const auto& variant : variants) {
        QString path = QDir(base_dir).filePath(variant);
        if (QDir(path).exists()) {
            paths.append(path);
        }
    }
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
    Q_ASSERT_X(!app_name.isEmpty(), "backupAppData", "app_name must not be empty");
    Q_ASSERT_X(!backup_dir.isEmpty(), "backupAppData", "backup_dir must not be empty");
    Q_EMIT operationStarted(app_name, "backup");

    if (!validateBackupRequest(app_name, source_paths, backup_dir, config)) {
        return std::nullopt;
    }

    // Generate a collision-free backup path (payload plus a ".json" sidecar).
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString safe_name = app_name;
    safe_name.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    QString base_name = QString("%1_%2").arg(safe_name, timestamp);
    QString backup_path = uniqueBackupPath(backup_dir, base_name, config.compress);

    // Calculate total size
    qint64 total_size = calculateSize(source_paths);
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

bool UserDataManager::validateBackupRequest(const QString& app_name,
                                            const QStringList& source_paths,
                                            const QString& backup_dir,
                                            const BackupConfig& config) {
    if (source_paths.isEmpty()) {
        Q_EMIT operationError(app_name, "No source paths specified");
        return false;
    }
    QDir dir(backup_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        Q_EMIT operationError(app_name, "Failed to create backup directory");
        return false;
    }
    // Encryption is only applied to compressed archives, and needs a password.
    // Reject either mismatch up front so a requested encryption can never
    // silently produce a readable plaintext archive or copy directory.
    if (config.encrypt && !config.compress) {
        Q_EMIT operationError(app_name, "Encryption requires compression");
        return false;
    }
    if (config.encrypt && config.password.isEmpty()) {
        Q_EMIT operationError(app_name, "Encryption requires a password");
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
    QDir dir(backup_dir);
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

bool UserDataManager::backupMultipleApps(const QStringList& app_names,
                                         const QString& backup_dir,
                                         const BackupConfig& config) {
    Q_ASSERT_X(!app_names.isEmpty(), "backupMultipleApps", "app_names must not be empty");
    Q_ASSERT_X(!backup_dir.isEmpty(), "backupMultipleApps", "backup_dir must not be empty");
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
    Q_ASSERT_X(!backup_path.isEmpty(), "restoreAppData", "backup_path must not be empty");
    Q_ASSERT_X(!restore_dir.isEmpty(), "restoreAppData", "restore_dir must not be empty");
    // Read metadata
    auto entry_opt = readMetadata(backup_path + ".json");
    if (!entry_opt.has_value()) {
        Q_EMIT operationError("Unknown", "Failed to read backup metadata");
        return false;
    }

    auto entry = entry_opt.value();
    Q_EMIT operationStarted(entry.app_name, "restore");

    // Verify checksum
    if (config.verify_checksum && !entry.checksum.isEmpty()) {
        Q_EMIT progressUpdate(0, kPercentMax, "Verifying backup integrity...");
        QString current_checksum = generateChecksum(backup_path);
        if (current_checksum != entry.checksum) {
            Q_EMIT operationError(entry.app_name, "Checksum mismatch - backup may be corrupted");
            return false;
        }
    }

    // Create backup of existing data. Fail closed: if a complete safety copy
    // cannot be made, abort before the -Force extraction below overwrites the
    // only intact original with an incomplete/absent backup.
    if (config.create_backup) {
        QDir restore(restore_dir);
        if (restore.exists()) {
            QString backup_name = "backup_" +
                                  QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            QString backup_existing = restore.filePath("../" + backup_name);
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
    // Uncompressed backups are a directory payload: copy the tree back in.
    Q_EMIT progressUpdate(0, entry.total_size_bytes, "Copying backup...");
    if (!copyDirectory(backup_path, restore_dir, {})) {
        Q_EMIT operationError(entry.app_name, "Failed to copy backup contents");
        return false;
    }
    return true;
}

bool UserDataManager::restoreMultipleApps(const QStringList& backup_paths,
                                          const QString& restore_dir,
                                          const RestoreConfig& config) {
    Q_ASSERT_X(!backup_paths.isEmpty(), "restoreMultipleApps", "backup_paths must not be empty");
    Q_ASSERT_X(!restore_dir.isEmpty(), "restoreMultipleApps", "restore_dir must not be empty");
    bool all_success = true;

    for (const auto& backup_path : backup_paths) {
        if (!restoreAppData(backup_path, restore_dir, config)) {
            all_success = false;
        }
    }

    return all_success;
}

QStringList UserDataManager::discoverAppDataPaths(const QString& app_name) const {
    Q_ASSERT_X(!app_name.isEmpty(), "discoverAppDataPaths", "app_name must not be empty");
    QStringList paths;
    QStringList base_dirs = getStandardDataPaths();

    // Common name variations
    QString nospace = app_name;
    nospace.replace(" ", "");
    QString underscore = app_name;
    underscore.replace(" ", "_");
    QStringList name_variants = {app_name, app_name.toLower(), nospace, underscore};

    // Search in standard locations
    for (const auto& base : base_dirs) {
        appendExistingVariants(paths, base, name_variants);
    }

    return paths;
}

std::vector<UserDataManager::DataLocation> UserDataManager::getCommonDataLocations() const {
    std::vector<DataLocation> locations;

    QString appdata_local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QString appdata_roaming = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    // Chrome
    locations.push_back({"Google Chrome",
                         {appdata_local + "/../Google/Chrome/User Data"},
                         "Browser profile, history, bookmarks, extensions"});

    // Firefox
    locations.push_back({"Mozilla Firefox",
                         {appdata_roaming + "/../Mozilla/Firefox/Profiles"},
                         "Browser profile, history, bookmarks, extensions"});

    // VS Code
    locations.push_back({"Visual Studio Code",
                         {appdata_roaming + "/../Code/User"},
                         "Settings, keybindings, extensions, snippets"});

    // BitLocker Recovery Keys (sentinel path -- handled specially by backup wizard)
    locations.push_back({"BitLocker Recovery Keys",
                         {"bitlocker://recovery-keys"},
                         "BitLocker recovery keys for all encrypted volumes"});

    return locations;
}

QStringList UserDataManager::scanForAppData(const QString& app_name) const {
    Q_ASSERT_X(!app_name.isEmpty(), "scanForAppData", "app_name must not be empty");
    QStringList found_paths;
    QStringList search_dirs = getStandardDataPaths();

    for (const auto& dir : search_dirs) {
        QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
        while (it.hasNext()) {
            QString path = it.next();
            if (path.contains(app_name, Qt::CaseInsensitive)) {
                found_paths.append(path);
            }
        }
    }

    return found_paths;
}

std::vector<UserDataManager::BackupEntry> UserDataManager::listBackups(
    const QString& backup_dir) const {
    Q_ASSERT_X(!backup_dir.isEmpty(), "listBackups", "backup_dir must not be empty");
    std::vector<BackupEntry> backups;

    QDir dir(backup_dir);
    // Compressed backups are *.zip files; uncompressed backups are directories.
    // Both carry a "<name>.json" sidecar, so entries without one are skipped
    // below and unrelated files/directories are ignored.
    auto files = dir.entryList({"*.zip"}, QDir::Files, QDir::Time);
    files += dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);

    for (const auto& file : files) {
        QString metadata_path = dir.filePath(file) + ".json";
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
    Q_ASSERT_X(!backup_path.isEmpty(), "getBackupInfo", "backup_path must not be empty");
    auto entry = readMetadata(backup_path + ".json");
    return entry.value_or(BackupEntry{});
}

bool UserDataManager::deleteBackup(const QString& backup_path) {
    Q_ASSERT_X(!backup_path.isEmpty(), "deleteBackup", "backup_path must not be empty");
    bool success = true;

    // Delete archive
    if (QFile::exists(backup_path)) {
        success &= QFile::remove(backup_path);
    }

    // Delete metadata
    QString metadata = backup_path + ".json";
    if (QFile::exists(metadata)) {
        success &= QFile::remove(metadata);
    }

    return success;
}

bool UserDataManager::verifyBackup(const QString& backup_path) {
    Q_ASSERT_X(!backup_path.isEmpty(), "verifyBackup", "backup_path must not be empty");
    auto entry = readMetadata(backup_path + ".json");
    if (!entry.has_value()) {
        return false;
    }

    if (entry->checksum.isEmpty()) {
        return true;  // No checksum to verify
    }

    QString current = generateChecksum(backup_path);
    return current == entry->checksum;
}

qint64 UserDataManager::calculateSize(const QStringList& paths) const {
    Q_ASSERT(!paths.isEmpty());
    qint64 total = 0;

    for (const auto& path : paths) {
        QFileInfo info(path);
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
    Q_ASSERT_X(!file_path.isEmpty(), "generateChecksum", "file_path must not be empty");
    return calculateSHA256(file_path);
}

bool UserDataManager::compareChecksums(const QString& file1, const QString& file2) const {
    Q_ASSERT_X(!file1.isEmpty(), "compareChecksums", "file1 must not be empty");
    Q_ASSERT_X(!file2.isEmpty(), "compareChecksums", "file2 must not be empty");
    return generateChecksum(file1) == generateChecksum(file2);
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
    QByteArray data = archive.readAll();
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

    // Write encrypted data. On any write failure remove the file so a readable
    // plaintext (or a truncated) archive is never left behind when encryption
    // was requested.
    if (!archive.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sak::logWarning("[UserDataManager] Failed to write encrypted file");
        QFile::remove(archive_path);
        return false;
    }
    if (archive.write(*encrypted) != encrypted->size()) {
        sak::logWarning("[UserDataManager] Incomplete write of encrypted file");
        archive.close();
        QFile::remove(archive_path);
        return false;
    }
    archive.close();
    return true;
}

bool UserDataManager::createArchive(const QStringList& source_paths,
                                    const QString& archive_path,
                                    const BackupConfig& config) {
    Q_ASSERT_X(!source_paths.isEmpty(), "createArchive", "source_paths must not be empty");
    Q_ASSERT_X(!archive_path.isEmpty(), "createArchive", "archive_path must not be empty");

    QString compressionLevel = mapCompressionLevel(config.compression_level);

    // Use PowerShell's Compress-Archive for Windows
    QStringList args;
    args << "-NoProfile" << "-Command";

    // Escape single quotes in paths to prevent PowerShell injection
    QStringList escaped_sources;
    for (const auto& path : source_paths) {
        escaped_sources << QString(path).replace("'", "''");
    }
    QString sources = escaped_sources.join("','");
    QString safe_archive = QString(archive_path).replace("'", "''");
    QString command = QString(
                          "Compress-Archive -Path '%1' -DestinationPath '%2' -CompressionLevel "
                          "%3 -Force")
                          .arg(sources, safe_archive, compressionLevel);

    args << command;

    const auto result = sak::runProcess(QStringLiteral("powershell.exe"),
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

    // Encrypt archive if requested
    if (config.encrypt && !config.password.isEmpty()) {
        if (!encryptArchiveInPlace(archive_path, config)) {
            return false;
        }
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
    QByteArray encrypted_data = archive.readAll();
    archive.close();

    // Decrypt data
    auto decrypted = sak::decryptData(encrypted_data, password);
    if (!decrypted) {
        sak::logWarning("[UserDataManager] Decryption failed: {}",
                        static_cast<int>(decrypted.error()));
        return {};
    }

    // QTemporaryFile gives us an OS-generated unique name, preventing
    // adversaries from predicting the path and racing to read the
    // plaintext (TOCTOU mitigation).
    QTemporaryFile temp;
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

bool UserDataManager::extractArchive(const QString& archive_path,
                                     const QString& destination,
                                     const RestoreConfig& config) {
    Q_ASSERT_X(!archive_path.isEmpty(), "extractArchive", "archive_path must not be empty");
    Q_ASSERT_X(!destination.isEmpty(), "extractArchive", "destination must not be empty");
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

    // Use PowerShell's Expand-Archive for Windows
    QStringList args;
    // -NoProfile avoids loading the user's PS profile, which could alter
    // execution behaviour or add unwanted delays during restore.
    args << "-NoProfile" << "-Command";

    // Doubling single-quotes is PowerShell's escape for literal quotes
    // inside a single-quoted string; this neutralises paths containing
    // apostrophes without opening a code-injection vector.
    QString safe_source = QString(file_to_extract).replace("'", "''");
    QString safe_dest = QString(destination).replace("'", "''");
    QString command = QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
                          .arg(safe_source, safe_dest);

    args << command;

    const auto result = sak::runProcess(QStringLiteral("powershell.exe"),
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
    return std::any_of(patterns.begin(), patterns.end(), [&path](const auto& pattern) {
        QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern));
        return re.match(path).hasMatch();
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
        return copyDirectory(source, dest_dir, exclude_patterns);
    });
}

bool UserDataManager::copyPlainFiles(const QDir& source_dir,
                                     const QDir& dest_dir,
                                     const QStringList& exclude_patterns) {
    const auto files = source_dir.entryList(QDir::Files);
    for (const auto& file : files) {
        const QString source_file = source_dir.filePath(file);
        if (isExcluded(source_file, exclude_patterns)) {
            continue;
        }

        const QString dest_file = dest_dir.filePath(file);
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
                                    const QStringList& exclude_patterns) {
    Q_ASSERT_X(!source.isEmpty(), "copyDirectory", "source must not be empty");
    Q_ASSERT_X(!destination.isEmpty(), "copyDirectory", "destination must not be empty");
    QDir source_dir(source);
    if (!source_dir.exists()) {
        return false;
    }

    QDir dest_dir(destination);
    if (!dest_dir.exists() && !dest_dir.mkpath(".")) {
        return false;
    }

    if (!copyPlainFiles(source_dir, dest_dir, exclude_patterns)) {
        return false;
    }

    // Copy subdirectories
    auto dirs = source_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& dir : dirs) {
        QString source_subdir = source_dir.filePath(dir);
        if (isExcluded(source_subdir, exclude_patterns)) {
            continue;
        }

        // Skip directory reparse points (junctions / symlinks). Descending into
        // one would follow it outside the source root -- pulling in arbitrary
        // external data -- or loop forever if it targets an ancestor.
        if (isReparsePoint(source_subdir)) {
            sak::logInfo("[UserDataManager] Skipping reparse point {}",
                         source_subdir.toStdString());
            continue;
        }

        QString dest_subdir = dest_dir.filePath(dir);
        if (!copyDirectory(source_subdir, dest_subdir, exclude_patterns)) {
            return false;
        }
    }

    return true;
}

QString UserDataManager::calculateSHA256(const QString& file_path) const {
    Q_ASSERT_X(!file_path.isEmpty(), "calculateSHA256", "file_path must not be empty");
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);

    // Read in chunks to handle large files
    const qint64 chunk_size = 1024 * 1024;  // 1 MB
    while (!file.atEnd()) {
        hash.addData(file.read(chunk_size));
    }

    return QString(hash.result().toHex());
}

bool UserDataManager::writeMetadata(const BackupEntry& entry, const QString& metadata_path) {
    Q_ASSERT_X(!metadata_path.isEmpty(), "writeMetadata", "metadata_path must not be empty");
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

    QJsonDocument doc(json);

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
    Q_ASSERT_X(!metadata_path.isEmpty(), "readMetadata", "metadata_path must not be empty");
    QFile file(metadata_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return std::nullopt;
    }

    QJsonObject json = doc.object();

    BackupEntry entry;
    entry.app_name = json["app_name"].toString();
    entry.app_version = json["app_version"].toString();

    QJsonArray paths = json["source_paths"].toArray();
    for (const auto& path : paths) {
        entry.source_paths.append(path.toString());
    }

    entry.backup_path = json["backup_path"].toString();
    entry.backup_date = QDateTime::fromString(json["backup_date"].toString(), Qt::ISODate);
    entry.total_size_bytes = json["total_size"].toInteger();
    entry.compressed_size_bytes = json["compressed_size"].toInteger();
    entry.checksum = json["checksum"].toString();
    entry.encrypted = json["encrypted"].toBool();

    QJsonArray excluded = json["excluded_patterns"].toArray();
    for (const auto& pattern : excluded) {
        entry.excluded_patterns.append(pattern.toString());
    }

    return entry;
}

}  // namespace sak
