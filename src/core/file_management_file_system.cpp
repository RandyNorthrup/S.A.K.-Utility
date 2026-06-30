// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_file_system.cpp
/// @brief Shared file-system target bridge for File Management tools.

#include "sak/file_management_file_system.h"

#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_apfs_writer.h"
#include "sak/partition_ext_file_system_reader.h"
#include "sak/partition_file_system_registry.h"
#include "sak/partition_hfs_file_system_reader.h"
#include "sak/partition_raw_device_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <algorithm>
#include <optional>

namespace sak {

namespace {

constexpr int kDriveRootPrefixLength = 3;
constexpr uint64_t kFileManagementMaxWriteBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMinimumGeneratedApfsBytes = 64ULL * 1024ULL * 1024ULL;
// APFS File Management mutation is limited to a root file (1 path part) or one
// level of directory child (2 parts: directory + file).
constexpr int kApfsMaxPathDepth = 2;

QString normalizedPath(QString path) {
    path = path.trimmed();
    if (path.endsWith(QLatin1Char('\\'))) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    }
    return path;
}

QString displayFileSystem(const QString& fileSystem) {
    const QString normalized = FileManagementFileSystemBridge::normalizedFileSystem(fileSystem);
    if (normalized == QStringLiteral("hfsplus")) {
        return QStringLiteral("HFS+");
    }
    if (normalized == QStringLiteral("hfsx")) {
        return QStringLiteral("HFSX");
    }
    if (normalized == QStringLiteral("apfs")) {
        return QStringLiteral("APFS");
    }
    if (normalized == QStringLiteral("exfat")) {
        return QStringLiteral("exFAT");
    }
    if (normalized == QStringLiteral("linux-swap")) {
        return QStringLiteral("Linux swap");
    }
    return fileSystem.trimmed().isEmpty() ? QStringLiteral("Unknown") : fileSystem.trimmed();
}

QString partitionAlias(uint32_t diskNumber, uint32_t partitionNumber) {
    if (partitionNumber == 0) {
        return {};
    }
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
        .arg(diskNumber)
        .arg(partitionNumber);
}

bool isRawDevicePath(const QString& path) {
    return path.startsWith(QStringLiteral("\\\\.\\")) ||
           path.startsWith(QStringLiteral("\\\\?\\GLOBALROOT\\"));
}

bool isApfsPathSupported(const QString& path, bool directory) {
    QString clean = path.trimmed();
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!clean.startsWith(QLatin1Char('/'))) {
        clean.prepend(QLatin1Char('/'));
    }
    const auto parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return directory ? parts.size() == 1 : parts.size() == 1 || parts.size() == kApfsMaxPathDepth;
}

QStringList apfsParts(const QString& path) {
    QString clean = path.trimmed();
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!clean.startsWith(QLatin1Char('/'))) {
        clean.prepend(QLatin1Char('/'));
    }
    return clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

QString displayPath(const QString& path) {
    QString clean = path.trimmed();
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!clean.startsWith(QLatin1Char('/'))) {
        clean.prepend(QLatin1Char('/'));
    }
    while (clean.contains(QStringLiteral("//"))) {
        clean.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    return clean;
}

sak::PartitionApfsWriteOptions apfsRawWriteOptions() {
    sak::PartitionApfsWriteOptions options;
    options.enable_experimental_writer = true;
    options.image_only = false;
    options.destructive_certification_evidence = true;
    options.raw_media_hardware_certification_evidence = true;
    options.max_payload_bytes = kFileManagementMaxWriteBytes;
    options.evidence_id = QStringLiteral("file-management.apfs.raw");
    return options;
}

sak::PartitionHfsFileWriteOptions hfsWriteOptions(const FileManagementTarget& target) {
    sak::PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.image_only = !isRawDevicePath(target.root_path);
    options.allow_journaled_volume = true;
    options.allow_wrapped_volume = true;
    options.evidence_id = QStringLiteral("file-management.hfs");
    options.max_write_bytes = kFileManagementMaxWriteBytes;
    return options;
}

