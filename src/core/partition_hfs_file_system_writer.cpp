// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_hfs_file_system_writer.cpp
/// @brief PartitionHfsFileSystemWriter public entry points (HFS+/HFSX).
///        Thin wrappers over the shared HfsReader engine in
///        partition_hfs_internal.h. Split out of the reader translation unit
///        so the writer/driver surface is reviewable on its own.

#include "sak/partition_hfs_internal.h"

namespace sak {

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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ same-size writer is image-only; raw targets require a separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ allocated-block writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ resource-fork writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ allocation-growth writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ compressed-file writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ resource-fork allocation-growth writer is image-only; raw targets "
                           "require a separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ truncate writer is image-only; raw targets require a separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ resource-fork truncate writer is image-only; raw targets require "
                           "a separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ catalog rename/move is image-only; raw targets require a separate "
                           "hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ empty-file create is image-only; raw targets require a separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ file create is image-only; raw targets require a separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
        return result;
    }
    return createFileWithData(image.get(), path, data, options);
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ symlink create is image-only; raw targets require a separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ hard-link create is image-only; raw targets require a separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ hard-link delete is image-only; raw targets require a separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ empty-file delete is image-only; raw targets require a separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ allocated-file delete is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ folder-tree delete is image-only; raw targets require a separate hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ empty-folder create is image-only; raw targets require a separate "
                           "hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ empty-folder delete is image-only; raw targets require a separate "
                           "hardware gate"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ inline attribute writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(QStringLiteral(
            "HFS+ journal replay is image-only; raw targets require a separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ inline attribute writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ inline attribute writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ inline attribute writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ fork attribute writer is image-only; raw targets require a "
                           "separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
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
    if (image_path.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    if (options.image_only && isWindowsRawDevicePath(image_path)) {
        result.blockers.append(
            QStringLiteral("HFS+ fork attribute allocation-growth writer is image-only; raw "
                           "targets require a separate hardware gate"));
        return result;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadWrite(image_path, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open HFS+ image read/write: %1").arg(openError));
        return result;
    }
    return replaceForkAttributeValueWithAllocationGrowth(
        image.get(), file_id, attribute_name, data, options);
}

}  // namespace sak
