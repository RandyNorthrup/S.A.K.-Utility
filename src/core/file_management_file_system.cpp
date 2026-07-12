// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_file_system.cpp
/// @brief Shared file-system target bridge for File Management tools.

#include "sak/file_management_file_system.h"

#include "sak/error_codes.h"
#include "sak/file_hash.h"
#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_apfs_writer.h"
#include "sak/partition_ext_file_system_reader.h"
#include "sak/partition_file_system_registry.h"
#include "sak/partition_hfs_file_system_reader.h"
#include "sak/partition_raw_device_io.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStorageInfo>
#include <QtEndian>

#include <algorithm>
#include <filesystem>
#include <optional>

namespace sak {

namespace {

constexpr int kDriveRootPrefixLength = 3;
// Sourced from the shared constants (partition_manager_types.h) so the File
// Management write gate cannot drift from the rest of the codebase.
constexpr uint64_t kFileManagementMaxWriteBytes = kMaximumNonNativeFileWriteBytes;
constexpr uint64_t kMinimumGeneratedApfsBytes = kMinimumApfsGeneratedContainerBytes;

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

// Derive an APFS container's byte size from its block-0 superblock (nx_block_size *
// nx_block_count) when the caller could not supply one: a raw partition device reports
// no file size, and without a size the certified write engine cannot be range-gated.
// Returns 0 when the device does not present a valid NXSB superblock.
uint64_t probeApfsContainerBytes(const QString& root_path) {
    QString error;
    const auto device = openFileOrRawDeviceReadOnly(root_path, &error);
    if (!device) {
        return 0;
    }
    const QByteArray block0 = device->read(4096);
    constexpr int kMagicOffset = 0x20;       // obj_phys_t header is 32 bytes
    constexpr int kBlockSizeOffset = 0x24;   // nx_block_size (u32)
    constexpr int kBlockCountOffset = 0x28;  // nx_block_count (u64)
    if (block0.size() < kBlockCountOffset + 8) {
        return 0;
    }
    const auto u32 = [&block0](const int offset) {
        return qFromLittleEndian<quint32>(block0.constData() + offset);
    };
    if (u32(kMagicOffset) != 0x42'53'58'4EU) {  // 'NXSB'
        return 0;
    }
    const quint64 block_size = u32(kBlockSizeOffset);
    const quint64 block_count = qFromLittleEndian<quint64>(block0.constData() + kBlockCountOffset);
    const bool sane_block_size = block_size >= 4096 && block_size <= 65'536 &&
                                 (block_size & (block_size - 1)) == 0;
    if (!sane_block_size || block_count == 0) {
        return 0;
    }
    return block_size * block_count;
}

bool isApfsPathSupported(const QString& path, bool /*directory*/) {
    QString clean = path.trimmed();
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!clean.startsWith(QLatin1Char('/'))) {
        clean.prepend(QLatin1Char('/'));
    }
    // Files and directories both nest to any depth: the certified COW engine resolves the parent
    // path and fails closed if a parent is missing, so only an empty (root) path is rejected.
    return !clean.split(QLatin1Char('/'), Qt::SkipEmptyParts).isEmpty();
}

QStringList apfsParts(const QString& path) {
    QString clean = path.trimmed();
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!clean.startsWith(QLatin1Char('/'))) {
        clean.prepend(QLatin1Char('/'));
    }
    return clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

// Split an APFS path into (parent directory path, leaf name): "/a/b/c" -> ("/a/b", "c");
// "/c" -> ("", "c"). The parent path (empty = container root) feeds the COW engine's
// arbitrary-depth resolver, so File Management file ops are no longer capped at one level.
std::pair<QString, QString> apfsParentAndName(const QString& path) {
    const QStringList parts = apfsParts(path);
    if (parts.size() <= 1) {
        return {QString(), parts.value(0)};
    }
    return {QStringLiteral("/") + parts.mid(0, parts.size() - 1).join(QLatin1Char('/')),
            parts.last()};
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
    // No artificial payload cap: the APFS write is bounded only by the container's free
    // space (the multi-chunk allocator fails closed when it will not fit). 0 = unbounded.
    options.max_payload_bytes = 0;
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
    // APFS writes route through the certified in-place COW engine, which supports both
    // S.A.K.-generated and real Apple-created (foreign) containers on raw partitions and
    // image files. The only bridge-level gate is a KNOWN container size inside the
    // engine's certified range; everything the engine has not certified for a given
    // container (snapshot-frozen deletes, Fusion/Tier2, locked encrypted volumes) fails
    // closed inside the engine itself with an exact blocker.
    const bool apfsWriteCapableKind = target.kind == FileManagementTargetKind::Partition ||
                                      target.kind == FileManagementTargetKind::ImageFile;
    const bool apfsWriteSupported = fs == QStringLiteral("apfs") && apfsWriteCapableKind &&
                                    target.size_bytes >= kMinimumGeneratedApfsBytes &&
                                    target.size_bytes <= kMaximumApfsGeneratedContainerBytes;
    return fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx") || apfsWriteSupported;
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
        const bool apfs_kind_supported = fs == QStringLiteral("apfs") &&
                                         (target.kind == FileManagementTargetKind::Partition ||
                                          target.kind == FileManagementTargetKind::ImageFile);
        target.blockers.append(
            apfs_kind_supported
                ? (target.size_bytes == 0
                       ? QStringLiteral(
                             "APFS writes need a known container size to range-gate the "
                             "certified engine; rescan disks or re-add the target with its size")
                       : QStringLiteral("APFS writes are certified for containers of %1; this "
                                        "container is outside that range")
                             .arg(apfsCapacityRangeText()))
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
    // The certified crash-safe in-place COW engine create-or-replaces the file under its parent
    // path at any depth (empty parent = container root).
    const auto [parent, name] = apfsParentAndName(cleanPath);
    return fromApfsCommitResult(PartitionApfsWriter::commitRawFileWrite(
                                    {.target_path = target.root_path,
                                     .target_container_bytes = target.size_bytes,
                                     .file_name = name,
                                     .file_data = data,
                                     .parent_directory_path = parent,
                                     .target_mutation_confirmed = true,
                                     .allow_raw_device_target = isRawDevicePath(target.root_path),
                                     .options = apfsRawWriteOptions()}),
                                cleanPath,
                                static_cast<uint64_t>(data.size()));
}

// Streaming APFS write: the payload is pulled from @p hostPath block-by-block by the
// certified multi-chunk COW engine (never held whole in RAM), so an arbitrarily large
// file is bounded only by the container's free space.
FileManagementMutationResult writeApfsFileStreamed(const FileManagementTarget& target,
                                                   const QString& cleanPath,
                                                   const QString& hostPath,
                                                   uint64_t size) {
    const QString fs = QStringLiteral("apfs");
    if (!isApfsPathSupported(cleanPath, false)) {
        return mutationBlocked(fs,
                               cleanPath,
                               QStringLiteral("APFS File Management file write is limited "
                                              "to root files or one root-directory child"));
    }
    // One streaming create-or-replace under the file's parent path at any depth (empty = root).
    const auto [parent, name] = apfsParentAndName(cleanPath);
    return fromApfsCommitResult(PartitionApfsWriter::commitRawFileWrite(
                                    {.target_path = target.root_path,
                                     .target_container_bytes = target.size_bytes,
                                     .file_name = name,
                                     .file_data_path = hostPath,
                                     .file_data_stream_size = size,
                                     .parent_directory_path = parent,
                                     .target_mutation_confirmed = true,
                                     .allow_raw_device_target = isRawDevicePath(target.root_path),
                                     .options = apfsRawWriteOptions()}),
                                cleanPath,
                                size);
}

// Streaming HFS+ write: the payload is pulled from @p hostPath one allocation block at a
// time by the certified fork writer (never held whole in RAM), so an arbitrarily large
// file is bounded only by the volume's free space. Byte-identical on disk to the buffered
// in-memory write of the same payload.
FileManagementMutationResult writeHfsFileStreamed(const FileManagementTarget& target,
                                                  const QString& cleanPath,
                                                  const QString& hostPath,
                                                  uint64_t size) {
    sak::PartitionHfsFileWriteOptions options = hfsWriteOptions(target);
    // Lift the buffered-write cap for streaming: bound the write by the file's own size
    // (still fails closed on a full volume via the allocator).
    options.max_write_bytes = std::max<uint64_t>(size, options.max_write_bytes);
    return fromHfsWriteResult(PartitionHfsFileSystemWriter::createFileFromHostPathStreamedFromImage(
        target.root_path, cleanPath, hostPath, size, options));
}

// Streaming local-filesystem copy: host file -> destination through a fixed 1 MiB
// window, so a multi-GB copy never holds the whole payload in RAM.
FileManagementMutationResult copyLocalFileStreamed(const QString& destPath,
                                                   const QString& hostPath) {
    FileManagementMutationResult result;
    result.path = destPath;
    QFile src(hostPath);
    QFile dst(destPath);
    if (!src.open(QIODevice::ReadOnly)) {
        result.blockers.append(
            QStringLiteral("Unable to read source file: %1").arg(src.errorString()));
        return result;
    }
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.blockers.append(QStringLiteral("Unable to write file: %1").arg(dst.errorString()));
        return result;
    }
    QByteArray window(1 << 20, Qt::Uninitialized);
    uint64_t written = 0;
    for (;;) {
        const qint64 n = src.read(window.data(), window.size());
        if (n < 0) {
            result.blockers.append(
                QStringLiteral("Read error while streaming: %1").arg(src.errorString()));
            return result;
        }
        if (n == 0) {
            break;
        }
        if (dst.write(window.constData(), n) != n) {
            result.blockers.append(
                QStringLiteral("Short write while streaming file: %1").arg(destPath));
            return result;
        }
        written += static_cast<uint64_t>(n);
    }
    result.bytes_written = written;
    result.ok = true;
    return result;
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
    } else if (FileManagementFileSystemBridge::normalizedFileSystem(file_system) ==
               QStringLiteral("apfs")) {
        // A raw partition device has no file size; derive the APFS container size from
        // its own superblock so the certified write engine can be range-gated instead
        // of falling back to read-only for want of a size.
        target.size_bytes = probeApfsContainerBytes(target.root_path);
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

QString FileManagementFileSystemBridge::identifierLabel(const QString& file_system) {
    const QString fs = file_system.toLower();
    if (fs.contains(QStringLiteral("apfs"))) {
        return QStringLiteral("Object ID");
    }
    if (fs.contains(QStringLiteral("hfs"))) {
        return QStringLiteral("Catalog ID");
    }
    if (fs.contains(QStringLiteral("ext"))) {
        return QStringLiteral("Inode");
    }
    return QStringLiteral("Identifier");
}

QStringList FileManagementFileSystemBridge::safetyNotes(const FileManagementTarget& target) {
    const QString fs = target.file_system.toLower();
    QStringList notes;
    if (fs.contains(QStringLiteral("apfs"))) {
        notes.append(
            target.can_write_files
                ? QStringLiteral(
                      "APFS writes commit through the Apple-certified in-place COW engine; "
                      "S.A.K.-generated and real Apple-created (foreign) containers are both "
                      "supported. Operations the engine has not certified for this container "
                      "(snapshot-frozen deletes/renames, Fusion/Tier2 sets, locked encrypted "
                      "volumes) fail closed with exact blockers.")
                : QStringLiteral(
                      "APFS writes need a known container size within the certified engine "
                      "range (%1); this target is read-only because its size is unknown or "
                      "out of range. Fusion/Tier2 sets and unprovided-credential encrypted "
                      "volumes also stay read-only.")
                      .arg(apfsCapacityRangeText()));
    } else if (fs.contains(QStringLiteral("hfs"))) {
        notes.append(target.can_write_files
                         ? QStringLiteral("HFS+/HFSX writes commit through the Apple-certified "
                                          "catalog/extents/attributes B-tree writer.")
                         : QStringLiteral("This HFS+/HFSX target is read-only; certified writes "
                                          "require a write-capable raw/image slice."));
    } else if (fs.contains(QStringLiteral("xfs")) || fs.contains(QStringLiteral("btrfs"))) {
        notes.append(
            QStringLiteral("XFS/Btrfs targets are metadata-only in this build; no "
                           "browse/read/write reader ships yet."));
    } else if (fs.contains(QStringLiteral("ext"))) {
        notes.append(
            QStringLiteral("ext2/ext3/ext4 targets are read-only browse/read/copy-out; "
                           "no write path ships."));
    }
    return notes;
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

FileManagementHashResult FileManagementFileSystemBridge::hashFile(
    const FileManagementTarget& target, const QString& path, uint64_t max_bytes) {
    FileManagementHashResult result;
    result.file_system = displayFileSystem(normalizedFileSystem(target.file_system));

    if (target.local_file_system) {
        // Local files hash in full through the chunked reader (memory-safe for large files).
        const QString local = path.trimmed().isEmpty() ? target.root_path : path;
        const auto digest = file_hasher(hash_algorithm::sha256)
                                .calculateHash(std::filesystem::path(local.toStdWString()));
        if (!digest) {
            result.blockers.append(QStringLiteral("Could not hash %1.").arg(local));
            return result;
        }
        result.ok = true;
        result.sha256 = QString::fromStdString(*digest);
        result.hashed_bytes = static_cast<uint64_t>(QFileInfo(local).size());
        return result;
    }

    // Raw/non-native targets are read through their reader up to the cap, then hashed.
    // Request one extra byte so a file of exactly max_bytes is provably complete and is
    // not falsely reported as capped.
    const FileManagementReadResult read =
        readFile(target, path, max_bytes == 0 ? 0 : max_bytes + 1);
    if (!read.ok) {
        result.blockers = read.blockers;
        return result;
    }
    QByteArray data = read.data;
    result.capped = max_bytes != 0 && static_cast<uint64_t>(data.size()) > max_bytes;
    if (result.capped) {
        data.truncate(static_cast<qsizetype>(max_bytes));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(data);
    result.ok = true;
    result.sha256 = QString::fromLatin1(hash.result().toHex());
    result.hashed_bytes = static_cast<uint64_t>(data.size());
    return result;
}

namespace {

// Stream a local source file into an already-open destination, hashing as it goes.
// Returns false and appends a blocker on any read/write error.
bool streamLocalSourceOut(const QString& local,
                          QFileDevice& dest,
                          QCryptographicHash& hash,
                          FileManagementExportResult& result) {
    QFile src(local);
    if (!src.open(QIODevice::ReadOnly)) {
        result.blockers.append(QStringLiteral("Could not open source %1.").arg(local));
        return false;
    }
    constexpr qint64 kWindowBytes = 1 << 20;
    while (!src.atEnd()) {
        const QByteArray chunk = src.read(kWindowBytes);
        if (chunk.isEmpty()) {
            if (src.error() != QFileDevice::NoError) {
                result.blockers.append(
                    QStringLiteral("Read error on %1: %2").arg(local, src.errorString()));
                return false;
            }
            break;
        }
        if (dest.write(chunk) != chunk.size()) {
            result.blockers.append(QStringLiteral("Write error: %1").arg(dest.errorString()));
            return false;
        }
        hash.addData(chunk);
        result.bytes_written += static_cast<uint64_t>(chunk.size());
    }
    return true;
}

// Read a raw/non-native source through its reader (capped) into an open destination.
// The caller reads one byte past the cap, so a source exactly at the cap is complete;
// only a genuinely longer source is truncated back to the cap and flagged.
bool writeRawSourceOut(const FileManagementReadResult& read,
                       QFileDevice& dest,
                       QCryptographicHash& hash,
                       uint64_t max_bytes,
                       FileManagementExportResult& result) {
    if (!read.ok) {
        result.blockers = read.blockers;
        return false;
    }
    QByteArray data = read.data;
    result.capped = max_bytes != 0 && static_cast<uint64_t>(data.size()) > max_bytes;
    if (result.capped) {
        data.truncate(static_cast<qsizetype>(max_bytes));
    }
    if (dest.write(data) != data.size()) {
        result.blockers.append(QStringLiteral("Write error: %1").arg(dest.errorString()));
        return false;
    }
    hash.addData(data);
    result.bytes_written = static_cast<uint64_t>(data.size());
    return true;
}

}  // namespace

FileManagementExportResult FileManagementFileSystemBridge::copyFileToHost(
    const FileManagementTarget& target,
    const QString& source_path,
    const QString& destination_path,
    uint64_t max_bytes) {
    FileManagementExportResult result;
    result.destination = destination_path;

    // QSaveFile writes to a temporary and renames on commit, so a failed copy never
    // truncates or destroys a pre-existing destination file, and commit() surfaces
    // flush errors (disk full) instead of silently reporting a short file as ok.
    QSaveFile dest(destination_path);
    if (!dest.open(QIODevice::WriteOnly)) {
        result.blockers.append(
            QStringLiteral("Could not open destination %1 for writing.").arg(destination_path));
        return result;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QString local = source_path.trimmed().isEmpty() ? target.root_path : source_path;
    const bool ok =
        target.local_file_system
            ? streamLocalSourceOut(local, dest, hash, result)
            : writeRawSourceOut(readFile(target, source_path, max_bytes == 0 ? 0 : max_bytes + 1),
                                dest,
                                hash,
                                max_bytes,
                                result);

    if (!ok) {
        dest.cancelWriting();
        return result;
    }
    if (!dest.commit()) {
        result.blockers.append(QStringLiteral("Could not finalize destination %1: %2")
                                   .arg(destination_path, dest.errorString()));
        return result;
    }
    result.ok = true;
    result.sha256 = QString::fromLatin1(hash.result().toHex());
    return result;
}

namespace {

constexpr int kPreviewHexDumpBytes = 4096;
constexpr int kPreviewHexBytesPerRow = 16;

// Heuristic: a NUL byte, or more than 5% C0 control bytes (excluding tab/newline/return),
// marks the bytes as binary and routes them to the hex dump instead of a text decode.
bool looksBinary(const QByteArray& data) {
    int controls = 0;
    for (const char ch : data) {
        const auto u = static_cast<unsigned char>(ch);
        if (u == 0) {
            return true;
        }
        if (u < 0x09 || (u > 0x0D && u < 0x20) || u == 0x7F) {
            ++controls;
        }
    }
    return !data.isEmpty() && controls * 20 > data.size();
}

// One "OFFSET  HH HH ... |ascii|" row for up to kPreviewHexBytesPerRow bytes at @p offset.
QString hexDumpRow(const QByteArray& data, int offset) {
    QString hex;
    QString ascii;
    for (int col = 0; col < kPreviewHexBytesPerRow; ++col) {
        if (offset + col < data.size()) {
            const auto u = static_cast<unsigned char>(data.at(offset + col));
            hex += QStringLiteral("%1 ").arg(u, 2, 16, QLatin1Char('0'));
            ascii += (u >= 0x20 && u < 0x7F) ? QChar(u) : QLatin1Char('.');
        } else {
            hex += QStringLiteral("   ");
        }
    }
    return QStringLiteral("%1  %2 |%3|").arg(offset, 8, 16, QLatin1Char('0')).arg(hex, ascii);
}

QString hexDump(const QByteArray& window) {
    QStringList rows;
    for (int offset = 0; offset < window.size(); offset += kPreviewHexBytesPerRow) {
        rows << hexDumpRow(window, offset);
    }
    return rows.join(QLatin1Char('\n'));
}

}  // namespace

FileManagementPreview FileManagementFileSystemBridge::renderPreview(const QByteArray& data,
                                                                    bool truncated) {
    FileManagementPreview preview;
    preview.shown_bytes = static_cast<uint64_t>(data.size());
    preview.truncated = truncated;
    if (!looksBinary(data)) {
        preview.text = QString::fromUtf8(data);
        return preview;
    }
    preview.is_binary = true;
    const QByteArray window = data.left(kPreviewHexDumpBytes);
    if (window.size() < data.size()) {
        preview.truncated = true;
        preview.shown_bytes = static_cast<uint64_t>(window.size());
    }
    preview.text = hexDump(window);
    return preview;
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
                                   QStringLiteral(
                                       "APFS File Management directory create needs a name"));
        }
        const auto parts = apfsParts(cleanPath);
        // The new directory's own name is the last path component; everything before it is the
        // parent path the certified crash-safe COW engine nests it under (empty = container root).
        const QString parentPath = parts.size() > 1
                                       ? QLatin1Char('/') +
                                             parts.mid(0, parts.size() - 1).join(QLatin1Char('/'))
                                       : QString();
        return fromApfsCommitResult(
            PartitionApfsWriter::commitRawDirectoryCreate(
                {.target_path = target.root_path,
                 .target_container_bytes = target.size_bytes,
                 .directory_name = parts.last(),
                 .parent_directory_path = parentPath,
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
        // The directory's own name is the last path component; everything before it is the parent
        // path the COW engine resolves the target directory under (empty = container root). The
        // engine matches the directory by name within that resolved parent, so a nested target
        // deletes the leaf -- not the root ancestor -- and fails closed on a non-empty directory.
        const QString parentPath = parts.size() > 1
                                       ? QLatin1Char('/') +
                                             parts.mid(0, parts.size() - 1).join(QLatin1Char('/'))
                                       : QString();
        return fromApfsCommitResult(
            PartitionApfsWriter::commitRawDirectoryDelete(
                {.target_path = target.root_path,
                 .target_container_bytes = target.size_bytes,
                 .directory_name = parts.last(),
                 .parent_directory_path = parentPath,
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
    // No artificial File Management cap: each backend enforces its own real bound (APFS
    // is container-bound; HFS caps inside its writer; a local write is host-FS-bound).
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

FileManagementMutationResult FileManagementFileSystemBridge::writeFileFromHostPath(
    const FileManagementTarget& target, const QString& path, const QString& host_file_path) {
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanPath = displayPath(path);
    const QFileInfo srcInfo(host_file_path);
    if (!srcInfo.isFile()) {
        return mutationBlocked(
            fs, cleanPath, QStringLiteral("Source is not a readable file: %1").arg(host_file_path));
    }
    const uint64_t size = static_cast<uint64_t>(srcInfo.size());
    // APFS and the local filesystem stream from the host file (peak RAM one window), so
    // an arbitrarily large copy is bounded only by the destination's free space.
    if (target.local_file_system) {
        FileManagementMutationResult result = copyLocalFileStreamed(path, host_file_path);
        result.file_system = target.file_system;
        return result;
    }
    if (fs == QStringLiteral("apfs")) {
        return writeApfsFileStreamed(target, cleanPath, host_file_path, size);
    }
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        return writeHfsFileStreamed(target, cleanPath, host_file_path, size);
    }
    // Any remaining backend has no streaming fork writer yet, so it reads the whole file;
    // guard RAM by size before the read. The limit is that backend's honest current bound
    // and is lifted once it gains a streaming writer.
    if (size > kFileManagementMaxWriteBytes) {
        return mutationBlocked(fs,
                               cleanPath,
                               QStringLiteral("Streaming write is not yet available for %1; the "
                                              "file exceeds the buffered write limit")
                                   .arg(displayFileSystem(fs)));
    }
    QFile src(host_file_path);
    if (!src.open(QIODevice::ReadOnly)) {
        return mutationBlocked(
            fs, cleanPath, QStringLiteral("Unable to read source file: %1").arg(src.errorString()));
    }
    return writeFile(target, path, src.readAll());
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
            return mutationBlocked(
                fs, cleanPath, QStringLiteral("APFS File Management file delete requires a name"));
        }
        // The certified crash-safe in-place COW engine deletes the file under its parent path at
        // any depth (empty parent = container root).
        const auto [parent, name] = apfsParentAndName(cleanPath);
        return fromApfsCommitResult(
            PartitionApfsWriter::commitRawFileDelete(
                {.target_path = target.root_path,
                 .target_container_bytes = target.size_bytes,
                 .file_name = name,
                 .parent_directory_path = parent,
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
    const auto [sourceParent, sourceName] = apfsParentAndName(cleanSource);
    const auto [destParent, destName] = apfsParentAndName(cleanDestination);
    if (sourceName.isEmpty() || destName.isEmpty()) {
        return mutationBlocked(QStringLiteral("apfs"),
                               cleanSource,
                               QStringLiteral("APFS File Management rename/move requires a source "
                                              "and destination file name"));
    }
    // commitRawFileMove resolves both parents by full path, so a same-parent move is a plain
    // rename and a cross-parent move reparents the file, each at arbitrary directory depth.
    return fromApfsCommitResult(PartitionApfsWriter::commitRawFileMove(
                                    {.target_path = target.root_path,
                                     .target_container_bytes = target.size_bytes,
                                     .source_directory_name = sourceParent,
                                     .file_name = sourceName,
                                     .destination_directory_name = destParent,
                                     .new_file_name = destName,
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