FileManagementMutationResult mutationBlocked(const QString& fileSystem,
                                             const QString& path,
                                             const QString& blocker) {
    FileManagementMutationResult result;
    result.file_system = displayFileSystem(fileSystem);
    result.path = path;
    result.blockers.append(blocker);
    return result;
}

bool computeWritableNonNative(const QString& fs, const FileManagementTarget& target) {
    const bool apfsGeneratedWriteSizeSupported =
        fs == QStringLiteral("apfs") && target.kind == FileManagementTargetKind::Partition &&
        target.size_bytes >= kMinimumGeneratedApfsBytes &&
        target.size_bytes <= kMaximumApfsGeneratedContainerBytes;
    return fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx") ||
           apfsGeneratedWriteSizeSupported;
}

void appendTargetBlockers(FileManagementTarget& target, const QString& fs) {
    target.blockers.clear();
    if (!target.can_browse) {
        target.blockers.append(
            QStringLiteral("No directory browser is registered for %1").arg(target.file_system));
    }
    if (!target.can_organize) {
        target.blockers.append(
            QStringLiteral("Generic organizer moves are blocked for raw/non-native targets; use "
                           "certified Partition Manager file actions for supported writes"));
    }
    if (!target.can_write_files) {
        target.blockers.append(
            fs == QStringLiteral("apfs") && target.kind == FileManagementTargetKind::Partition
                ? QStringLiteral("APFS File Explorer writes are limited to S.A.K.-generated "
                                 "containers from 64 MiB through 24 TiB")
                : QStringLiteral("File Management opens this target read-only"));
    } else if (!target.local_file_system) {
        target.blockers.append(
            QStringLiteral("Raw/non-native write support is limited to explicit File Explorer "
                           "create/write/delete/rename actions with confirmation"));
    }
}

FileManagementTarget applyCapabilities(FileManagementTarget target) {
    const QString fs = FileManagementFileSystemBridge::normalizedFileSystem(target.file_system);
    const bool native = FileManagementFileSystemBridge::isNativeFileSystem(fs);
    const bool readableNonNative =
        FileManagementFileSystemBridge::isReadableNonNativeFileSystem(fs);

    target.file_system = displayFileSystem(fs);
    target.local_file_system = target.kind == FileManagementTargetKind::LocalPath || native;
    target.read_only = !target.local_file_system;
    target.can_browse = target.local_file_system || readableNonNative;
    target.can_read_files = target.local_file_system || readableNonNative;
    target.can_write_files = target.local_file_system || computeWritableNonNative(fs, target);
    target.can_organize = target.local_file_system;
    target.can_duplicate_scan = target.local_file_system || readableNonNative;
    target.can_advanced_search = target.local_file_system || readableNonNative;
    appendTargetBlockers(target, fs);
    return target;
}

FileManagementEntry fromLocalInfo(const QFileInfo& info, const QString& basePath) {
    FileManagementEntry entry;
    entry.name = info.fileName();
    entry.path = info.absoluteFilePath();
    entry.type = info.isDir() ? QStringLiteral("Directory") : QStringLiteral("File");
    entry.size_bytes = static_cast<uint64_t>(std::max<qint64>(0, info.size()));
    entry.modified_time = info.lastModified();
    entry.created_time = info.birthTime();
    entry.directory = info.isDir();
    entry.regular_file = info.isFile();
    entry.symlink = info.isSymLink();
    entry.link_target = info.symLinkTarget();
    if (entry.name.isEmpty()) {
        entry.name = basePath;
    }
    return entry;
}

FileManagementListResult listLocalDirectory(const QString& path, int maxEntries) {
    FileManagementListResult result;
    result.ok = false;
    result.file_system = QStringLiteral("Local");
    const QDir dir(path);
    if (!dir.exists()) {
        result.blockers.append(QStringLiteral("Directory does not exist: %1").arg(path));
        return result;
    }

    const auto entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                           QDir::DirsFirst | QDir::IgnoreCase | QDir::Name);
    const qsizetype limit = maxEntries > 0 ? std::min<qsizetype>(maxEntries, entries.size())
                                           : entries.size();
    result.entries.reserve(limit);
    for (qsizetype i = 0; i < limit; ++i) {
        result.entries.append(fromLocalInfo(entries.at(i), path));
    }
    if (limit < entries.size()) {
        result.warnings.append(QStringLiteral("Listing truncated to %1 entries").arg(limit));
    }
    result.ok = true;
    return result;
}

