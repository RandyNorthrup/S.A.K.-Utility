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
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStorageInfo>
#include <QtEndian>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

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
    // Windows device paths are case-insensitive, so a lowercase "\\?\globalroot\..." names
    // the same raw device; matching case-sensitively would misclassify it as an image file
    // and skip the raw-partition safety gate.
    return path.startsWith(QStringLiteral("\\\\.\\"), Qt::CaseInsensitive) ||
           path.startsWith(QStringLiteral("\\\\?\\GLOBALROOT\\"), Qt::CaseInsensitive);
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
    // Reject a block count that would overflow the 64-bit byte size. A corrupt
    // superblock with an absurd nx_block_count otherwise wraps to a small,
    // in-range value and marks a garbage container as writable.
    if (block_count > std::numeric_limits<quint64>::max() / block_size) {
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

sak::PartitionApfsWriteOptions apfsRawWriteOptions(const FileManagementTarget& target) {
    // The certified writer's enable + destructive/hardware certification-evidence
    // attestations are derived from the target actually being writable, so a
    // read-only / non-writable target makes the engine fail closed with its own
    // certification blockers rather than mutating the medium (B8-04).
    const bool writable = FileManagementFileSystemBridge::targetPermitsMutation(target);
    sak::PartitionApfsWriteOptions options;
    options.enable_experimental_writer = writable;
    options.image_only = false;
    options.destructive_certification_evidence = writable;
    options.raw_media_hardware_certification_evidence = writable;
    // No artificial payload cap: the APFS write is bounded only by the container's free
    // space (the multi-chunk allocator fails closed when it will not fit). 0 = unbounded.
    options.max_payload_bytes = 0;
    options.evidence_id = QStringLiteral("file-management.apfs.raw");
    return options;
}

sak::PartitionHfsFileWriteOptions hfsWriteOptions(const FileManagementTarget& target) {
    // enable_writer + target_write_confirmed track the target's real writability so a
    // read-only / non-writable target fails closed in the writer engine (B8-04).
    const bool writable = FileManagementFileSystemBridge::targetPermitsMutation(target);
    sak::PartitionHfsFileWriteOptions options;
    options.enable_writer = writable;
    options.target_write_confirmed = writable;
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

// Fail-closed capability gate every bridge mutation entrypoint re-checks itself.
// The bridge is directly callable (headless / MCP), so it must not trust an
// upstream UI/registry gate: a target that does not permit mutation is refused
// here before any filesystem or certified-writer call. Returns a populated blocked
// result to return, or nullopt when the mutation may proceed (Codex-3, defense in
// depth over the engine's own evidence gate).
std::optional<FileManagementMutationResult> mutationCapabilityBlock(
    const FileManagementTarget& target, const QString& path) {
    if (FileManagementFileSystemBridge::targetPermitsMutation(target)) {
        return std::nullopt;
    }
    const QString fs = FileManagementFileSystemBridge::normalizedFileSystem(target.file_system);
    return mutationBlocked(fs,
                           path,
                           target.read_only
                               ? QStringLiteral("Target is read-only; the write was refused.")
                               : QStringLiteral("Target does not permit writes; the write was "
                                                "refused."));
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

    // Inventory may report a volume as read-only (write-protected, read-only
    // mount). Preserve that so a writable-looking badge is not shown for a
    // volume the OS will reject writes on.
    const bool inbound_read_only = target.read_only;

    target.file_system = displayFileSystem(fs);
    target.local_file_system = target.kind == FileManagementTargetKind::LocalPath || native;
    target.read_only = inbound_read_only || !target.local_file_system;
    target.can_browse = target.local_file_system || readableNonNative;
    target.can_read_files = target.local_file_system || readableNonNative;
    target.can_write_files = target.local_file_system || computeWritableNonNative(fs, target);
    target.can_organize = target.local_file_system;
    target.can_duplicate_scan = target.local_file_system || readableNonNative;
    target.can_advanced_search = target.local_file_system || readableNonNative;
    // A volume the inventory reports read-only (hardware write-protect, read-only
    // mount) cannot accept writes -- whether it is a LOCAL volume or a raw
    // non-native target the certified engine would otherwise write. Gate on the
    // INBOUND flag: target.read_only is forced true for every non-local target as a
    // browsing-semantics badge, so it cannot distinguish a real write-protect;
    // inbound_read_only can. Previously this was gated on local_file_system, so a
    // write-protected raw HFS/APFS target was still reported writable (B8-03).
    if (inbound_read_only) {
        target.can_write_files = false;
        target.can_organize = false;
    }
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
    entry.hidden = info.isHidden();
    entry.link_target = info.symLinkTarget();
    if (entry.name.isEmpty()) {
        entry.name = basePath;
    }
    return entry;
}

// Display order for a local listing: directories first, then a case-insensitive
// name compare. The GUI proxy re-sorts, so this only decides which entries survive
// the cap and the order seen by direct (non-GUI) callers.
bool localEntryLess(const QFileInfo& a, const QFileInfo& b) {
    if (a.isDir() != b.isDir()) {
        return a.isDir();
    }
    return QString::compare(a.fileName(), b.fileName(), Qt::CaseInsensitive) < 0;
}

// Stream a directory, keeping only the display-order-smallest `cap` entries in
// O(cap) memory via a bounded max-heap, so an enormous or hostile directory can
// never force its whole QFileInfoList (one stat per entry) into RAM before the
// cap is applied (B8-20). cap <= 0 keeps every entry. Hidden and System entries
// are enumerated -- otherwise the source listing drops them before the view's
// show-hidden filter can ever act, leaving that toggle dead and hiding files a
// folder copy would still transfer. `overflowed` is set when entries were dropped.
std::vector<QFileInfo> collectLocalEntries(const QString& path, int cap, bool& overflowed) {
    overflowed = false;
    std::vector<QFileInfo> kept;
    QDirIterator iter(path,
                      QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                      QDirIterator::NoIteratorFlags);
    while (iter.hasNext()) {
        iter.next();
        QFileInfo info = iter.fileInfo();
        if (cap <= 0 || static_cast<int>(kept.size()) < cap) {
            kept.push_back(std::move(info));
            if (cap > 0) {
                std::push_heap(kept.begin(), kept.end(), localEntryLess);
            }
            continue;
        }
        overflowed = true;
        if (localEntryLess(info, kept.front())) {
            std::pop_heap(kept.begin(), kept.end(), localEntryLess);
            kept.back() = std::move(info);
            std::push_heap(kept.begin(), kept.end(), localEntryLess);
        }
    }
    std::sort(kept.begin(), kept.end(), localEntryLess);
    return kept;
}

FileManagementListResult listLocalDirectory(const QString& path, int maxEntries) {
    FileManagementListResult result;
    result.ok = false;
    result.file_system = QStringLiteral("Local");
    if (!QDir(path).exists()) {
        result.blockers.append(QStringLiteral("Directory does not exist: %1").arg(path));
        return result;
    }

    bool overflowed = false;
    const std::vector<QFileInfo> entries = collectLocalEntries(path, maxEntries, overflowed);
    result.entries.reserve(static_cast<qsizetype>(entries.size()));
    for (const QFileInfo& info : entries) {
        result.entries.append(fromLocalInfo(info, path));
    }
    if (overflowed) {
        result.warnings.append(
            QStringLiteral("Listing truncated to %1 entries").arg(result.entries.size()));
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
        FileManagementEntry entry;
        entry.name = item.name;
        entry.path = item.path;
        entry.type = item.type;
        entry.size_bytes = item.size_bytes;
        entry.identifier = QString::number(item.catalog_id);
        entry.directory = item.directory;
        entry.regular_file = item.regular_file;
        entry.symlink = item.symbolic_link;
        entry.link_target = item.symbolic_link ? item.name : QString();
        entry.resource_fork_bytes = item.resource_fork_size_bytes;
        result.entries.append(entry);
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
        FileManagementEntry entry;
        entry.name = item.name;
        entry.path = item.path;
        entry.type = item.type;
        entry.size_bytes = item.size_bytes;
        entry.identifier = QString::number(item.object_id);
        entry.directory = item.directory;
        entry.regular_file = item.regular_file;
        entry.symlink = item.symlink;
        entry.compressed = item.compressed;
        entry.sparse = item.sparse;
        result.entries.append(entry);
    }
    return result;
}

QString pathOrRoot(const QString& path) {
    const QString trimmed = path.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("/") : trimmed;
}

// One past the read cap for the "read one extra to detect an over-cap / TOCTOU-grown
// file" probe, saturating so a UINT64_MAX cap does not wrap to 0 -- which would either
// read nothing (local) or disable the cap entirely (raw readers treat 0 as unlimited).
// A 0 cap ("no limit") stays 0 (B8-18).
uint64_t readCapProbe(uint64_t max_bytes) {
    if (max_bytes == 0 || max_bytes == std::numeric_limits<uint64_t>::max()) {
        return max_bytes;
    }
    return max_bytes + 1;
}

FileManagementReadResult readLocalFile(const QString& path, uint64_t maxBytes) {
    FileManagementReadResult result;
    result.file_system = QStringLiteral("Local");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.blockers.append(QStringLiteral("Could not open file: %1").arg(path));
        return result;
    }

    QByteArray data;
    if (maxBytes > 0) {
        if (static_cast<uint64_t>(file.size()) > maxBytes) {
            result.blockers.append(
                QStringLiteral("File exceeds read limit: %1 bytes").arg(file.size()));
            return result;
        }
        // Read one past the cap so a file that grew between size() and read
        // (TOCTOU) is rejected rather than returned over-limit. readCapProbe
        // saturates so a UINT64_MAX cap does not wrap to read(0). A probe past the
        // QFile::read/QByteArray argument range means a cap no real file can exceed
        // (file.size() <= maxBytes was already checked), so read the whole file --
        // the over-limit check below then never trips, which is correct (B8-18).
        const uint64_t probe = readCapProbe(maxBytes);
        const auto qint64Max = static_cast<uint64_t>(std::numeric_limits<qint64>::max());
        data = probe > qint64Max ? file.readAll() : file.read(static_cast<qint64>(probe));
        if (static_cast<uint64_t>(data.size()) > maxBytes) {
            result.blockers.append(
                QStringLiteral("File exceeds read limit: %1 bytes").arg(maxBytes));
            return result;
        }
    } else {
        data = file.readAll();
    }

    // readAll()/read() return whatever was read before an I/O fault without
    // signalling failure; check error() so truncated data is not surfaced as a
    // complete, successful read.
    if (file.error() != QFileDevice::NoError) {
        result.blockers.append(
            QStringLiteral("Read error on %1: %2").arg(path, file.errorString()));
        return result;
    }

    result.data = data;
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
    return fromApfsCommitResult(
        PartitionApfsWriter::commitRawFileWrite(
            {.target_path = target.root_path,
             .target_container_bytes = target.size_bytes,
             .file_name = name,
             .file_data = data,
             .parent_directory_path = parent,
             .target_mutation_confirmed = true,
             .allow_raw_device_target =
                 PartitionApfsWriter::acceptsRawDeviceTargetPath(target.root_path),
             .options = apfsRawWriteOptions(target)}),
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
    return fromApfsCommitResult(
        PartitionApfsWriter::commitRawFileWrite(
            {.target_path = target.root_path,
             .target_container_bytes = target.size_bytes,
             .file_name = name,
             .file_data_path = hostPath,
             .file_data_stream_size = size,
             .parent_directory_path = parent,
             .target_mutation_confirmed = true,
             .allow_raw_device_target =
                 PartitionApfsWriter::acceptsRawDeviceTargetPath(target.root_path),
             .options = apfsRawWriteOptions(target)}),
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

// APFS/HFS+ streamed write with observer reporting: the certified engines
// stream internally (and stay untouched), so the observer sees one whole-file
// delta on success.
FileManagementMutationResult writeObservedRawStream(
    const FileManagementTarget& target,
    const QString& cleanPath,
    const QString& hostPath,
    const uint64_t size,
    const FileManagementTransferObserver& observer) {
    const QString fs = FileManagementFileSystemBridge::normalizedFileSystem(target.file_system);
    const FileManagementMutationResult result =
        fs == QStringLiteral("apfs") ? writeApfsFileStreamed(target, cleanPath, hostPath, size)
                                     : writeHfsFileStreamed(target, cleanPath, hostPath, size);
    if (result.ok) {
        observer.addBytes(static_cast<qint64>(size));
    }
    return result;
}

// Streaming local-filesystem copy: host file -> destination through a fixed 1 MiB
// window, so a multi-GB copy never holds the whole payload in RAM. The observer
// sees every window land and can cancel between windows.
FileManagementMutationResult copyLocalFileStreamed(const QString& destPath,
                                                   const QString& hostPath,
                                                   const FileManagementTransferObserver& observer) {
    FileManagementMutationResult result;
    result.path = destPath;
    QFile src(hostPath);
    // QSaveFile streams into a temporary and renames on commit(), so a canceled
    // copy, a mid-stream read error, or a short/failed flush leaves any existing
    // destination file untouched instead of truncating it to a partial file.
    QSaveFile dst(destPath);
    if (!src.open(QIODevice::ReadOnly)) {
        result.blockers.append(
            QStringLiteral("Unable to read source file: %1").arg(src.errorString()));
        return result;
    }
    if (!dst.open(QIODevice::WriteOnly)) {
        result.blockers.append(QStringLiteral("Unable to write file: %1").arg(dst.errorString()));
        return result;
    }
    QByteArray window(1 << 20, Qt::Uninitialized);
    uint64_t written = 0;
    for (;;) {
        if (observer.isCancelled()) {
            // Files removes the partial destination of a canceled copy; cancelWriting
            // discards the temporary and keeps the original destination intact.
            dst.cancelWriting();
            result.blockers.append(kFileManagementTransferCancelledBlocker);
            return result;
        }
        const qint64 n = src.read(window.data(), window.size());
        if (n < 0) {
            dst.cancelWriting();
            result.blockers.append(
                QStringLiteral("Read error while streaming: %1").arg(src.errorString()));
            return result;
        }
        if (n == 0) {
            break;
        }
        if (dst.write(window.constData(), n) != n) {
            dst.cancelWriting();
            result.blockers.append(
                QStringLiteral("Short write while streaming file: %1").arg(destPath));
            return result;
        }
        written += static_cast<uint64_t>(n);
        observer.addBytes(n);
    }
    if (!dst.commit()) {
        result.blockers.append(
            QStringLiteral("Unable to finalize file: %1").arg(dst.errorString()));
        return result;
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

namespace {

// True when a bare filename carries a byte that is unsafe on a Windows host: the
// NTFS alternate-data-stream separator ':', any reserved path/wildcard character,
// or a raw control byte. A ':' would divert the export into a hidden ADS.
bool hasUnsafeHostNameChar(const QString& base) {
    static const QString kReserved = QStringLiteral(":<>\"|?*/\\");
    for (const QChar ch : base) {
        if (ch.unicode() < 0x20 || kReserved.contains(ch)) {
            return true;
        }
    }
    return false;
}

// True when @p base resolves to a Windows reserved device name (CON, PRN, AUX,
// NUL, COM1-9, LPT1-9) even with an extension -- "NUL.txt" still opens the device.
// Matched case-insensitively on the stem before the first dot.
bool isWindowsReservedDeviceName(const QString& base) {
    const QString stem = base.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    static const QStringList kNames = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL")};
    if (kNames.contains(stem)) {
        return true;
    }
    const QString prefix = stem.left(3);
    if (stem.size() != 4 || (prefix != QStringLiteral("COM") && prefix != QStringLiteral("LPT"))) {
        return false;
    }
    const QChar last = stem.at(3);
    return last >= QLatin1Char('1') && last <= QLatin1Char('9');
}

}  // namespace

QString FileManagementFileSystemBridge::confinedHostName(const QString& raw_name) {
    // QFileInfo::fileName drops any path components (a crafted "../../evil" or "a\b"
    // collapses to its last segment); "."/".."/empty are then rejected (B8-02).
    const QString base = QFileInfo(raw_name).fileName();
    if (base.isEmpty() || base == QStringLiteral(".") || base == QStringLiteral("..")) {
        return QString();
    }
    // Foreign (untrusted image) entry names must additionally be hardened against
    // Windows filename hazards before they name a host file (Codex-3): an NTFS
    // alternate-data-stream separator or other reserved character, a reserved
    // device name (CON/NUL/COM1...), and a trailing dot or space that Windows
    // silently strips -- collapsing "name " onto sibling "name" and overwriting it.
    // Reject fail-closed rather than coerce the name. The raw name is also scanned
    // for ':' because QFileInfo may split "foo:bar" on a ':' it reads as a drive,
    // hiding the ADS separator from the reduced base.
    if (hasUnsafeHostNameChar(base) || raw_name.contains(QLatin1Char(':')) ||
        isWindowsReservedDeviceName(base)) {
        return QString();
    }
    if (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' '))) {
        return QString();
    }
    return base;
}

bool FileManagementFileSystemBridge::isUnsafeLocalDeletePath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return true;  // QDir("") resolves to the current working directory
    }
    const QDir dir(trimmed);
    // A filesystem root ("C:/", "/", "\\server\share") must never be recursively
    // deleted. Check both the given path and its resolved absolute form.
    if (dir.isRoot()) {
        return true;
    }
    const QString absolute = QDir::cleanPath(dir.absolutePath());
    return absolute.isEmpty() || QDir(absolute).isRoot();
}

RawReplaceCaseAction FileManagementFileSystemBridge::rawReplaceCaseAction(
    const QString& normalized_fs, int case_insensitive_matches) {
    if (case_insensitive_matches > 1) {
        return RawReplaceCaseAction::RefuseMultipleCase;
    }
    if (case_insensitive_matches <= 0) {
        return RawReplaceCaseAction::TreatAsVacant;
    }
    // Exactly one occupant that differs from the target only by case.
    if (normalized_fs == QStringLiteral("hfsplus")) {
        return RawReplaceCaseAction::DeleteDifferingCase;  // HFS+ is case-insensitive: same file.
    }
    if (normalized_fs == QStringLiteral("hfsx")) {
        return RawReplaceCaseAction::TreatAsVacant;  // HFSX is case-sensitive: a distinct file.
    }
    // APFS per-volume case-sensitivity is not surfaced to the bridge, so we cannot
    // tell whether this is the same file. Refuse rather than delete the wrong one.
    return RawReplaceCaseAction::RefuseAmbiguous;
}

bool FileManagementFileSystemBridge::targetPermitsMutation(const FileManagementTarget& target) {
    // can_write_files is the single genuine-writability signal after
    // applyCapabilities: it is set only for a local volume or a writer-certified raw
    // FS, and is forced FALSE when the inventory reports a real write-protect /
    // read-only mount (inbound_read_only, B8-03). The target.read_only FIELD is a
    // DIFFERENT thing -- it is forced true for every non-local target purely as a
    // browsing-semantics badge, so the previous `!read_only` clause disabled EVERY
    // advertised raw HFS/APFS write (B8-04 accidentally broke the certified
    // raw-writer path: enable_writer never turned on for a non-local target).
    // Gate on can_write_files alone: a genuine write-protect still fails closed
    // (can_write_files == false) while an advertised, in-range raw write reaches the
    // certified engine (Codex-3).
    return target.can_write_files;
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
        // QFileInfo::size() returns -1 on a stat failure; casting that to uint64_t
        // would report UINT64_MAX hashed bytes. Fail closed on an unresolved size
        // rather than surface a bogus count.
        const qint64 size = QFileInfo(local).size();
        if (size < 0) {
            result.blockers.append(
                QStringLiteral("Could not determine the size of %1.").arg(local));
            return result;
        }
        result.ok = true;
        result.sha256 = QString::fromStdString(*digest);
        result.hashed_bytes = static_cast<uint64_t>(size);
        return result;
    }

    // Raw/non-native targets are read through their reader up to the cap, then hashed.
    // Request one extra byte so a file of exactly max_bytes is provably complete and is
    // not falsely reported as capped.
    const FileManagementReadResult read = readFile(target, path, readCapProbe(max_bytes));
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
// Returns false and appends a blocker on any read/write error or a cancel.
bool streamLocalSourceOut(const QString& local,
                          QFileDevice& dest,
                          QCryptographicHash& hash,
                          FileManagementExportResult& result,
                          const FileManagementTransferObserver& observer) {
    QFile src(local);
    if (!src.open(QIODevice::ReadOnly)) {
        result.blockers.append(QStringLiteral("Could not open source %1.").arg(local));
        return false;
    }
    constexpr qint64 kWindowBytes = 1 << 20;
    while (!src.atEnd()) {
        if (observer.isCancelled()) {
            result.blockers.append(kFileManagementTransferCancelledBlocker);
            return false;
        }
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
        observer.addBytes(chunk.size());
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
    uint64_t max_bytes,
    const FileManagementTransferObserver& observer) {
    FileManagementExportResult result;
    result.destination = destination_path;
    if (observer.isCancelled()) {
        result.blockers.append(kFileManagementTransferCancelledBlocker);
        return result;
    }

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
    const bool ok = target.local_file_system
                        ? streamLocalSourceOut(local, dest, hash, result, observer)
                        : writeRawSourceOut(readFile(target, source_path, readCapProbe(max_bytes)),
                                            dest,
                                            hash,
                                            max_bytes,
                                            result);
    if (ok && !target.local_file_system) {
        // Raw sources land as one buffered read; report the whole delta.
        observer.addBytes(static_cast<qint64>(result.bytes_written));
    }

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

// Bounds shared by the recursive directory walks (export, import, and tree delete):
// deep-enough for real trees, still finite so a cyclic or hostile directory graph
// cannot run away.
constexpr int kDirectoryExportMaxDepth = 32;
constexpr int kDirectoryExportMaxEntriesPerDirectory = 10'000;

struct DirectoryExportContext {
    const FileManagementTarget& target;
    uint64_t max_file_bytes;
    FileManagementDirectoryExportResult& result;
    const FileManagementTransferObserver& observer;
};

void exportEntryToHost(const DirectoryExportContext& ctx,
                       const FileManagementEntry& entry,
                       const QDir& host_dir,
                       int depth);

void exportDirectoryLevel(const DirectoryExportContext& ctx,
                          const QString& source_path,
                          const QDir& host_dir,
                          const int depth) {
    if (depth > kDirectoryExportMaxDepth) {
        ++ctx.result.entries_skipped;
        ctx.result.warnings.append(
            QStringLiteral("Skipped %1: exceeds the export depth bound.").arg(source_path));
        return;
    }
    // Request one past the cap so an over-full directory is detected precisely
    // (a plain cap-sized listing cannot tell "exactly full" from "truncated").
    const FileManagementListResult listing = FileManagementFileSystemBridge::listDirectory(
        ctx.target, source_path, kDirectoryExportMaxEntriesPerDirectory + 1);
    if (!listing.ok) {
        ctx.result.blockers.append(listing.blockers.isEmpty()
                                       ? QStringLiteral("Could not list %1.").arg(source_path)
                                       : listing.blockers.join(QStringLiteral("; ")));
        return;
    }
    ctx.result.warnings.append(listing.warnings);
    qsizetype process = listing.entries.size();
    if (process > kDirectoryExportMaxEntriesPerDirectory) {
        ctx.result.entries_skipped +=
            static_cast<int>(process - kDirectoryExportMaxEntriesPerDirectory);
        ctx.result.warnings.append(
            QStringLiteral("%1 holds more than %2 entries; the remainder was skipped.")
                .arg(source_path)
                .arg(kDirectoryExportMaxEntriesPerDirectory));
        process = kDirectoryExportMaxEntriesPerDirectory;
    }
    for (qsizetype index = 0; index < process; ++index) {
        if (ctx.observer.isCancelled()) {
            ctx.result.blockers.append(kFileManagementTransferCancelledBlocker);
            return;
        }
        exportEntryToHost(ctx, listing.entries.at(index), host_dir, depth);
    }
}

void exportEntryToHost(const DirectoryExportContext& ctx,
                       const FileManagementEntry& entry,
                       const QDir& host_dir,
                       const int depth) {
    if (entry.symlink) {
        ++ctx.result.symlinks_skipped;
        ctx.result.warnings.append(
            QStringLiteral("Skipped symlink %1 (links are not exported).").arg(entry.path));
        return;
    }
    // The entry name comes verbatim from a parsed (untrusted) foreign image; confine
    // it to a bare host filename so it cannot escape the export directory (B8-02).
    const QString host_name = FileManagementFileSystemBridge::confinedHostName(entry.name);
    if (host_name.isEmpty()) {
        ctx.result.warnings.append(
            QStringLiteral("Skipped entry with unsafe name %1.").arg(entry.path));
        return;
    }
    if (entry.directory) {
        if (!host_dir.mkpath(host_name)) {
            ctx.result.blockers.append(QStringLiteral("Could not create host directory %1.")
                                           .arg(host_dir.filePath(host_name)));
            return;
        }
        ++ctx.result.directories_created;
        exportDirectoryLevel(ctx, entry.path, QDir(host_dir.filePath(host_name)), depth + 1);
        return;
    }
    if (!entry.regular_file) {
        ctx.result.warnings.append(QStringLiteral("Skipped special entry %1.").arg(entry.path));
        return;
    }
    const FileManagementExportResult exported = FileManagementFileSystemBridge::copyFileToHost(
        ctx.target, entry.path, host_dir.filePath(host_name), ctx.max_file_bytes, ctx.observer);
    if (!exported.ok) {
        ctx.result.blockers.append(exported.blockers.isEmpty()
                                       ? QStringLiteral("Could not export %1.").arg(entry.path)
                                       : exported.blockers.join(QStringLiteral("; ")));
        return;
    }
    ++ctx.result.files_exported;
    ctx.result.bytes_written += exported.bytes_written;
    if (exported.capped) {
        ++ctx.result.capped_files;
        ctx.result.warnings.append(
            QStringLiteral("%1 was truncated at the per-file read window.").arg(entry.path));
    }
}

}  // namespace

FileManagementDirectoryExportResult FileManagementFileSystemBridge::exportDirectoryToHost(
    const FileManagementTarget& target,
    const QString& source_path,
    const QString& destination_dir,
    const uint64_t max_file_bytes,
    const FileManagementTransferObserver& observer) {
    FileManagementDirectoryExportResult result;
    result.destination = destination_dir;
    if (!QDir().mkpath(destination_dir)) {
        result.blockers.append(
            QStringLiteral("Could not create destination directory %1.").arg(destination_dir));
        return result;
    }
    const DirectoryExportContext ctx{target, max_file_bytes, result, observer};
    exportDirectoryLevel(ctx, source_path, QDir(destination_dir), 0);
    // ok == "the walk hit no hard blocker"; it is NOT a wholeness claim BY DESIGN.
    // A caller that must distinguish a whole transfer from a partial one reads
    // `complete` (below) and `capped_files` -- ok alone must never be read as "the
    // entire tree copied" (Codex-3 finding 5, surfaced not coerced).
    result.ok = result.blockers.isEmpty();
    result.complete = result.ok && result.symlinks_skipped == 0 && result.entries_skipped == 0 &&
                      result.capped_files == 0;
    return result;
}

namespace {

// Join a child name onto a target-side base path in the target's own path style:
// host-native for local targets, "/"-separated for raw file-system paths.
QString targetChildPath(const FileManagementTarget& target,
                        const QString& base,
                        const QString& name) {
    if (target.local_file_system) {
        return QDir(base).filePath(name);
    }
    QString clean = base;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!clean.startsWith(QLatin1Char('/'))) {
        clean.prepend(QLatin1Char('/'));
    }
    if (!clean.endsWith(QLatin1Char('/'))) {
        clean.append(QLatin1Char('/'));
    }
    return clean + name;
}

struct DirectoryImportContext {
    const FileManagementTarget& target;
    FileManagementDirectoryImportResult& result;
    const FileManagementTransferObserver& observer;
};

void importEntryFromHost(const DirectoryImportContext& ctx,
                         const QFileInfo& info,
                         const QString& destination_dir,
                         int depth);

void importDirectoryLevel(const DirectoryImportContext& ctx,
                          const QString& host_dir,
                          const QString& destination_dir,
                          const int depth) {
    if (depth > kDirectoryExportMaxDepth) {
        ++ctx.result.entries_skipped;
        ctx.result.warnings.append(
            QStringLiteral("Skipped %1: exceeds the import depth bound.").arg(host_dir));
        return;
    }
    const QFileInfoList infos = QDir(host_dir).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    if (infos.size() > kDirectoryExportMaxEntriesPerDirectory) {
        ctx.result.entries_skipped +=
            static_cast<int>(infos.size() - kDirectoryExportMaxEntriesPerDirectory);
        ctx.result.warnings.append(
            QStringLiteral("%1 holds more than %2 entries; the remainder was skipped.")
                .arg(host_dir)
                .arg(kDirectoryExportMaxEntriesPerDirectory));
    }
    const qsizetype count = std::min<qsizetype>(infos.size(),
                                                kDirectoryExportMaxEntriesPerDirectory);
    for (qsizetype index = 0; index < count; ++index) {
        if (ctx.observer.isCancelled()) {
            ctx.result.blockers.append(kFileManagementTransferCancelledBlocker);
            return;
        }
        importEntryFromHost(ctx, infos.at(index), destination_dir, depth);
    }
}

void importEntryFromHost(const DirectoryImportContext& ctx,
                         const QFileInfo& info,
                         const QString& destination_dir,
                         const int depth) {
    if (info.isSymLink()) {
        ++ctx.result.symlinks_skipped;
        ctx.result.warnings.append(QStringLiteral("Skipped symlink %1 (links are not imported.)")
                                       .arg(info.absoluteFilePath()));
        return;
    }
    const QString destination = targetChildPath(ctx.target, destination_dir, info.fileName());
    if (info.isDir()) {
        const FileManagementMutationResult created =
            FileManagementFileSystemBridge::createDirectory(ctx.target, destination);
        if (!created.ok) {
            ctx.result.blockers.append(
                created.blockers.isEmpty()
                    ? QStringLiteral("Could not create directory %1.").arg(destination)
                    : created.blockers.join(QStringLiteral("; ")));
            return;
        }
        ++ctx.result.directories_created;
        importDirectoryLevel(ctx, info.absoluteFilePath(), destination, depth + 1);
        return;
    }
    if (!info.isFile()) {
        ctx.result.warnings.append(
            QStringLiteral("Skipped special entry %1.").arg(info.absoluteFilePath()));
        return;
    }
    const FileManagementMutationResult written =
        FileManagementFileSystemBridge::writeFileFromHostPath(
            ctx.target, destination, info.absoluteFilePath(), ctx.observer);
    if (!written.ok) {
        ctx.result.blockers.append(
            written.blockers.isEmpty()
                ? QStringLiteral("Could not import %1.").arg(info.absoluteFilePath())
                : written.blockers.join(QStringLiteral("; ")));
        return;
    }
    ++ctx.result.files_imported;
    ctx.result.bytes_written += written.bytes_written;
}

}  // namespace

FileManagementDirectoryImportResult FileManagementFileSystemBridge::importDirectoryFromHost(
    const FileManagementTarget& target,
    const QString& host_source_dir,
    const QString& destination_path,
    const FileManagementTransferObserver& observer) {
    FileManagementDirectoryImportResult result;
    result.destination = destination_path;
    const QFileInfo source(host_source_dir);
    if (!source.isDir() || source.isSymLink()) {
        result.blockers.append(
            QStringLiteral("Source is not a readable directory: %1").arg(host_source_dir));
        return result;
    }
    const FileManagementMutationResult created = createDirectory(target, destination_path);
    if (!created.ok) {
        result.blockers.append(
            created.blockers.isEmpty()
                ? QStringLiteral("Could not create destination directory %1.").arg(destination_path)
                : created.blockers.join(QStringLiteral("; ")));
        return result;
    }
    const DirectoryImportContext ctx{target, result, observer};
    importDirectoryLevel(ctx, source.absoluteFilePath(), destination_path, 0);
    result.ok = result.blockers.isEmpty();
    result.complete = result.ok && result.symlinks_skipped == 0 && result.entries_skipped == 0;
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
    if (auto blocked = mutationCapabilityBlock(target, path)) {
        return *blocked;
    }
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
                 .allow_raw_device_target =
                     PartitionApfsWriter::acceptsRawDeviceTargetPath(target.root_path),
                 .options = apfsRawWriteOptions(target)}),
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
    if (auto blocked = mutationCapabilityBlock(target, path)) {
        return *blocked;
    }
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanPath = displayPath(path);
    if (target.local_file_system) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        // Refuse a catastrophic delete before touching the filesystem: an empty path
        // makes QDir the current working directory and a root would wipe the whole
        // volume (B8-01).
        if (isUnsafeLocalDeletePath(path)) {
            result.ok = false;
            result.blockers.append(
                QStringLiteral("Refusing to recursively delete an empty path or filesystem root: "
                               "%1")
                    .arg(path));
            return result;
        }
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
                 .allow_raw_device_target =
                     PartitionApfsWriter::acceptsRawDeviceTargetPath(target.root_path),
                 .options = apfsRawWriteOptions(target)}),
            cleanPath,
            0);
    }
    return mutationBlocked(
        fs,
        cleanPath,
        QStringLiteral("Directory delete is not supported for %1").arg(displayFileSystem(fs)));
}

