// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_hfs_file_system_reader.cpp
/// @brief Read-only HFS+/HFSX file browser for Partition Manager.

#include "sak/partition_hfs_file_system_reader.h"

#include "sak/partition_hfs_internal.h"
#include "sak/partition_raw_device_io.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace sak {

namespace {
PartitionHfsFileReadResult withOpenImage(
    const QString& imagePath,
    const std::function<PartitionHfsFileReadResult(QIODevice*)>& callback) {
    PartitionHfsFileReadResult result;
    if (imagePath.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadOnly(imagePath, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read-only: %1").arg(openError));
        return result;
    }
    return callback(image.get());
}

QString safeExportName(QString name, const QString& fallbackId) {
    name = name.trimmed();
    static const QRegularExpression unsafeChars(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    name.replace(unsafeChars, QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    if (name.isEmpty()) {
        name = QStringLiteral("entry_%1").arg(fallbackId);
    }
    return name.left(kExportNameMaxCharacters);
}

QString uniquePath(const QDir& dir, const QString& safeName, const QString& suffixId) {
    QString candidate = dir.filePath(safeName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }

    const QFileInfo info(safeName);
    const QString base = info.completeBaseName().isEmpty() ? safeName : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString()
                                                   : QStringLiteral(".%1").arg(info.suffix());
    for (int index = kFirstExportNameCollisionIndex; index < kMaxExportNameCollisionAttempts;
         ++index) {
        candidate =
            dir.filePath(QStringLiteral("%1_%2_%3%4").arg(base, suffixId).arg(index).arg(suffix));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

// True when @p child resolves (junctions/symlinks followed) to a real path at or under
// @p canonicalRoot. Fail closed on an empty root or an unresolvable/vanished child: a reparse
// point planted at an ancestor after the root check would otherwise redirect the write outside
// the export directory. canonicalFilePath yields '/'-separated paths, so the boundary test uses
// '/'; a sibling-prefix (Root vs RootX) is rejected by requiring the separator.
[[nodiscard]] bool realizedPathWithinRoot(const QString& canonicalRoot, const QString& child) {
    if (canonicalRoot.isEmpty()) {
        return false;
    }
    const QString canonicalChild = QFileInfo(child).canonicalFilePath();
    if (canonicalChild.isEmpty()) {
        return false;
    }
    return canonicalChild == canonicalRoot ||
           canonicalChild.startsWith(canonicalRoot + QLatin1Char('/'));
}

bool writeExportFile(const QString& path,
                     const QByteArray& data,
                     QStringList* blockers,
                     const QString& label,
                     const QString& canonicalRoot) {
    // Re-check the leaf's PARENT directory on every write: mkpath created the ancestor chain, but
    // a junction swapped in afterward would redirect this file outside the export root. The
    // NewOnly guard below only protects the final component.
    if (!realizedPathWithinRoot(canonicalRoot, QFileInfo(path).absolutePath())) {
        blockers->append(
            QStringLiteral("Export target escapes the export root (reparse point): %1").arg(label));
        return false;
    }
    QFile output(path);
    // NewOnly (O_EXCL/CREATE_NEW) fails closed if anything already exists at `path`: uniquePath
    // returned a non-existent candidate, so a file/symlink appearing here is a TOCTOU race and
    // an overwrite would have followed it to an arbitrary target. Refuse instead.
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        output.write(data) != data.size()) {
        blockers->append(QStringLiteral("Unable to write exported %1: %2").arg(label, path));
        return false;
    }
    return true;
}

struct HfsExportFrame {
    QString source_path;
    QString output_directory;
    uint32_t catalog_id{0};  // CNID of this directory; 0 = the export root
};

void appendHfsExportRequestBlockers(const QString& imagePath,
                                    const QString& outputDirectory,
                                    const PartitionHfsDirectoryExportOptions& options,
                                    QStringList* blockers) {
    if (imagePath.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("Image path is required"));
    }
    if (outputDirectory.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("Output directory is required"));
    }
    if (options.max_entries <= 0) {
        blockers->append(QStringLiteral("Export entry cap must be positive"));
    }
    if (options.max_file_bytes == 0 || options.max_total_bytes == 0) {
        blockers->append(QStringLiteral("Export byte caps must be positive"));
    }
}

class HfsDirectoryExporter {
public:
    HfsDirectoryExporter(QIODevice* image,
                         const PartitionHfsDirectoryExportOptions& options,
                         const QString& canonicalRoot)
        : image_(image), options_(options), canonical_root_(canonicalRoot) {}

    PartitionHfsDirectoryExportResult run(const QString& sourcePath,
                                          const QString& outputDirectory) {
        pending_.append(
            {sourcePath.trimmed().isEmpty() ? QStringLiteral("/") : sourcePath.trimmed(),
             outputDirectory});
        while (!pending_.isEmpty()) {
            if (!processFrame(pending_.takeLast())) {
                break;
            }
        }
        result_.ok = result_.blockers.isEmpty();
        return result_;
    }

private:
    bool processFrame(const HfsExportFrame& frame) {
        // Dedupe by CNID, not a folded path: on an HFSX (binary-compare) volume /Foo and /foo
        // are genuinely distinct directories that a lowercased path key would collide (dropping
        // one subtree), and HFS+ directory hard links reach one CNID by several paths (a path
        // key would re-export it or loop). The catalog id is unambiguous under either compare.
        if (visited_directories_.contains(frame.catalog_id)) {
            return true;
        }
        visited_directories_.insert(frame.catalog_id);

        const auto listing = PartitionHfsFileSystemReader::listDirectory(
            image_, frame.source_path, std::max(1, options_.max_entries - result_.entries_scanned));
        result_.warnings.append(listing.warnings);
        if (!listing.ok) {
            result_.blockers.append(listing.blockers);
            return false;
        }
        return processEntries(listing.entries, QDir(frame.output_directory));
    }

    bool processEntries(const QVector<PartitionHfsFileEntry>& entries, const QDir& targetDir) {
        for (const auto& entry : entries) {
            if (!processEntry(entry, targetDir)) {
                return false;
            }
        }
        return true;
    }

    bool processEntry(const PartitionHfsFileEntry& entry, const QDir& targetDir) {
        if (!consumeEntrySlot()) {
            return false;
        }
        const QString suffixId = QString::number(entry.catalog_id, kExportNameFallbackBase);
        const QString safeName = safeExportName(entry.name, suffixId);
        const QString targetPath = uniquePath(targetDir, safeName, suffixId);
        if (targetPath.isEmpty()) {
            result_.blockers.append(
                QStringLiteral("Unable to allocate unique output path for %1").arg(entry.path));
            return false;
        }
        if (entry.directory) {
            return exportDirectory(entry, targetPath);
        }
        if (!entry.regular_file) {
            result_.warnings.append(
                QStringLiteral("Skipped unsupported HFS+ entry: %1").arg(entry.path));
            return true;
        }
        return exportFile(entry, targetDir, safeName, suffixId, targetPath);
    }

    bool consumeEntrySlot() {
        if (result_.entries_scanned >= options_.max_entries) {
            result_.blockers.append(QStringLiteral("Export entry cap reached"));
            return false;
        }
        ++result_.entries_scanned;
        return true;
    }

    bool exportDirectory(const PartitionHfsFileEntry& entry, const QString& targetPath) {
        if (!QDir().mkpath(targetPath)) {
            result_.blockers.append(
                QStringLiteral("Unable to create exported directory: %1").arg(targetPath));
            return false;
        }
        // mkpath follows an existing reparse point instead of failing, so prove the realized
        // directory is still under the export root before anything is written beneath it.
        if (!realizedPathWithinRoot(canonical_root_, targetPath)) {
            result_.blockers.append(
                QStringLiteral("Exported directory escapes the export root (reparse point): %1")
                    .arg(targetPath));
            return false;
        }
        ++result_.directories_exported;
        pending_.append({entry.path, targetPath, entry.catalog_id});
        return true;
    }

    bool exportFile(const PartitionHfsFileEntry& entry,
                    const QDir& targetDir,
                    const QString& safeName,
                    const QString& suffixId,
                    const QString& targetPath) {
        if (!fitsByteCaps(entry, entry.path)) {
            return false;
        }
        if (!exportDataFork(entry, targetPath)) {
            return false;
        }
        return entry.resource_fork_size_bytes == 0 ||
               exportResourceFork(entry, targetDir, safeName, suffixId);
    }

    bool exportDataFork(const PartitionHfsFileEntry& entry, const QString& targetPath) {
        const auto dataFork =
            PartitionHfsFileSystemReader::readFile(image_, entry.path, options_.max_file_bytes);
        result_.warnings.append(dataFork.warnings);
        if (!dataFork.ok) {
            result_.blockers.append(dataFork.blockers);
            return false;
        }
        if (!writeExportFile(
                targetPath, dataFork.data, &result_.blockers, entry.path, canonical_root_)) {
            return false;
        }
        ++result_.files_exported;
        result_.bytes_exported += static_cast<uint64_t>(dataFork.data.size());
        return true;
    }

    bool exportResourceFork(const PartitionHfsFileEntry& entry,
                            const QDir& targetDir,
                            const QString& safeName,
                            const QString& suffixId) {
        const auto resourceFork = PartitionHfsFileSystemReader::readResourceFork(
            image_, entry.path, options_.max_file_bytes);
        result_.warnings.append(resourceFork.warnings);
        if (!resourceFork.ok) {
            result_.blockers.append(resourceFork.blockers);
            return false;
        }
        const QString resourcePath =
            uniquePath(targetDir, QStringLiteral("%1.rsrc").arg(safeName), suffixId);
        if (resourcePath.isEmpty()) {
            result_.blockers.append(
                QStringLiteral("Unable to allocate resource-fork output path for %1")
                    .arg(entry.path));
            return false;
        }
        if (!writeExportFile(resourcePath,
                             resourceFork.data,
                             &result_.blockers,
                             QStringLiteral("%1 resource fork").arg(entry.path),
                             canonical_root_)) {
            return false;
        }
        ++result_.resource_forks_exported;
        result_.bytes_exported += static_cast<uint64_t>(resourceFork.data.size());
        return true;
    }

    bool fitsByteCaps(const PartitionHfsFileEntry& entry, const QString& entryPath) {
        const uint64_t entryBytes = entry.size_bytes + entry.resource_fork_size_bytes;
        const bool fileTooLarge = entry.size_bytes > options_.max_file_bytes ||
                                  entry.resource_fork_size_bytes > options_.max_file_bytes;
        const bool totalTooLarge = result_.bytes_exported > options_.max_total_bytes ||
                                   entryBytes > options_.max_total_bytes - result_.bytes_exported;
        if (fileTooLarge || totalTooLarge) {
            result_.blockers.append(
                QStringLiteral("Export byte cap reached before %1").arg(entryPath));
            return false;
        }
        return true;
    }

    QIODevice* image_;
    PartitionHfsDirectoryExportOptions options_;
    PartitionHfsDirectoryExportResult result_;
    QVector<HfsExportFrame> pending_;
    QSet<uint32_t> visited_directories_;
    const QString canonical_root_;
};

}  // namespace

PartitionHfsConsistencyCheckResult PartitionHfsFileSystemReader::checkConsistency(QIODevice* device,
                                                                                  int max_records) {
    HfsReader reader(device);
    if (!reader.load()) {
        return reader.consistencyFailureResult(
            QStringLiteral("Unable to open HFS+ filesystem for consistency check"));
    }
    return reader.checkConsistency(max_records);
}

PartitionHfsConsistencyCheckResult PartitionHfsFileSystemReader::checkConsistencyFromImage(
    const QString& image_path, int max_records) {
    PartitionHfsConsistencyCheckResult result;
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadOnly(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read-only: %1").arg(openError));
        return result;
    }
    return checkConsistency(image.get(), max_records);
}

PartitionHfsFileReadResult PartitionHfsFileSystemReader::listDirectory(QIODevice* device,
                                                                       const QString& path,
                                                                       int max_entries) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileReadResult result;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for listing"));
        return result;
    }
    return reader.listDirectory(path, max_entries);
}

