// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_hfs_file_system_writer.cpp
/// @brief PartitionHfsFileSystemWriter public entry points (HFS+/HFSX).
///        Thin wrappers over the shared HfsReader engine in
///        partition_hfs_internal.h. Split out of the reader translation unit
///        so the writer/driver surface is reviewable on its own.

#include "sak/partition_hfs_internal.h"

namespace sak {

namespace {

// Shared raw-target gate for every *FromImage wrapper: reject an empty path,
// enforce the image-only hardware gate for raw devices, then open read/write.
// Fail closed -- on any failure the blocker is recorded and nullptr returned so
// the single policy lives in one place instead of 27 near-identical copies.
[[nodiscard]] std::unique_ptr<QIODevice> openImageTargetForWrite(
    const QString& image_path,
    const PartitionHfsFileWriteOptions& options,
    const QString& gate_label,
    QStringList* blockers) {
    if (image_path.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("Image path is required"));
        return nullptr;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        blockers->append(QStringLiteral("HFS+ %1 is image-only; raw targets require a "
                                        "separate hardware gate")
                             .arg(gate_label));
        return nullptr;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        blockers->append(QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
    }
    return image;
}

}  // namespace

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::overwriteFileSameSize(
    QIODevice* device,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.overwriteFileSameSize(path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
    const QString& image_path,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("same-size writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return overwriteFileSameSize(image.get(), path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceFileWithinAllocatedBlocks(
    QIODevice* device,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceFileWithinAllocatedBlocks(path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceFileWithinAllocatedBlocksFromImage(
    const QString& image_path,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("allocated-block writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return replaceFileWithinAllocatedBlocks(image.get(), path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceResourceForkWithinAllocatedBlocks(
    QIODevice* device,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceResourceForkWithinAllocatedBlocks(path, data, options);
}

PartitionHfsFileWriteResult
PartitionHfsFileSystemWriter::replaceResourceForkWithinAllocatedBlocksFromImage(
    const QString& image_path,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("resource-fork writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return replaceResourceForkWithinAllocatedBlocks(image.get(), path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowth(
    QIODevice* device,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceFileWithAllocationGrowth(path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
    const QString& image_path,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("allocation-growth writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return replaceFileWithAllocationGrowth(image.get(), path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceResourceForkWithAllocationGrowth(
    QIODevice* device,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceResourceForkWithAllocationGrowth(path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceCompressedFileContent(
    QIODevice* device,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceCompressedFileContent(path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replaceCompressedFileContentFromImage(
    const QString& image_path,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("compressed-file writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return replaceCompressedFileContent(image.get(), path, data, options);
}

PartitionHfsFileWriteResult
PartitionHfsFileSystemWriter::replaceResourceForkWithAllocationGrowthFromImage(
    const QString& image_path,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(image_path,
                                         options,
                                         QStringLiteral("resource-fork allocation-growth writer"),
                                         &result.blockers);
    if (!image) {
        return result;
    }
    return replaceResourceForkWithAllocationGrowth(image.get(), path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::truncateFileWithinAllocatedBlocks(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.truncateFileWithinAllocatedBlocks(path, options);
}

PartitionHfsFileWriteResult
PartitionHfsFileSystemWriter::truncateFileWithinAllocatedBlocksFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("truncate writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return truncateFileWithinAllocatedBlocks(image.get(), path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::truncateResourceForkWithinAllocatedBlocks(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.truncateResourceForkWithinAllocatedBlocks(path, options);
}

PartitionHfsFileWriteResult
PartitionHfsFileSystemWriter::truncateResourceForkWithinAllocatedBlocksFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("resource-fork truncate writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return truncateResourceForkWithinAllocatedBlocks(image.get(), path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::renameOrMoveCatalogEntry(
    QIODevice* device,
    const QString& source_path,
    const QString& destination_path,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = source_path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for catalog rename/move"));
        result.blockers.append(reader.blockers());
        result.warnings.append(reader.warnings());
        return result;
    }
    return reader.renameOrMoveCatalogEntry(source_path, destination_path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
    const QString& image_path,
    const QString& source_path,
    const QString& destination_path,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = source_path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("catalog rename/move"), &result.blockers);
    if (!image) {
        return result;
    }
    return renameOrMoveCatalogEntry(image.get(), source_path, destination_path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createEmptyFile(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for empty-file create"));
        return result;
    }
    return reader.createEmptyFile(path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createEmptyFileFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("empty-file create"), &result.blockers);
    if (!image) {
        return result;
    }
    return createEmptyFile(image.get(), path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createFileWithData(
    QIODevice* device,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for file create"));
        return result;
    }
    return reader.createFileWithData(path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createFileWithDataFromImage(
    const QString& image_path,
    const QString& path,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("file create"), &result.blockers);
    if (!image) {
        return result;
    }
    return createFileWithData(image.get(), path, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createFileFromHostPathStreamed(
    QIODevice* device,
    const QString& path,
    const QString& host_file_path,
    uint64_t size,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for file create"));
        return result;
    }
    return reader.createFileFromHostPathStreamed(path, host_file_path, size, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createFileFromHostPathStreamedFromImage(
    const QString& image_path,
    const QString& path,
    const QString& host_file_path,
    uint64_t size,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("file create"), &result.blockers);
    if (!image) {
        return result;
    }
    return createFileFromHostPathStreamed(image.get(), path, host_file_path, size, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createSymlink(
    QIODevice* device,
    const QString& path,
    const QString& target,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for symlink create"));
        return result;
    }
    return reader.createSymlink(path, target, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createSymlinkFromImage(
    const QString& image_path,
    const QString& path,
    const QString& target,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("symlink create"), &result.blockers);
    if (!image) {
        return result;
    }
    return createSymlink(image.get(), path, target, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createHardlink(
    QIODevice* device,
    const QString& existing_path,
    const QString& link_path,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = link_path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for hard-link create"));
        return result;
    }
    return reader.createHardlink(existing_path, link_path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createHardlinkFromImage(
    const QString& image_path,
    const QString& existing_path,
    const QString& link_path,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = link_path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("hard-link create"), &result.blockers);
    if (!image) {
        return result;
    }
    return createHardlink(image.get(), existing_path, link_path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteHardlink(
    QIODevice* device, const QString& link_path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = link_path.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for hard-link delete"));
        return result;
    }
    return reader.deleteHardlink(link_path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteHardlinkFromImage(
    const QString& image_path,
    const QString& link_path,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = link_path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("hard-link delete"), &result.blockers);
    if (!image) {
        return result;
    }
    return deleteHardlink(image.get(), link_path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteEmptyFile(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for empty-file delete"));
        return result;
    }
    return reader.deleteEmptyFile(path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("empty-file delete"), &result.blockers);
    if (!image) {
        return result;
    }
    return deleteEmptyFile(image.get(), path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocks(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for allocated-file delete"));
        return result;
    }
    return reader.deleteFileAndReleaseAllocatedBlocks(path, options);
}

PartitionHfsFileWriteResult
PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("allocated-file delete"), &result.blockers);
    if (!image) {
        return result;
    }
    return deleteFileAndReleaseAllocatedBlocks(image.get(), path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocks(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for folder-tree delete"));
        return result;
    }
    return reader.deleteFolderTreeAndReleaseAllocatedBlocks(path, options);
}

PartitionHfsFileWriteResult
PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("folder-tree delete"), &result.blockers);
    if (!image) {
        return result;
    }
    return deleteFolderTreeAndReleaseAllocatedBlocks(image.get(), path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createEmptyFolder(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for empty-folder create"));
        return result;
    }
    return reader.createEmptyFolder(path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::createEmptyFolderFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("empty-folder create"), &result.blockers);
    if (!image) {
        return result;
    }
    return createEmptyFolder(image.get(), path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteEmptyFolder(
    QIODevice* device, const QString& path, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ filesystem for empty-folder delete"));
        return result;
    }
    return reader.deleteEmptyFolder(path, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::deleteEmptyFolderFromImage(
    const QString& image_path, const QString& path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = path.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("empty-folder delete"), &result.blockers);
    if (!image) {
        return result;
    }
    return deleteEmptyFolder(image.get(), path, options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::createInlineAttributeValue(
    QIODevice* device,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsAttributeWriteResult result;
        result.file_id = file_id;
        result.attribute_name = attribute_name.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.createInlineAttributeValue(file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::createInlineAttributeValueFromImage(
    const QString& image_path,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsAttributeWriteResult result;
    result.file_id = file_id;
    result.attribute_name = attribute_name.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("inline attribute writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return createInlineAttributeValue(image.get(), file_id, attribute_name, data, options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replayJournal(
    QIODevice* device, const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsFileWriteResult result;
        result.path = QStringLiteral("(journal)");
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replayJournal(options);
}

PartitionHfsFileWriteResult PartitionHfsFileSystemWriter::replayJournalFromImage(
    const QString& image_path, const PartitionHfsFileWriteOptions& options) {
    PartitionHfsFileWriteResult result;
    result.path = QStringLiteral("(journal)");
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("journal replay"), &result.blockers);
    if (!image) {
        return result;
    }
    return replayJournal(image.get(), options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::createForkAttributeValue(
    QIODevice* device,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsAttributeWriteResult result;
        result.file_id = file_id;
        result.attribute_name = attribute_name.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.createForkAttributeValue(file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::createForkAttributeValueFromImage(
    const QString& image_path,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsAttributeWriteResult result;
    result.file_id = file_id;
    result.attribute_name = attribute_name.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("inline attribute writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return createForkAttributeValue(image.get(), file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::deleteAttributeValue(
    QIODevice* device,
    uint32_t file_id,
    const QString& attribute_name,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsAttributeWriteResult result;
        result.file_id = file_id;
        result.attribute_name = attribute_name.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.deleteAttributeValue(file_id, attribute_name, options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::deleteAttributeValueFromImage(
    const QString& image_path,
    uint32_t file_id,
    const QString& attribute_name,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsAttributeWriteResult result;
    result.file_id = file_id;
    result.attribute_name = attribute_name.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("inline attribute writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return deleteAttributeValue(image.get(), file_id, attribute_name, options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::replaceInlineAttributeValue(
    QIODevice* device,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsAttributeWriteResult result;
        result.file_id = file_id;
        result.attribute_name = attribute_name.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceInlineAttributeValue(file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult PartitionHfsFileSystemWriter::replaceInlineAttributeValueFromImage(
    const QString& image_path,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsAttributeWriteResult result;
    result.file_id = file_id;
    result.attribute_name = attribute_name.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("inline attribute writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return replaceInlineAttributeValue(image.get(), file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult
PartitionHfsFileSystemWriter::replaceForkAttributeValueWithinAllocatedBlocks(
    QIODevice* device,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsAttributeWriteResult result;
        result.file_id = file_id;
        result.attribute_name = attribute_name.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceForkAttributeValueWithinAllocatedBlocks(
        file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult
PartitionHfsFileSystemWriter::replaceForkAttributeValueWithinAllocatedBlocksFromImage(
    const QString& image_path,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsAttributeWriteResult result;
    result.file_id = file_id;
    result.attribute_name = attribute_name.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(
        image_path, options, QStringLiteral("fork attribute writer"), &result.blockers);
    if (!image) {
        return result;
    }
    return replaceForkAttributeValueWithinAllocatedBlocks(
        image.get(), file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult
PartitionHfsFileSystemWriter::replaceForkAttributeValueWithAllocationGrowth(
    QIODevice* device,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    HfsReader reader(device);
    if (!reader.load()) {
        PartitionHfsAttributeWriteResult result;
        result.file_id = file_id;
        result.attribute_name = attribute_name.trimmed();
        result.evidence_id = options.evidence_id;
        result.blockers.append(QStringLiteral("Unable to open HFS+ filesystem for writing"));
        return result;
    }
    return reader.replaceForkAttributeValueWithAllocationGrowth(
        file_id, attribute_name, data, options);
}

PartitionHfsAttributeWriteResult
PartitionHfsFileSystemWriter::replaceForkAttributeValueWithAllocationGrowthFromImage(
    const QString& image_path,
    uint32_t file_id,
    const QString& attribute_name,
    const QByteArray& data,
    const PartitionHfsFileWriteOptions& options) {
    PartitionHfsAttributeWriteResult result;
    result.file_id = file_id;
    result.attribute_name = attribute_name.trimmed();
    result.evidence_id = options.evidence_id;
    auto image = openImageTargetForWrite(image_path,
                                         options,
                                         QStringLiteral("fork attribute allocation-growth writer"),
                                         &result.blockers);
    if (!image) {
        return result;
    }
    return replaceForkAttributeValueWithAllocationGrowth(
        image.get(), file_id, attribute_name, data, options);
}

}  // namespace sak