namespace {

// Depth-first tree delete for backends whose directory delete fails closed on a
// non-empty directory (the APFS guard stays): every child file and subdirectory is
// removed through the same certified per-entry commits, then the emptied directory
// itself is deleted.
FileManagementMutationResult deleteDirectoryTreeDepthFirst(const FileManagementTarget& target,
                                                           const QString& path,
                                                           const int depth) {
    if (depth > kDirectoryExportMaxDepth) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        result.blockers.append(
            QStringLiteral("Skipped %1: exceeds the delete depth bound.").arg(path));
        return result;
    }
    // List one past the per-directory cap so a directory too large to enumerate
    // in a single pass is detected. Deleting a truncated listing would remove the
    // first N entries and then fail closed on the still-non-empty parent, leaving
    // the tree partially and irreversibly destroyed.
    const FileManagementListResult listing = FileManagementFileSystemBridge::listDirectory(
        target, path, kDirectoryExportMaxEntriesPerDirectory + 1);
    if (!listing.ok) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        result.blockers.append(listing.blockers.isEmpty()
                                   ? QStringLiteral("Could not list %1.").arg(path)
                                   : listing.blockers.join(QStringLiteral("; ")));
        return result;
    }
    if (listing.entries.size() > kDirectoryExportMaxEntriesPerDirectory ||
        !listing.warnings.isEmpty()) {
        // Fail closed BEFORE removing anything so we never partially destroy a
        // directory we cannot finish enumerating.
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        result.blockers.append(
            QStringLiteral("Directory %1 has more than %2 entries; aborting before any deletion "
                           "to avoid partial, irreversible destruction.")
                .arg(path)
                .arg(kDirectoryExportMaxEntriesPerDirectory));
        return result;
    }
    for (const FileManagementEntry& entry : listing.entries) {
        const FileManagementMutationResult removed =
            entry.directory ? deleteDirectoryTreeDepthFirst(target, entry.path, depth + 1)
                            : FileManagementFileSystemBridge::deleteFile(target, entry.path);
        if (!removed.ok) {
            return removed;
        }
    }
    return FileManagementFileSystemBridge::deleteDirectory(target, path);
}

}  // namespace