PartitionHfsFileReadResult PartitionHfsFileSystemReader::listDirectoryFromImage(
    const QString& image_path, const QString& path, int max_entries) {
    return withOpenImage(image_path, [path, max_entries](QIODevice* device) {
        return PartitionHfsFileSystemReader::listDirectory(device, path, max_entries);
    });
}

PartitionHfsFileReadResult PartitionHfsFileSystemReader::readFile(QIODevice* device,
                                                                  const QString& path,
                                                                  uint64_t max_bytes) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileReadResult result;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for reading"));
        return result;
    }
    return reader.readFile(path, max_bytes);
}

PartitionHfsFileReadResult PartitionHfsFileSystemReader::readFileFromImage(
    const QString& image_path, const QString& path, uint64_t max_bytes) {
    return withOpenImage(image_path, [path, max_bytes](QIODevice* device) {
        return PartitionHfsFileSystemReader::readFile(device, path, max_bytes);
    });
}

PartitionHfsFileReadResult PartitionHfsFileSystemReader::readResourceFork(QIODevice* device,
                                                                          const QString& path,
                                                                          uint64_t max_bytes) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileReadResult result;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for reading"));
        return result;
    }
    return reader.readResourceFork(path, max_bytes);
}

PartitionHfsFileReadResult PartitionHfsFileSystemReader::readResourceForkFromImage(
    const QString& image_path, const QString& path, uint64_t max_bytes) {
    return withOpenImage(image_path, [path, max_bytes](QIODevice* device) {
        return PartitionHfsFileSystemReader::readResourceFork(device, path, max_bytes);
    });
}