FileManagementListResult fromExtResult(const PartitionExtFileReadResult& input) {
    FileManagementListResult result;
    result.ok = input.ok;
    result.file_system = input.file_system;
    result.blockers = input.blockers;
    result.warnings = input.warnings;
    result.entries.reserve(input.entries.size());
    for (const auto& item : input.entries) {
        result.entries.append({item.name,
                               item.path,
                               item.type,
                               item.size_bytes,
                               {},
                               {},
                               QString::number(item.inode),
                               item.symlink_target,
                               item.directory,
                               item.regular_file,
                               item.symlink});
    }
    return result;
}

FileManagementListResult fromHfsResult(const PartitionHfsFileReadResult& input) {
    FileManagementListResult result;
    result.ok = input.ok;
    result.file_system = input.file_system;
    result.blockers = input.blockers;
    result.warnings = input.warnings;
    result.entries.reserve(input.entries.size());
    for (const auto& item : input.entries) {
        result.entries.append({item.name,
                               item.path,
                               item.type,
                               item.size_bytes,
                               {},
                               {},
                               QString::number(item.catalog_id),
                               {},
                               item.directory,
                               item.regular_file,
                               false});
    }
    return result;
}

FileManagementListResult fromApfsResult(const PartitionApfsFileReadResult& input) {
    FileManagementListResult result;
    result.ok = input.ok;
    result.file_system = input.file_system;
    result.volume_name = input.volume_name;
    result.blockers = input.blockers;
    result.warnings = input.warnings;
    result.entries.reserve(input.entries.size());
    for (const auto& item : input.entries) {
        result.entries.append({item.name,
                               item.path,
                               item.type,
                               item.size_bytes,
                               {},
                               {},
                               QString::number(item.object_id),
                               {},
                               item.directory,
                               item.regular_file,
                               item.symlink});
    }
    return result;
}

QString pathOrRoot(const QString& path) {
    const QString trimmed = path.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("/") : trimmed;
}

FileManagementReadResult readLocalFile(const QString& path, uint64_t maxBytes) {
    FileManagementReadResult result;
    result.file_system = QStringLiteral("Local");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.blockers.append(QStringLiteral("Could not open file: %1").arg(path));
        return result;
    }
    if (maxBytes > 0 && static_cast<uint64_t>(file.size()) > maxBytes) {
        result.blockers.append(
            QStringLiteral("File exceeds read limit: %1 bytes").arg(file.size()));
        return result;
    }
    result.data = file.readAll();
    result.ok = true;
    return result;
}

FileManagementReadResult fromExtReadResult(const PartitionExtFileReadResult& input) {
    return {input.ok, input.file_system, input.blockers, input.warnings, input.data};
}

FileManagementReadResult fromHfsReadResult(const PartitionHfsFileReadResult& input) {
    return {input.ok, input.file_system, input.blockers, input.warnings, input.data};
}

FileManagementReadResult fromApfsReadResult(const PartitionApfsFileReadResult& input) {
    return {input.ok, input.file_system, input.blockers, input.warnings, input.data};
}

FileManagementMutationResult fromHfsWriteResult(const PartitionHfsFileWriteResult& input) {
    return {.ok = input.ok,
            .file_system = input.file_system,
            .path = input.path,
            .bytes_written = input.bytes_written,
            .before_sha256 = input.before_sha256,
            .after_sha256 = input.after_sha256,
            .blockers = input.blockers,
            .warnings = input.warnings};
}

// Map a raw in-place COW checkpoint commit result (the certified crash-safe engine,
// shared by file write/delete/rename) onto a File Management mutation result.
FileManagementMutationResult fromApfsCommitResult(
    const PartitionApfsImageCheckpointCommitResult& input,
    const QString& path,
    uint64_t bytes_written) {
    return {.ok = input.ok,
            .file_system = QStringLiteral("APFS"),
            .path = path,
            .bytes_written = bytes_written,
            .blockers = input.blockers,
            .warnings = input.warnings};
}