FileManagementMutationResult FileManagementFileSystemBridge::deleteDirectoryTree(
    const FileManagementTarget& target, const QString& path) {
    if (auto blocked = mutationCapabilityBlock(target, path)) {
        return *blocked;
    }
    const QString fs = normalizedFileSystem(target.file_system);
    // Local (QDir::removeRecursively) and HFS+ (deleteFolderTree) already remove a
    // whole tree in one operation; everything else empties depth-first first.
    if (target.local_file_system || fs == QStringLiteral("hfsplus") ||
        fs == QStringLiteral("hfsx")) {
        return deleteDirectory(target, path);
    }
    return deleteDirectoryTreeDepthFirst(target, path, 0);
}

FileManagementMutationResult FileManagementFileSystemBridge::writeFile(
    const FileManagementTarget& target, const QString& path, const QByteArray& data) {
    if (auto blocked = mutationCapabilityBlock(target, path)) {
        return *blocked;
    }
    const QString fs = normalizedFileSystem(target.file_system);
    const QString cleanPath = displayPath(path);
    // No artificial File Management cap: each backend enforces its own real bound (APFS
    // is container-bound; HFS caps inside its writer; a local write is host-FS-bound).
    if (target.local_file_system) {
        FileManagementMutationResult result;
        result.file_system = target.file_system;
        result.path = path;
        // QSaveFile writes to a temporary and atomically renames on commit(), so
        // a short write (e.g. the destination volume runs out of space) leaves
        // the original file intact instead of truncating it to zero and losing
        // the previous contents.
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            result.blockers.append(
                QStringLiteral("Unable to write file: %1").arg(file.errorString()));
            return result;
        }
        const qint64 written = file.write(data);
        if (written != static_cast<qint64>(data.size())) {
            result.blockers.append(QStringLiteral("Short write while writing file: %1").arg(path));
            file.cancelWriting();
            return result;
        }
        if (!file.commit()) {
            result.blockers.append(
                QStringLiteral("Failed to commit file %1: %2").arg(path, file.errorString()));
            return result;
        }
        result.bytes_written = static_cast<uint64_t>(written);
        result.ok = true;
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

namespace {

// True for the raw backends that have a certified streaming fork writer (peak RAM
// one window). Others must buffer the whole host file, so they take the size-bound
// fallback path instead.
bool isRawStreamingWriteFs(const QString& fs) {
    return fs == QStringLiteral("apfs") || fs == QStringLiteral("hfsplus") ||
           fs == QStringLiteral("hfsx");
}

}  // namespace

FileManagementMutationResult FileManagementFileSystemBridge::writeFileFromHostPath(
    const FileManagementTarget& target,
    const QString& path,
    const QString& host_file_path,
    const FileManagementTransferObserver& observer) {
    if (auto blocked = mutationCapabilityBlock(target, path)) {
        return *blocked;
    }
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
        FileManagementMutationResult result = copyLocalFileStreamed(path, host_file_path, observer);
        result.file_system = target.file_system;
        return result;
    }
    if (observer.isCancelled()) {
        return mutationBlocked(fs, cleanPath, kFileManagementTransferCancelledBlocker);
    }
    if (isRawStreamingWriteFs(fs)) {
        return writeObservedRawStream(target, cleanPath, host_file_path, size, observer);
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
    const FileManagementMutationResult result = writeFile(target, path, src.readAll());
    if (result.ok) {
        observer.addBytes(static_cast<qint64>(size));
    }
    return result;
}

FileManagementMutationResult FileManagementFileSystemBridge::deleteFile(
    const FileManagementTarget& target, const QString& path) {
    if (auto blocked = mutationCapabilityBlock(target, path)) {
        return *blocked;
    }
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
                 .allow_raw_device_target =
                     PartitionApfsWriter::acceptsRawDeviceTargetPath(target.root_path),
                 .options = apfsRawWriteOptions(target)}),
            cleanPath,
            0);
    }
    return mutationBlocked(
        fs,
        cleanPath,
        QStringLiteral("File delete is not supported for %1").arg(displayFileSystem(fs)));
}