PartitionHfsAttributeReadResult PartitionHfsFileSystemReader::readAttributeValue(
    QIODevice* device, uint32_t file_id, const QString& attribute_name, uint64_t max_bytes) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsAttributeReadResult result;
        result.file_id = file_id;
        result.attribute_name = attribute_name.trimmed();
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for attribute reading"));
        return result;
    }
    return reader.readAttributeValue(file_id, attribute_name, max_bytes);
}

PartitionHfsAttributeReadResult PartitionHfsFileSystemReader::readAttributeValueFromImage(
    const QString& image_path,
    uint32_t file_id,
    const QString& attribute_name,
    uint64_t max_bytes) {
    PartitionHfsAttributeReadResult result;
    result.file_id = file_id;
    result.attribute_name = attribute_name.trimmed();
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadOnly(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read-only: %1").arg(openError));
        return result;
    }
    return readAttributeValue(image.get(), file_id, attribute_name, max_bytes);
}

PartitionHfsDirectoryExportResult PartitionHfsFileSystemReader::exportDirectoryFromImage(
    const QString& image_path,
    const QString& source_path,
    const QString& output_directory,
    const PartitionHfsDirectoryExportOptions& options) {
    PartitionHfsDirectoryExportResult exportResult;
    appendHfsExportRequestBlockers(image_path, output_directory, options, &exportResult.blockers);
    if (!exportResult.blockers.isEmpty()) {
        return exportResult;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadOnly(image_path, &openError);
    if (!image) {
        exportResult.blockers.append(
            QStringLiteral("Unable to open HFS+ image read-only: %1").arg(openError));
        return exportResult;
    }

    QDir root(output_directory);
    if (!root.mkpath(QStringLiteral("."))) {
        exportResult.blockers.append(QStringLiteral("Unable to create output directory"));
        return exportResult;
    }
    // Resolve the export root ONCE: every realized directory and file target is proved to sit
    // under this canonical path, so an image-supplied name or a reparse point planted at an
    // ancestor cannot redirect a write outside it.
    const QString canonicalRoot = QFileInfo(root.absolutePath()).canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        exportResult.blockers.append(QStringLiteral("Unable to resolve export root path"));
        return exportResult;
    }

    return HfsDirectoryExporter(image.get(), options, canonicalRoot)
        .run(source_path, root.absolutePath());
}

bool PartitionHfsFileSystemReader::exportPathWithinRootForTesting(const QString& canonical_root,
                                                                  const QString& child) {
    return realizedPathWithinRoot(canonical_root, child);
}

}  // namespace sak