// Route an APFS File Management file write onto the certified crash-safe in-place COW
// engine: a single path component is a root file (create-or-replace), two components are
// a one-level directory child (create-or-replace under a parent). Limited to root + one
// level.
FileManagementMutationResult writeApfsFile(const FileManagementTarget& target,
                                           const QString& cleanPath,
                                           const QByteArray& data) {
    const QString fs = QStringLiteral("apfs");
    if (!isApfsPathSupported(cleanPath, false)) {
        return mutationBlocked(fs,
                               cleanPath,
                               QStringLiteral("APFS File Management file write is limited "
                                              "to root files or one root-directory child"));
    }
    const auto parts = apfsParts(cleanPath);
    if (parts.size() == 1) {
        // Root files use the certified crash-safe in-place COW engine
        // (create-or-replace).
        return fromApfsCommitResult(
            PartitionApfsWriter::commitRawFileWrite(
                {.target_path = target.root_path,
                 .target_container_bytes = target.size_bytes,
                 .file_name = parts.value(0),
                 .file_data = data,
                 .target_mutation_confirmed = true,
                 .allow_raw_device_target = isRawDevicePath(target.root_path),
                 .options = apfsRawWriteOptions()}),
            cleanPath,
            static_cast<uint64_t>(data.size()));
    }
    // Directory children use the same COW engine (create-or-replace under a parent).
    return fromApfsCommitResult(PartitionApfsWriter::commitRawDirectoryChildWrite(
                                    {.target_path = target.root_path,
                                     .target_container_bytes = target.size_bytes,
                                     .directory_name = parts.value(0),
                                     .file_name = parts.value(1),
                                     .file_data = data,
                                     .target_mutation_confirmed = true,
                                     .allow_raw_device_target = isRawDevicePath(target.root_path),
                                     .options = apfsRawWriteOptions()}),
                                cleanPath,
                                static_cast<uint64_t>(data.size()));
}

}  // namespace

QVector<FileManagementTarget> FileManagementFileSystemBridge::mountedTargets() {
    QVector<FileManagementTarget> targets;
    const QString homePath = QDir::homePath();
    if (!homePath.isEmpty()) {
        auto home = localTarget(homePath);
        home.id = QStringLiteral("home");
        home.label = QStringLiteral("Home (%1)").arg(QFileInfo(homePath).fileName());
        targets.append(home);
    }

    const auto volumes = QStorageInfo::mountedVolumes();
    for (const auto& volume : volumes) {
        if (!volume.isValid() || !volume.isReady()) {
            continue;
        }
        FileManagementTarget target = localTarget(volume.rootPath());
        QString label = volume.displayName();
        if (label.isEmpty()) {
            label = volume.rootPath();
        }
        target.id = QStringLiteral("volume:%1").arg(volume.rootPath());
        target.label =
            QStringLiteral("%1 (%2)").arg(label, volume.rootPath().left(kDriveRootPrefixLength));
        target.file_system = QString::fromUtf8(volume.fileSystemType());
        target.source = QStringLiteral("Mounted volume");
        targets.append(applyCapabilities(target));
    }
    return targets;
}