namespace {

// Probe one past the caller's bound so a truncated listing is detectable, without
// overflowing int when max_entries is INT_MAX -- a wrapped negative cap would turn
// the bounded replacement check into an unbounded / empty enumeration (Codex-3).
int listingProbeCap(const int max_entries) {
    return max_entries < std::numeric_limits<int>::max() ? max_entries + 1 : max_entries;
}

FileManagementMutationResult deleteRawEntry(const FileManagementTarget& target,
                                            const FileManagementEntry& entry) {
    return entry.directory ? FileManagementFileSystemBridge::deleteDirectoryTree(target, entry.path)
                           : FileManagementFileSystemBridge::deleteFile(target, entry.path);
}

// Return the sole entry whose name matches @p name case-insensitively, or
// nullptr when there are zero or (ambiguously) more than one. @p count receives
// the number of matches.
const FileManagementEntry* uniqueCaseInsensitiveMatch(const QVector<FileManagementEntry>& entries,
                                                      const QString& name,
                                                      int& count) {
    const FileManagementEntry* match = nullptr;
    count = 0;
    for (const FileManagementEntry& entry : entries) {
        if (QString::compare(entry.name, name, Qt::CaseInsensitive) == 0) {
            match = &entry;
            ++count;
        }
    }
    return match;
}

// Raw-target occupant removal for removeExistingEntry: the occupant is found in
// a fresh listing one past the bound so truncation is detectable. An exact
// (case-correct) name match is deleted directly since it is unambiguous on both
// case-sensitive and case-insensitive volumes. When no exact match exists, a
// differing-case occupant is handled per rawReplaceCaseAction: replaced only on a
// known case-insensitive volume (HFS+), left alone on a known case-sensitive one
// (HFSX, where the exact name is simply vacant), and refused when the volume's
// case-sensitivity is unknown (APFS) or two case-only matches prove the volume is
// case-sensitive -- deleting the wrong file would be silent data loss (B8-05).
FileManagementMutationResult removeExistingRawEntry(const FileManagementTarget& target,
                                                    const QString& path,
                                                    const int max_entries,
                                                    FileManagementMutationResult result) {
    const auto [parent, name] = apfsParentAndName(displayPath(path));
    const FileManagementListResult listing =
        FileManagementFileSystemBridge::listDirectory(target, parent, listingProbeCap(max_entries));
    if (listing.ok) {
        for (const FileManagementEntry& entry : listing.entries) {
            if (entry.name == name) {
                return deleteRawEntry(target, entry);  // exact (case-correct) match
            }
        }
        int ci_count = 0;
        const FileManagementEntry* ci_match =
            uniqueCaseInsensitiveMatch(listing.entries, name, ci_count);
        switch (FileManagementFileSystemBridge::rawReplaceCaseAction(
            FileManagementFileSystemBridge::normalizedFileSystem(target.file_system), ci_count)) {
        case RawReplaceCaseAction::DeleteDifferingCase:
            return deleteRawEntry(target, *ci_match);
        case RawReplaceCaseAction::RefuseMultipleCase:
            result.blockers.append(
                QStringLiteral("Multiple entries match %1 only by case; refusing to delete on a "
                               "case-sensitive volume to avoid removing the wrong file.")
                    .arg(name));
            return result;
        case RawReplaceCaseAction::RefuseAmbiguous:
            result.blockers.append(
                QStringLiteral("An entry (%1) matches %2 only by case and this volume's "
                               "case-sensitivity is unknown; refusing to replace to avoid "
                               "removing the wrong file.")
                    .arg(ci_match ? ci_match->name : QString(), name));
            return result;
        case RawReplaceCaseAction::TreatAsVacant:
            break;  // exact name is vacant -> fall through to the nothing-occupies path
        }
        if (listing.entries.size() <= max_entries) {
            result.ok = true;
            return result;
        }
    }
    result.blockers.append(
        QStringLiteral("Could not verify what occupies %1; nothing was replaced.").arg(path));
    result.blockers.append(listing.blockers);
    return result;
}

}  // namespace

