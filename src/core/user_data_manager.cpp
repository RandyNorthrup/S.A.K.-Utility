#include "sak/user_data_manager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QDebug>
#include <optional>

namespace sak {

UserDataManager::UserDataManager(QObject* parent)
    : QObject(parent)
{
}

std::optional<UserDataManager::BackupEntry> UserDataManager::backupAppData(const QString& app_name,
                                     const QStringList& source_paths,
                                     const QString& backup_dir,
                                     const BackupConfig& config)
{
    qDebug() << "[UserDataManager] Starting backup for" << app_name;
    Q_EMIT operationStarted(app_name, "backup");
    
    // Validate inputs
    if (source_paths.isEmpty()) {
        Q_EMIT operationError(app_name, "No source paths specified");
        return std::nullopt;
    }
    
    // Create backup directory
    QDir dir(backup_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        Q_EMIT operationError(app_name, "Failed to create backup directory");
        return std::nullopt;
    }
    
    // Generate backup filename
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString safe_name = app_name;
    safe_name.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    QString archive_name = QString("%1_%2.zip").arg(safe_name, timestamp);
    QString archive_path = dir.filePath(archive_name);
    
    // Calculate total size
    qint64 total_size = calculateSize(source_paths);
    Q_EMIT progressUpdate(0, total_size, "Calculating size...");
    
    // Create archive
    if (config.compress) {
        if (!createArchive(source_paths, archive_path, config)) {
            Q_EMIT operationError(app_name, "Failed to create archive");
            return std::nullopt;
        }
    } else {
        // Direct copy without compression
        QString copy_dir = dir.filePath(safe_name + "_" + timestamp);
        for (const auto& source : source_paths) {
            if (!copyDirectory(source, copy_dir, config.exclude_patterns)) {
                Q_EMIT operationError(app_name, "Failed to copy directory");
                return std::nullopt;
            }
        }
    }
    
    // Verify checksum
    QString checksum;
    if (config.verify_checksum && config.compress) {
        Q_EMIT progressUpdate(total_size, total_size, "Verifying checksum...");
        checksum = generateChecksum(archive_path);
        if (checksum.isEmpty()) {
            Q_EMIT operationError(app_name, "Failed to generate checksum");
            return std::nullopt;
        }
    }
    
    // Create backup entry
    BackupEntry entry;
    entry.app_name = app_name;
    entry.source_paths = source_paths;
    entry.backup_path = archive_path;
    entry.backup_date = QDateTime::currentDateTime();
    entry.total_size = total_size;
    entry.compressed_size = config.compress ? QFileInfo(archive_path).size() : total_size;
    entry.checksum = checksum;
    entry.encrypted = false;
    entry.excluded_patterns = config.exclude_patterns;
    
    // Write metadata
    QString metadata_path = archive_path + ".json";
    if (!writeMetadata(entry, metadata_path)) {
        qWarning() << "[UserDataManager] Failed to write metadata";
    }
    
    qDebug() << "[UserDataManager] Backup completed:"
             << "Size:" << entry.compressed_size << "bytes"
             << "Compression:" << QString::number(100.0 * entry.compressed_size / entry.total_size, 'f', 1) << "%";
    
    Q_EMIT operationCompleted(app_name, true, "Backup completed successfully");
    return entry;
}

bool UserDataManager::backupMultipleApps(const QStringList& app_names,
                                         const QString& backup_dir,
                                         const BackupConfig& config)
{
    bool all_success = true;
    
    for (const auto& app_name : app_names) {
        // Discover data paths
        auto paths = discoverAppDataPaths(app_name);
        if (paths.isEmpty()) {
            qWarning() << "[UserDataManager] No data paths found for" << app_name;
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
                                     const RestoreConfig& config)
{
    qDebug() << "[UserDataManager] Starting restore from" << backup_path;
    
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
        Q_EMIT progressUpdate(0, 100, "Verifying backup integrity...");
        QString current_checksum = generateChecksum(backup_path);
        if (current_checksum != entry.checksum) {
            Q_EMIT operationError(entry.app_name, "Checksum mismatch - backup may be corrupted");
            return false;
        }
    }
    
    // Create backup of existing data
    if (config.create_backup) {
        QDir restore(restore_dir);
        if (restore.exists()) {
            QString backup_name = "backup_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            QString backup_existing = restore.filePath("../" + backup_name);
            if (!copyDirectory(restore_dir, backup_existing, {})) {
                qWarning() << "[UserDataManager] Failed to backup existing data";
            }
        }
    }
    
    // Extract archive
    if (backup_path.endsWith(".zip")) {
        Q_EMIT progressUpdate(0, entry.compressed_size, "Extracting archive...");
        if (!extractArchive(backup_path, restore_dir, config)) {
            Q_EMIT operationError(entry.app_name, "Failed to extract archive");
            return false;
        }
    }
    
    qDebug() << "[UserDataManager] Restore completed";
    Q_EMIT operationCompleted(entry.app_name, true, "Restore completed successfully");
    return true;
}

bool UserDataManager::restoreMultipleApps(const QStringList& backup_paths,
                                          const QString& restore_dir,
                                          const RestoreConfig& config)
{
    bool all_success = true;
    
    for (const auto& backup_path : backup_paths) {
        if (!restoreAppData(backup_path, restore_dir, config)) {
            all_success = false;
        }
    }
    
    return all_success;
}

QStringList UserDataManager::discoverAppDataPaths(const QString& app_name) const
{
    QStringList paths;
    QStringList base_dirs = getStandardDataPaths();
    
    // Common name variations
    QString nospace = app_name;
    nospace.replace(" ", "");
    QString underscore = app_name;
    underscore.replace(" ", "_");
    QStringList name_variants = {
        app_name,
        app_name.toLower(),
        nospace,
        underscore
    };
    
    // Search in standard locations
    for (const auto& base : base_dirs) {
        for (const auto& variant : name_variants) {
            QString path = QDir(base).filePath(variant);
            if (QDir(path).exists()) {
                paths.append(path);
            }
        }
    }
    
    return paths;
}

std::vector<UserDataManager::DataLocation> UserDataManager::getCommonDataLocations() const
{
    std::vector<DataLocation> locations;
    
    QString appdata_local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QString appdata_roaming = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    
    // Chrome
    locations.push_back({
        "Google Chrome",
        {appdata_local + "/../Google/Chrome/User Data"},
        "Browser profile, history, bookmarks, extensions"
    });
    
    // Firefox
    locations.push_back({
        "Mozilla Firefox",
        {appdata_roaming + "/../Mozilla/Firefox/Profiles"},
        "Browser profile, history, bookmarks, extensions"
    });
    
    // VS Code
    locations.push_back({
        "Visual Studio Code",
        {appdata_roaming + "/../Code/User"},
        "Settings, keybindings, extensions, snippets"
    });
    
    return locations;
}

QStringList UserDataManager::scanForAppData(const QString& app_name) const
{
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

std::vector<UserDataManager::BackupEntry> UserDataManager::listBackups(const QString& backup_dir) const
{
    std::vector<BackupEntry> backups;
    
    QDir dir(backup_dir);
    QStringList filters = {"*.zip"};
    auto files = dir.entryList(filters, QDir::Files, QDir::Time);
    
    for (const auto& file : files) {
        QString metadata_path = dir.filePath(file) + ".json";
        if (QFile::exists(metadata_path)) {
            auto entry = readMetadata(metadata_path);
            if (entry.has_value()) {
                backups.push_back(entry.value());
            }
        }
    }
    
    return backups;
}

UserDataManager::BackupEntry UserDataManager::getBackupInfo(const QString& backup_path) const
{
    auto entry = readMetadata(backup_path + ".json");
    return entry.value_or(BackupEntry{});
}

bool UserDataManager::deleteBackup(const QString& backup_path)
{
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

bool UserDataManager::verifyBackup(const QString& backup_path)
{
    auto entry = readMetadata(backup_path + ".json");
    if (!entry.has_value()) {
        return false;
    }
    
    if (entry->checksum.isEmpty()) {
        return true; // No checksum to verify
    }
    
    QString current = generateChecksum(backup_path);
    return current == entry->checksum;
}

qint64 UserDataManager::calculateSize(const QStringList& paths) const
{
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

QString UserDataManager::generateChecksum(const QString& file_path) const
{
    return calculateSHA256(file_path);
}

bool UserDataManager::compareChecksums(const QString& file1, const QString& file2) const
{
    return generateChecksum(file1) == generateChecksum(file2);
}

bool UserDataManager::createArchive(const QStringList& source_paths,
                                    const QString& archive_path,
                                    const BackupConfig& /*config*/)
{
    // Use PowerShell's Compress-Archive for Windows
    QStringList args;
    args << "-NoProfile" << "-Command";
    
    QString sources = source_paths.join("','");
    QString command = QString("Compress-Archive -Path '%1' -DestinationPath '%2' -CompressionLevel Optimal -Force")
                        .arg(sources, archive_path);
    
    args << command;
    
    QProcess process;
    process.start("powershell.exe", args);
    
    if (!process.waitForStarted()) {
        return false;
    }
    
    if (!process.waitForFinished(300000)) { // 5 minute timeout
        process.kill();
        return false;
    }
    
    return process.exitCode() == 0 && QFile::exists(archive_path);
}

bool UserDataManager::extractArchive(const QString& archive_path,
                                     const QString& destination,
                                     const RestoreConfig& /*config*/)
{
    // Use PowerShell's Expand-Archive for Windows
    QStringList args;
    args << "-NoProfile" << "-Command";
    
    QString command = QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
                        .arg(archive_path, destination);
    
    args << command;
    
    QProcess process;
    process.start("powershell.exe", args);
    
    if (!process.waitForStarted()) {
        return false;
    }
    
    if (!process.waitForFinished(300000)) { // 5 minute timeout
        process.kill();
        return false;
    }
    
    return process.exitCode() == 0;
}

bool UserDataManager::isExcluded(const QString& path, const QStringList& patterns) const
{
    for (const auto& pattern : patterns) {
        QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern));
        if (re.match(path).hasMatch()) {
            return true;
        }
    }
    return false;
}

QStringList UserDataManager::getStandardDataPaths() const
{
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

bool UserDataManager::copyDirectory(const QString& source,
                                    const QString& destination,
                                    const QStringList& exclude_patterns)
{
    QDir source_dir(source);
    if (!source_dir.exists()) {
        return false;
    }
    
    QDir dest_dir(destination);
    if (!dest_dir.exists() && !dest_dir.mkpath(".")) {
        return false;
    }
    
    // Copy files
    auto files = source_dir.entryList(QDir::Files);
    for (const auto& file : files) {
        QString source_file = source_dir.filePath(file);
        if (isExcluded(source_file, exclude_patterns)) {
            continue;
        }
        
        QString dest_file = dest_dir.filePath(file);
        if (!QFile::copy(source_file, dest_file)) {
            qWarning() << "[UserDataManager] Failed to copy" << source_file;
        }
    }
    
    // Copy subdirectories
    auto dirs = source_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& dir : dirs) {
        QString source_subdir = source_dir.filePath(dir);
        if (isExcluded(source_subdir, exclude_patterns)) {
            continue;
        }
        
        QString dest_subdir = dest_dir.filePath(dir);
        if (!copyDirectory(source_subdir, dest_subdir, exclude_patterns)) {
            return false;
        }
    }
    
    return true;
}

QString UserDataManager::calculateSHA256(const QString& file_path) const
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QCryptographicHash hash(QCryptographicHash::Sha256);
    
    // Read in chunks to handle large files
    const qint64 chunk_size = 1024 * 1024; // 1 MB
    while (!file.atEnd()) {
        hash.addData(file.read(chunk_size));
    }
    
    return QString(hash.result().toHex());
}

bool UserDataManager::writeMetadata(const BackupEntry& entry, const QString& metadata_path)
{
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
    json["total_size"] = entry.total_size;
    json["compressed_size"] = entry.compressed_size;
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
    
    file.write(doc.toJson());
    return true;
}

std::optional<UserDataManager::BackupEntry> UserDataManager::readMetadata(const QString& metadata_path) const
{
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
    entry.total_size = json["total_size"].toInteger();
    entry.compressed_size = json["compressed_size"].toInteger();
    entry.checksum = json["checksum"].toString();
    entry.encrypted = json["encrypted"].toBool();
    
    QJsonArray excluded = json["excluded_patterns"].toArray();
    for (const auto& pattern : excluded) {
        entry.excluded_patterns.append(pattern.toString());
    }
    
    return entry;
}

} // namespace sak