QVector<FileManagementTarget> FileManagementFileSystemBridge::targetsFromInventory(
    const PartitionInventory& inventory) {
    QVector<FileManagementTarget> targets = mountedTargets();
    for (const auto& disk : inventory.disks) {
        for (const auto& partition : disk.partitions) {
            const QString fs = partition.volume ? partition.volume->file_system
                                                : QStringLiteral("Unknown");
            const QString mountedPath =
                partition.volume && partition.volume->hasDriveLetter()
                    ? QStringLiteral("%1:/").arg(partition.volume->drive_letter)
                    : QString();
            FileManagementTarget target;
            target.id = QStringLiteral("disk:%1:partition:%2")
                            .arg(disk.disk_number)
                            .arg(partition.partition_number);
            target.label = QStringLiteral("Disk %1 Partition %2 - %3")
                               .arg(disk.disk_number)
                               .arg(partition.partition_number)
                               .arg(displayFileSystem(fs));
            target.root_path = mountedPath.isEmpty()
                                   ? partitionAlias(disk.disk_number, partition.partition_number)
                                   : mountedPath;
            target.file_system = fs;
            target.source = partition.volume ? partition.volume->file_system_source
                                             : QStringLiteral("Partition inventory");
            target.details = partition.volume ? partition.volume->file_system_details
                                              : QStringList{};
            target.size_bytes = partition.size_bytes;
            target.kind = mountedPath.isEmpty() ? FileManagementTargetKind::Partition
                                                : FileManagementTargetKind::LocalPath;
            target.read_only = partition.is_read_only;
            targets.append(applyCapabilities(target));
        }
    }
    return targets;
}

FileManagementTarget FileManagementFileSystemBridge::manualTarget(const QString& root_path,
                                                                  const QString& file_system,
                                                                  uint64_t size_bytes) {
    FileManagementTarget target;
    target.id = QStringLiteral("manual:%1:%2").arg(file_system.trimmed(), root_path.trimmed());
    target.label = QStringLiteral("%1 target - %2")
                       .arg(displayFileSystem(file_system), QFileInfo(root_path).fileName());
    if (QFileInfo(root_path).fileName().isEmpty()) {
        target.label = QStringLiteral("%1 target - %2")
                           .arg(displayFileSystem(file_system), root_path.trimmed());
    }
    target.root_path = root_path.trimmed();
    target.file_system = file_system;
    target.source = QStringLiteral("Manual raw/image target");
    if (size_bytes > 0) {
        target.size_bytes = size_bytes;
    } else if (!isRawDevicePath(target.root_path)) {
        target.size_bytes =
            static_cast<uint64_t>(std::max<qint64>(0, QFileInfo(target.root_path).size()));
    }
    target.kind = isRawDevicePath(target.root_path) ? FileManagementTargetKind::Partition
                                                    : FileManagementTargetKind::ImageFile;
    return applyCapabilities(target);
}

FileManagementTarget FileManagementFileSystemBridge::localTarget(const QString& root_path) {
    FileManagementTarget target;
    target.id = QStringLiteral("local:%1").arg(root_path.trimmed());
    target.label = QDir::toNativeSeparators(root_path.trimmed());
    target.root_path = normalizedPath(root_path);
    target.file_system = QStringLiteral("Local");
    target.source = QStringLiteral("Local path");
    target.size_bytes =
        static_cast<uint64_t>(std::max<qint64>(0, QFileInfo(target.root_path).size()));
    target.kind = FileManagementTargetKind::LocalPath;
    return applyCapabilities(target);
}

bool FileManagementFileSystemBridge::isNativeFileSystem(const QString& file_system) {
    const QString fs = normalizedFileSystem(file_system);
    return fs == QStringLiteral("local") || fs == QStringLiteral("ntfs") ||
           fs == QStringLiteral("fat") || fs == QStringLiteral("fat12") ||
           fs == QStringLiteral("fat16") || fs == QStringLiteral("fat32") ||
           fs == QStringLiteral("exfat");
}

bool FileManagementFileSystemBridge::isReadableNonNativeFileSystem(const QString& file_system) {
    const QString fs = normalizedFileSystem(file_system);
    return fs == QStringLiteral("ext2") || fs == QStringLiteral("ext3") ||
           fs == QStringLiteral("ext4") || fs == QStringLiteral("hfsplus") ||
           fs == QStringLiteral("hfsx") || fs == QStringLiteral("apfs");
}

QString FileManagementFileSystemBridge::normalizedFileSystem(const QString& file_system) {
    QString fs = file_system.trimmed().toLower();
    fs.replace(QLatin1Char('_'), QLatin1Char('-'));
    if (fs == QStringLiteral("hfs+") || fs == QStringLiteral("hfs plus") ||
        fs == QStringLiteral("hfsplus")) {
        return QStringLiteral("hfsplus");
    }
    if (fs == QStringLiteral("linux swap") || fs == QStringLiteral("swap")) {
        return QStringLiteral("linux-swap");
    }
    return fs;
}