FileManagementMutationResult FileManagementFileSystemBridge::removeExistingEntry(
    const FileManagementTarget& target, const QString& path, const int max_entries) {
    if (auto blocked = mutationCapabilityBlock(target, path)) {
        return *blocked;
    }
    FileManagementMutationResult result;
    result.file_system = target.file_system;
    result.path = path;
    if (!target.local_file_system) {
        return removeExistingRawEntry(target, path, max_entries, std::move(result));
    }
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        // "Does not exist" from QFileInfo can also mean the stat FAILED (e.g. a
        // permission-denied ancestor). Confirm GENUINE absence before declaring the path
        // vacant, or the caller would overwrite an existing-but-inaccessible file. The
        // returned file_type is authoritative (MSVC also sets ec to not-found for an absent
        // path, so ec alone would misfire): only file_type::not_found is provably vacant;
        // any other outcome (unknown/none from a permission error, or an occupant QFileInfo
        // missed) cannot be proven vacant -> fail closed (Codex-3).
        std::error_code ec;
        const auto status =
            std::filesystem::symlink_status(std::filesystem::path(path.toStdWString()), ec);
        if (status.type() != std::filesystem::file_type::not_found) {
            result.blockers.append(
                QStringLiteral("Could not determine what occupies %1; nothing was replaced.")
                    .arg(path));
            return result;
        }
        result.ok = true;
        return result;
    }
    // A symlink to a directory is removed as the link itself, never as the
    // tree behind it.
    return info.isDir() && !info.isSymLink() ? deleteDirectoryTree(target, path)
                                             : deleteFile(target, path);
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
    return fromApfsCommitResult(
        PartitionApfsWriter::commitRawFileMove(
            {.target_path = target.root_path,
             .target_container_bytes = target.size_bytes,
             .source_directory_name = sourceParent,
             .file_name = sourceName,
             .destination_directory_name = destParent,
             .new_file_name = destName,
             .target_mutation_confirmed = true,
             .allow_raw_device_target =
                 PartitionApfsWriter::acceptsRawDeviceTargetPath(target.root_path),
             .options = apfsRawWriteOptions(target)}),
        cleanDestination,
        0);
}

}  // namespace

FileManagementMutationResult FileManagementFileSystemBridge::renameEntry(
    const FileManagementTarget& target,
    const QString& source_path,
    const QString& destination_path) {
    if (auto blocked = mutationCapabilityBlock(target, source_path)) {
        return *blocked;
    }
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