QString FileManagementFileSystemBridge::capabilitySummary(const FileManagementTarget& target) {
    QStringList parts;
    parts.append(target.file_system);
    parts.append(target.local_file_system ? QStringLiteral("local file API")
                                          : QStringLiteral("raw/image reader"));
    if (target.can_write_files) {
        parts.append(target.local_file_system ? QStringLiteral("read/write")
                                              : QStringLiteral("explicit writes"));
    } else {
        parts.append(QStringLiteral("read-only"));
    }
    if (!target.can_organize) {
        parts.append(QStringLiteral("organizer blocked"));
    }
    return parts.join(QStringLiteral(" - "));
}

FileManagementListResult FileManagementFileSystemBridge::listDirectory(
    const FileManagementTarget& target, const QString& path, int max_entries) {
    const QString fs = normalizedFileSystem(target.file_system);
    if (target.local_file_system) {
        const QString localPath = path.trimmed().isEmpty() ? target.root_path : path;
        return listLocalDirectory(localPath, max_entries);
    }
    if (fs == QStringLiteral("ext2") || fs == QStringLiteral("ext3") ||
        fs == QStringLiteral("ext4")) {
        return fromExtResult(PartitionExtFileSystemReader::listDirectoryFromImage(
            target.root_path, pathOrRoot(path), max_entries));
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return fromHfsResult(PartitionHfsFileSystemReader::listDirectoryFromImage(
            target.root_path, pathOrRoot(path), max_entries));
    }
    if (fs == QStringLiteral("apfs")) {
        return fromApfsResult(PartitionApfsFileSystemReader::listDirectoryFromImage(
            target.root_path, pathOrRoot(path), max_entries));
    }

    FileManagementListResult result;
    result.file_system = displayFileSystem(fs);
    result.blockers.append(
        QStringLiteral("No File Management browser is registered for %1").arg(result.file_system));
    return result;
}

FileManagementReadResult FileManagementFileSystemBridge::readFile(
    const FileManagementTarget& target, const QString& path, uint64_t max_bytes) {
    const QString fs = normalizedFileSystem(target.file_system);
    if (target.local_file_system) {
        return readLocalFile(path.trimmed().isEmpty() ? target.root_path : path, max_bytes);
    }
    if (fs == QStringLiteral("ext2") || fs == QStringLiteral("ext3") ||
        fs == QStringLiteral("ext4")) {
        return fromExtReadResult(
            PartitionExtFileSystemReader::readFileFromImage(target.root_path, path, max_bytes));
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return fromHfsReadResult(
            PartitionHfsFileSystemReader::readFileFromImage(target.root_path, path, max_bytes));
    }
    if (fs == QStringLiteral("apfs")) {
        return fromApfsReadResult(
            PartitionApfsFileSystemReader::readFileFromImage(target.root_path, path, max_bytes));
    }

    FileManagementReadResult result;
    result.file_system = displayFileSystem(fs);
    result.blockers.append(
        QStringLiteral("No File Management reader is registered for %1").arg(result.file_system));
    return result;
}

FileManagementMutationResult FileManagementFileSystemBridge::createDirectory(
    const FileManagementTarget& target, const QString& path) {
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanPath = displayPath(path);
    if (target.local_file_system) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        result.ok = QDir().mkpath(path);
        if (!result.ok) {
            result.blockers.append(QStringLiteral("Unable to create directory: %1").arg(path));
        }
        return result;
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return fromHfsWriteResult(PartitionHfsFileSystemWriter::createEmptyFolderFromImage(
            target.root_path, cleanPath, hfsWriteOptions(target)));
    }
    if (fs == QStringLiteral("apfs")) {
        if (!isApfsPathSupported(cleanPath, true)) {
            return mutationBlocked(fs,
                                   cleanPath,
                                   QStringLiteral("APFS File Management directory create is "
                                                  "limited to root directories"));
        }
        const auto parts = apfsParts(cleanPath);
        // Root directories use the certified crash-safe in-place COW engine.
        return fromApfsCommitResult(
            PartitionApfsWriter::commitRawDirectoryCreate(
                {.target_path = target.root_path,
                 .target_container_bytes = target.size_bytes,
                 .directory_name = parts.value(0),
                 .target_mutation_confirmed = true,
                 .allow_raw_device_target = isRawDevicePath(target.root_path),
                 .options = apfsRawWriteOptions()}),
            cleanPath,
            0);
    }
    return mutationBlocked(
        fs,
        cleanPath,
        QStringLiteral("Directory create is not supported for %1").arg(displayFileSystem(fs)));
}

FileManagementMutationResult FileManagementFileSystemBridge::deleteDirectory(
    const FileManagementTarget& target, const QString& path) {
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanPath = displayPath(path);
    if (target.local_file_system) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        QDir dir(path);
        result.ok = dir.removeRecursively();
        if (!result.ok) {
            result.blockers.append(QStringLiteral("Unable to delete directory: %1").arg(path));
        }
        return result;
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return fromHfsWriteResult(
            PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
                target.root_path, cleanPath, hfsWriteOptions(target)));
    }
    if (fs == QStringLiteral("apfs")) {
        if (!isApfsPathSupported(cleanPath, true)) {
            return mutationBlocked(fs,
                                   cleanPath,
                                   QStringLiteral("APFS File Management directory delete is "
                                                  "limited to root directories"));
        }
        const auto parts = apfsParts(cleanPath);
        // Root directories use the certified crash-safe in-place COW engine
        // (fails closed on a non-empty directory; empty children first).
        return fromApfsCommitResult(
            PartitionApfsWriter::commitRawDirectoryDelete(
                {.target_path = target.root_path,
                 .target_container_bytes = target.size_bytes,
                 .directory_name = parts.value(0),
                 .target_mutation_confirmed = true,
                 .allow_raw_device_target = isRawDevicePath(target.root_path),
                 .options = apfsRawWriteOptions()}),
            cleanPath,
            0);
    }
    return mutationBlocked(
        fs,
        cleanPath,
        QStringLiteral("Directory delete is not supported for %1").arg(displayFileSystem(fs)));
}

FileManagementMutationResult FileManagementFileSystemBridge::writeFile(
    const FileManagementTarget& target, const QString& path, const QByteArray& data) {
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanPath = displayPath(path);
    if (data.size() > static_cast<qsizetype>(kFileManagementMaxWriteBytes)) {
        return mutationBlocked(fs,
                               cleanPath,
                               QStringLiteral("File write exceeds 64 MiB File Management cap"));
    }
    if (target.local_file_system) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            result.blockers.append(
                QStringLiteral("Unable to write file: %1").arg(file.errorString()));
            return result;
        }
        result.bytes_written = static_cast<uint64_t>(file.write(data));
        result.ok = result.bytes_written == static_cast<uint64_t>(data.size());
        if (!result.ok) {
            result.blockers.append(QStringLiteral("Short write while writing file: %1").arg(path));
        }
        return result;
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return fromHfsWriteResult(PartitionHfsFileSystemWriter::createFileWithDataFromImage(
            target.root_path, cleanPath, data, hfsWriteOptions(target)));
    }
    if (fs == QStringLiteral("apfs")) {
        return writeApfsFile(target, cleanPath, data);
    }
    return mutationBlocked(
        fs,
        cleanPath,
        QStringLiteral("File write is not supported for %1").arg(displayFileSystem(fs)));
}

FileManagementMutationResult FileManagementFileSystemBridge::deleteFile(
    const FileManagementTarget& target, const QString& path) {
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanPath = displayPath(path);
    if (target.local_file_system) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        result.ok = QFile::remove(path);
        if (!result.ok) {
            result.blockers.append(QStringLiteral("Unable to delete file: %1").arg(path));
        }
        return result;
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return fromHfsWriteResult(
            PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
                target.root_path, cleanPath, hfsWriteOptions(target)));
    }
    if (fs == QStringLiteral("apfs")) {
        if (!isApfsPathSupported(cleanPath, false)) {
            return mutationBlocked(fs,
                                   cleanPath,
                                   QStringLiteral("APFS File Management file delete is limited "
                                                  "to root files or one root-directory child"));
        }
        const auto parts = apfsParts(cleanPath);
        if (parts.size() == 1) {
            // Root files use the certified crash-safe in-place COW engine.
            return fromApfsCommitResult(
                PartitionApfsWriter::commitRawFileDelete(
                    {.target_path = target.root_path,
                     .target_container_bytes = target.size_bytes,
                     .file_name = parts.value(0),
                     .target_mutation_confirmed = true,
                     .allow_raw_device_target = isRawDevicePath(target.root_path),
                     .options = apfsRawWriteOptions()}),
                cleanPath,
                0);
        }
        // Directory children use the same COW engine (delete under a parent).
        return fromApfsCommitResult(
            PartitionApfsWriter::commitRawDirectoryChildDelete(
                {.target_path = target.root_path,
                 .target_container_bytes = target.size_bytes,
                 .directory_name = parts.value(0),
                 .file_name = parts.value(1),
                 .target_mutation_confirmed = true,
                 .allow_raw_device_target = isRawDevicePath(target.root_path),
                 .options = apfsRawWriteOptions()}),
            cleanPath,
            0);
    }
    return mutationBlocked(
        fs,
        cleanPath,
        QStringLiteral("File delete is not supported for %1").arg(displayFileSystem(fs)));
}

namespace {

// Route an APFS rename/move onto the certified COW engine via the general file-move
// commit (a same-parent move is a plain rename; a cross-parent move reparents the file).
// An empty directory component means the container root. Limited to root + one level.
FileManagementMutationResult renameApfsEntry(const FileManagementTarget& target,
                                             const QString& cleanSource,
                                             const QString& cleanDestination) {
    const auto sourceParts = apfsParts(cleanSource);
    const auto destParts = apfsParts(cleanDestination);
    if (sourceParts.isEmpty() || sourceParts.size() > kApfsMaxPathDepth || destParts.isEmpty() ||
        destParts.size() > kApfsMaxPathDepth) {
        return mutationBlocked(QStringLiteral("apfs"),
                               cleanSource,
                               QStringLiteral("APFS File Management rename/move is limited to root "
                                              "files and one level of directory children"));
    }
    return fromApfsCommitResult(
        PartitionApfsWriter::commitRawFileMove(
            {.target_path = target.root_path,
             .target_container_bytes = target.size_bytes,
             .source_directory_name = sourceParts.size() == kApfsMaxPathDepth ? sourceParts.value(0)
                                                                              : QString(),
             .file_name = sourceParts.last(),
             .destination_directory_name =
                 destParts.size() == kApfsMaxPathDepth ? destParts.value(0) : QString(),
             .new_file_name = destParts.last(),
             .target_mutation_confirmed = true,
             .allow_raw_device_target = isRawDevicePath(target.root_path),
             .options = apfsRawWriteOptions()}),
        cleanDestination,
        0);
}

}  // namespace

FileManagementMutationResult FileManagementFileSystemBridge::renameEntry(
    const FileManagementTarget& target,
    const QString& source_path,
    const QString& destination_path) {
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanSource = displayPath(source_path);
    const QString cleanDestination = displayPath(destination_path);
    if (target.local_file_system) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = destination_path;
        result.ok = QFile::rename(source_path, destination_path);
        if (!result.ok) {
            result.blockers.append(QStringLiteral("Unable to rename: %1").arg(source_path));
        }
        return result;
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return fromHfsWriteResult(PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
            target.root_path, cleanSource, cleanDestination, hfsWriteOptions(target)));
    }
    if (fs == QStringLiteral("apfs")) {
        return renameApfsEntry(target, cleanSource, cleanDestination);
    }
    return mutationBlocked(
        fs,
        cleanSource,
        QStringLiteral("Rename is not supported for %1").arg(displayFileSystem(fs)));
}

}  // namespace sak
