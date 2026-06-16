// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_hfs_internal.h
/// @brief HFS+/HFSX engine (HfsReader), shared by the read-only browser and the
///        writer via their thin .cpp wrappers. Header-only, internal anonymous
///        namespace; shared primitives live in partition_hfs_core.h. Follow-up
///        (docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md): promote to a compiled-once
///        sak::hfs_detail module.

#pragma once

#include "sak/partition_hfs_core.h"

namespace sak {
namespace {

class HfsReader {
public:
    explicit HfsReader(QIODevice* device) : m_device(device) {}

    [[nodiscard]] bool load() {
        if (!m_device || !m_device->isReadable()) {
            m_blockers.append(QStringLiteral("Readable HFS+ device or image is required"));
            return false;
        }

        if (!loadDirectVolumeHeader() && !loadWrappedVolumeHeader()) {
            m_blockers.append(QStringLiteral("HFS+ volume header was not found"));
            return false;
        }
        validateVolume();
        if (!m_blockers.isEmpty()) {
            return false;
        }
        loadExtentsOverflowRecords();
        if (!m_blockers.isEmpty()) {
            return false;
        }
        loadCatalogHeader();
        return m_blockers.isEmpty();
    }

    [[nodiscard]] QStringList blockers() const { return m_blockers; }
    [[nodiscard]] QStringList warnings() const { return m_warnings; }

    [[nodiscard]] PartitionHfsFileReadResult listDirectory(const QString& path, int maxEntries) {
        PartitionHfsFileReadResult result = baseResult();
        const auto folderId = resolveFolderPath(path);
        if (!folderId.has_value()) {
            result.blockers = m_blockers;
            return result;
        }

        const QString parent = normalizedDisplayPath(path);
        const auto records = childRecords(*folderId, std::max(1, maxEntries));
        if (!records.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        for (const auto& record : *records) {
            result.entries.append(entryFor(record, parent));
        }
        result.blockers = m_blockers;
        result.warnings.append(m_warnings);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileReadResult readFile(const QString& path, uint64_t maxBytes) {
        return readCatalogFileFork(path, maxBytes, HfsForkSelector::Data);
    }

    [[nodiscard]] PartitionHfsFileReadResult readResourceFork(const QString& path,
                                                              uint64_t maxBytes) {
        return readCatalogFileFork(path, maxBytes, HfsForkSelector::Resource);
    }

    [[nodiscard]] PartitionHfsFileWriteResult overwriteFileSameSize(
        const QString& path, const QByteArray& data, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.catalog_id = record->catalog_id;
        appendWriterRecordBlockers(*record, data, options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            result.blockers.append(m_blockers);
            return result;
        }

        const auto before = readForkBytes(
            record->data_fork, record->catalog_id, kHfsDataForkType, 0, record->data_size);
        if (!before.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.before_sha256 = sha256Hex(*before);
        const auto chunks =
            writeForkBytes(record->data_fork, record->catalog_id, kHfsDataForkType, 0, data);
        if (!chunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto after = readForkBytes(
            record->data_fork, record->catalog_id, kHfsDataForkType, 0, record->data_size);
        if (!after.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        if (*after != data) {
            result.blockers.append(QStringLiteral("HFS+ write read-back verification failed"));
            result.blockers.append(m_blockers);
            return result;
        }

        result.after_sha256 = sha256Hex(*after);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *chunks;
        result.warnings = m_warnings;
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult replaceFileWithinAllocatedBlocks(
        const QString& path, const QByteArray& data, const PartitionHfsFileWriteOptions& options) {
        return replaceCatalogForkWithinAllocatedBlocks(path, data, options, HfsForkSelector::Data);
    }

    [[nodiscard]] PartitionHfsFileWriteResult replaceResourceForkWithinAllocatedBlocks(
        const QString& path, const QByteArray& data, const PartitionHfsFileWriteOptions& options) {
        return replaceCatalogForkWithinAllocatedBlocks(
            path, data, options, HfsForkSelector::Resource);
    }

    [[nodiscard]] PartitionHfsFileWriteResult replaceFileWithAllocationGrowth(
        const QString& path, const QByteArray& data, const PartitionHfsFileWriteOptions& options) {
        return replaceCatalogForkWithAllocationGrowth(path, data, options, HfsForkSelector::Data);
    }

    [[nodiscard]] PartitionHfsFileWriteResult replaceResourceForkWithAllocationGrowth(
        const QString& path, const QByteArray& data, const PartitionHfsFileWriteOptions& options) {
        return replaceCatalogForkWithAllocationGrowth(
            path, data, options, HfsForkSelector::Resource);
    }

    [[nodiscard]] PartitionHfsFileWriteResult truncateFileWithinAllocatedBlocks(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        return truncateCatalogForkWithinAllocatedBlocks(path, options, HfsForkSelector::Data);
    }

    [[nodiscard]] PartitionHfsFileWriteResult truncateResourceForkWithinAllocatedBlocks(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        return truncateCatalogForkWithinAllocatedBlocks(path, options, HfsForkSelector::Resource);
    }

    [[nodiscard]] PartitionHfsFileWriteResult createEmptyFile(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendCatalogMutationOptionBlockers(options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan = prepareEmptyFileCreate(path, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->file_id;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto chunks =
            applyCatalogTreeMutation(plan->mutation,
                                     HfsCatalogCounterUpdate{plan->mutation.leaf_record_delta,
                                                             kHfsEmptyFileCreateFileCountDelta,
                                                             0,
                                                             plan->file_id + 1},
                                     &result.blockers);
        if (!chunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        if (!appendEmptyFileCreateReadBack(
                plan->target.path, plan->mutation.insert_node_number, &result)) {
            return result;
        }
        result.chunks_written = *chunks;
        result.warnings.append(m_warnings);
        result.warnings.append(QStringLiteral(
            "HFS+ empty file created without allocating data or resource fork blocks"));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult createFileWithData(
        const QString& path, const QByteArray& data, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan = prepareFileCreate(path, data, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->file_id;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto chunks = writeCreatedFileDataAndMetadata(*plan, data, &result.blockers);
        if (!chunks.has_value()) {
            return result;
        }
        const auto readBack =
            readBackCreatedFileData(*plan, data, options.max_write_bytes, &result.blockers);
        if (!readBack.has_value()) {
            return result;
        }
        if (!appendForkGrowthCounterReadBack(plan->allocated_blocks, &result)) {
            return result;
        }
        result.after_sha256 = sha256Hex(*readBack);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *chunks;
        result.warnings.append(m_warnings);
        result.warnings.append(
            plan->extents_mutation.has_value()
                ? QStringLiteral("HFS+ file created with %1 allocated data-fork block(s) and %2 "
                                 "extents-overflow record(s)")
                      .arg(plan->allocated_blocks)
                      .arg(plan->overflow_records.size())
                : QStringLiteral("HFS+ file created with %1 allocated data-fork block(s)")
                      .arg(plan->allocated_blocks));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult deleteEmptyFile(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendCatalogMutationOptionBlockers(options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan = prepareEmptyFileDelete(path, options, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->record.catalog_id;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto chunks =
            applyCatalogTreeMutation(plan->mutation,
                                     HfsCatalogCounterUpdate{plan->mutation.leaf_record_delta,
                                                             kHfsEmptyFileDeleteFileCountDelta,
                                                             0,
                                                             m_volume.next_catalog_id},
                                     &result.blockers);
        if (!chunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        if (!appendEmptyFileDeleteReadBack(
                plan->record, plan->mutation.insert_node_number, &result)) {
            return result;
        }
        result.chunks_written = *chunks;
        result.warnings.append(m_warnings);
        result.warnings.append(
            QStringLiteral("HFS+ empty file deleted without changing allocation bitmap blocks"));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult deleteFileAndReleaseAllocatedBlocks(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendCatalogMutationOptionBlockers(options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan =
            prepareAllocatedFileDelete(path, options, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->record.catalog_id;
        result.bytes_written = plan->released_bytes;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto wipeChunks = wipeReleasedExtentsIfRequested(options.secure_wipe_deleted_blocks,
                                                               plan->released_extents,
                                                               &result.blockers);
        if (!wipeChunks.has_value()) {
            return result;
        }
        const auto catalogChunks =
            applyCatalogTreeMutation(plan->mutation,
                                     HfsCatalogCounterUpdate{plan->mutation.leaf_record_delta,
                                                             kHfsEmptyFileDeleteFileCountDelta,
                                                             0,
                                                             m_volume.next_catalog_id},
                                     &result.blockers);
        if (!catalogChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto extentsChunks = applyDeleteExtentsMutationStep(
            plan->extents_mutation,
            QStringLiteral("HFS+ extents-overflow records for the deleted file were removed"),
            &result);
        if (!extentsChunks.has_value()) {
            return result;
        }
        const auto bitmapChunks = writeUpdatedAllocationBitmapBytes(plan->allocation_bytes);
        if (!bitmapChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto freeBlockChunks =
            writeVolumeHeaderCounter(kHfsFreeBlocksOffset,
                                     m_volume.free_blocks,
                                     static_cast<int>(plan->released_blocks),
                                     &result.blockers);
        if (!freeBlockChunks.has_value()) {
            return result;
        }
        if (!appendAllocatedFileDeleteReadBack(
                plan->record, plan->mutation.insert_node_number, plan->released_blocks, &result)) {
            return result;
        }
        result.chunks_written = *wipeChunks + *catalogChunks + *extentsChunks + *bitmapChunks +
                                *freeBlockChunks;
        result.warnings.append(m_warnings);
        result.warnings.append(options.secure_wipe_deleted_blocks
                                   ? QStringLiteral("HFS+ file deleted after zeroing released "
                                                    "allocated blocks with read-back verification")
                                   : QStringLiteral("HFS+ file deleted and allocated blocks "
                                                    "released without secure block wiping"));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult deleteFolderTreeAndReleaseAllocatedBlocks(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendCatalogMutationOptionBlockers(options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan =
            prepareFolderTreeDelete(path, options, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->record.catalog_id;
        result.bytes_written = plan->released_bytes;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto wipeChunks = wipeReleasedExtentsIfRequested(options.secure_wipe_deleted_blocks,
                                                               plan->released_extents,
                                                               &result.blockers);
        if (!wipeChunks.has_value()) {
            return result;
        }
        const auto catalogChunks =
            applyCatalogTreeMutation(plan->mutation,
                                     HfsCatalogCounterUpdate{plan->mutation.leaf_record_delta,
                                                             -plan->removed_files,
                                                             -plan->removed_folders,
                                                             m_volume.next_catalog_id},
                                     &result.blockers);
        if (!catalogChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto extentsChunks = applyDeleteExtentsMutationStep(
            plan->extents_mutation,
            QStringLiteral(
                "HFS+ extents-overflow records for deleted folder-tree files were removed"),
            &result);
        if (!extentsChunks.has_value()) {
            return result;
        }
        const auto bitmapChunks = writeUpdatedAllocationBitmapBytes(plan->allocation_bytes);
        if (!bitmapChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto freeBlockChunks = writeReleasedFreeBlockCounter(plan->released_blocks,
                                                                   &result.blockers);
        if (!freeBlockChunks.has_value()) {
            return result;
        }
        if (!appendFolderTreeDeleteReadBack(
                plan->record, plan->mutation.insert_node_number, plan->released_blocks, &result)) {
            return result;
        }
        result.chunks_written = *wipeChunks + *catalogChunks + *extentsChunks + *bitmapChunks +
                                *freeBlockChunks;
        result.warnings.append(m_warnings);
        result.warnings.append(
            folderTreeDeleteWarning(options.secure_wipe_deleted_blocks, plan->released_blocks));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult renameOrMoveCatalogEntry(
        const QString& sourcePath,
        const QString& destinationPath,
        const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(destinationPath);
        result.evidence_id = options.evidence_id;
        appendCatalogMutationOptionBlockers(options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan = prepareCatalogRenameMove(
            sourcePath, destinationPath, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->source_record.catalog_id;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto chunks = applyCatalogTreeMutation(
            plan->mutation,
            HfsCatalogCounterUpdate{
                plan->mutation.leaf_record_delta, 0, 0, m_volume.next_catalog_id},
            &result.blockers);
        if (!chunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        if (!appendCatalogRenameMoveReadBack(sourcePath, *plan, &result)) {
            return result;
        }
        result.chunks_written = *chunks;
        result.warnings.append(m_warnings);
        result.warnings.append(
            plan->parent_changed
                ? QStringLiteral("HFS+ catalog entry moved by rewriting catalog leaf records")
                : QStringLiteral("HFS+ catalog entry renamed by rewriting catalog leaf records"));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult createEmptyFolder(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendCatalogMutationOptionBlockers(options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan = prepareEmptyFolderCreate(path, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->file_id;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto chunks =
            applyCatalogTreeMutation(plan->mutation,
                                     HfsCatalogCounterUpdate{plan->mutation.leaf_record_delta,
                                                             0,
                                                             kHfsEmptyFolderCreateFolderCountDelta,
                                                             plan->file_id + 1},
                                     &result.blockers);
        if (!chunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        if (!appendEmptyFolderCreateReadBack(
                plan->target.path, plan->mutation.insert_node_number, &result)) {
            return result;
        }
        result.chunks_written = *chunks;
        result.warnings.append(m_warnings);
        result.warnings.append(QStringLiteral(
            "HFS+ empty folder created without allocating data or resource fork blocks"));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult deleteEmptyFolder(
        const QString& path, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendCatalogMutationOptionBlockers(options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan = prepareEmptyFolderDelete(path, &result.blockers, &result.warnings);
        if (!plan.has_value()) {
            return result;
        }
        result.catalog_id = plan->record.catalog_id;
        result.before_sha256 = sha256Hex(plan->mutation.before_leaf_bytes);
        const auto chunks =
            applyCatalogTreeMutation(plan->mutation,
                                     HfsCatalogCounterUpdate{plan->mutation.leaf_record_delta,
                                                             0,
                                                             kHfsEmptyFolderDeleteFolderCountDelta,
                                                             m_volume.next_catalog_id},
                                     &result.blockers);
        if (!chunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        if (!appendEmptyFileDeleteReadBack(
                plan->record, plan->mutation.insert_node_number, &result)) {
            return result;
        }
        result.chunks_written = *chunks;
        result.warnings.append(m_warnings);
        result.warnings.append(
            QStringLiteral("HFS+ empty folder deleted without changing allocation bitmap blocks"));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsAttributeReadResult readAttributeValue(uint32_t fileId,
                                                                     const QString& name,
                                                                     uint64_t maxBytes) {
        PartitionHfsAttributeReadResult result = attributeBaseResult(fileId, name);
        appendAttributeReadRequestBlockers(fileId, name, maxBytes, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto attributes = scanAttributeRecords(kPartitionHfsDefaultCheckRecordLimit);
        if (!attributes.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        const auto record = findReadableAttribute(*attributes, fileId, name);
        if (!record.has_value()) {
            result.blockers.append(
                QStringLiteral("HFS+ attribute was not found or is not readable"));
            result.blockers.append(m_blockers);
            return result;
        }
        return readAttributeRecordValue(*record, attributes->parsed_records, maxBytes, result);
    }

    [[nodiscard]] PartitionHfsAttributeWriteResult replaceInlineAttributeValue(
        uint32_t fileId,
        const QString& name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options) {
        PartitionHfsAttributeWriteResult result;
        result.file_system = m_volume.file_system;
        result.file_id = fileId;
        result.attribute_name = name.trimmed();
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        appendAttributeWriteRequestBlockers(fileId, name, data, options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto attributes = scanAttributeRecords(kPartitionHfsDefaultCheckRecordLimit);
        if (!attributes.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto record = findAttributeByIdAndName(*attributes, fileId, name.trimmed());
        if (!record.has_value()) {
            result.blockers.append(QStringLiteral("HFS+ inline attribute was not found"));
            result.blockers.append(m_blockers);
            return result;
        }
        appendInlineAttributeRecordBlockers(*record, data, options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            result.blockers.append(m_blockers);
            return result;
        }

        result.before_sha256 = sha256Hex(record->inline_data);
        const auto sizeChunks = writeForkBytes(m_volume.attributes_fork,
                                               kHfsAttributesFileId,
                                               kHfsDataForkType,
                                               record->inline_size_offset,
                                               be32Bytes(static_cast<uint32_t>(data.size())));
        if (!sizeChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto dataChunks = writeForkBytes(m_volume.attributes_fork,
                                               kHfsAttributesFileId,
                                               kHfsDataForkType,
                                               record->inline_data_offset,
                                               data);
        if (!dataChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto zeroChunks = zeroInlineAttributeTail(*record,
                                                        static_cast<uint64_t>(data.size()));
        if (!zeroChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }

        const auto readBack =
            readAttributeValue(result.file_id, result.attribute_name, options.max_write_bytes);
        if (!readBack.ok || readBack.data != data) {
            result.blockers.append(
                QStringLiteral("HFS+ inline attribute write read-back verification failed"));
            result.blockers.append(readBack.blockers);
            result.blockers.append(m_blockers);
            return result;
        }

        result.after_sha256 = sha256Hex(readBack.data);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *sizeChunks + *dataChunks + *zeroChunks;
        result.warnings = m_warnings;
        result.warnings.append(QStringLiteral(
            "HFS+ inline attribute value changed only within existing record capacity"));
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsAttributeWriteResult replaceForkAttributeValueWithinAllocatedBlocks(
        uint32_t fileId,
        const QString& name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options) {
        PartitionHfsAttributeWriteResult result;
        result.file_system = m_volume.file_system;
        result.file_id = fileId;
        result.attribute_name = name.trimmed();
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        appendAttributeWriteRequestBlockers(fileId, name, data, options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto selection =
            prepareForkAttributeWriteSelection(fileId, name, data, options, &result);
        return selection.has_value() ? replaceForkAttributeValueFromSelection(
                                           *selection, data, options, std::move(result))
                                     : result;
    }

    [[nodiscard]] std::optional<HfsForkAttributeWriteSelection> prepareForkAttributeWriteSelection(
        uint32_t fileId,
        const QString& name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options,
        PartitionHfsAttributeWriteResult* result) {
        const auto attributes = scanAttributeRecords(kPartitionHfsDefaultCheckRecordLimit);
        if (!attributes.has_value()) {
            result->blockers.append(m_blockers);
            return std::nullopt;
        }
        const auto record = findAttributeByIdAndName(*attributes, fileId, name.trimmed());
        if (!record.has_value()) {
            result->blockers.append(QStringLiteral("HFS+ fork attribute was not found"));
            result->blockers.append(m_blockers);
            return std::nullopt;
        }
        appendForkAttributeRecordBlockers(
            *record, attributes->parsed_records, data, options, &result->blockers);
        if (!result->blockers.isEmpty()) {
            result->blockers.append(m_blockers);
            return std::nullopt;
        }
        return HfsForkAttributeWriteSelection{.record = *record,
                                              .all_records = attributes->parsed_records};
    }

    [[nodiscard]] PartitionHfsAttributeWriteResult replaceForkAttributeValueFromSelection(
        const HfsForkAttributeWriteSelection& selection,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options,
        PartitionHfsAttributeWriteResult result) {
        const auto before = readAttributeForkBytes(
            selection.record, selection.all_records, 0, selection.record.fork_logical_size);
        if (!before.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.before_sha256 = sha256Hex(*before);

        const auto dataChunks = writeAttributeForkBytesWithinAllocated(
            selection.record, selection.all_records, 0, data);
        if (!dataChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto zeroChunks = zeroForkAttributeTail(selection.record,
                                                      selection.all_records,
                                                      static_cast<uint64_t>(data.size()));
        if (!zeroChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto metadataChunks =
            updateForkAttributeLogicalSize(selection.record, static_cast<uint64_t>(data.size()));
        if (!metadataChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }

        const auto readBack =
            readAttributeValue(result.file_id, result.attribute_name, options.max_write_bytes);
        if (!readBack.ok || readBack.storage != QStringLiteral("fork") || readBack.data != data) {
            result.blockers.append(
                QStringLiteral("HFS+ fork attribute write read-back verification failed"));
            result.blockers.append(readBack.blockers);
            result.blockers.append(m_blockers);
            return result;
        }

        result.after_sha256 = sha256Hex(readBack.data);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *dataChunks + *zeroChunks + *metadataChunks;
        result.warnings = m_warnings;
        result.warnings.append(QStringLiteral(
            "HFS+ fork attribute length changed only within already allocated blocks"));
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsAttributeWriteResult replaceForkAttributeValueWithAllocationGrowth(
        uint32_t fileId,
        const QString& name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options) {
        return replaceForkAttributeValueWithAllocationGrowthImpl(fileId, name, data, options);
    }

    [[nodiscard]] PartitionHfsConsistencyCheckResult checkConsistency(int maxRecords) {
        PartitionHfsConsistencyCheckResult result;
        result.file_system = m_volume.file_system;
        const auto summary = scanCatalogRecords(std::max(1, maxRecords));
        if (summary.has_value()) {
            result.records_scanned = summary->records;
            result.directories = summary->directories;
            result.files = summary->files;
            result.threads = summary->threads;
            result.other_records = summary->other_records;
            result.invalid_records_skipped = summary->invalid_records;
            appendConsistencyDetails(*summary, &result.details);
        }
        const auto attributes = scanAttributeRecords(std::max(1, maxRecords));
        if (attributes.has_value()) {
            result.attributes_present = attributes->present;
            result.attribute_records_scanned = attributes->records;
            result.inline_attribute_records = attributes->inline_records;
            result.fork_attribute_records = attributes->fork_records;
            result.extent_attribute_records = attributes->extent_records;
            result.other_attribute_records = attributes->other_records;
            result.attribute_names = attributes->names;
            result.attribute_metadata = attributes->metadata;
            result.attribute_records = attributeMetadataRecords(*attributes);
            appendAttributeDetails(*attributes, &result.details);
        }
        result.warnings = m_warnings;
        result.blockers = m_blockers;
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsConsistencyCheckResult consistencyFailureResult(
        const QString& fallbackBlocker) const {
        PartitionHfsConsistencyCheckResult result;
        result.file_system = m_volume.file_system;
        result.blockers = m_blockers;
        result.warnings = m_warnings;
        if (result.blockers.isEmpty()) {
            result.blockers.append(fallbackBlocker);
        }
        return result;
    }

private:
    enum class HfsForkSelector {
        Data,
        Resource
    };

    struct HfsCatalogMutationTarget {
        QString path;
        QString parent_path;
        QString name;
    };

    struct HfsCatalogLeafMutation {
        uint32_t node_number{0};
        QByteArray node;
        QVector<HfsRawCatalogRecord> records;
    };

    struct HfsCatalogCounterUpdate {
        int leaf_record_delta{0};
        int file_count_delta{0};
        int folder_count_delta{0};
        uint32_t next_catalog_id{0};
    };

    struct HfsBTreeNodeWrite {
        uint32_t node_number{0};
        QByteArray bytes;
    };

    struct HfsBTreeHeaderNodeContext {
        QByteArray node;
        qsizetype map_offset{0};
        qsizetype map_end{0};
    };

    struct HfsExtentsTreeMutation {
        bool materialize{false};
        bool split{false};
        bool free_tree{false};
        QVector<HfsBTreeNodeWrite> node_writes;
        QVector<HfsBTreeNodeWrite> post_commit_writes;
        std::optional<HfsBTreeNodeWrite> leaf_rewrite;
        QByteArray header_node;
        HfsBTreeHeader updated_header;
        QVector<HfsOverflowExtentRecord> final_records;
    };

    struct HfsCatalogWorkingLeaf {
        uint32_t node_number{0};
        QVector<HfsRawCatalogRecord> records;
        bool dirty{false};
        bool is_new{false};
    };

    struct HfsCatalogTreeModel {
        HfsBTreeHeaderNodeContext header;
        QVector<HfsCatalogWorkingLeaf> leaves;
        QVector<uint32_t> old_index_nodes;
        int working_free_nodes{0};
        QVector<uint32_t> freed_nodes;
    };

    struct HfsCatalogValenceUpdate {
        uint32_t folder_id{0};
        int delta{0};
    };

    struct HfsCatalogTreeEdit {
        QVector<HfsRawCatalogRecord> insertions;
        QVector<QPair<uint32_t, QString>> removals;
        QVector<QPair<uint32_t, QString>> optional_removals;
        QSet<QString> removal_tokens;
        QVector<HfsRawCatalogRecord> replacements;
        QVector<HfsCatalogValenceUpdate> valence_updates;
        uint32_t read_back_parent_id{0};
        QString read_back_name;
    };

    struct HfsCatalogTreeMutation {
        QVector<HfsBTreeNodeWrite> orphan_writes;
        QVector<HfsBTreeNodeWrite> commit_writes;
        QVector<HfsBTreeNodeWrite> post_commit_writes;
        QByteArray header_node;
        HfsBTreeHeader updated_header;
        QByteArray before_leaf_bytes;
        uint32_t before_leaf_node{0};
        uint32_t insert_node_number{0};
        int removed_records{0};
        int leaf_record_delta{0};
    };

    struct HfsEmptyFileCreatePlan {
        HfsCatalogMutationTarget target;
        HfsCatalogTreeMutation mutation;
        uint32_t file_id{0};
    };

    struct HfsAllocationBitmapByte {
        uint64_t fork_offset{0};
        quint8 original_value{0};
        quint8 updated_value{0};
    };

    struct HfsFileCreatePlan {
        HfsCatalogMutationTarget target;
        HfsCatalogTreeMutation mutation;
        HfsForkData data_fork;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t file_id{0};
        uint32_t allocated_blocks{0};
        QVector<HfsOverflowExtentRecord> overflow_records;
        std::optional<HfsExtentsTreeMutation> extents_mutation;
    };

    struct HfsFileCreateAllocationPlan {
        HfsForkData data_fork;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t allocated_blocks{0};
    };

    struct HfsEmptyFileDeletePlan {
        HfsCatalogRecord record;
        HfsCatalogTreeMutation mutation;
        int removed_records{0};
    };

    struct HfsAllocatedFileDeletePlan {
        HfsCatalogRecord record;
        HfsCatalogTreeMutation mutation;
        QVector<HfsExtent> released_extents;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t released_blocks{0};
        uint64_t released_bytes{0};
        int removed_records{0};
        std::optional<HfsExtentsTreeMutation> extents_mutation;
    };

    struct HfsForkAllocationGrowthPlan {
        HfsCatalogRecord record;
        HfsForkData new_fork;
        QVector<HfsExtent> allocated_extents;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t allocated_blocks{0};
        uint64_t allocated_bytes{0};
        QVector<HfsOverflowExtentRecord> overflow_records;
        std::optional<HfsExtentsTreeMutation> extents_mutation;
    };

    struct HfsForkAttributeAllocationGrowthPlan {
        HfsAttributeRecord record;
        QVector<HfsAttributeRecord> all_records;
        HfsForkData new_fork;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t allocated_blocks{0};
        uint64_t allocated_bytes{0};
    };

    struct HfsFreeExtentScanState {
        uint32_t blocks_needed{0};
        uint32_t found_blocks{0};
        QVector<HfsExtent> extents;
        QStringList* blockers{nullptr};
    };

    [[nodiscard]] PartitionHfsAttributeWriteResult
    replaceForkAttributeValueWithAllocationGrowthImpl(uint32_t fileId,
                                                      const QString& name,
                                                      const QByteArray& data,
                                                      const PartitionHfsFileWriteOptions& options) {
        PartitionHfsAttributeWriteResult result;
        result.file_system = m_volume.file_system;
        result.file_id = fileId;
        result.attribute_name = name.trimmed();
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        appendAttributeWriteRequestBlockers(fileId, name, data, options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }

        const auto plan =
            prepareForkAttributeAllocationGrowth(fileId, name, data, options, &result);
        if (!plan.has_value()) {
            return result;
        }
        const auto before = readAttributeForkBytes(
            plan->record, plan->all_records, 0, plan->record.fork_logical_size);
        if (!before.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.before_sha256 = sha256Hex(*before);

        const HfsAttributeRecord grownRecord = grownAttributeRecord(plan->record, plan->new_fork);
        const auto chunksWritten = writeForkAttributeAllocationGrowth(
            grownRecord, plan->all_records, data, *plan, &result.blockers);
        if (!chunksWritten.has_value()) {
            return result;
        }
        const auto readBack =
            readAttributeValue(result.file_id, result.attribute_name, options.max_write_bytes);
        if (!readBack.ok || readBack.storage != QStringLiteral("fork") || readBack.data != data) {
            result.blockers.append(QStringLiteral(
                "HFS+ fork attribute allocation-growth read-back verification failed"));
            result.blockers.append(readBack.blockers);
            result.blockers.append(m_blockers);
            return result;
        }
        if (!appendForkGrowthCounterReadBack(plan->allocated_blocks, &result)) {
            return result;
        }

        result.after_sha256 = sha256Hex(readBack.data);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *chunksWritten;
        result.warnings = m_warnings;
        result.warnings.append(QStringLiteral("HFS+ fork attribute grew by allocating %1 new "
                                              "block(s) inside initial extent records")
                                   .arg(plan->allocated_blocks));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] std::optional<HfsForkAttributeAllocationGrowthPlan>
    prepareForkAttributeAllocationGrowth(uint32_t fileId,
                                         const QString& name,
                                         const QByteArray& data,
                                         const PartitionHfsFileWriteOptions& options,
                                         PartitionHfsAttributeWriteResult* result) {
        const auto attributes = scanAttributeRecords(kPartitionHfsDefaultCheckRecordLimit);
        if (!attributes.has_value()) {
            result->blockers.append(m_blockers);
            return std::nullopt;
        }
        const auto record = findAttributeByIdAndName(*attributes, fileId, name.trimmed());
        if (!record.has_value()) {
            result->blockers.append(QStringLiteral("HFS+ fork attribute was not found"));
            result->blockers.append(m_blockers);
            return std::nullopt;
        }
        appendForkAttributeAllocationGrowthBlockers(
            *record, attributes->parsed_records, data, options, &result->blockers);
        if (!result->blockers.isEmpty()) {
            result->blockers.append(m_blockers);
            return std::nullopt;
        }

        const auto oldExtents = initialForkExtentsForAllocatedBlocks(
            record->fork_data, QStringLiteral("fork attribute"), &result->blockers);
        const auto requiredBlocks = requiredAllocationBlocksForBytes(data, &result->blockers);
        if (!oldExtents.has_value() || !requiredBlocks.has_value()) {
            return std::nullopt;
        }
        const uint32_t newBlocks = *requiredBlocks - record->fork_data.total_blocks;
        const auto newExtents = findFreeAllocationExtents(newBlocks,
                                                          QStringLiteral("fork attribute"),
                                                          &result->blockers);
        if (!newExtents.has_value()) {
            return std::nullopt;
        }
        auto combinedExtents = *oldExtents;
        appendExtentsCoalesced(&combinedExtents, *newExtents);
        if (combinedExtents.size() > kHfsExtentCount) {
            result->blockers.append(QStringLiteral(
                "HFS+ fork attribute growth needs more than eight initial extent records"));
            return std::nullopt;
        }
        const auto allocationBytes = prepareAllocationBitmapSet(*newExtents, &result->blockers);
        if (!allocationBytes.has_value()) {
            return std::nullopt;
        }
        return HfsForkAttributeAllocationGrowthPlan{
            .record = *record,
            .all_records = attributes->parsed_records,
            .new_fork = HfsForkData{.logical_size = static_cast<uint64_t>(data.size()),
                                    .total_blocks = *requiredBlocks,
                                    .extents = combinedExtents},
            .allocation_bytes = *allocationBytes,
            .allocated_blocks = newBlocks,
            .allocated_bytes = static_cast<uint64_t>(newBlocks) * m_volume.block_size};
    }

    void appendForkAttributeAllocationGrowthBlockers(const HfsAttributeRecord& record,
                                                     const QVector<HfsAttributeRecord>& allRecords,
                                                     const QByteArray& data,
                                                     const PartitionHfsFileWriteOptions& options,
                                                     QStringList* blockers) {
        appendForkAttributeTypeBlockers(record, options, blockers);
        if (!blockers->isEmpty()) {
            return;
        }
        if (record.attribute_data_offset == 0) {
            blockers->append(QStringLiteral("HFS+ fork attribute record location is unavailable"));
        }
        appendAllocationForkGrowthBlockers(blockers);
        appendForkAttributeGrowthSizeBlockers(record, data, options, blockers);
        appendForkAttributeGrowthMapBlockers(record, allRecords, blockers);
    }

    void appendForkAttributeGrowthSizeBlockers(const HfsAttributeRecord& record,
                                               const QByteArray& data,
                                               const PartitionHfsFileWriteOptions& options,
                                               QStringList* blockers) const {
        if (record.fork_logical_size > options.max_write_bytes) {
            blockers->append(
                QStringLiteral("HFS+ fork attribute existing value exceeds configured write cap"));
        }
        const auto requiredBlocks = requiredAllocationBlocksForBytes(data, blockers);
        if (!requiredBlocks.has_value()) {
            return;
        }
        if (*requiredBlocks <= record.fork_data.total_blocks) {
            blockers->append(QStringLiteral(
                "HFS+ fork attribute replacement does not require allocation growth"));
            return;
        }
        if (*requiredBlocks - record.fork_data.total_blocks > m_volume.free_blocks) {
            blockers->append(QStringLiteral("HFS+ volume does not have enough free blocks"));
        }
    }

    void appendForkAttributeGrowthMapBlockers(const HfsAttributeRecord& record,
                                              const QVector<HfsAttributeRecord>& allRecords,
                                              QStringList* blockers) {
        if (record.fork_data.total_blocks > 0 && record.fork_data.extents.isEmpty()) {
            blockers->append(
                QStringLiteral("HFS+ existing fork attribute extents are not available"));
            return;
        }
        const auto oldExtents = initialForkExtentsForAllocatedBlocks(
            record.fork_data, QStringLiteral("fork attribute"), blockers);
        if (!oldExtents.has_value()) {
            return;
        }
        const auto allocatedBytes = forkAllocatedBytes(record.fork_data);
        if (!allocatedBytes.has_value()) {
            blockers->append(QStringLiteral("HFS+ fork attribute allocation byte count overflow"));
            return;
        }
        appendAttributeForkPhysicalMapBlockers(record, allRecords, *allocatedBytes, blockers);
    }

    [[nodiscard]] HfsAttributeRecord grownAttributeRecord(const HfsAttributeRecord& record,
                                                          const HfsForkData& fork) const {
        HfsAttributeRecord grown = record;
        grown.fork_data = fork;
        grown.fork_logical_size = fork.logical_size;
        grown.fork_total_blocks = fork.total_blocks;
        grown.fork_extent_count = fork.extents.size();
        return grown;
    }

    [[nodiscard]] std::optional<int> writeForkAttributeAllocationGrowth(
        const HfsAttributeRecord& grownRecord,
        const QVector<HfsAttributeRecord>& allRecords,
        const QByteArray& data,
        const HfsForkAttributeAllocationGrowthPlan& plan,
        QStringList* blockers) {
        const auto dataChunks =
            writeAttributeForkBytesWithinAllocated(grownRecord, allRecords, 0, data);
        if (!dataChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto slackChunks = zeroForkAttributeAllocationSlack(
            grownRecord, allRecords, static_cast<uint64_t>(data.size()));
        if (!slackChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto bitmapChunks = writeUpdatedAllocationBitmapBytes(plan.allocation_bytes);
        if (!bitmapChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto metadataChunks = updateForkAttributeForkData(plan.record, plan.new_fork);
        if (!metadataChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto freeBlockChunks =
            writeVolumeHeaderCounter(kHfsFreeBlocksOffset,
                                     m_volume.free_blocks,
                                     -static_cast<int>(plan.allocated_blocks),
                                     blockers);
        if (!freeBlockChunks.has_value()) {
            return std::nullopt;
        }
        return *dataChunks + *slackChunks + *bitmapChunks + *metadataChunks + *freeBlockChunks;
    }

    [[nodiscard]] std::optional<int> zeroForkAttributeAllocationSlack(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t logicalSize) {
        const auto allocatedBytes = forkAllocatedBytes(record.fork_data);
        if (!allocatedBytes.has_value()) {
            m_blockers.append(QStringLiteral("HFS+ fork attribute allocation byte count overflow"));
            return std::nullopt;
        }
        if (logicalSize >= *allocatedBytes) {
            return 0;
        }
        const uint64_t slackBytes = *allocatedBytes - logicalSize;
        if (slackBytes > m_volume.block_size ||
            slackBytes > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
            m_blockers.append(
                QStringLiteral("HFS+ fork attribute allocation slack zeroing is too large"));
            return std::nullopt;
        }
        const QByteArray zeros(static_cast<qsizetype>(slackBytes), '\0');
        return writeAttributeForkBytesWithinAllocated(record, allRecords, logicalSize, zeros);
    }

    [[nodiscard]] std::optional<int> updateForkAttributeForkData(const HfsAttributeRecord& record,
                                                                 const HfsForkData& fork) {
        uint64_t forkBaseOffset = 0;
        if (!checkedAdd(
                record.attribute_data_offset, kHfsAttributeForkDataOffset, &forkBaseOffset)) {
            m_blockers.append(QStringLiteral("HFS+ fork attribute data offset overflow"));
            return std::nullopt;
        }
        int chunks = 0;
        const auto logicalSizeChunks = writeForkBytes(m_volume.attributes_fork,
                                                      kHfsAttributesFileId,
                                                      kHfsDataForkType,
                                                      forkBaseOffset + kHfsForkLogicalSizeOffset,
                                                      be64Bytes(fork.logical_size));
        if (!logicalSizeChunks.has_value()) {
            return std::nullopt;
        }
        chunks += *logicalSizeChunks;
        const auto totalBlockChunks = writeForkBytes(m_volume.attributes_fork,
                                                     kHfsAttributesFileId,
                                                     kHfsDataForkType,
                                                     forkBaseOffset + kHfsForkTotalBlocksOffset,
                                                     be32Bytes(fork.total_blocks));
        if (!totalBlockChunks.has_value()) {
            return std::nullopt;
        }
        chunks += *totalBlockChunks;
        const auto extentChunks = writeForkBytes(m_volume.attributes_fork,
                                                 kHfsAttributesFileId,
                                                 kHfsDataForkType,
                                                 forkBaseOffset + kHfsForkExtentsOffset,
                                                 hfsForkExtentBytes(fork.extents));
        if (!extentChunks.has_value()) {
            return std::nullopt;
        }
        return chunks + *extentChunks;
    }

    struct HfsFolderTreeDeletePlan {
        HfsCatalogRecord record;
        HfsCatalogTreeMutation mutation;
        QVector<HfsExtent> released_extents;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t released_blocks{0};
        uint64_t released_bytes{0};
        int removed_records{0};
        int removed_files{0};
        int removed_folders{0};
        std::optional<HfsExtentsTreeMutation> extents_mutation;
    };

    struct HfsFolderTreeDeleteSelection {
        QVector<HfsCatalogRecord> records;
        QVector<HfsExtent> released_extents;
        QSet<QString> keys_to_remove;
        int removed_records{0};
        int removed_files{0};
        int removed_folders{0};
    };

    struct HfsFolderTreeSelectionContext {
        const HfsCatalogLeafMutation& leaf;
        const PartitionHfsFileWriteOptions& options;
        QSet<uint32_t>* folder_ids{nullptr};
        HfsFolderTreeDeleteSelection* selection{nullptr};
        QStringList* blockers{nullptr};
        QStringList* warnings{nullptr};
    };

    struct HfsFolderTreeReleasePlan {
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t released_blocks{0};
        uint64_t released_bytes{0};
    };

    struct HfsOverflowForkExtentAppendContext {
        const HfsForkData& fork;
        uint32_t file_id{0};
        uint8_t fork_type{0};
        uint32_t covered_blocks{0};
        QVector<HfsExtent>* extents{nullptr};
        QStringList* blockers{nullptr};
    };

    using HfsEmptyFolderCreatePlan = HfsEmptyFileCreatePlan;
    using HfsEmptyFolderDeletePlan = HfsEmptyFileDeletePlan;

    struct HfsCatalogRenameMovePlan {
        HfsCatalogRecord source_record;
        HfsCatalogMutationTarget destination;
        HfsCatalogTreeMutation mutation;
        uint32_t destination_parent_id{0};
        bool parent_changed{false};
    };

    struct HfsCatalogRenameMoveResolved {
        HfsCatalogRecord source_record;
        HfsCatalogMutationTarget destination;
        uint32_t destination_parent_id{0};
    };

    void appendCatalogMutationOptionBlockers(const PartitionHfsFileWriteOptions& options,
                                             QStringList* blockers) const {
        appendWriterActivationBlockers(options, blockers);
        appendWriterDeviceBlockers(blockers);
        if (options.max_write_bytes == 0) {
            blockers->append(QStringLiteral("HFS+ catalog mutation requires a nonzero write cap"));
        }
        appendWriterVolumeBlockers(options, blockers);
    }

    [[nodiscard]] std::optional<HfsCatalogMutationTarget> catalogMutationTarget(
        const QString& path, QStringList* blockers) const {
        QStringList pathBlockers;
        const QStringList parts = hfsPathParts(path, &pathBlockers);
        blockers->append(pathBlockers);
        if (!pathBlockers.isEmpty()) {
            return std::nullopt;
        }
        if (parts.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ file path is required"));
            return std::nullopt;
        }
        const QString name = parts.last().trimmed();
        if (name.isEmpty() || name.size() > kHfsMaximumNameChars || name.contains(QChar::Null) ||
            name.contains(QLatin1Char('/')) || name.contains(QLatin1Char(':'))) {
            blockers->append(QStringLiteral("Unsupported HFS+ file name"));
            return std::nullopt;
        }
        const QString parentPath =
            parts.size() == 1
                ? QStringLiteral("/")
                : QStringLiteral("/%1").arg(parts.mid(0, parts.size() - 1).join(QLatin1Char('/')));
        return HfsCatalogMutationTarget{.path =
                                            QStringLiteral("/%1").arg(parts.join(QLatin1Char('/'))),
                                        .parent_path = parentPath,
                                        .name = name};
    }

    struct HfsCatalogCreateContext {
        HfsCatalogMutationTarget target;
        HfsCatalogTreeModel model;
        uint32_t parent_id{0};
        uint32_t new_id{0};
    };

    [[nodiscard]] std::optional<HfsCatalogCreateContext> prepareCatalogCreateContext(
        const QString& path, QStringList* blockers) {
        const auto target = catalogMutationTarget(path, blockers);
        if (!target.has_value()) {
            return std::nullopt;
        }
        const auto parentId = resolveEmptyFileCreateParent(*target, blockers);
        if (!parentId.has_value()) {
            return std::nullopt;
        }
        auto model = loadCatalogTreeForMutation(blockers);
        if (!model.has_value()) {
            return std::nullopt;
        }
        const uint32_t newId = nextSafeCatalogId(flattenedCatalogModelRecords(*model), blockers);
        if (!blockers->isEmpty()) {
            return std::nullopt;
        }
        return HfsCatalogCreateContext{*target, std::move(*model), *parentId, newId};
    }

    [[nodiscard]] std::optional<HfsEmptyFileCreatePlan> prepareEmptyFileCreate(
        const QString& path, QStringList* blockers, QStringList* warnings) {
        return withCatalogNodePoolGrowth(
            [&](QStringList* b, QStringList* w) { return prepareEmptyFileCreateOnce(path, b, w); },
            blockers,
            warnings);
    }

    [[nodiscard]] std::optional<HfsEmptyFileCreatePlan> prepareEmptyFileCreateOnce(
        const QString& path, QStringList* blockers, QStringList* warnings) {
        auto context = prepareCatalogCreateContext(path, blockers);
        if (!context.has_value()) {
            return std::nullopt;
        }
        HfsCatalogTreeEdit edit;
        appendEmptyFileCatalogRecords(
            &edit.insertions, context->parent_id, context->target.name, context->new_id);
        edit.valence_updates.append({context->parent_id, 1});
        edit.read_back_parent_id = context->parent_id;
        edit.read_back_name = context->target.name;
        const auto mutation = finishCatalogTreeMutation(&context->model, edit, blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        return HfsEmptyFileCreatePlan{context->target, *mutation, context->new_id};
    }

    [[nodiscard]] std::optional<HfsFileCreatePlan> prepareFileCreate(const QString& path,
                                                                     const QByteArray& data,
                                                                     QStringList* blockers,
                                                                     QStringList* warnings) {
        return withCatalogNodePoolGrowth(
            [&](QStringList* b, QStringList* w) { return prepareFileCreateOnce(path, data, b, w); },
            blockers,
            warnings);
    }

    [[nodiscard]] std::optional<HfsFileCreatePlan> prepareFileCreateOnce(const QString& path,
                                                                         const QByteArray& data,
                                                                         QStringList* blockers,
                                                                         QStringList* warnings) {
        const auto allocation = prepareFileCreateAllocation(data, blockers);
        if (!allocation.has_value()) {
            return std::nullopt;
        }
        auto context = prepareCatalogCreateContext(path, blockers);
        if (!context.has_value()) {
            return std::nullopt;
        }
        HfsCatalogTreeEdit edit;
        appendDataFileCatalogRecords(&edit.insertions,
                                     context->parent_id,
                                     context->target.name,
                                     context->new_id,
                                     allocation->data_fork);
        edit.valence_updates.append({context->parent_id, 1});
        edit.read_back_parent_id = context->parent_id;
        edit.read_back_name = context->target.name;
        const auto mutation = finishCatalogTreeMutation(&context->model, edit, blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        HfsFileCreatePlan plan;
        plan.target = context->target;
        plan.mutation = *mutation;
        plan.data_fork = allocation->data_fork;
        plan.allocation_bytes = allocation->allocation_bytes;
        plan.file_id = context->new_id;
        plan.allocated_blocks = allocation->allocated_blocks;
        if (plan.data_fork.extents.size() > kHfsExtentCount) {
            plan.overflow_records = overflowRecordsForCombinedExtents(plan.data_fork.extents,
                                                                      plan.file_id,
                                                                      kHfsDataForkType);
            const auto extentsMutation =
                prepareExtentsTreeMutation({}, plan.overflow_records, blockers);
            if (!extentsMutation.has_value()) {
                return std::nullopt;
            }
            plan.extents_mutation = *extentsMutation;
        }
        return plan;
    }

    [[nodiscard]] std::optional<HfsFileCreateAllocationPlan> prepareFileCreateAllocation(
        const QByteArray& data, QStringList* blockers) {
        appendAllocationForkGrowthBlockers(blockers);
        const auto requiredBlocks = requiredAllocationBlocksForBytes(data, blockers);
        if (!requiredBlocks.has_value()) {
            return std::nullopt;
        }
        if (*requiredBlocks > m_volume.free_blocks) {
            blockers->append(QStringLiteral("HFS+ volume does not have enough free blocks"));
            return std::nullopt;
        }
        const auto extents =
            findFreeAllocationExtents(*requiredBlocks, QStringLiteral("created file"), blockers);
        if (!extents.has_value()) {
            return std::nullopt;
        }
        const auto allocationBytes = prepareAllocationBitmapSet(*extents, blockers);
        if (!allocationBytes.has_value()) {
            return std::nullopt;
        }
        HfsFileCreateAllocationPlan plan;
        plan.data_fork.logical_size = static_cast<uint64_t>(data.size());
        plan.data_fork.total_blocks = *requiredBlocks;
        plan.data_fork.extents = *extents;
        plan.allocation_bytes = *allocationBytes;
        plan.allocated_blocks = *requiredBlocks;
        return plan;
    }

    [[nodiscard]] std::optional<int> writeCreatedFileDataAndMetadata(const HfsFileCreatePlan& plan,
                                                                     const QByteArray& data,
                                                                     QStringList* blockers) {
        const auto dataChunks =
            writeForkBytesWithinAllocated(plan.data_fork, plan.file_id, kHfsDataForkType, 0, data);
        if (!dataChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto slackChunks =
            zeroForkAllocationSlack(plan.data_fork, plan.file_id, kHfsDataForkType, data.size());
        if (!slackChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto bitmapChunks = writeUpdatedAllocationBitmapBytes(plan.allocation_bytes);
        if (!bitmapChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        int extentsChunks = 0;
        if (plan.extents_mutation.has_value()) {
            const auto treeChunks = applyExtentsTreeMutation(*plan.extents_mutation, blockers);
            if (!treeChunks.has_value()) {
                return std::nullopt;
            }
            extentsChunks = *treeChunks;
        }
        const auto catalogChunks =
            applyCatalogTreeMutation(plan.mutation,
                                     HfsCatalogCounterUpdate{plan.mutation.leaf_record_delta,
                                                             kHfsEmptyFileCreateFileCountDelta,
                                                             0,
                                                             plan.file_id + 1},
                                     blockers);
        if (!catalogChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto freeBlockChunks =
            writeVolumeHeaderCounter(kHfsFreeBlocksOffset,
                                     m_volume.free_blocks,
                                     -static_cast<int>(plan.allocated_blocks),
                                     blockers);
        if (!freeBlockChunks.has_value()) {
            return std::nullopt;
        }
        return *dataChunks + *slackChunks + *bitmapChunks + extentsChunks + *catalogChunks +
               *freeBlockChunks;
    }

    [[nodiscard]] std::optional<QByteArray> readBackCreatedFileData(const HfsFileCreatePlan& plan,
                                                                    const QByteArray& data,
                                                                    uint64_t maxBytes,
                                                                    QStringList* blockers) {
        const auto readBack = readFile(plan.target.path, maxBytes);
        if (readBack.ok && readBack.data == data) {
            return readBack.data;
        }
        blockers->append(QStringLiteral("HFS+ file create read-back failed"));
        blockers->append(readBack.blockers);
        blockers->append(m_blockers);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<uint32_t> resolveEmptyFileCreateParent(
        const HfsCatalogMutationTarget& target, QStringList* blockers) {
        const auto parentId = resolveFolderPath(target.parent_path);
        if (!parentId.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        if (findChild(*parentId, target.name).has_value()) {
            blockers->append(QStringLiteral("HFS+ catalog entry already exists"));
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return parentId;
    }

    void appendDataFileCatalogRecords(QVector<HfsRawCatalogRecord>* records,
                                      uint32_t parentId,
                                      const QString& name,
                                      uint32_t fileId,
                                      const HfsForkData& dataFork) const {
        records->append(HfsRawCatalogRecord{
            .parent_id = parentId,
            .name = name,
            .record_type = kHfsCatalogFileRecord,
            .catalog_id = fileId,
            .bytes = hfsCatalogRecordBytes(
                parentId, name, hfsCatalogFileRecordWithDataFork(fileId, dataFork))});
        records->append(HfsRawCatalogRecord{
            .parent_id = fileId,
            .name = QString(),
            .record_type = kHfsCatalogFileThreadRecord,
            .catalog_id = 0,
            .bytes = hfsCatalogRecordBytes(
                fileId, QString(), hfsCatalogFileThreadRecord(parentId, name))});
    }

    void appendEmptyFileCatalogRecords(QVector<HfsRawCatalogRecord>* records,
                                       uint32_t parentId,
                                       const QString& name,
                                       uint32_t fileId) const {
        records->append(HfsRawCatalogRecord{
            .parent_id = parentId,
            .name = name,
            .record_type = kHfsCatalogFileRecord,
            .catalog_id = fileId,
            .bytes = hfsCatalogRecordBytes(parentId, name, hfsCatalogEmptyFileRecord(fileId))});
        records->append(HfsRawCatalogRecord{
            .parent_id = fileId,
            .name = QString(),
            .record_type = kHfsCatalogFileThreadRecord,
            .catalog_id = 0,
            .bytes = hfsCatalogRecordBytes(
                fileId, QString(), hfsCatalogFileThreadRecord(parentId, name))});
    }

    bool appendEmptyFileCreateReadBack(const QString& path,
                                       uint32_t leafNode,
                                       PartitionHfsFileWriteResult* result) {
        const auto readBack = readFile(path, 1);
        if (!readBack.ok || !readBack.data.isEmpty()) {
            result->blockers.append(QStringLiteral("HFS+ empty-file create read-back failed"));
            result->blockers.append(readBack.blockers);
            result->blockers.append(m_blockers);
            return false;
        }
        return appendCatalogLeafAfterHash(leafNode, result);
    }

    [[nodiscard]] std::optional<HfsEmptyFolderCreatePlan> prepareEmptyFolderCreate(
        const QString& path, QStringList* blockers, QStringList* warnings) {
        return withCatalogNodePoolGrowth(
            [&](QStringList* b, QStringList* w) {
                return prepareEmptyFolderCreateOnce(path, b, w);
            },
            blockers,
            warnings);
    }

    [[nodiscard]] std::optional<HfsEmptyFolderCreatePlan> prepareEmptyFolderCreateOnce(
        const QString& path, QStringList* blockers, QStringList* warnings) {
        auto context = prepareCatalogCreateContext(path, blockers);
        if (!context.has_value()) {
            return std::nullopt;
        }
        HfsCatalogTreeEdit edit;
        appendEmptyFolderCatalogRecords(
            &edit.insertions, context->parent_id, context->target.name, context->new_id);
        edit.valence_updates.append({context->parent_id, 1});
        edit.read_back_parent_id = context->parent_id;
        edit.read_back_name = context->target.name;
        const auto mutation = finishCatalogTreeMutation(&context->model, edit, blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        return HfsEmptyFolderCreatePlan{context->target, *mutation, context->new_id};
    }

    void appendEmptyFolderCatalogRecords(QVector<HfsRawCatalogRecord>* records,
                                         uint32_t parentId,
                                         const QString& name,
                                         uint32_t folderId) const {
        records->append(HfsRawCatalogRecord{
            .parent_id = parentId,
            .name = name,
            .record_type = kHfsCatalogFolderRecord,
            .catalog_id = folderId,
            .bytes = hfsCatalogRecordBytes(parentId, name, hfsCatalogEmptyFolderRecord(folderId))});
        records->append(HfsRawCatalogRecord{
            .parent_id = folderId,
            .name = QString(),
            .record_type = kHfsCatalogFolderThreadRecord,
            .catalog_id = 0,
            .bytes = hfsCatalogRecordBytes(
                folderId, QString(), hfsCatalogFolderThreadRecord(parentId, name))});
    }

    bool appendEmptyFolderCreateReadBack(const QString& path,
                                         uint32_t leafNode,
                                         PartitionHfsFileWriteResult* result) {
        const auto folderId = resolveFolderPath(path);
        if (!folderId.has_value()) {
            result->blockers.append(QStringLiteral("HFS+ empty-folder create read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        const auto children = childRecords(*folderId, 1);
        if (!children.has_value() || !children->isEmpty()) {
            result->blockers.append(QStringLiteral("HFS+ empty-folder child scan failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        return appendCatalogLeafAfterHash(leafNode, result);
    }

    [[nodiscard]] HfsCatalogTreeEdit catalogDeleteEdit(const HfsCatalogRecord& record) const {
        HfsCatalogTreeEdit edit;
        edit.removals.append({record.parent_id, record.name});
        edit.optional_removals.append({record.catalog_id, QString()});
        edit.valence_updates.append({record.parent_id, -1});
        edit.read_back_parent_id = record.parent_id;
        edit.read_back_name = record.name;
        return edit;
    }

    [[nodiscard]] std::optional<HfsEmptyFileDeletePlan> prepareEmptyFileDelete(
        const QString& path,
        const PartitionHfsFileWriteOptions& options,
        QStringList* blockers,
        QStringList* warnings) {
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        appendDeleteEmptyFileBlockers(*record, options, blockers);
        if (!blockers->isEmpty()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto mutation =
            prepareCatalogTreeMutation(catalogDeleteEdit(*record), blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        return HfsEmptyFileDeletePlan{*record, *mutation, mutation->removed_records};
    }

    struct HfsReleasableAllocation {
        QVector<HfsExtent> extents;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t released_blocks{0};
        uint64_t released_bytes{0};
    };

    [[nodiscard]] std::optional<HfsReleasableAllocation> prepareFileReleaseAllocation(
        const HfsCatalogRecord& record,
        const PartitionHfsFileWriteOptions& options,
        QStringList* blockers) {
        const auto extents = releasableFileExtents(record, blockers);
        if (!extents.has_value()) {
            return std::nullopt;
        }
        const auto releasedBlocks = extentBlockSum(*extents);
        const auto releasedBytes = bytesForReleasedBlocks(releasedBlocks, blockers);
        if (!releasedBytes.has_value() ||
            !releasedBlocksWithinCap(*releasedBytes, options, blockers)) {
            return std::nullopt;
        }
        const auto allocationBytes = prepareAllocationBitmapClear(*extents, blockers);
        if (!allocationBytes.has_value()) {
            return std::nullopt;
        }
        return HfsReleasableAllocation{*extents, *allocationBytes, releasedBlocks, *releasedBytes};
    }

    [[nodiscard]] std::optional<HfsAllocatedFileDeletePlan> prepareAllocatedFileDelete(
        const QString& path,
        const PartitionHfsFileWriteOptions& options,
        QStringList* blockers,
        QStringList* warnings) {
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        appendDeleteAllocatedFileBlockers(*record, options, blockers);
        if (!blockers->isEmpty()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto release = prepareFileReleaseAllocation(*record, options, blockers);
        if (!release.has_value()) {
            return std::nullopt;
        }
        const auto mutation =
            prepareCatalogTreeMutation(catalogDeleteEdit(*record), blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        std::optional<HfsExtentsTreeMutation> extentsMutation;
        if (!prepareOverflowRemovalMutation(
                overflowRemovalsForFile(record->catalog_id), &extentsMutation, blockers)) {
            return std::nullopt;
        }
        return HfsAllocatedFileDeletePlan{*record,
                                          *mutation,
                                          release->extents,
                                          release->allocation_bytes,
                                          release->released_blocks,
                                          release->released_bytes,
                                          mutation->removed_records,
                                          extentsMutation};
    }

    [[nodiscard]] bool prepareOverflowRemovalMutation(
        const QVector<QPair<uint32_t, uint8_t>>& removals,
        std::optional<HfsExtentsTreeMutation>* mutation,
        QStringList* blockers) {
        if (removals.isEmpty()) {
            return true;
        }
        const auto prepared = prepareExtentsTreeMutation(removals, {}, blockers);
        if (!prepared.has_value()) {
            return false;
        }
        *mutation = *prepared;
        return true;
    }

    [[nodiscard]] std::optional<int> applyDeleteExtentsMutationStep(
        const std::optional<HfsExtentsTreeMutation>& mutation,
        const QString& warning,
        PartitionHfsFileWriteResult* result) {
        if (!mutation.has_value()) {
            return 0;
        }
        const auto chunks = applyExtentsTreeMutation(*mutation, &result->blockers);
        if (!chunks.has_value()) {
            return std::nullopt;
        }
        result->warnings.append(warning);
        return chunks;
    }

    [[nodiscard]] QVector<QPair<uint32_t, uint8_t>> folderTreeOverflowRemovals(
        const QVector<HfsCatalogRecord>& deletedRecords) const {
        QVector<QPair<uint32_t, uint8_t>> removals;
        for (const auto& deleted : deletedRecords) {
            if (deleted.regularFile()) {
                removals.append(overflowRemovalsForFile(deleted.catalog_id));
            }
        }
        return removals;
    }

    [[nodiscard]] QVector<QPair<uint32_t, uint8_t>> overflowRemovalsForFile(uint32_t fileId) const {
        bool hasData = false;
        bool hasResource = false;
        for (const auto& record : m_overflow_extents) {
            if (record.file_id != fileId) {
                continue;
            }
            if (record.fork_type == kHfsDataForkType) {
                hasData = true;
            } else if (record.fork_type == kHfsResourceForkType) {
                hasResource = true;
            }
        }
        QVector<QPair<uint32_t, uint8_t>> removals;
        if (hasData) {
            removals.append({fileId, kHfsDataForkType});
        }
        if (hasResource) {
            removals.append({fileId, kHfsResourceForkType});
        }
        return removals;
    }

    [[nodiscard]] std::optional<HfsEmptyFolderDeletePlan> prepareEmptyFolderDelete(
        const QString& path, QStringList* blockers, QStringList* warnings) {
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        appendDeleteEmptyFolderBlockers(*record, blockers);
        if (!blockers->isEmpty()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto mutation =
            prepareCatalogTreeMutation(catalogDeleteEdit(*record), blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        return HfsEmptyFolderDeletePlan{*record, *mutation, mutation->removed_records};
    }

    [[nodiscard]] std::optional<HfsFolderTreeDeletePlan> prepareFolderTreeDelete(
        const QString& path,
        const PartitionHfsFileWriteOptions& options,
        QStringList* blockers,
        QStringList* warnings) {
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        appendDeleteFolderTreeBlockers(*record, blockers);
        if (!blockers->isEmpty()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        auto model = loadCatalogTreeForMutation(blockers);
        if (!model.has_value()) {
            return std::nullopt;
        }
        const HfsCatalogLeafMutation flattened{.node_number = 0,
                                               .node = QByteArray(),
                                               .records = flattenedCatalogModelRecords(*model)};
        const auto selection =
            folderTreeDeleteSelection(*record, flattened, options, blockers, warnings);
        if (!selection.has_value()) {
            return std::nullopt;
        }
        const auto release = folderTreeReleasePlan(*selection, options, blockers);
        if (!release.has_value()) {
            return std::nullopt;
        }
        HfsCatalogTreeEdit edit;
        edit.removal_tokens = selection->keys_to_remove;
        edit.valence_updates.append({record->parent_id, -1});
        edit.read_back_parent_id = record->parent_id;
        edit.read_back_name = record->name;
        const auto mutation = finishCatalogTreeMutation(&*model, edit, blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        std::optional<HfsExtentsTreeMutation> extentsMutation;
        if (!prepareOverflowRemovalMutation(
                folderTreeOverflowRemovals(selection->records), &extentsMutation, blockers)) {
            return std::nullopt;
        }
        return HfsFolderTreeDeletePlan{*record,
                                       *mutation,
                                       selection->released_extents,
                                       release->allocation_bytes,
                                       release->released_blocks,
                                       release->released_bytes,
                                       mutation->removed_records,
                                       selection->removed_files,
                                       selection->removed_folders,
                                       extentsMutation};
    }

    [[nodiscard]] std::optional<HfsCatalogRenameMoveResolved> resolveCatalogRenameMoveTargets(
        const QString& sourcePath, const QString& destinationPath, QStringList* blockers) {
        const auto sourceRecord = resolveCatalogPath(sourcePath);
        if (!sourceRecord.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        if (sourceRecord->catalog_id == kHfsRootFolderId) {
            blockers->append(QStringLiteral("HFS+ root folder rename/move is blocked"));
            return std::nullopt;
        }
        const auto destination = catalogMutationTarget(destinationPath, blockers);
        if (!destination.has_value()) {
            return std::nullopt;
        }
        const auto destinationParentId = resolveFolderPath(destination->parent_path);
        if (!destinationParentId.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        if (catalogKeysEqual(sourceRecord->parent_id,
                             sourceRecord->name,
                             *destinationParentId,
                             destination->name,
                             m_catalog.key_compare_type)) {
            blockers->append(
                QStringLiteral("HFS+ rename/move destination must change the catalog key"));
            return std::nullopt;
        }
        if (findChild(*destinationParentId, destination->name).has_value()) {
            blockers->append(QStringLiteral("HFS+ rename/move destination already exists"));
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return HfsCatalogRenameMoveResolved{*sourceRecord, *destination, *destinationParentId};
    }

    [[nodiscard]] bool catalogRenameMoveLeafIsSafe(const HfsCatalogRecord& source,
                                                   uint32_t destinationParentId,
                                                   const HfsCatalogLeafMutation& leaf,
                                                   QStringList* blockers) const {
        if (!source.directory()) {
            return true;
        }
        if (catalogParentChainContains(
                leaf.records, destinationParentId, source.catalog_id, blockers)) {
            blockers->append(
                QStringLiteral("HFS+ folder cannot be moved into itself or a descendant"));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<HfsCatalogRenameMovePlan> prepareCatalogRenameMove(
        const QString& sourcePath,
        const QString& destinationPath,
        QStringList* blockers,
        QStringList* warnings) {
        return withCatalogNodePoolGrowth(
            [&](QStringList* b, QStringList* w) {
                return prepareCatalogRenameMoveOnce(sourcePath, destinationPath, b, w);
            },
            blockers,
            warnings);
    }

    [[nodiscard]] std::optional<HfsCatalogRenameMovePlan> prepareCatalogRenameMoveOnce(
        const QString& sourcePath,
        const QString& destinationPath,
        QStringList* blockers,
        QStringList* warnings) {
        const auto resolved =
            resolveCatalogRenameMoveTargets(sourcePath, destinationPath, blockers);
        if (!resolved.has_value()) {
            return std::nullopt;
        }
        auto model = loadCatalogTreeForMutation(blockers);
        if (!model.has_value()) {
            return std::nullopt;
        }
        const HfsCatalogLeafMutation flattened{.node_number = 0,
                                               .node = QByteArray(),
                                               .records = flattenedCatalogModelRecords(*model)};
        if (!catalogRenameMoveLeafIsSafe(
                resolved->source_record, resolved->destination_parent_id, flattened, blockers)) {
            return std::nullopt;
        }

        const auto& source = resolved->source_record;
        const int sourceLeaf = catalogModelLeafContainingKey(*model, source.parent_id, source.name);
        const int threadLeaf = catalogModelLeafContainingKey(*model, source.catalog_id, QString());
        if (sourceLeaf < 0 || threadLeaf < 0) {
            blockers->append(
                QStringLiteral("HFS+ rename/move source catalog records were not found"));
            return std::nullopt;
        }
        const auto& sourceRecords = model->leaves.at(sourceLeaf).records;
        const auto sourceRaw =
            sourceRecords.at(findRawCatalogRecord(sourceRecords, source.parent_id, source.name));
        const auto mainPayload = catalogRecordPayload(sourceRaw, source.record_type, blockers);
        if (!mainPayload.has_value()) {
            return std::nullopt;
        }
        const uint16_t threadType = source.directory() ? kHfsCatalogFolderThreadRecord
                                                       : kHfsCatalogFileThreadRecord;

        HfsCatalogTreeEdit edit;
        edit.removals.append({source.parent_id, source.name});
        edit.insertions.append(
            HfsRawCatalogRecord{.parent_id = resolved->destination_parent_id,
                                .name = resolved->destination.name,
                                .record_type = source.record_type,
                                .catalog_id = source.catalog_id,
                                .bytes = hfsCatalogRecordBytes(resolved->destination_parent_id,
                                                               resolved->destination.name,
                                                               *mainPayload)});
        edit.replacements.append(HfsRawCatalogRecord{
            .parent_id = source.catalog_id,
            .name = QString(),
            .record_type = threadType,
            .catalog_id = 0,
            .bytes = hfsCatalogRecordBytes(source.catalog_id,
                                           QString(),
                                           hfsCatalogThreadRecord(threadType,
                                                                  resolved->destination_parent_id,
                                                                  resolved->destination.name))});
        if (source.parent_id != resolved->destination_parent_id) {
            edit.valence_updates.append({source.parent_id, -1});
            edit.valence_updates.append({resolved->destination_parent_id, 1});
        }
        edit.read_back_parent_id = resolved->destination_parent_id;
        edit.read_back_name = resolved->destination.name;
        const auto mutation = finishCatalogTreeMutation(&*model, edit, blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        return HfsCatalogRenameMovePlan{resolved->source_record,
                                        resolved->destination,
                                        *mutation,
                                        resolved->destination_parent_id,
                                        resolved->source_record.parent_id !=
                                            resolved->destination_parent_id};
    }

    [[nodiscard]] std::optional<QByteArray> catalogRecordPayload(const HfsRawCatalogRecord& raw,
                                                                 uint16_t recordType,
                                                                 QStringList* blockers) const {
        const qsizetype payloadSize = recordType == kHfsCatalogFileRecord
                                          ? kHfsCatalogFileRecordBytes
                                          : kHfsCatalogFolderRecordBytes;
        if (!catalogRecordTypeIsPayload(recordType)) {
            blockers->append(QStringLiteral("HFS+ rename/move source record type is unsupported"));
            return std::nullopt;
        }
        const uint16_t keyLength = be16(raw.bytes, 0);
        const qsizetype payloadOffset = kUint16Size + keyLength;
        if (!hasBytes(raw.bytes, payloadOffset, payloadSize)) {
            blockers->append(QStringLiteral("HFS+ rename/move source record payload is truncated"));
            return std::nullopt;
        }
        return raw.bytes.mid(payloadOffset, payloadSize);
    }

    [[nodiscard]] bool catalogParentChainContains(const QVector<HfsRawCatalogRecord>& records,
                                                  uint32_t startParentId,
                                                  uint32_t searchedAncestorId,
                                                  QStringList* blockers) const {
        uint32_t current = startParentId;
        QSet<uint32_t> visited;
        while (current != 0 && current != kHfsRootFolderId) {
            if (current == searchedAncestorId) {
                return true;
            }
            if (visited.contains(current)) {
                blockers->append(QStringLiteral("HFS+ folder thread parent chain loops"));
                return true;
            }
            visited.insert(current);
            const auto next = threadParentId(records, current, blockers);
            if (!next.has_value()) {
                return true;
            }
            current = *next;
        }
        return current == searchedAncestorId;
    }

    [[nodiscard]] std::optional<uint32_t> threadParentId(
        const QVector<HfsRawCatalogRecord>& records,
        uint32_t catalogId,
        QStringList* blockers) const {
        const int index = findRawCatalogRecord(records, catalogId, QString());
        if (index < 0) {
            blockers->append(QStringLiteral("HFS+ folder thread record was not found"));
            return std::nullopt;
        }
        const auto& raw = records.at(index);
        if (raw.record_type != kHfsCatalogFolderThreadRecord &&
            raw.record_type != kHfsCatalogFileThreadRecord) {
            blockers->append(QStringLiteral("HFS+ catalog thread record type is invalid"));
            return std::nullopt;
        }
        const uint16_t keyLength = be16(raw.bytes, 0);
        const qsizetype payloadOffset = kUint16Size + keyLength;
        if (!hasBytes(raw.bytes, payloadOffset + kHfsCatalogThreadParentIdOffset, kUint32Size)) {
            blockers->append(QStringLiteral("HFS+ catalog thread parent ID is unavailable"));
            return std::nullopt;
        }
        return be32(raw.bytes, payloadOffset + kHfsCatalogThreadParentIdOffset);
    }

    bool appendCatalogRenameMoveReadBack(const QString& sourcePath,
                                         const HfsCatalogRenameMovePlan& plan,
                                         PartitionHfsFileWriteResult* result) {
        const auto renamed = resolveCatalogPath(plan.destination.path);
        if (!renamed.has_value() || renamed->catalog_id != plan.source_record.catalog_id ||
            renamed->record_type != plan.source_record.record_type) {
            result->blockers.append(QStringLiteral("HFS+ rename/move read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        if (!appendCatalogLeafAfterHash(plan.mutation.insert_node_number, result)) {
            return false;
        }
        result->before_sha256 = result->before_sha256.isEmpty()
                                    ? sha256Hex(plan.mutation.before_leaf_bytes)
                                    : result->before_sha256;
        result->path = normalizedDisplayPath(plan.destination.path);
        result->bytes_written = sourcePath == plan.destination.path ? 0 : 1;
        return true;
    }

    [[nodiscard]] std::optional<HfsFolderTreeReleasePlan> folderTreeReleasePlan(
        const HfsFolderTreeDeleteSelection& selection,
        const PartitionHfsFileWriteOptions& options,
        QStringList* blockers) {
        const uint32_t releasedBlocks = extentBlockSum(selection.released_extents);
        const auto releasedBytes = bytesForReleasedBlocks(releasedBlocks, blockers);
        if (!releasedBytes.has_value()) {
            return std::nullopt;
        }
        if (releasedBlocks > 0 && !releasedBlocksWithinCap(*releasedBytes, options, blockers)) {
            return std::nullopt;
        }
        const auto allocationBytes = selection.released_extents.isEmpty()
                                         ? std::optional<QVector<HfsAllocationBitmapByte>>(
                                               QVector<HfsAllocationBitmapByte>())
                                         : prepareAllocationBitmapClear(selection.released_extents,
                                                                        blockers);
        if (!allocationBytes.has_value()) {
            return std::nullopt;
        }
        return HfsFolderTreeReleasePlan{*allocationBytes, releasedBlocks, *releasedBytes};
    }

    void appendDeleteFolderTreeBlockers(const HfsCatalogRecord& record,
                                        QStringList* blockers) const {
        if (record.catalog_id == kHfsRootFolderId) {
            blockers->append(QStringLiteral("HFS+ root folder tree delete is blocked"));
            return;
        }
        if (!record.directory()) {
            blockers->append(QStringLiteral("Selected HFS+ path is not a directory"));
        }
    }

    [[nodiscard]] std::optional<HfsFolderTreeDeleteSelection> folderTreeDeleteSelection(
        const HfsCatalogRecord& root,
        const HfsCatalogLeafMutation& leaf,
        const PartitionHfsFileWriteOptions& options,
        QStringList* blockers,
        QStringList* warnings) {
        HfsFolderTreeDeleteSelection selection;
        selection.keys_to_remove.insert(catalogKeyToken(root.parent_id, root.name));
        selection.keys_to_remove.insert(catalogKeyToken(root.catalog_id, QString()));
        selection.records.append(root);
        selection.removed_folders = 1;
        appendAttributeRecordDeleteBlocker(root.catalog_id, blockers);
        if (!blockers->isEmpty()) {
            return std::nullopt;
        }

        QSet<uint32_t> folderIds;
        folderIds.insert(root.catalog_id);
        if (!appendDescendantFolderTreeRecords(HfsFolderTreeSelectionContext{
                leaf, options, &folderIds, &selection, blockers, warnings})) {
            return std::nullopt;
        }
        if (!releaseExtentsAreSafe(selection.released_extents, blockers)) {
            return std::nullopt;
        }
        selection.removed_records =
            countRemovedFolderTreeRecords(leaf.records, selection.keys_to_remove, warnings);
        if (selection.removed_records <= 0) {
            blockers->append(QStringLiteral("HFS+ folder tree delete found no catalog records"));
            return std::nullopt;
        }
        return selection;
    }

    [[nodiscard]] bool appendDescendantFolderTreeRecords(
        const HfsFolderTreeSelectionContext& context) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& raw : context.leaf.records) {
                if (!catalogRecordTypeIsPayload(raw.record_type) ||
                    !context.folder_ids->contains(raw.parent_id) ||
                    context.selection->keys_to_remove.contains(
                        catalogKeyToken(raw.parent_id, raw.name))) {
                    continue;
                }
                const auto record = catalogRecordFromRaw(raw, context.blockers);
                if (!record.has_value() || !appendFolderTreeRecord(*record,
                                                                   context.options,
                                                                   context.folder_ids,
                                                                   context.selection,
                                                                   context.blockers)) {
                    return false;
                }
                changed = true;
            }
        }
        appendFolderTreeThreadKeys(context.selection, context.warnings);
        return true;
    }

    [[nodiscard]] bool appendFolderTreeRecord(const HfsCatalogRecord& record,
                                              const PartitionHfsFileWriteOptions& options,
                                              QSet<uint32_t>* folderIds,
                                              HfsFolderTreeDeleteSelection* selection,
                                              QStringList* blockers) {
        selection->keys_to_remove.insert(catalogKeyToken(record.parent_id, record.name));
        selection->keys_to_remove.insert(catalogKeyToken(record.catalog_id, QString()));
        selection->records.append(record);
        if (record.directory()) {
            folderIds->insert(record.catalog_id);
            ++selection->removed_folders;
            appendAttributeRecordDeleteBlocker(record.catalog_id, blockers);
            return blockers->isEmpty();
        }
        if (!record.regularFile()) {
            blockers->append(
                QStringLiteral("HFS+ folder tree delete found unsupported catalog record"));
            return false;
        }
        ++selection->removed_files;
        appendFolderTreeFileBlockers(record, options, blockers);
        if (!blockers->isEmpty()) {
            return false;
        }
        if (!recordHasNoAllocatedForks(record)) {
            const auto extents = releasableFileExtents(record, blockers);
            if (!extents.has_value()) {
                return false;
            }
            selection->released_extents.append(*extents);
        }
        return true;
    }

    void appendFolderTreeFileBlockers(const HfsCatalogRecord& record,
                                      const PartitionHfsFileWriteOptions& options,
                                      QStringList* blockers) {
        appendCompressedFileDeleteBlocker(record.catalog_id, options, blockers);
        appendAttributeRecordDeleteBlocker(record.catalog_id, blockers);
        if (!recordHasNoAllocatedForks(record)) {
            appendAllocationForkBlockers(blockers);
        }
    }

    void appendFolderTreeThreadKeys(HfsFolderTreeDeleteSelection* selection,
                                    QStringList* warnings) const {
        for (const auto& record : selection->records) {
            selection->keys_to_remove.insert(catalogKeyToken(record.catalog_id, QString()));
        }
        Q_UNUSED(warnings);
    }

    [[nodiscard]] int countRemovedFolderTreeRecords(const QVector<HfsRawCatalogRecord>& records,
                                                    const QSet<QString>& keysToRemove,
                                                    QStringList* warnings) const {
        int removed = 0;
        for (const auto& raw : records) {
            if (keysToRemove.contains(catalogKeyToken(raw.parent_id, raw.name))) {
                ++removed;
            }
        }
        if (removed < keysToRemove.size()) {
            warnings->append(
                QStringLiteral("HFS+ folder tree delete did not find every thread record"));
        }
        return removed;
    }


    void appendDeleteEmptyFolderBlockers(const HfsCatalogRecord& record, QStringList* blockers) {
        if (record.catalog_id == kHfsRootFolderId) {
            blockers->append(QStringLiteral("HFS+ root folder delete is blocked"));
            return;
        }
        if (!record.directory()) {
            blockers->append(QStringLiteral("Selected HFS+ path is not a directory"));
            return;
        }
        const auto children = childRecords(record.catalog_id, 1);
        if (!children.has_value()) {
            blockers->append(QStringLiteral("HFS+ folder children could not be verified"));
        } else if (!children->isEmpty()) {
            blockers->append(
                QStringLiteral("HFS+ folder delete currently supports only empty folders"));
        }
    }

    bool appendEmptyFileDeleteReadBack(const HfsCatalogRecord& record,
                                       uint32_t leafNode,
                                       PartitionHfsFileWriteResult* result) {
        if (findChild(record.parent_id, record.name).has_value()) {
            result->blockers.append(QStringLiteral("HFS+ empty-file delete read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        return appendCatalogLeafAfterHash(leafNode, result);
    }

    bool appendAllocatedFileDeleteReadBack(const HfsCatalogRecord& record,
                                           uint32_t leafNode,
                                           uint32_t releasedBlocks,
                                           PartitionHfsFileWriteResult* result) {
        if (findChild(record.parent_id, record.name).has_value()) {
            result->blockers.append(QStringLiteral("HFS+ allocated-file delete read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        const auto header = readVolumeHeaderAt(m_volume.volume_offset);
        if (!header.has_value() || be32(*header, kHfsFreeBlocksOffset) != m_volume.free_blocks ||
            m_volume.free_blocks < releasedBlocks) {
            result->blockers.append(QStringLiteral("HFS+ free-block counter read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        return appendCatalogLeafAfterHash(leafNode, result);
    }

    bool appendFolderTreeDeleteReadBack(const HfsCatalogRecord& record,
                                        uint32_t leafNode,
                                        uint32_t releasedBlocks,
                                        PartitionHfsFileWriteResult* result) {
        if (findChild(record.parent_id, record.name).has_value()) {
            result->blockers.append(QStringLiteral("HFS+ folder tree delete read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        if (releasedBlocks == 0) {
            return appendCatalogLeafAfterHash(leafNode, result);
        }
        const auto header = readVolumeHeaderAt(m_volume.volume_offset);
        if (!header.has_value() || be32(*header, kHfsFreeBlocksOffset) != m_volume.free_blocks ||
            m_volume.free_blocks < releasedBlocks) {
            result->blockers.append(
                QStringLiteral("HFS+ folder tree free-block counter read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        return appendCatalogLeafAfterHash(leafNode, result);
    }

    bool appendCatalogLeafAfterHash(uint32_t leafNode, PartitionHfsFileWriteResult* result) {
        const auto afterLeaf = readCatalogNode(leafNode);
        if (!afterLeaf.has_value()) {
            result->blockers.append(m_blockers);
            return false;
        }
        result->after_sha256 = sha256Hex(*afterLeaf);
        return true;
    }

    void appendDeleteEmptyFileBlockers(const HfsCatalogRecord& record,
                                       const PartitionHfsFileWriteOptions& options,
                                       QStringList* blockers) {
        if (!record.regularFile()) {
            blockers->append(QStringLiteral("Selected HFS+ path is not a regular file"));
            return;
        }
        if (!recordHasNoAllocatedForks(record)) {
            blockers->append(QStringLiteral(
                "HFS+ delete currently supports only zero-byte files with no allocated forks"));
        }
        appendCompressedFileDeleteBlocker(record.catalog_id, options, blockers);
        appendAttributeRecordDeleteBlocker(record.catalog_id, blockers);
    }

    [[nodiscard]] bool recordHasNoAllocatedForks(const HfsCatalogRecord& record) const {
        return record.data_size == 0 && record.resource_size == 0 &&
               record.data_fork.logical_size == 0 && record.resource_fork.logical_size == 0 &&
               record.data_fork.total_blocks == 0 && record.resource_fork.total_blocks == 0 &&
               record.data_fork.extents.isEmpty() && record.resource_fork.extents.isEmpty();
    }

    void appendDeleteAllocatedFileBlockers(const HfsCatalogRecord& record,
                                           const PartitionHfsFileWriteOptions& options,
                                           QStringList* blockers) {
        if (!record.regularFile()) {
            blockers->append(QStringLiteral("Selected HFS+ path is not a regular file"));
            return;
        }
        if (recordHasNoAllocatedForks(record)) {
            blockers->append(QStringLiteral(
                "HFS+ allocated-file delete requires a file with allocated fork blocks"));
        }
        appendCompressedFileDeleteBlocker(record.catalog_id, options, blockers);
        appendAttributeRecordDeleteBlocker(record.catalog_id, blockers);
        appendAllocationForkBlockers(blockers);
    }

    void appendAllocationForkBlockers(QStringList* blockers) const {
        if (m_volume.allocation_fork.logical_size == 0 ||
            m_volume.allocation_fork.total_blocks == 0 ||
            m_volume.allocation_fork.extents.isEmpty()) {
            blockers->append(QStringLiteral(
                "HFS+ allocation bitmap fork is required for allocated-file delete"));
        }
    }

    [[nodiscard]] std::optional<QVector<HfsExtent>> releasableFileExtents(
        const HfsCatalogRecord& record, QStringList* blockers) const {
        QVector<HfsExtent> extents;
        if (!appendReleasableForkExtents(
                record.data_fork, record.catalog_id, kHfsDataForkType, &extents, blockers) ||
            !appendReleasableForkExtents(record.resource_fork,
                                         record.catalog_id,
                                         kHfsResourceForkType,
                                         &extents,
                                         blockers)) {
            return std::nullopt;
        }
        if (extents.isEmpty()) {
            blockers->append(
                QStringLiteral("HFS+ allocated-file delete found no allocated extents"));
            return std::nullopt;
        }
        if (!releaseExtentsAreSafe(extents, blockers)) {
            return std::nullopt;
        }
        return extents;
    }

    [[nodiscard]] bool appendReleasableForkExtents(const HfsForkData& fork,
                                                   uint32_t fileId,
                                                   uint8_t forkType,
                                                   QVector<HfsExtent>* output,
                                                   QStringList* blockers) const {
        if (fork.total_blocks == 0) {
            return true;
        }
        const auto extents = materializedForkExtents(fork, fileId, forkType, blockers);
        if (!extents.has_value()) {
            return false;
        }
        output->append(*extents);
        return true;
    }

    [[nodiscard]] std::optional<QVector<HfsExtent>> materializedForkExtents(
        const HfsForkData& fork, uint32_t fileId, uint8_t forkType, QStringList* blockers) const {
        QVector<HfsExtent> extents;
        uint32_t coveredBlocks = appendInitialForkExtents(fork, &extents);
        if (coveredBlocks < fork.total_blocks &&
            !appendOverflowForkExtents(HfsOverflowForkExtentAppendContext{
                fork, fileId, forkType, coveredBlocks, &extents, blockers})) {
            return std::nullopt;
        }
        if (extentBlockSum(extents) != fork.total_blocks) {
            blockers->append(QStringLiteral("HFS+ fork extents do not cover all allocated blocks"));
            return std::nullopt;
        }
        return extents;
    }

    [[nodiscard]] uint32_t appendInitialForkExtents(const HfsForkData& fork,
                                                    QVector<HfsExtent>* extents) const {
        uint32_t coveredBlocks = 0;
        for (const auto& extent : fork.extents) {
            if (coveredBlocks >= fork.total_blocks || extent.block_count == 0) {
                continue;
            }
            const uint32_t blockCount = std::min<uint32_t>(extent.block_count,
                                                           fork.total_blocks - coveredBlocks);
            extents->append(HfsExtent{extent.start_block, blockCount});
            coveredBlocks += blockCount;
        }
        return coveredBlocks;
    }

    [[nodiscard]] bool appendOverflowForkExtents(
        const HfsOverflowForkExtentAppendContext& context) const {
        if (m_overflow_extents.isEmpty()) {
            context.blockers->append(
                QStringLiteral("HFS+ overflow extents are required for allocated-file delete"));
            return false;
        }
        uint32_t cursor = context.covered_blocks;
        for (const auto& record : sortedOverflowExtentsFor(context.file_id, context.fork_type)) {
            if (record.start_block != cursor) {
                continue;
            }
            for (const auto& extent : record.extents) {
                if (cursor >= context.fork.total_blocks || extent.block_count == 0) {
                    continue;
                }
                const uint32_t blockCount = std::min<uint32_t>(extent.block_count,
                                                               context.fork.total_blocks - cursor);
                context.extents->append(HfsExtent{extent.start_block, blockCount});
                cursor += blockCount;
            }
        }
        if (cursor != context.fork.total_blocks) {
            context.blockers->append(
                QStringLiteral("HFS+ allocated-file delete cannot materialize overflow extents"));
            return false;
        }
        return true;
    }

    [[nodiscard]] QVector<HfsOverflowExtentRecord> sortedOverflowExtentsFor(
        uint32_t fileId, uint8_t forkType) const {
        QVector<HfsOverflowExtentRecord> records;
        for (const auto& record : m_overflow_extents) {
            if (record.file_id == fileId && record.fork_type == forkType) {
                records.append(record);
            }
        }
        std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
            return left.start_block < right.start_block;
        });
        return records;
    }

    [[nodiscard]] uint32_t extentBlockSum(const QVector<HfsExtent>& extents) const {
        uint64_t blocks = 0;
        for (const auto& extent : extents) {
            blocks += extent.block_count;
        }
        return blocks > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                             : static_cast<uint32_t>(blocks);
    }

    [[nodiscard]] std::optional<uint64_t> bytesForReleasedBlocks(uint32_t blocks,
                                                                 QStringList* blockers) const {
        uint64_t bytes = 0;
        if (!checkedMul(blocks, m_volume.block_size, &bytes)) {
            blockers->append(QStringLiteral("HFS+ released byte count overflow"));
            return std::nullopt;
        }
        return bytes;
    }

    [[nodiscard]] bool releasedBlocksWithinCap(uint64_t releasedBytes,
                                               const PartitionHfsFileWriteOptions& options,
                                               QStringList* blockers) const {
        if (m_volume.block_size == 0) {
            blockers->append(QStringLiteral("HFS+ block size is invalid"));
            return false;
        }
        if (releasedBytes / m_volume.block_size >
            static_cast<uint64_t>(m_volume.total_blocks -
                                  std::min(m_volume.free_blocks, m_volume.total_blocks))) {
            blockers->append(
                QStringLiteral("HFS+ free-block counter cannot accept released blocks"));
            return false;
        }
        if (releasedBytes / m_volume.block_size >
            static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            blockers->append(QStringLiteral("HFS+ allocated-file delete releases too many blocks"));
            return false;
        }
        if (releasedBytes > options.max_write_bytes) {
            blockers->append(QStringLiteral(
                "HFS+ allocated-file delete released bytes exceed configured write cap"));
            return false;
        }
        if (releasedBytes == 0) {
            blockers->append(QStringLiteral("HFS+ allocated-file delete requires allocated bytes"));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool releaseExtentsAreSafe(const QVector<HfsExtent>& extents,
                                             QStringList* blockers) const {
        for (int index = 0; index < extents.size(); ++index) {
            if (!releaseExtentLooksSafe(extents.at(index), blockers) ||
                releaseExtentOverlapsEarlier(extents, index, blockers)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool releaseExtentLooksSafe(const HfsExtent& extent,
                                              QStringList* blockers) const {
        uint64_t endBlock = 0;
        if (extent.block_count == 0 ||
            !checkedAdd(extent.start_block, extent.block_count, &endBlock) ||
            endBlock > m_volume.total_blocks) {
            blockers->append(QStringLiteral("HFS+ allocated-file delete extent is out of range"));
            return false;
        }
        if (extent.start_block == 0) {
            blockers->append(QStringLiteral(
                "HFS+ allocated-file delete blocks extents that include allocation block 0"));
            return false;
        }
        if (extentOverlapsMetadata(extent)) {
            blockers->append(QStringLiteral(
                "HFS+ allocated-file delete refuses extents that overlap metadata forks"));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool releaseExtentOverlapsEarlier(const QVector<HfsExtent>& extents,
                                                    int index,
                                                    QStringList* blockers) const {
        for (int earlier = 0; earlier < index; ++earlier) {
            if (extentsOverlap(extents.at(earlier), extents.at(index))) {
                blockers->append(
                    QStringLiteral("HFS+ allocated-file delete refuses overlapping file extents"));
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool extentOverlapsMetadata(const HfsExtent& extent) const {
        return extentOverlapsFork(extent, m_volume.allocation_fork) ||
               extentOverlapsFork(extent, m_volume.extents_fork) ||
               extentOverlapsFork(extent, m_volume.catalog_fork) ||
               extentOverlapsFork(extent, m_volume.attributes_fork);
    }

    [[nodiscard]] bool extentOverlapsFork(const HfsExtent& extent, const HfsForkData& fork) const {
        for (const auto& forkExtent : fork.extents) {
            if (extentsOverlap(extent, forkExtent)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static bool extentsOverlap(const HfsExtent& left, const HfsExtent& right) {
        const uint64_t leftEnd = static_cast<uint64_t>(left.start_block) + left.block_count;
        const uint64_t rightEnd = static_cast<uint64_t>(right.start_block) + right.block_count;
        return left.block_count != 0 && right.block_count != 0 && left.start_block < rightEnd &&
               right.start_block < leftEnd;
    }

    [[nodiscard]] std::optional<int> wipeReleasedExtentsWithReadBack(
        const QVector<HfsExtent>& extents, QStringList* blockers) {
        int chunks = 0;
        for (const auto& extent : extents) {
            const auto extentChunks = wipeReleasedExtentWithReadBack(extent, blockers);
            if (!extentChunks.has_value()) {
                return std::nullopt;
            }
            chunks += *extentChunks;
        }
        return chunks;
    }

    [[nodiscard]] std::optional<int> wipeReleasedExtentsIfRequested(
        bool secureWipe, const QVector<HfsExtent>& extents, QStringList* blockers) {
        if (!secureWipe) {
            return 0;
        }
        return wipeReleasedExtentsWithReadBack(extents, blockers);
    }

    [[nodiscard]] std::optional<int> writeReleasedFreeBlockCounter(uint64_t releasedBlocks,
                                                                   QStringList* blockers) {
        if (releasedBlocks == 0) {
            return 0;
        }
        return writeVolumeHeaderCounter(
            kHfsFreeBlocksOffset, m_volume.free_blocks, static_cast<int>(releasedBlocks), blockers);
    }

    [[nodiscard]] static QString folderTreeDeleteWarning(bool secureWipe, uint64_t releasedBlocks) {
        if (secureWipe && releasedBlocks > 0) {
            return QStringLiteral(
                "HFS+ folder tree deleted after zeroing released file blocks with read-back "
                "verification");
        }
        return QStringLiteral(
            "HFS+ folder tree deleted; allocated file blocks released without secure block wiping");
    }

    [[nodiscard]] std::optional<int> wipeReleasedExtentWithReadBack(const HfsExtent& extent,
                                                                    QStringList* blockers) {
        uint64_t remaining = 0;
        if (!checkedMul(extent.block_count, m_volume.block_size, &remaining)) {
            blockers->append(QStringLiteral("HFS+ secure delete byte count overflow"));
            return std::nullopt;
        }
        uint64_t cursor = 0;
        int chunks = 0;
        while (remaining > 0) {
            const uint64_t chunkSize = std::min<uint64_t>(remaining, kHfsSecureWipeChunkBytes);
            const auto deviceOffset = deviceOffsetForReleasedExtentCursor(extent, cursor);
            if (!deviceOffset.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            const QByteArray zeros(static_cast<qsizetype>(chunkSize), '\0');
            if (!writeAt(*deviceOffset, zeros.constData(), chunkSize)) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            const auto readBack = readAt(*deviceOffset, zeros.size());
            if (!readBack.has_value() || *readBack != zeros) {
                blockers->append(QStringLiteral("HFS+ secure delete zero read-back failed"));
                blockers->append(m_blockers);
                return std::nullopt;
            }
            cursor += chunkSize;
            remaining -= chunkSize;
            ++chunks;
        }
        return chunks;
    }

    [[nodiscard]] std::optional<uint64_t> deviceOffsetForReleasedExtentCursor(
        const HfsExtent& extent, uint64_t cursor) {
        const uint64_t blockDelta = cursor / m_volume.block_size;
        const uint64_t withinBlock = cursor % m_volume.block_size;
        uint64_t block = 0;
        if (!checkedAdd(extent.start_block, blockDelta, &block)) {
            m_blockers.append(QStringLiteral("HFS+ secure delete extent offset overflow"));
            return std::nullopt;
        }
        return deviceOffsetForPhysicalBlock(block, withinBlock);
    }

    [[nodiscard]] std::optional<QVector<HfsAllocationBitmapByte>> prepareAllocationBitmapClear(
        const QVector<HfsExtent>& extents, QStringList* blockers) {
        const auto bytes = loadAllocationBitmapBytes(extents, blockers);
        if (!bytes.has_value()) {
            return std::nullopt;
        }
        auto updatedBytes = *bytes;
        if (!clearAllocationBits(extents, &updatedBytes, blockers)) {
            return std::nullopt;
        }
        return updatedBytes;
    }

    [[nodiscard]] std::optional<QVector<HfsAllocationBitmapByte>> loadAllocationBitmapBytes(
        const QVector<HfsExtent>& extents, QStringList* blockers) {
        const auto offsets = allocationBitmapByteOffsets(extents, blockers);
        if (!offsets.has_value()) {
            return std::nullopt;
        }
        QVector<HfsAllocationBitmapByte> bytes;
        bytes.reserve(offsets->size());
        for (uint64_t offset : *offsets) {
            const auto byte = readForkBytes(
                m_volume.allocation_fork, kHfsAllocationFileId, kHfsDataForkType, offset, 1);
            if (!byte.has_value() || byte->size() != 1) {
                blockers->append(QStringLiteral("Unable to read HFS+ allocation bitmap byte"));
                blockers->append(m_blockers);
                return std::nullopt;
            }
            bytes.append(HfsAllocationBitmapByte{
                offset, static_cast<quint8>(byte->at(0)), static_cast<quint8>(byte->at(0))});
        }
        return bytes;
    }

    [[nodiscard]] std::optional<QVector<uint64_t>> allocationBitmapByteOffsets(
        const QVector<HfsExtent>& extents, QStringList* blockers) const {
        QVector<uint64_t> offsets;
        for (const auto& extent : extents) {
            if (!appendAllocationBitmapByteOffsets(extent, &offsets, blockers)) {
                return std::nullopt;
            }
        }
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
        return offsets;
    }

    [[nodiscard]] bool appendAllocationBitmapByteOffsets(const HfsExtent& extent,
                                                         QVector<uint64_t>* offsets,
                                                         QStringList* blockers) const {
        for (uint32_t block = 0; block < extent.block_count; ++block) {
            const uint64_t allocationBlock = static_cast<uint64_t>(extent.start_block) + block;
            const uint64_t byteOffset = allocationBlock / 8U;
            if (byteOffset >= m_volume.allocation_fork.logical_size) {
                blockers->append(
                    QStringLiteral("HFS+ allocation bitmap does not cover file extent"));
                return false;
            }
            offsets->append(byteOffset);
        }
        return true;
    }

    [[nodiscard]] bool clearAllocationBits(const QVector<HfsExtent>& extents,
                                           QVector<HfsAllocationBitmapByte>* bytes,
                                           QStringList* blockers) const {
        for (const auto& extent : extents) {
            if (!clearAllocationExtentBits(extent, bytes, blockers)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool clearAllocationExtentBits(const HfsExtent& extent,
                                                 QVector<HfsAllocationBitmapByte>* bytes,
                                                 QStringList* blockers) const {
        for (uint32_t block = 0; block < extent.block_count; ++block) {
            const uint64_t allocationBlock = static_cast<uint64_t>(extent.start_block) + block;
            if (!clearAllocationBlockBit(allocationBlock, bytes, blockers)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool clearAllocationBlockBit(uint64_t allocationBlock,
                                               QVector<HfsAllocationBitmapByte>* bytes,
                                               QStringList* blockers) const {
        const uint64_t byteOffset = allocationBlock / 8U;
        const quint8 mask = static_cast<quint8>(0x80U >> (allocationBlock % 8U));
        auto* byte = allocationBitmapByte(bytes, byteOffset);
        if (!byte) {
            blockers->append(QStringLiteral("HFS+ allocation bitmap byte was not staged"));
            return false;
        }
        if ((byte->updated_value & mask) == 0) {
            blockers->append(
                QStringLiteral("HFS+ allocation bitmap did not mark file block allocated"));
            return false;
        }
        byte->updated_value = static_cast<quint8>(byte->updated_value & ~mask);
        return true;
    }

    [[nodiscard]] HfsAllocationBitmapByte* allocationBitmapByte(
        QVector<HfsAllocationBitmapByte>* bytes, uint64_t byteOffset) const {
        auto it = std::lower_bound(bytes->begin(),
                                   bytes->end(),
                                   byteOffset,
                                   [](const HfsAllocationBitmapByte& byte, uint64_t offset) {
                                       return byte.fork_offset < offset;
                                   });
        return it != bytes->end() && it->fork_offset == byteOffset ? &(*it) : nullptr;
    }

    [[nodiscard]] std::optional<int> writeUpdatedAllocationBitmapBytes(
        const QVector<HfsAllocationBitmapByte>& bytes) {
        int chunks = 0;
        for (const auto& byte : bytes) {
            if (byte.original_value == byte.updated_value) {
                continue;
            }
            const QByteArray payload(1, static_cast<char>(byte.updated_value));
            const auto written = writeForkBytes(m_volume.allocation_fork,
                                                kHfsAllocationFileId,
                                                kHfsDataForkType,
                                                byte.fork_offset,
                                                payload);
            if (!written.has_value()) {
                return std::nullopt;
            }
            chunks += *written;
        }
        return chunks;
    }

    void appendCompressedFileDeleteBlocker(uint32_t catalogId,
                                           const PartitionHfsFileWriteOptions& options,
                                           QStringList* blockers) {
        if (options.allow_compressed_file_mutation) {
            return;
        }
        if (fileHasAttribute(catalogId, QStringLiteral("com.apple.decmpfs"))) {
            blockers->append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
    }

    void appendAttributeRecordDeleteBlocker(uint32_t catalogId, QStringList* blockers) {
        if (fileHasAnyAttribute(catalogId)) {
            blockers->append(QStringLiteral("HFS+ delete blocks files with attribute records"));
        }
    }

    [[nodiscard]] std::optional<QVector<HfsRawCatalogRecord>> rawCatalogRecordsFromLeaf(
        const QByteArray& node, uint32_t nodeNumber) {
        const uint16_t numRecords = be16(node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(node, numRecords);
        if (!offsets.has_value()) {
            return std::nullopt;
        }
        QVector<HfsRawCatalogRecord> records;
        records.reserve(offsets->size());
        for (int index = 0; index < offsets->size(); ++index) {
            const qsizetype start = offsets->at(index);
            const qsizetype end = recordEnd(node, *offsets, index);
            if (end <= start || end > node.size()) {
                m_blockers.append(QStringLiteral("Invalid HFS+ catalog record length"));
                return std::nullopt;
            }
            const auto parsed = parseCatalogRecord(node, start, end, nodeNumber);
            if (!parsed.has_value()) {
                m_blockers.append(QStringLiteral("Invalid HFS+ catalog record"));
                return std::nullopt;
            }
            records.append(HfsRawCatalogRecord{.parent_id = parsed->parent_id,
                                               .name = parsed->name,
                                               .record_type = parsed->record_type,
                                               .catalog_id = parsed->catalog_id,
                                               .bytes = node.mid(start, end - start)});
        }
        return records;
    }

    [[nodiscard]] std::optional<HfsCatalogRecord> catalogRecordFromRaw(
        const HfsRawCatalogRecord& raw, QStringList* blockers) const {
        if (raw.bytes.size() < kUint16Size) {
            blockers->append(QStringLiteral("Invalid HFS+ raw catalog record"));
            return std::nullopt;
        }
        const uint16_t keyLength = be16(raw.bytes, 0);
        const qsizetype dataOffset = kUint16Size + keyLength;
        if (!hasBytes(raw.bytes, dataOffset, kUint16Size)) {
            blockers->append(QStringLiteral("Invalid HFS+ raw catalog record payload"));
            return std::nullopt;
        }
        const auto parsed = parseCatalogDataRecord(raw.bytes,
                                                   HfsCatalogKeyInfo{.parent_id = raw.parent_id,
                                                                     .name = raw.name,
                                                                     .data_offset = dataOffset,
                                                                     .catalog_data_offset = 0});
        if (!parsed.has_value()) {
            blockers->append(QStringLiteral("Unsupported HFS+ catalog record in folder tree"));
            return std::nullopt;
        }
        return parsed;
    }

    [[nodiscard]] static QString catalogKeyToken(uint32_t parentId, const QString& name) {
        return QStringLiteral("%1\n%2").arg(parentId).arg(name);
    }

    [[nodiscard]] int findRawCatalogRecord(const QVector<HfsRawCatalogRecord>& records,
                                           uint32_t parentId,
                                           const QString& name) const {
        for (int index = 0; index < records.size(); ++index) {
            const auto& record = records.at(index);
            if (catalogKeysEqual(
                    record.parent_id, record.name, parentId, name, m_catalog.key_compare_type)) {
                return index;
            }
        }
        return -1;
    }

    void updateParentFolderValence(QVector<HfsRawCatalogRecord>* records,
                                   uint32_t parentId,
                                   int delta,
                                   QStringList* warnings) const {
        if (!records) {
            return;
        }
        for (auto& record : *records) {
            if (record.record_type != kHfsCatalogFolderRecord || record.catalog_id != parentId) {
                continue;
            }
            const uint16_t keyLength = be16(record.bytes, 0);
            const qsizetype dataOffset = kUint16Size + keyLength;
            const qsizetype valenceOffset = dataOffset + kHfsCatalogFolderValenceOffset;
            if (!hasBytes(record.bytes, valenceOffset, kUint32Size)) {
                warnings->append(
                    QStringLiteral("HFS+ parent folder valence field was unavailable"));
                return;
            }
            const uint32_t oldValence = be32(record.bytes, valenceOffset);
            const uint32_t newValence =
                delta < 0
                    ? oldValence - std::min<uint32_t>(oldValence, static_cast<uint32_t>(-delta))
                    : oldValence + static_cast<uint32_t>(delta);
            writeBe32(&record.bytes, valenceOffset, newValence);
            return;
        }
        warnings->append(
            QStringLiteral("HFS+ parent folder record was not present for valence update"));
    }

    [[nodiscard]] uint32_t nextSafeCatalogId(const QVector<HfsRawCatalogRecord>& records,
                                             QStringList* blockers) const {
        uint32_t maxCatalogId = kHfsRootFolderId;
        for (const auto& record : records) {
            maxCatalogId = std::max(maxCatalogId, record.catalog_id);
        }
        uint32_t nextId = std::max(m_volume.next_catalog_id,
                                   std::max(kHfsMinimumUserCatalogId, maxCatalogId + 1));
        if (nextId <= maxCatalogId || nextId == 0 ||
            nextId == std::numeric_limits<uint32_t>::max()) {
            blockers->append(QStringLiteral("HFS+ next catalog ID is unavailable"));
            return 0;
        }
        return nextId;
    }

    void sortRawCatalogRecords(QVector<HfsRawCatalogRecord>* records) const {
        std::sort(records->begin(),
                  records->end(),
                  [&](const HfsRawCatalogRecord& left, const HfsRawCatalogRecord& right) {
                      return compareCatalogKeys(left.parent_id,
                                                left.name,
                                                right.parent_id,
                                                right.name,
                                                m_catalog.key_compare_type) < 0;
                  });
    }

    [[nodiscard]] std::optional<HfsBTreeHeaderNodeContext> parseBTreeHeaderNodeForMutation(
        const QByteArray& node, const QString& label, QStringList* blockers) {
        if (static_cast<int8_t>(node.at(kBTreeKindOffset)) != kBTreeHeaderNode) {
            blockers->append(QStringLiteral("HFS+ %1 header node is invalid").arg(label));
            return std::nullopt;
        }
        const uint16_t numRecords = be16(node, kBTreeNumRecordsOffset);
        if (numRecords < kBTreeHeaderNodeRecordCount) {
            blockers->append(
                QStringLiteral("HFS+ %1 header node record table is invalid").arg(label));
            return std::nullopt;
        }
        const auto offsets = recordOffsets(node, numRecords);
        if (!offsets.has_value() || offsets->size() <= kBTreeHeaderMapRecordIndex) {
            blockers->append(m_blockers);
            blockers->append(QStringLiteral("HFS+ %1 header node offsets are invalid").arg(label));
            return std::nullopt;
        }
        const qsizetype mapOffset = offsets->at(kBTreeHeaderMapRecordIndex);
        const qsizetype mapEnd = recordEnd(node, *offsets, kBTreeHeaderMapRecordIndex);
        if (mapEnd <= mapOffset || mapEnd > node.size()) {
            blockers->append(QStringLiteral("HFS+ %1 header map record is invalid").arg(label));
            return std::nullopt;
        }
        return HfsBTreeHeaderNodeContext{.node = node, .map_offset = mapOffset, .map_end = mapEnd};
    }

    [[nodiscard]] std::optional<HfsBTreeHeaderNodeContext> loadCatalogHeaderNodeForMutation(
        QStringList* blockers) {
        const auto node = readCatalogNode(0);
        if (!node.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return parseBTreeHeaderNodeForMutation(*node, QStringLiteral("catalog"), blockers);
    }

    [[nodiscard]] std::optional<HfsBTreeHeaderNodeContext> loadExtentsHeaderNodeForMutation(
        QStringList* blockers) {
        const auto node = readExtentsNode(0);
        if (!node.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return parseBTreeHeaderNodeForMutation(*node, QStringLiteral("extents overflow"), blockers);
    }

    [[nodiscard]] static bool headerMapBitSet(const HfsBTreeHeaderNodeContext& header,
                                              uint32_t nodeNumber) {
        const qsizetype byteOffset = header.map_offset +
                                     static_cast<qsizetype>(nodeNumber / kBitsPerByte);
        if (byteOffset >= header.map_end) {
            return false;
        }
        const auto value = static_cast<quint8>(header.node.at(byteOffset));
        const auto mask = static_cast<quint8>(0x80U >> (nodeNumber % kBitsPerByte));
        return (value & mask) != 0;
    }

    static void writeHeaderMapBit(HfsBTreeHeaderNodeContext* header,
                                  uint32_t nodeNumber,
                                  bool set) {
        const qsizetype byteOffset = header->map_offset +
                                     static_cast<qsizetype>(nodeNumber / kBitsPerByte);
        if (byteOffset >= header->map_end) {
            return;
        }
        const auto value = static_cast<quint8>(header->node.at(byteOffset));
        const auto mask = static_cast<quint8>(0x80U >> (nodeNumber % kBitsPerByte));
        header->node[byteOffset] = static_cast<char>(set ? (value | mask)
                                                         : (value & static_cast<quint8>(~mask)));
    }

    struct HfsBTreeNodeAllocationRequest {
        const HfsBTreeHeaderNodeContext* header{nullptr};
        const HfsBTreeHeader* tree{nullptr};
        QVector<uint32_t> must_be_allocated;
        int count{0};
        QString label;
    };

    [[nodiscard]] std::optional<QVector<uint32_t>> allocateBTreeNodesFromHeaderMap(
        const HfsBTreeNodeAllocationRequest& request, QStringList* blockers) const {
        const HfsBTreeHeaderNodeContext& header = *request.header;
        const HfsBTreeHeader& tree = *request.tree;
        const uint64_t mapCapacityBits = static_cast<uint64_t>(header.map_end - header.map_offset) *
                                         kBitsPerByte;
        if (static_cast<uint64_t>(tree.total_nodes) > mapCapacityBits) {
            blockers->append(
                QStringLiteral("HFS+ %1 B-tree node map spans multiple map nodes; split is blocked")
                    .arg(request.label));
            return std::nullopt;
        }
        if (tree.free_nodes < static_cast<uint32_t>(request.count)) {
            blockers->append(
                QStringLiteral("HFS+ %1 B-tree does not have enough free nodes for a split")
                    .arg(request.label));
            return std::nullopt;
        }
        for (uint32_t nodeNumber : request.must_be_allocated) {
            if (!headerMapBitSet(header, nodeNumber)) {
                blockers->append(
                    QStringLiteral("HFS+ %1 B-tree node map is inconsistent with allocated nodes")
                        .arg(request.label));
                return std::nullopt;
            }
        }
        QVector<uint32_t> allocated;
        allocated.reserve(request.count);
        for (uint32_t nodeNumber = 1;
             nodeNumber < tree.total_nodes && allocated.size() < request.count;
             ++nodeNumber) {
            if (!headerMapBitSet(header, nodeNumber)) {
                allocated.append(nodeNumber);
            }
        }
        if (allocated.size() < request.count) {
            blockers->append(
                QStringLiteral("HFS+ %1 B-tree node map is inconsistent with the free-node counter")
                    .arg(request.label));
            return std::nullopt;
        }
        return allocated;
    }

    [[nodiscard]] static std::optional<QByteArray> rawRecordKeyBytes(
        const QByteArray& recordBytes) {
        const uint16_t keyLength = be16(recordBytes, 0);
        const qsizetype total = kUint16Size + keyLength;
        if (keyLength == 0 || !hasBytes(recordBytes, 0, total)) {
            return std::nullopt;
        }
        QByteArray key = recordBytes.left(total);
        if ((key.size() % kHfsRecordAlignmentBytes) != 0) {
            key.append('\0');
        }
        return key;
    }

    [[nodiscard]] std::optional<QByteArray> buildCatalogLeafNodeBytes(
        uint32_t forwardLink,
        uint32_t backwardLink,
        const QVector<HfsRawCatalogRecord>& records) const {
        QByteArray node(m_catalog.node_size, '\0');
        writeBe32(&node, kBTreeNodeForwardLinkOffset, forwardLink);
        writeBe32(&node, kBTreeNodeBackwardLinkOffset, backwardLink);
        node[kBTreeKindOffset] = static_cast<char>(kBTreeLeafNode);
        node[kBTreeHeightOffset] = static_cast<char>(kHfsLeafNodeHeight);
        writeBe16(&node, kBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));
        QVector<qsizetype> offsets;
        offsets.reserve(records.size());
        qsizetype cursor = kBTreeNodeDescriptorBytes;
        for (const auto& record : records) {
            if (!appendCatalogRecordToNode(&node, record.bytes, &offsets, &cursor)) {
                return std::nullopt;
            }
        }
        writeCatalogNodeOffsets(&node, offsets, cursor);
        return node;
    }

    [[nodiscard]] static std::optional<QByteArray> buildIndexNodeBytes(
        uint16_t nodeSize,
        const QVector<QPair<QByteArray, uint32_t>>& children,
        uint8_t height,
        const QString& label,
        QStringList* blockers) {
        QByteArray node(nodeSize, '\0');
        node[kBTreeKindOffset] = static_cast<char>(kBTreeIndexNode);
        node[kBTreeHeightOffset] = static_cast<char>(height);
        writeBe16(&node, kBTreeNumRecordsOffset, static_cast<uint16_t>(children.size()));
        QVector<qsizetype> offsets;
        offsets.reserve(children.size());
        qsizetype cursor = kBTreeNodeDescriptorBytes;
        for (const auto& child : children) {
            QByteArray record = child.first;
            record.append(be32Bytes(child.second));
            if (!appendCatalogRecordToNode(&node, record, &offsets, &cursor)) {
                blockers->append(
                    QStringLiteral("HFS+ %1 index node lacks record space").arg(label));
                return std::nullopt;
            }
        }
        writeCatalogNodeOffsets(&node, offsets, cursor);
        return node;
    }

    [[nodiscard]] std::optional<int> catalogRootLeafSplitIndex(
        const QVector<HfsRawCatalogRecord>& records, QStringList* blockers) const {
        if (records.size() < kHfsRootLeafSplitMinimumRecords) {
            blockers->append(
                QStringLiteral("HFS+ catalog root-leaf split needs at least two records"));
            return std::nullopt;
        }
        qsizetype total = 0;
        for (const auto& record : records) {
            total += record.bytes.size() + kUint16Size;
        }
        qsizetype left = 0;
        int split = -1;
        for (int index = 0; index < records.size() - 1; ++index) {
            left += records.at(index).bytes.size() + kUint16Size;
            if (left * kHfsRootLeafSplitHalves >= total) {
                split = index + 1;
                break;
            }
        }
        if (split < 1) {
            split = records.size() / kHfsRootLeafSplitHalves;
        }
        return std::max(1, std::min(split, static_cast<int>(records.size()) - 1));
    }


    // ---- Unified HFS+ catalog tree mutation engine (one- and two-level trees) ----

    [[nodiscard]] bool catalogLeafRecordsFit(const QVector<HfsRawCatalogRecord>& records) const {
        qsizetype total = kBTreeNodeDescriptorBytes;
        for (const auto& record : records) {
            total += record.bytes.size();
        }
        return records.size() <= std::numeric_limits<uint16_t>::max() &&
               total + (records.size() + 1) * kUint16Size <=
                   static_cast<qsizetype>(m_catalog.node_size);
    }

    [[nodiscard]] std::optional<QVector<QPair<QByteArray, uint32_t>>> parseCatalogIndexEntries(
        const QByteArray& node, QStringList* blockers) {
        const uint16_t numRecords = be16(node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(node, numRecords);
        if (!offsets.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        QVector<QPair<QByteArray, uint32_t>> entries;
        entries.reserve(offsets->size());
        for (int index = 0; index < offsets->size(); ++index) {
            const qsizetype start = offsets->at(index);
            const qsizetype end = recordEnd(node, *offsets, index);
            const uint16_t keyLength = be16(node, start);
            qsizetype keyTotal = kUint16Size + keyLength;
            if ((keyTotal % kHfsRecordAlignmentBytes) != 0) {
                ++keyTotal;
            }
            if (end <= start || !hasBytes(node, start + keyTotal, kUint32Size)) {
                blockers->append(QStringLiteral("Invalid HFS+ catalog index record"));
                return std::nullopt;
            }
            entries.append({node.mid(start, keyTotal), be32(node, start + keyTotal)});
        }
        return entries;
    }

    [[nodiscard]] bool loadCatalogModelLeaf(uint32_t nodeNumber,
                                            HfsCatalogTreeModel* model,
                                            QStringList* blockers) {
        const auto node = readCatalogNode(nodeNumber);
        if (!node.has_value()) {
            blockers->append(m_blockers);
            return false;
        }
        if (static_cast<int8_t>(node->at(kBTreeKindOffset)) != kBTreeLeafNode ||
            static_cast<uint8_t>(node->at(kBTreeHeightOffset)) != kHfsLeafNodeHeight) {
            blockers->append(QStringLiteral("Invalid HFS+ catalog leaf node"));
            return false;
        }
        const auto records = rawCatalogRecordsFromLeaf(*node, nodeNumber);
        if (!records.has_value()) {
            blockers->append(m_blockers);
            return false;
        }
        if (!headerMapBitSet(model->header, nodeNumber)) {
            blockers->append(QStringLiteral(
                "HFS+ catalog B-tree node map is inconsistent with allocated nodes"));
            return false;
        }
        model->leaves.append(HfsCatalogWorkingLeaf{.node_number = nodeNumber, .records = *records});
        return true;
    }

    [[nodiscard]] bool validateCatalogModelMap(const HfsCatalogTreeModel& model,
                                               QStringList* blockers) const {
        const uint64_t mapCapacityBits =
            static_cast<uint64_t>(model.header.map_end - model.header.map_offset) * kBitsPerByte;
        if (static_cast<uint64_t>(m_catalog.total_nodes) > mapCapacityBits) {
            blockers->append(QStringLiteral(
                "HFS+ catalog B-tree node map spans multiple map nodes; mutation is blocked"));
            return false;
        }
        if (!headerMapBitSet(model.header, 0) ||
            !headerMapBitSet(model.header, m_catalog.root_node)) {
            blockers->append(QStringLiteral(
                "HFS+ catalog B-tree node map is inconsistent with allocated nodes"));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<QVector<QPair<QByteArray, uint32_t>>>
    readCatalogIndexEntriesChecked(uint32_t nodeNumber,
                                   uint8_t expectedHeight,
                                   const HfsCatalogTreeModel& model,
                                   QStringList* blockers) {
        const auto indexNode = readCatalogNode(nodeNumber);
        if (!indexNode.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        if (static_cast<int8_t>(indexNode->at(kBTreeKindOffset)) != kBTreeIndexNode ||
            static_cast<uint8_t>(indexNode->at(kBTreeHeightOffset)) != expectedHeight) {
            blockers->append(QStringLiteral("Invalid HFS+ catalog index node"));
            return std::nullopt;
        }
        if (!headerMapBitSet(model.header, nodeNumber)) {
            blockers->append(QStringLiteral(
                "HFS+ catalog B-tree node map is inconsistent with allocated nodes"));
            return std::nullopt;
        }
        auto entries = parseCatalogIndexEntries(*indexNode, blockers);
        if (entries.has_value() && entries->isEmpty()) {
            blockers->append(QStringLiteral("Invalid HFS+ catalog index node"));
            return std::nullopt;
        }
        return entries;
    }

    [[nodiscard]] bool loadCatalogModelIndexLevel(uint32_t nodeNumber,
                                                  uint8_t expectedHeight,
                                                  HfsCatalogTreeModel* model,
                                                  QStringList* blockers) {
        const auto entries =
            readCatalogIndexEntriesChecked(nodeNumber, expectedHeight, *model, blockers);
        if (!entries.has_value()) {
            return false;
        }
        model->old_index_nodes.append(nodeNumber);
        for (const auto& entry : *entries) {
            if (model->leaves.size() > kHfsMaxMutationLeaves) {
                blockers->append(
                    QStringLiteral("HFS+ catalog mutation supports at most %1 leaf nodes")
                        .arg(kHfsMaxMutationLeaves));
                return false;
            }
            const bool childOk =
                expectedHeight == kHfsRootIndexNodeHeight
                    ? loadCatalogModelLeaf(entry.second, model, blockers)
                    : loadCatalogModelIndexLevel(entry.second, expectedHeight - 1, model, blockers);
            if (!childOk) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<HfsCatalogTreeModel> loadCatalogTreeForMutation(
        QStringList* blockers) {
        if (m_catalog.tree_depth == 0 || m_catalog.tree_depth > kHfsMaxMutationTreeDepth) {
            blockers->append(
                QStringLiteral("HFS+ catalog mutation supports B-trees up to %1 levels deep")
                    .arg(kHfsMaxMutationTreeDepth));
            return std::nullopt;
        }
        auto header = loadCatalogHeaderNodeForMutation(blockers);
        if (!header.has_value()) {
            return std::nullopt;
        }
        HfsCatalogTreeModel model;
        model.header = *header;
        model.working_free_nodes = static_cast<int>(m_catalog.free_nodes);
        if (!validateCatalogModelMap(model, blockers)) {
            return std::nullopt;
        }
        if (!loadCatalogModelLevels(&model, blockers)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] bool loadCatalogModelLevels(HfsCatalogTreeModel* model, QStringList* blockers) {
        if (m_catalog.tree_depth == 1) {
            if (m_catalog.root_node != m_catalog.first_leaf_node ||
                m_catalog.first_leaf_node != m_catalog.last_leaf_node) {
                blockers->append(QStringLiteral("Invalid HFS+ catalog B-tree geometry"));
                return false;
            }
            return loadCatalogModelLeaf(m_catalog.root_node, model, blockers);
        }
        if (!loadCatalogModelIndexLevel(
                m_catalog.root_node, static_cast<uint8_t>(m_catalog.tree_depth), model, blockers)) {
            return false;
        }
        if (model->leaves.isEmpty() || model->leaves.size() > kHfsMaxMutationLeaves ||
            model->leaves.first().node_number != m_catalog.first_leaf_node ||
            model->leaves.last().node_number != m_catalog.last_leaf_node) {
            blockers->append(QStringLiteral("Invalid HFS+ catalog B-tree geometry"));
            return false;
        }
        return true;
    }

    [[nodiscard]] QVector<HfsRawCatalogRecord> flattenedCatalogModelRecords(
        const HfsCatalogTreeModel& model) const {
        QVector<HfsRawCatalogRecord> records;
        for (const auto& leaf : model.leaves) {
            records.append(leaf.records);
        }
        return records;
    }

    [[nodiscard]] int catalogModelLeafForKey(const HfsCatalogTreeModel& model,
                                             uint32_t parentId,
                                             const QString& name) const {
        int target = 0;
        for (int index = 1; index < model.leaves.size(); ++index) {
            const auto& records = model.leaves.at(index).records;
            if (records.isEmpty()) {
                continue;
            }
            if (compareCatalogKeys(records.first().parent_id,
                                   records.first().name,
                                   parentId,
                                   name,
                                   m_catalog.key_compare_type) <= 0) {
                target = index;
            }
        }
        return target;
    }

    [[nodiscard]] int catalogModelLeafContainingKey(const HfsCatalogTreeModel& model,
                                                    uint32_t parentId,
                                                    const QString& name) const {
        for (int index = 0; index < model.leaves.size(); ++index) {
            if (findRawCatalogRecord(model.leaves.at(index).records, parentId, name) >= 0) {
                return index;
            }
        }
        return -1;
    }

    struct HfsCatalogKeyRemoval {
        uint32_t parent_id{0};
        QString name;
        bool required{false};
    };

    [[nodiscard]] bool removeCatalogModelKey(HfsCatalogTreeModel* model,
                                             const HfsCatalogKeyRemoval& removal,
                                             int* removedCount,
                                             QStringList* warnings,
                                             QStringList* blockers) const {
        const int leafIndex =
            catalogModelLeafContainingKey(*model, removal.parent_id, removal.name);
        if (leafIndex < 0) {
            if (removal.required) {
                blockers->append(QStringLiteral("HFS+ catalog record for delete was not found"));
                return false;
            }
            warnings->append(QStringLiteral("HFS+ file thread record was not present"));
            return true;
        }
        auto& leaf = (*model).leaves[leafIndex];
        const int recordIndex = findRawCatalogRecord(leaf.records, removal.parent_id, removal.name);
        leaf.records.removeAt(recordIndex);
        leaf.dirty = true;
        ++(*removedCount);
        return true;
    }

    [[nodiscard]] bool applyCatalogModelValence(HfsCatalogTreeModel* model,
                                                const HfsCatalogValenceUpdate& update,
                                                QStringList* warnings) const {
        for (auto& leaf : model->leaves) {
            for (const auto& record : leaf.records) {
                if (record.record_type == kHfsCatalogFolderRecord &&
                    record.catalog_id == update.folder_id) {
                    QStringList localWarnings;
                    updateParentFolderValence(
                        &leaf.records, update.folder_id, update.delta, &localWarnings);
                    warnings->append(localWarnings);
                    leaf.dirty = true;
                    return true;
                }
            }
        }
        warnings->append(
            QStringLiteral("HFS+ parent folder record was not present for valence update"));
        return true;
    }

    void applyCatalogModelTokenRemovals(HfsCatalogTreeModel* model,
                                        const QSet<QString>& removalTokens,
                                        int* removedCount) const {
        for (auto& leaf : model->leaves) {
            for (int index = leaf.records.size() - 1; index >= 0; --index) {
                const auto& record = leaf.records.at(index);
                if (removalTokens.contains(catalogKeyToken(record.parent_id, record.name))) {
                    leaf.records.removeAt(index);
                    leaf.dirty = true;
                    ++(*removedCount);
                }
            }
        }
    }

    [[nodiscard]] bool applyCatalogModelReplacements(
        HfsCatalogTreeModel* model,
        const QVector<HfsRawCatalogRecord>& replacements,
        QStringList* blockers) const {
        for (const auto& replacement : replacements) {
            const int leafIndex =
                catalogModelLeafContainingKey(*model, replacement.parent_id, replacement.name);
            if (leafIndex < 0) {
                blockers->append(QStringLiteral("HFS+ catalog record for update was not found"));
                return false;
            }
            auto& leaf = (*model).leaves[leafIndex];
            const int recordIndex =
                findRawCatalogRecord(leaf.records, replacement.parent_id, replacement.name);
            leaf.records[recordIndex] = replacement;
            leaf.dirty = true;
        }
        return true;
    }

    [[nodiscard]] bool applyCatalogModelInsertions(HfsCatalogTreeModel* model,
                                                   const QVector<HfsRawCatalogRecord>& insertions,
                                                   QStringList* blockers) const {
        for (const auto& insertion : insertions) {
            if (catalogModelLeafContainingKey(*model, insertion.parent_id, insertion.name) >= 0) {
                blockers->append(QStringLiteral("HFS+ catalog entry already exists"));
                return false;
            }
            const int leafIndex =
                catalogModelLeafForKey(*model, insertion.parent_id, insertion.name);
            auto& leaf = (*model).leaves[leafIndex];
            leaf.records.append(insertion);
            sortRawCatalogRecords(&leaf.records);
            leaf.dirty = true;
        }
        return true;
    }

    [[nodiscard]] bool applyCatalogEditToModel(HfsCatalogTreeModel* model,
                                               const HfsCatalogTreeEdit& edit,
                                               int* removedCount,
                                               QStringList* warnings,
                                               QStringList* blockers) const {
        for (const auto& removal : edit.removals) {
            if (!removeCatalogModelKey(model,
                                       {removal.first, removal.second, true},
                                       removedCount,
                                       warnings,
                                       blockers)) {
                return false;
            }
        }
        for (const auto& removal : edit.optional_removals) {
            if (!removeCatalogModelKey(model,
                                       {removal.first, removal.second, false},
                                       removedCount,
                                       warnings,
                                       blockers)) {
                return false;
            }
        }
        applyCatalogModelTokenRemovals(model, edit.removal_tokens, removedCount);
        if (!applyCatalogModelReplacements(model, edit.replacements, blockers)) {
            return false;
        }
        for (const auto& update : edit.valence_updates) {
            if (!applyCatalogModelValence(model, update, warnings)) {
                return false;
            }
        }
        return applyCatalogModelInsertions(model, edit.insertions, blockers);
    }

    [[nodiscard]] std::optional<uint32_t> allocateCatalogModelNode(HfsCatalogTreeModel* model,
                                                                   QStringList* blockers) const {
        if (model->working_free_nodes < 1) {
            blockers->append(
                QStringLiteral("HFS+ catalog B-tree does not have enough free nodes for a split"));
            return std::nullopt;
        }
        for (uint32_t nodeNumber = 1; nodeNumber < m_catalog.total_nodes; ++nodeNumber) {
            if (!headerMapBitSet(model->header, nodeNumber) &&
                !model->freed_nodes.contains(nodeNumber)) {
                writeHeaderMapBit(&model->header, nodeNumber, true);
                --model->working_free_nodes;
                return nodeNumber;
            }
        }
        // Nodes freed within this mutation are reserved until the post-commit
        // erase, so a nonzero counter can still mean no node is available.
        blockers->append(
            model->freed_nodes.isEmpty()
                ? QStringLiteral(
                      "HFS+ catalog B-tree node map is inconsistent with the free-node counter")
                : QStringLiteral(
                      "HFS+ catalog B-tree does not have enough free nodes for this mutation"));
        return std::nullopt;
    }

    void freeCatalogModelNode(HfsCatalogTreeModel* model, uint32_t nodeNumber) {
        writeHeaderMapBit(&model->header, nodeNumber, false);
        ++model->working_free_nodes;
        model->freed_nodes.append(nodeNumber);
    }

    [[nodiscard]] bool splitCatalogModelLeaf(HfsCatalogTreeModel* model,
                                             int leafIndex,
                                             QStringList* blockers) {
        auto& leaf = (*model).leaves[leafIndex];
        if (model->old_index_nodes.isEmpty() && !leaf.is_new) {
            // Splitting the depth-1 root leaf: move the surviving left half to a
            // fresh node so the old root leaf stays intact until the header-node
            // commit write switches the tree to the new root index node.
            if ((m_catalog.attributes_mask & kBTreeBigKeysMask) == 0 ||
                (m_catalog.attributes_mask & kBTreeVariableIndexKeysMask) == 0) {
                blockers->append(QStringLiteral(
                    "HFS+ catalog B-tree split requires big keys and variable-length index keys"));
                return false;
            }
            const auto newLeft = allocateCatalogModelNode(model, blockers);
            if (!newLeft.has_value()) {
                return false;
            }
            freeCatalogModelNode(model, leaf.node_number);
            leaf.node_number = *newLeft;
            leaf.is_new = true;
        }
        const auto splitIndex = catalogRootLeafSplitIndex(leaf.records, blockers);
        if (!splitIndex.has_value()) {
            return false;
        }
        const auto rightNode = allocateCatalogModelNode(model, blockers);
        if (!rightNode.has_value()) {
            return false;
        }
        HfsCatalogWorkingLeaf right;
        right.node_number = *rightNode;
        right.records = leaf.records.mid(*splitIndex);
        right.dirty = true;
        right.is_new = true;
        (*model).leaves[leafIndex].records = (*model).leaves[leafIndex].records.mid(0, *splitIndex);
        (*model).leaves[leafIndex].dirty = true;
        model->leaves.insert(leafIndex + 1, right);
        return true;
    }

    [[nodiscard]] bool rebalanceCatalogModel(HfsCatalogTreeModel* model, QStringList* blockers) {
        for (int index = 0; index < model->leaves.size(); ++index) {
            int guard = 0;
            while (!catalogLeafRecordsFit(model->leaves.at(index).records)) {
                if (++guard > kHfsMaxMutationLeaves) {
                    blockers->append(QStringLiteral("HFS+ catalog leaf split limit reached"));
                    return false;
                }
                if (!splitCatalogModelLeaf(model, index, blockers)) {
                    return false;
                }
            }
        }
        for (int index = model->leaves.size() - 1; index >= 0; --index) {
            if (!model->leaves.at(index).records.isEmpty()) {
                continue;
            }
            if (model->leaves.size() == 1) {
                // A one-level catalog keeps its (now empty) root leaf in place.
                break;
            }
            if (!model->leaves.at(index).is_new) {
                freeCatalogModelNode(model, model->leaves.at(index).node_number);
            } else {
                writeHeaderMapBit(&model->header, model->leaves.at(index).node_number, false);
                ++model->working_free_nodes;
            }
            model->leaves.removeAt(index);
        }
        if (model->leaves.size() > kHfsMaxMutationLeaves) {
            blockers->append(QStringLiteral("HFS+ catalog mutation supports at most %1 leaf nodes")
                                 .arg(kHfsMaxMutationLeaves));
            return false;
        }
        return true;
    }

    struct HfsCatalogTreeShape {
        uint32_t root_node{0};
        uint16_t tree_depth{1};
    };

    [[nodiscard]] bool emitCatalogLeafWrites(HfsCatalogTreeModel* model,
                                             HfsCatalogTreeMutation* mutation,
                                             QStringList* blockers) const {
        for (int index = 0; index < model->leaves.size(); ++index) {
            auto& leaf = (*model).leaves[index];
            const uint32_t forward =
                index + 1 < model->leaves.size() ? model->leaves.at(index + 1).node_number : 0;
            const uint32_t backward = index > 0 ? model->leaves.at(index - 1).node_number : 0;
            const auto bytes = buildCatalogLeafNodeBytes(forward, backward, leaf.records);
            if (!bytes.has_value()) {
                blockers->append(QStringLiteral("HFS+ catalog leaf rebuild failed"));
                return false;
            }
            // Sibling links can change even on untouched leaves, so every leaf in
            // the working model is rewritten.
            if (leaf.is_new) {
                mutation->orphan_writes.append(HfsBTreeNodeWrite{leaf.node_number, *bytes});
            } else {
                mutation->commit_writes.append(HfsBTreeNodeWrite{leaf.node_number, *bytes});
            }
        }
        return true;
    }

    // Packs one level of (key, child) entries into as few index nodes as fit,
    // appending each new node as an orphan write (new index nodes are never
    // referenced by the old tree, so they are safe to write before commit).
    [[nodiscard]] std::optional<QVector<QPair<QByteArray, uint32_t>>> emitCatalogIndexLevel(
        HfsCatalogTreeModel* model,
        const QVector<QPair<QByteArray, uint32_t>>& entries,
        uint8_t height,
        HfsCatalogTreeMutation* mutation,
        QStringList* blockers) const {
        QVector<QPair<QByteArray, uint32_t>> parents;
        QVector<HfsBTreeNodeWrite> levelWrites;
        int cursor = 0;
        while (cursor < entries.size()) {
            // Reserve the descriptor plus one trailing free-space offset slot;
            // each entry costs its key, a child pointer, and an offset slot.
            qsizetype usedBytes = kBTreeNodeDescriptorBytes + kUint16Size;
            int count = 0;
            while (cursor + count < entries.size()) {
                const auto& entry = entries.at(cursor + count);
                const qsizetype entryBytes = entry.first.size() + kUint32Size + kUint16Size;
                if (count > 0 && usedBytes + entryBytes > m_catalog.node_size) {
                    break;
                }
                usedBytes += entryBytes;
                ++count;
            }
            const QVector<QPair<QByteArray, uint32_t>> group = entries.mid(cursor, count);
            const auto indexBytes = buildIndexNodeBytes(
                m_catalog.node_size, group, height, QStringLiteral("catalog"), blockers);
            if (!indexBytes.has_value()) {
                return std::nullopt;
            }
            const auto nodeNumber = allocateCatalogModelNode(model, blockers);
            if (!nodeNumber.has_value()) {
                return std::nullopt;
            }
            levelWrites.append(HfsBTreeNodeWrite{*nodeNumber, *indexBytes});
            parents.append({group.first().first, *nodeNumber});
            cursor += count;
        }
        // Index nodes within one level form a sibling chain just like leaves.
        for (int index = 0; index < levelWrites.size(); ++index) {
            writeBe32(&levelWrites[index].bytes,
                      kBTreeNodeForwardLinkOffset,
                      index + 1 < levelWrites.size() ? levelWrites.at(index + 1).node_number : 0);
            writeBe32(&levelWrites[index].bytes,
                      kBTreeNodeBackwardLinkOffset,
                      index > 0 ? levelWrites.at(index - 1).node_number : 0);
            mutation->orphan_writes.append(levelWrites.at(index));
        }
        return parents;
    }

    [[nodiscard]] std::optional<HfsCatalogTreeShape> emitCatalogIndexLevels(
        HfsCatalogTreeModel* model, HfsCatalogTreeMutation* mutation, QStringList* blockers) const {
        if ((m_catalog.attributes_mask & kBTreeBigKeysMask) == 0 ||
            (m_catalog.attributes_mask & kBTreeVariableIndexKeysMask) == 0) {
            blockers->append(QStringLiteral(
                "HFS+ catalog B-tree split requires big keys and variable-length index keys"));
            return std::nullopt;
        }
        QVector<QPair<QByteArray, uint32_t>> entries;
        entries.reserve(model->leaves.size());
        for (const auto& leaf : model->leaves) {
            const auto key = rawRecordKeyBytes(leaf.records.first().bytes);
            if (!key.has_value()) {
                blockers->append(QStringLiteral("HFS+ catalog index keys are invalid"));
                return std::nullopt;
            }
            entries.append({*key, leaf.node_number});
        }
        uint8_t height = kHfsRootIndexNodeHeight;
        while (true) {
            if (height > kHfsMaxMutationTreeDepth) {
                blockers->append(
                    QStringLiteral("HFS+ catalog mutation supports B-trees up to %1 levels deep")
                        .arg(kHfsMaxMutationTreeDepth));
                return std::nullopt;
            }
            const auto parents = emitCatalogIndexLevel(model, entries, height, mutation, blockers);
            if (!parents.has_value()) {
                return std::nullopt;
            }
            if (parents->size() == 1) {
                return HfsCatalogTreeShape{parents->first().second, height};
            }
            entries = *parents;
            ++height;
        }
    }

    void composeCatalogTreeHeader(const HfsCatalogTreeModel& model,
                                  const HfsCatalogTreeShape& shape,
                                  HfsCatalogTreeMutation* mutation) {
        const uint32_t newLeafRecords =
            mutation->leaf_record_delta < 0
                ? m_catalog.leaf_records -
                      std::min<uint32_t>(m_catalog.leaf_records,
                                         static_cast<uint32_t>(-mutation->leaf_record_delta))
                : m_catalog.leaf_records + static_cast<uint32_t>(mutation->leaf_record_delta);
        HfsBTreeHeaderNodeContext updatedHeader = model.header;
        const qsizetype headerRecord = kBTreeHeaderRecordOffset;
        writeBe16(&updatedHeader.node,
                  headerRecord + kBTreeHeaderTreeDepthOffset,
                  shape.tree_depth);
        writeBe32(&updatedHeader.node, headerRecord + kBTreeHeaderRootNodeOffset, shape.root_node);
        writeBe32(&updatedHeader.node,
                  headerRecord + kBTreeHeaderLeafRecordsOffset,
                  newLeafRecords);
        writeBe32(&updatedHeader.node,
                  headerRecord + kBTreeHeaderFirstLeafNodeOffset,
                  model.leaves.first().node_number);
        writeBe32(&updatedHeader.node,
                  headerRecord + kBTreeHeaderLastLeafNodeOffset,
                  model.leaves.last().node_number);
        writeBe32(&updatedHeader.node,
                  headerRecord + kBTreeHeaderFreeNodesOffset,
                  static_cast<uint32_t>(model.working_free_nodes));
        mutation->header_node = updatedHeader.node;

        mutation->updated_header = m_catalog;
        mutation->updated_header.tree_depth = shape.tree_depth;
        mutation->updated_header.root_node = shape.root_node;
        mutation->updated_header.leaf_records = newLeafRecords;
        mutation->updated_header.first_leaf_node = model.leaves.first().node_number;
        mutation->updated_header.last_leaf_node = model.leaves.last().node_number;
        mutation->updated_header.free_nodes = static_cast<uint32_t>(model.working_free_nodes);
    }

    [[nodiscard]] std::optional<HfsCatalogTreeMutation> emitCatalogTreeMutation(
        HfsCatalogTreeModel* model,
        const HfsCatalogTreeEdit& edit,
        int removedCount,
        QStringList* blockers) {
        HfsCatalogTreeMutation mutation;
        mutation.removed_records = removedCount;
        mutation.leaf_record_delta = static_cast<int>(edit.insertions.size()) - removedCount;

        // Index levels are rebuilt from scratch on every mutation: the old
        // index nodes stay intact until the header-node commit write switches
        // the root, then are erased post-commit.
        for (uint32_t oldIndexNode : model->old_index_nodes) {
            freeCatalogModelNode(model, oldIndexNode);
        }
        if (!emitCatalogLeafWrites(model, &mutation, blockers)) {
            return std::nullopt;
        }
        HfsCatalogTreeShape shape{model->leaves.first().node_number, 1};
        if (model->leaves.size() > 1) {
            const auto indexShape = emitCatalogIndexLevels(model, &mutation, blockers);
            if (!indexShape.has_value()) {
                return std::nullopt;
            }
            shape = *indexShape;
        }
        for (uint32_t freed : model->freed_nodes) {
            mutation.post_commit_writes.append(
                HfsBTreeNodeWrite{freed, QByteArray(m_catalog.node_size, '\0')});
        }
        composeCatalogTreeHeader(*model, shape, &mutation);

        const int readBackLeaf =
            edit.read_back_name.isEmpty() && edit.read_back_parent_id == 0
                ? 0
                : catalogModelLeafForKey(*model, edit.read_back_parent_id, edit.read_back_name);
        mutation.insert_node_number = model->leaves.at(readBackLeaf).node_number;
        return mutation;
    }

    [[nodiscard]] int catalogModelReadBackLeaf(const HfsCatalogTreeModel& model,
                                               const HfsCatalogTreeEdit& edit) const {
        if (edit.read_back_name.isEmpty() && edit.read_back_parent_id == 0) {
            return 0;
        }
        const int containing =
            catalogModelLeafContainingKey(model, edit.read_back_parent_id, edit.read_back_name);
        if (containing >= 0) {
            return containing;
        }
        return catalogModelLeafForKey(model, edit.read_back_parent_id, edit.read_back_name);
    }

    [[nodiscard]] std::optional<HfsCatalogTreeMutation> finishCatalogTreeMutation(
        HfsCatalogTreeModel* model,
        const HfsCatalogTreeEdit& edit,
        QStringList* blockers,
        QStringList* warnings) {
        const uint32_t beforeNode =
            model->leaves.at(catalogModelReadBackLeaf(*model, edit)).node_number;
        QByteArray beforeBytes;
        const auto beforeRead = readCatalogNode(beforeNode);
        if (beforeRead.has_value()) {
            beforeBytes = *beforeRead;
        }
        int removedCount = 0;
        if (!applyCatalogEditToModel(model, edit, &removedCount, warnings, blockers)) {
            return std::nullopt;
        }
        if (!rebalanceCatalogModel(model, blockers)) {
            return std::nullopt;
        }
        auto mutation = emitCatalogTreeMutation(model, edit, removedCount, blockers);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        mutation->before_leaf_bytes = beforeBytes;
        mutation->before_leaf_node = beforeNode;
        if (m_catalog.tree_depth == 1 &&
            mutation->updated_header.tree_depth == kHfsRootLeafSplitTreeDepth) {
            warnings->append(QStringLiteral(
                "HFS+ catalog root leaf was split into two leaf nodes with a new index root"));
        }
        return mutation;
    }

    // H-a: special-file B-tree node-pool growth ------------------------------
    struct HfsNodePoolGrowthPlan {
        HfsForkData new_fork;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        QByteArray header_node;
        uint32_t add_blocks{0};
        uint32_t new_total_nodes{0};
        uint32_t new_nodes{0};
        uint64_t old_fork_bytes{0};
    };

    // Identifies one of the three special-file B-trees for generic node-pool
    // growth: its in-memory header + fork, the volume-header fork offset, the
    // reserved CNID, and a label for blockers.
    struct HfsSpecialTreeTarget {
        HfsBTreeHeader* header{nullptr};
        HfsForkData* fork{nullptr};
        qsizetype vh_fork_offset{0};
        uint32_t file_id{0};
        QString label;
    };

    // Writes a special-file (catalog/extents/attributes) fork descriptor back to
    // the on-disk volume header. The caller's syncAlternateVolumeHeader mirrors
    // the change to the alternate header.
    [[nodiscard]] bool writeSpecialFileForkToVolumeHeader(qsizetype forkOffset,
                                                          const HfsForkData& fork,
                                                          QStringList* blockers) {
        uint64_t headerOffset = 0;
        if (!checkedAdd(m_volume.volume_offset, kHfsVolumeHeaderOffset, &headerOffset)) {
            blockers->append(QStringLiteral("HFS+ volume header offset overflow"));
            return false;
        }
        const auto descriptor = readAt(headerOffset + static_cast<uint64_t>(forkOffset),
                                       kHfsForkDataBytes);
        if (!descriptor.has_value() || descriptor->size() != kHfsForkDataBytes) {
            blockers->append(QStringLiteral("HFS+ special-file fork descriptor read failed"));
            return false;
        }
        QByteArray bytes = *descriptor;
        const QByteArray logical = be64Bytes(fork.logical_size);
        bytes.replace(kHfsForkLogicalSizeOffset, logical.size(), logical);
        writeBe32(&bytes, kHfsForkTotalBlocksOffset, fork.total_blocks);
        bytes.replace(kHfsForkExtentsOffset,
                      kHfsExtentsRecordBytes,
                      hfsForkExtentBytes(fork.extents));
        if (!writeAt(headerOffset + static_cast<uint64_t>(forkOffset),
                     bytes.constData(),
                     bytes.size())) {
            blockers->append(QStringLiteral("HFS+ special-file fork descriptor write failed"));
            return false;
        }
        return true;
    }

    // H-a: plans how many allocation blocks to add to a special-file B-tree so it
    // gains at least minNewNodes free nodes, keeping the fork byte size a whole
    // multiple of the node size and staying inside the header node's map record.
    [[nodiscard]] std::optional<HfsNodePoolGrowthPlan> planSpecialTreeNodePoolGrowth(
        const HfsSpecialTreeTarget& tree, uint32_t minNewNodes, QStringList* blockers) {
        const uint32_t nodeSize = tree.header->node_size;
        const uint32_t blockSize = m_volume.block_size;
        if (nodeSize == 0 || blockSize == 0) {
            blockers->append(
                QStringLiteral("HFS+ %1 node-pool growth: invalid geometry").arg(tree.label));
            return std::nullopt;
        }
        const auto headerNode =
            readForkBytes(*tree.fork, tree.file_id, kHfsDataForkType, 0, nodeSize);
        if (!headerNode.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto headerCtx = parseBTreeHeaderNodeForMutation(*headerNode, tree.label, blockers);
        if (!headerCtx.has_value()) {
            return std::nullopt;
        }
        const uint64_t oldForkBytes = static_cast<uint64_t>(tree.fork->total_blocks) * blockSize;
        uint32_t addBlocks = static_cast<uint32_t>(
            (static_cast<uint64_t>(minNewNodes) * nodeSize + blockSize - 1) / blockSize);
        addBlocks = std::max<uint32_t>(addBlocks, 1);
        while (((oldForkBytes + static_cast<uint64_t>(addBlocks) * blockSize) % nodeSize) != 0) {
            ++addBlocks;
        }
        const uint64_t newForkBytes = oldForkBytes + static_cast<uint64_t>(addBlocks) * blockSize;
        const uint32_t newTotalNodes = static_cast<uint32_t>(newForkBytes / nodeSize);
        const uint64_t mapCapacityBits =
            static_cast<uint64_t>(headerCtx->map_end - headerCtx->map_offset) * kBitsPerByte;
        if (static_cast<uint64_t>(newTotalNodes) > mapCapacityBits) {
            blockers->append(QStringLiteral("HFS+ %1 node-pool growth would span multiple map "
                                            "nodes; not supported")
                                 .arg(tree.label));
            return std::nullopt;
        }
        const auto newExtents = findFreeAllocationExtents(addBlocks, tree.label, blockers);
        if (!newExtents.has_value()) {
            return std::nullopt;
        }
        QVector<HfsExtent> combined = tree.fork->extents;
        appendExtentsCoalesced(&combined, *newExtents);
        if (combined.size() > kHfsExtentCount) {
            blockers->append(QStringLiteral("HFS+ %1 node-pool growth needs an extents-overflow "
                                            "record; not yet supported")
                                 .arg(tree.label));
            return std::nullopt;
        }
        const auto allocationBytes = prepareAllocationBitmapSet(*newExtents, blockers);
        if (!allocationBytes.has_value()) {
            return std::nullopt;
        }
        return HfsNodePoolGrowthPlan{
            .new_fork = HfsForkData{.logical_size = newForkBytes,
                                    .total_blocks = tree.fork->total_blocks + addBlocks,
                                    .extents = combined},
            .allocation_bytes = *allocationBytes,
            .header_node = *headerNode,
            .add_blocks = addBlocks,
            .new_total_nodes = newTotalNodes,
            .new_nodes = newTotalNodes - tree.header->total_nodes,
            .old_fork_bytes = oldForkBytes};
    }

    // Commits a planned special-file node-pool growth: zero the new node region,
    // persist the allocation bitmap, volume-header fork descriptor, free-block
    // counter and B-tree header node counts, then update in-memory state.
    [[nodiscard]] bool applySpecialTreeNodePoolGrowth(const HfsNodePoolGrowthPlan& plan,
                                                      const HfsSpecialTreeTarget& tree,
                                                      QStringList* blockers) {
        const QByteArray zeros(
            static_cast<qsizetype>(plan.new_fork.logical_size - plan.old_fork_bytes), '\0');
        if (!writeForkBytes(
                 plan.new_fork, tree.file_id, kHfsDataForkType, plan.old_fork_bytes, zeros)
                 .has_value() ||
            !writeUpdatedAllocationBitmapBytes(plan.allocation_bytes).has_value()) {
            blockers->append(m_blockers);
            return false;
        }
        QByteArray headerNode = plan.header_node;
        writeBe32(&headerNode,
                  kBTreeHeaderRecordOffset + kBTreeHeaderTotalNodesOffset,
                  plan.new_total_nodes);
        writeBe32(&headerNode,
                  kBTreeHeaderRecordOffset + kBTreeHeaderFreeNodesOffset,
                  tree.header->free_nodes + plan.new_nodes);
        if (!writeSpecialFileForkToVolumeHeader(tree.vh_fork_offset, plan.new_fork, blockers) ||
            !writeVolumeHeaderCounter(kHfsFreeBlocksOffset,
                                      m_volume.free_blocks,
                                      -static_cast<int>(plan.add_blocks),
                                      blockers)
                 .has_value() ||
            !writeForkBytes(plan.new_fork, tree.file_id, kHfsDataForkType, 0, headerNode)
                 .has_value()) {
            blockers->append(m_blockers);
            return false;
        }
        *tree.fork = plan.new_fork;
        m_volume.free_blocks -= plan.add_blocks;
        tree.header->total_nodes = plan.new_total_nodes;
        tree.header->free_nodes += plan.new_nodes;
        return true;
    }

    [[nodiscard]] bool ensureSpecialTreeFreeNodes(const HfsSpecialTreeTarget& tree,
                                                  uint32_t needed,
                                                  QStringList* blockers) {
        if (tree.header->free_nodes >= needed) {
            return true;
        }
        const auto plan =
            planSpecialTreeNodePoolGrowth(tree, needed - tree.header->free_nodes, blockers);
        if (!plan.has_value()) {
            return false;
        }
        return applySpecialTreeNodePoolGrowth(*plan, tree, blockers);
    }

    [[nodiscard]] bool ensureCatalogFreeNodes(uint32_t needed, QStringList* blockers) {
        const HfsSpecialTreeTarget tree{.header = &m_catalog,
                                        .fork = &m_volume.catalog_fork,
                                        .vh_fork_offset = kHfsCatalogForkOffset,
                                        .file_id = kHfsCatalogFileId,
                                        .label = QStringLiteral("catalog")};
        return ensureSpecialTreeFreeNodes(tree, needed, blockers);
    }

    [[nodiscard]] bool ensureExtentsFreeNodes(uint32_t needed, QStringList* blockers) {
        const HfsSpecialTreeTarget tree{.header = &m_extents,
                                        .fork = &m_volume.extents_fork,
                                        .vh_fork_offset = kHfsExtentsForkOffset,
                                        .file_id = kHfsExtentsFileId,
                                        .label = QStringLiteral("extents overflow")};
        return ensureSpecialTreeFreeNodes(tree, needed, blockers);
    }

    // The attributes B-tree header is parsed on demand (no persistent member), so
    // growth updates a local header copy + the on-disk header node; the next
    // attributes mutation re-parses the grown header.
    [[nodiscard]] bool ensureAttributesFreeNodes(const HfsBTreeHeader& attributes,
                                                 uint32_t needed,
                                                 QStringList* blockers) {
        if (attributes.free_nodes >= needed) {
            return true;
        }
        HfsBTreeHeader header = attributes;
        const HfsSpecialTreeTarget tree{.header = &header,
                                        .fork = &m_volume.attributes_fork,
                                        .vh_fork_offset = kHfsAttributesForkOffset,
                                        .file_id = kHfsAttributesFileId,
                                        .label = QStringLiteral("attributes")};
        return ensureSpecialTreeFreeNodes(tree, needed, blockers);
    }

    [[nodiscard]] static bool blockersIndicateNodeExhaustion(const QStringList& blockers) {
        for (const auto& blocker : blockers) {
            if (blocker.contains(QStringLiteral("does not have enough free nodes"))) {
                return true;
            }
        }
        return false;
    }

    // H-a/H-b: runs a catalog-mutation "prepare" closure; if it fails purely because
    // the B-tree node pool is exhausted, grows the pool and retries. Building a
    // prepare plan only stages in-memory/idempotent work, so a failed attempt is
    // safe to discard. A mutation rebuilds the whole index level set, so a deep/wide
    // tree needs far more free nodes than a single split path - the free-node target
    // doubles each retry until the prepare fits or the volume cannot grow further.
    // prepareOnce(blockers, warnings) -> optional<Plan>.
    template <typename PrepareOnce>
    [[nodiscard]] auto withCatalogNodePoolGrowth(PrepareOnce prepareOnce,
                                                 QStringList* blockers,
                                                 QStringList* warnings)
        -> decltype(prepareOnce(blockers, warnings)) {
        const QStringList savedBlockers = m_blockers;
        QStringList attempt;
        QStringList attemptWarnings;
        auto plan = prepareOnce(&attempt, &attemptWarnings);
        if (plan.has_value()) {
            warnings->append(attemptWarnings);
            return plan;
        }
        if (!blockersIndicateNodeExhaustion(attempt)) {
            blockers->append(attempt);
            return std::nullopt;
        }
        uint32_t target = m_catalog.free_nodes + static_cast<uint32_t>(m_catalog.tree_depth) +
                          kHfsNodePoolGrowthReserve;
        while (true) {
            m_blockers = savedBlockers;
            if (!ensureCatalogFreeNodes(target, blockers)) {
                return std::nullopt;
            }
            m_blockers = savedBlockers;
            QStringList retry;
            QStringList retryWarnings;
            plan = prepareOnce(&retry, &retryWarnings);
            if (plan.has_value()) {
                warnings->append(retryWarnings);
                return plan;
            }
            if (!blockersIndicateNodeExhaustion(retry) || target > kHfsMaxNodePoolGrowthTarget) {
                blockers->append(retry);
                return std::nullopt;
            }
            target *= 2;
        }
    }

    [[nodiscard]] std::optional<HfsCatalogTreeMutation> prepareCatalogTreeMutationOnce(
        const HfsCatalogTreeEdit& edit, QStringList* blockers, QStringList* warnings) {
        auto model = loadCatalogTreeForMutation(blockers);
        if (!model.has_value()) {
            return std::nullopt;
        }
        return finishCatalogTreeMutation(&*model, edit, blockers, warnings);
    }

    [[nodiscard]] std::optional<HfsCatalogTreeMutation> prepareCatalogTreeMutation(
        const HfsCatalogTreeEdit& edit, QStringList* blockers, QStringList* warnings) {
        return withCatalogNodePoolGrowth(
            [&](QStringList* attemptBlockers, QStringList* attemptWarnings) {
                return prepareCatalogTreeMutationOnce(edit, attemptBlockers, attemptWarnings);
            },
            blockers,
            warnings);
    }

    [[nodiscard]] std::optional<int> applyCatalogTreeMutation(
        const HfsCatalogTreeMutation& mutation,
        const HfsCatalogCounterUpdate& update,
        QStringList* blockers) {
        int chunks = 0;
        for (const auto& nodeWrite : mutation.orphan_writes) {
            const auto nodeChunks = writeCatalogLeafNode(nodeWrite.node_number, nodeWrite.bytes);
            if (!nodeChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *nodeChunks;
        }
        for (const auto& nodeWrite : mutation.commit_writes) {
            const auto nodeChunks = writeCatalogLeafNode(nodeWrite.node_number, nodeWrite.bytes);
            if (!nodeChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *nodeChunks;
        }
        const auto headerChunks = writeCatalogLeafNode(0, mutation.header_node);
        if (!headerChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        chunks += *headerChunks;
        m_catalog = mutation.updated_header;
        for (const auto& nodeWrite : mutation.post_commit_writes) {
            const auto eraseChunks = writeCatalogLeafNode(nodeWrite.node_number, nodeWrite.bytes);
            if (!eraseChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *eraseChunks;
        }
        const auto counterChunks = applyCatalogVolumeCounters(update, blockers);
        if (!counterChunks.has_value()) {
            return std::nullopt;
        }
        return chunks + *counterChunks;
    }

    [[nodiscard]] std::optional<int> applyCatalogVolumeCounters(
        const HfsCatalogCounterUpdate& update, QStringList* blockers) {
        int chunks = 0;
        const auto fileCountChunks = writeVolumeHeaderCounter(
            kHfsFileCountOffset, m_volume.file_count, update.file_count_delta, blockers);
        if (!fileCountChunks.has_value()) {
            return std::nullopt;
        }
        chunks += *fileCountChunks;
        if (update.folder_count_delta != 0) {
            const auto folderCountChunks = writeVolumeHeaderCounter(
                kHfsFolderCountOffset, m_volume.folder_count, update.folder_count_delta, blockers);
            if (!folderCountChunks.has_value()) {
                return std::nullopt;
            }
            chunks += *folderCountChunks;
        }
        if (update.next_catalog_id != m_volume.next_catalog_id) {
            const auto nextIdChunks = writeVolumeHeaderU32(kHfsNextCatalogIdOffset,
                                                           update.next_catalog_id);
            if (!nextIdChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *nextIdChunks;
        }
        return chunks;
    }

    [[nodiscard]] static int compareOverflowExtentKeys(const HfsOverflowExtentRecord& left,
                                                       const HfsOverflowExtentRecord& right) {
        if (left.file_id != right.file_id) {
            return left.file_id < right.file_id ? -1 : 1;
        }
        if (left.fork_type != right.fork_type) {
            return left.fork_type < right.fork_type ? -1 : 1;
        }
        if (left.start_block != right.start_block) {
            return left.start_block < right.start_block ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] static QByteArray overflowExtentKeyBytes(const HfsOverflowExtentRecord& record) {
        QByteArray key(kUint16Size + kHfsExtentsKeyLength, '\0');
        writeBe16(&key, 0, static_cast<uint16_t>(kHfsExtentsKeyLength));
        key[kHfsExtentsKeyForkTypeOffset] = static_cast<char>(record.fork_type);
        writeBe32(&key, kHfsExtentsKeyFileIdOffset, record.file_id);
        writeBe32(&key, kHfsExtentsKeyStartBlockOffset, record.start_block);
        return key;
    }

    [[nodiscard]] static QByteArray overflowExtentRecordBytes(
        const HfsOverflowExtentRecord& record) {
        QByteArray bytes = overflowExtentKeyBytes(record);
        QByteArray payload(kHfsExtentsRecordBytes, '\0');
        const int count = std::min<int>(static_cast<int>(record.extents.size()), kHfsExtentCount);
        for (int index = 0; index < count; ++index) {
            const qsizetype offset = index * kHfsExtentBytes;
            writeBe32(&payload,
                      offset + kHfsExtentStartBlockOffset,
                      record.extents.at(index).start_block);
            writeBe32(&payload,
                      offset + kHfsExtentBlockCountOffset,
                      record.extents.at(index).block_count);
        }
        bytes.append(payload);
        return bytes;
    }

    [[nodiscard]] std::optional<QByteArray> buildExtentsLeafNodeBytes(
        uint32_t forwardLink,
        uint32_t backwardLink,
        const QVector<HfsOverflowExtentRecord>& records) const {
        QByteArray node(m_extents.node_size, '\0');
        writeBe32(&node, kBTreeNodeForwardLinkOffset, forwardLink);
        writeBe32(&node, kBTreeNodeBackwardLinkOffset, backwardLink);
        node[kBTreeKindOffset] = static_cast<char>(kBTreeLeafNode);
        node[kBTreeHeightOffset] = static_cast<char>(kHfsLeafNodeHeight);
        writeBe16(&node, kBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));
        QVector<qsizetype> offsets;
        offsets.reserve(records.size());
        qsizetype cursor = kBTreeNodeDescriptorBytes;
        for (const auto& record : records) {
            if (!appendCatalogRecordToNode(
                    &node, overflowExtentRecordBytes(record), &offsets, &cursor)) {
                return std::nullopt;
            }
        }
        writeCatalogNodeOffsets(&node, offsets, cursor);
        return node;
    }

    [[nodiscard]] std::optional<QVector<HfsOverflowExtentRecord>>
    loadSingleExtentsLeafRecordsForMutation(uint32_t nodeNumber, QStringList* blockers) {
        const auto node = readExtentsNode(nodeNumber);
        if (!node.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        if (static_cast<int8_t>(node->at(kBTreeKindOffset)) != kBTreeLeafNode ||
            static_cast<uint8_t>(node->at(kBTreeHeightOffset)) != kHfsLeafNodeHeight) {
            blockers->append(QStringLiteral("Invalid HFS+ extents overflow leaf node"));
            return std::nullopt;
        }
        const uint16_t numRecords = be16(*node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(*node, numRecords);
        if (!offsets.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        QVector<HfsOverflowExtentRecord> records;
        records.reserve(offsets->size());
        for (int index = 0; index < offsets->size(); ++index) {
            const qsizetype start = offsets->at(index);
            const qsizetype end = recordEnd(*node, *offsets, index);
            if (end <= start || end > node->size()) {
                blockers->append(QStringLiteral("Invalid HFS+ extents overflow record length"));
                return std::nullopt;
            }
            const auto record = parseOverflowExtentRecord(*node, start, end);
            if (!record.has_value()) {
                blockers->append(
                    QStringLiteral("HFS+ extents overflow mutation requires only supported "
                                   "data/resource records"));
                return std::nullopt;
            }
            records.append(*record);
        }
        return records;
    }

    [[nodiscard]] bool extentsTreeShapeSupportsMutation(bool treeIsEmpty,
                                                        QStringList* blockers) const {
        if (m_extents.node_size < kMinimumHfsBTreeNodeSize) {
            blockers->append(QStringLiteral("HFS+ extents overflow B-tree header is unavailable"));
            return false;
        }
        if (!treeIsEmpty &&
            (m_extents.tree_depth != 1 || m_extents.first_leaf_node != m_extents.last_leaf_node ||
             m_extents.root_node != m_extents.first_leaf_node)) {
            blockers->append(
                QStringLiteral("HFS+ extents overflow mutation currently requires an empty or "
                               "single-leaf B-tree"));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool applyExtentsRecordEdits(QVector<HfsOverflowExtentRecord>* records,
                                               const QVector<QPair<uint32_t, uint8_t>>& removeForks,
                                               const QVector<HfsOverflowExtentRecord>& insertions,
                                               QStringList* blockers) const {
        for (const auto& removal : removeForks) {
            records->erase(std::remove_if(records->begin(),
                                          records->end(),
                                          [&removal](const HfsOverflowExtentRecord& record) {
                                              return record.file_id == removal.first &&
                                                     record.fork_type == removal.second;
                                          }),
                           records->end());
        }
        for (const auto& insertion : insertions) {
            if (insertion.extents.isEmpty() || insertion.extents.size() > kHfsExtentCount ||
                (insertion.fork_type != kHfsDataForkType &&
                 insertion.fork_type != kHfsResourceForkType)) {
                blockers->append(
                    QStringLiteral("HFS+ extents overflow insertion record is invalid"));
                return false;
            }
            for (const auto& existing : *records) {
                if (compareOverflowExtentKeys(existing, insertion) == 0) {
                    blockers->append(
                        QStringLiteral("HFS+ extents overflow insertion key already exists"));
                    return false;
                }
            }
            records->append(insertion);
        }
        std::sort(records->begin(),
                  records->end(),
                  [](const HfsOverflowExtentRecord& left, const HfsOverflowExtentRecord& right) {
                      return compareOverflowExtentKeys(left, right) < 0;
                  });
        return true;
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> prepareExtentsTreeMutation(
        const QVector<QPair<uint32_t, uint8_t>>& removeForks,
        const QVector<HfsOverflowExtentRecord>& insertions,
        QStringList* blockers) {
        const QStringList saved = m_blockers;
        QStringList firstAttempt;
        auto mutation = prepareExtentsTreeMutationOnce(removeForks, insertions, &firstAttempt);
        if (mutation.has_value()) {
            return mutation;
        }
        if (!blockersIndicateNodeExhaustion(firstAttempt)) {
            blockers->append(firstAttempt);
            return std::nullopt;
        }
        m_blockers = saved;
        if (!ensureExtentsFreeNodes(static_cast<uint32_t>(m_extents.tree_depth) +
                                        kHfsNodePoolGrowthReserve,
                                    blockers)) {
            return std::nullopt;
        }
        return prepareExtentsTreeMutationOnce(removeForks, insertions, blockers);
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> prepareExtentsTreeMutationOnce(
        const QVector<QPair<uint32_t, uint8_t>>& removeForks,
        const QVector<HfsOverflowExtentRecord>& insertions,
        QStringList* blockers) {
        const bool treeIsEmpty = m_extents.leaf_records == 0 || m_extents.first_leaf_node == 0;
        if (!extentsTreeShapeSupportsMutation(treeIsEmpty, blockers)) {
            return std::nullopt;
        }

        QVector<HfsOverflowExtentRecord> records;
        if (!treeIsEmpty) {
            const auto existing = loadSingleExtentsLeafRecordsForMutation(m_extents.first_leaf_node,
                                                                          blockers);
            if (!existing.has_value()) {
                return std::nullopt;
            }
            records = *existing;
        }
        if (!applyExtentsRecordEdits(&records, removeForks, insertions, blockers)) {
            return std::nullopt;
        }

        HfsExtentsTreeMutation mutation;
        mutation.final_records = records;
        mutation.updated_header = m_extents;

        auto header = loadExtentsHeaderNodeForMutation(blockers);
        if (!header.has_value()) {
            return std::nullopt;
        }

        if (records.isEmpty()) {
            return buildExtentsTreeFree(treeIsEmpty, *header, mutation, blockers);
        }
        if (treeIsEmpty) {
            return buildExtentsTreeMaterialize(*header, mutation, blockers);
        }
        return buildExtentsTreeRebuildOrSplit(*header, mutation, blockers);
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> buildExtentsTreeFree(
        bool treeIsEmpty,
        const HfsBTreeHeaderNodeContext& header,
        HfsExtentsTreeMutation mutation,
        QStringList* blockers) {
        if (treeIsEmpty) {
            return mutation;
        }
        HfsBTreeHeaderNodeContext updated = header;
        const qsizetype headerRecord = kBTreeHeaderRecordOffset;
        if (!headerMapBitSet(updated, m_extents.first_leaf_node)) {
            blockers->append(QStringLiteral(
                "HFS+ extents overflow B-tree node map is inconsistent with allocated nodes"));
            return std::nullopt;
        }
        writeBe16(&updated.node, headerRecord + kBTreeHeaderTreeDepthOffset, 0);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderRootNodeOffset, 0);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderLeafRecordsOffset, 0);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFirstLeafNodeOffset, 0);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderLastLeafNodeOffset, 0);
        writeBe32(&updated.node,
                  headerRecord + kBTreeHeaderFreeNodesOffset,
                  m_extents.free_nodes + 1);
        writeHeaderMapBit(&updated, m_extents.first_leaf_node, false);
        mutation.free_tree = true;
        mutation.post_commit_writes.append(
            HfsBTreeNodeWrite{m_extents.first_leaf_node, QByteArray(m_extents.node_size, '\0')});
        mutation.header_node = updated.node;
        mutation.updated_header.tree_depth = 0;
        mutation.updated_header.root_node = 0;
        mutation.updated_header.leaf_records = 0;
        mutation.updated_header.first_leaf_node = 0;
        mutation.updated_header.last_leaf_node = 0;
        mutation.updated_header.free_nodes = m_extents.free_nodes + 1;
        return mutation;
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> buildExtentsTreeMaterialize(
        const HfsBTreeHeaderNodeContext& header,
        HfsExtentsTreeMutation mutation,
        QStringList* blockers) const {
        const auto allocated =
            allocateBTreeNodesFromHeaderMap({.header = &header,
                                             .tree = &m_extents,
                                             .must_be_allocated = {0},
                                             .count = 1,
                                             .label = QStringLiteral("extents overflow")},
                                            blockers);
        if (!allocated.has_value()) {
            return std::nullopt;
        }
        const uint32_t leafNode = allocated->first();
        const auto leafBytes = buildExtentsLeafNodeBytes(0, 0, mutation.final_records);
        if (!leafBytes.has_value()) {
            blockers->append(
                QStringLiteral("HFS+ extents overflow records do not fit a single new leaf node"));
            return std::nullopt;
        }
        HfsBTreeHeaderNodeContext updated = header;
        const qsizetype headerRecord = kBTreeHeaderRecordOffset;
        writeBe16(&updated.node, headerRecord + kBTreeHeaderTreeDepthOffset, 1);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderRootNodeOffset, leafNode);
        writeBe32(&updated.node,
                  headerRecord + kBTreeHeaderLeafRecordsOffset,
                  static_cast<uint32_t>(mutation.final_records.size()));
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFirstLeafNodeOffset, leafNode);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderLastLeafNodeOffset, leafNode);
        writeBe32(&updated.node,
                  headerRecord + kBTreeHeaderFreeNodesOffset,
                  m_extents.free_nodes - 1);
        writeHeaderMapBit(&updated, leafNode, true);
        mutation.materialize = true;
        mutation.node_writes.append(HfsBTreeNodeWrite{leafNode, *leafBytes});
        mutation.header_node = updated.node;
        mutation.updated_header.tree_depth = 1;
        mutation.updated_header.root_node = leafNode;
        mutation.updated_header.leaf_records = static_cast<uint32_t>(mutation.final_records.size());
        mutation.updated_header.first_leaf_node = leafNode;
        mutation.updated_header.last_leaf_node = leafNode;
        mutation.updated_header.free_nodes = m_extents.free_nodes - 1;
        return mutation;
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> buildExtentsTreeRebuildOrSplit(
        const HfsBTreeHeaderNodeContext& header,
        HfsExtentsTreeMutation mutation,
        QStringList* blockers) {
        const auto singleLeaf = buildExtentsLeafNodeBytes(0, 0, mutation.final_records);
        if (singleLeaf.has_value()) {
            HfsBTreeHeaderNodeContext updated = header;
            const qsizetype headerRecord = kBTreeHeaderRecordOffset;
            writeBe32(&updated.node,
                      headerRecord + kBTreeHeaderLeafRecordsOffset,
                      static_cast<uint32_t>(mutation.final_records.size()));
            mutation.leaf_rewrite = HfsBTreeNodeWrite{m_extents.first_leaf_node, *singleLeaf};
            mutation.header_node = updated.node;
            mutation.updated_header.leaf_records =
                static_cast<uint32_t>(mutation.final_records.size());
            return mutation;
        }
        return buildExtentsTreeRootLeafSplit(header, std::move(mutation), blockers);
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> buildExtentsTreeRootLeafSplit(
        const HfsBTreeHeaderNodeContext& header,
        HfsExtentsTreeMutation mutation,
        QStringList* blockers) {
        const auto allocated =
            allocateBTreeNodesFromHeaderMap({.header = &header,
                                             .tree = &m_extents,
                                             .must_be_allocated = {0, m_extents.first_leaf_node},
                                             .count = kHfsRootLeafSplitNodesNeeded,
                                             .label = QStringLiteral("extents overflow")},
                                            blockers);
        if (!allocated.has_value()) {
            return std::nullopt;
        }
        const uint32_t leftNode = allocated->at(kHfsSplitLeftNodeSlot);
        const uint32_t rightNode = allocated->at(kHfsSplitRightNodeSlot);
        const uint32_t indexNode = allocated->at(kHfsSplitIndexNodeSlot);
        const int splitIndex =
            std::max(1, static_cast<int>(mutation.final_records.size()) / kHfsRootLeafSplitHalves);
        if (splitIndex >= mutation.final_records.size()) {
            blockers->append(
                QStringLiteral("HFS+ extents overflow root-leaf split needs at least two records"));
            return std::nullopt;
        }
        const QVector<HfsOverflowExtentRecord> leftRecords = mutation.final_records.mid(0,
                                                                                        splitIndex);
        const QVector<HfsOverflowExtentRecord> rightRecords =
            mutation.final_records.mid(splitIndex);
        const auto leftBytes = buildExtentsLeafNodeBytes(rightNode, 0, leftRecords);
        const auto rightBytes = buildExtentsLeafNodeBytes(0, leftNode, rightRecords);
        if (!leftBytes.has_value() || !rightBytes.has_value()) {
            blockers->append(QStringLiteral(
                "HFS+ extents overflow root-leaf split could not fit records into two leaf nodes"));
            return std::nullopt;
        }
        const auto indexBytes =
            buildIndexNodeBytes(m_extents.node_size,
                                {{overflowExtentKeyBytes(leftRecords.first()), leftNode},
                                 {overflowExtentKeyBytes(rightRecords.first()), rightNode}},
                                kHfsRootIndexNodeHeight,
                                QStringLiteral("extents overflow"),
                                blockers);
        if (!indexBytes.has_value()) {
            return std::nullopt;
        }

        HfsBTreeHeaderNodeContext updated = header;
        const qsizetype headerRecord = kBTreeHeaderRecordOffset;
        const uint32_t newFreeNodes =
            m_extents.free_nodes -
            std::min<uint32_t>(m_extents.free_nodes,
                               static_cast<uint32_t>(-kHfsRootLeafSplitFreeNodeDelta));
        writeBe16(&updated.node,
                  headerRecord + kBTreeHeaderTreeDepthOffset,
                  kHfsRootLeafSplitTreeDepth);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderRootNodeOffset, indexNode);
        writeBe32(&updated.node,
                  headerRecord + kBTreeHeaderLeafRecordsOffset,
                  static_cast<uint32_t>(mutation.final_records.size()));
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFirstLeafNodeOffset, leftNode);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderLastLeafNodeOffset, rightNode);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFreeNodesOffset, newFreeNodes);
        writeHeaderMapBit(&updated, m_extents.first_leaf_node, false);
        writeHeaderMapBit(&updated, leftNode, true);
        writeHeaderMapBit(&updated, rightNode, true);
        writeHeaderMapBit(&updated, indexNode, true);

        mutation.split = true;
        mutation.node_writes.append(HfsBTreeNodeWrite{leftNode, *leftBytes});
        mutation.node_writes.append(HfsBTreeNodeWrite{rightNode, *rightBytes});
        mutation.node_writes.append(HfsBTreeNodeWrite{indexNode, *indexBytes});
        mutation.post_commit_writes.append(
            HfsBTreeNodeWrite{m_extents.first_leaf_node, QByteArray(m_extents.node_size, '\0')});
        mutation.header_node = updated.node;
        mutation.updated_header.tree_depth = kHfsRootLeafSplitTreeDepth;
        mutation.updated_header.root_node = indexNode;
        mutation.updated_header.leaf_records = static_cast<uint32_t>(mutation.final_records.size());
        mutation.updated_header.first_leaf_node = leftNode;
        mutation.updated_header.last_leaf_node = rightNode;
        mutation.updated_header.free_nodes = newFreeNodes;
        return mutation;
    }

    // ---- Bounded HFS+ attributes B-tree insertion (new inline attributes) ----

    struct HfsAttributeRawRecord {
        uint32_t file_id{0};
        QString name;
        QByteArray bytes;
    };

    [[nodiscard]] static int compareAttributeRawKeys(uint32_t leftId,
                                                     const QString& leftName,
                                                     uint32_t rightId,
                                                     const QString& rightName) {
        if (leftId != rightId) {
            return leftId < rightId ? -1 : 1;
        }
        return compareCatalogNames(leftName, rightName, kHfsBinaryCompare);
    }

    [[nodiscard]] static QByteArray attributeKeyBytes(uint32_t fileId, const QString& name) {
        const auto keyLength = static_cast<uint16_t>(kHfsAttributeKeyNameOffset - kUint16Size +
                                                     name.size() * kUint16Size);
        QByteArray key(kUint16Size + keyLength, '\0');
        writeBe16(&key, 0, keyLength);
        writeBe32(&key, kHfsAttributeKeyFileIdOffset, fileId);
        writeBe32(&key, kHfsAttributeKeyStartBlockOffset, 0);
        writeBe16(&key, kHfsAttributeKeyNameLengthOffset, static_cast<uint16_t>(name.size()));
        for (qsizetype index = 0; index < name.size(); ++index) {
            writeBe16(&key,
                      kHfsAttributeKeyNameOffset + index * kUint16Size,
                      name.at(index).unicode());
        }
        return key;
    }

    [[nodiscard]] static QByteArray inlineAttributeRecordBytes(uint32_t fileId,
                                                               const QString& name,
                                                               const QByteArray& value) {
        QByteArray record = attributeKeyBytes(fileId, name);
        QByteArray payload(kHfsAttributeInlineDataOffset + value.size(), '\0');
        writeBe32(&payload, 0, kHfsAttributeInlineDataRecord);
        writeBe32(&payload, kHfsAttributeInlineSizeOffset, static_cast<uint32_t>(value.size()));
        std::copy(value.cbegin(), value.cend(), payload.begin() + kHfsAttributeInlineDataOffset);
        record.append(payload);
        if ((record.size() % kHfsRecordAlignmentBytes) != 0) {
            record.append('\0');
        }
        return record;
    }

    [[nodiscard]] std::optional<QByteArray> readAttributesNode(const HfsBTreeHeader& tree,
                                                               uint32_t nodeNumber) {
        uint64_t offset = 0;
        if (nodeNumber >= tree.total_nodes || !checkedMul(nodeNumber, tree.node_size, &offset)) {
            m_blockers.append(QStringLiteral("HFS+ attributes node is out of range"));
            return std::nullopt;
        }
        return readForkBytes(m_volume.attributes_fork,
                             kHfsAttributesFileId,
                             kHfsDataForkType,
                             offset,
                             tree.node_size);
    }

    [[nodiscard]] std::optional<int> writeAttributesNode(const HfsBTreeHeader& tree,
                                                         uint32_t nodeNumber,
                                                         const QByteArray& node) {
        uint64_t offset = 0;
        if (!checkedMul(nodeNumber, tree.node_size, &offset)) {
            m_blockers.append(QStringLiteral("HFS+ attributes node offset overflow"));
            return std::nullopt;
        }
        return writeForkBytes(
            m_volume.attributes_fork, kHfsAttributesFileId, kHfsDataForkType, offset, node);
    }

    [[nodiscard]] std::optional<QVector<HfsAttributeRawRecord>> loadSingleAttributesLeafRecords(
        const HfsBTreeHeader& tree, uint32_t nodeNumber, QStringList* blockers) {
        const auto node = readAttributesNode(tree, nodeNumber);
        if (!node.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        if (static_cast<int8_t>(node->at(kBTreeKindOffset)) != kBTreeLeafNode ||
            static_cast<uint8_t>(node->at(kBTreeHeightOffset)) != kHfsLeafNodeHeight) {
            blockers->append(QStringLiteral("Invalid HFS+ attributes leaf node"));
            return std::nullopt;
        }
        const uint16_t numRecords = be16(*node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(*node, numRecords);
        if (!offsets.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        QVector<HfsAttributeRawRecord> records;
        records.reserve(offsets->size());
        for (int index = 0; index < offsets->size(); ++index) {
            const qsizetype start = offsets->at(index);
            const qsizetype end = recordEnd(*node, *offsets, index);
            const uint16_t keyLength = be16(*node, start);
            const uint16_t nameLength = be16(*node, start + kHfsAttributeKeyNameLengthOffset);
            if (end <= start || keyLength < kHfsAttributeMinimumKeyBytes ||
                !hasBytes(*node,
                          start + kHfsAttributeKeyNameOffset,
                          static_cast<qsizetype>(nameLength) * kUint16Size)) {
                blockers->append(QStringLiteral("Invalid HFS+ attributes record"));
                return std::nullopt;
            }
            QString name;
            name.reserve(nameLength);
            for (uint16_t charIndex = 0; charIndex < nameLength; ++charIndex) {
                name.append(QChar(be16(*node,
                                       start + kHfsAttributeKeyNameOffset +
                                           static_cast<qsizetype>(charIndex) * kUint16Size)));
            }
            records.append(
                HfsAttributeRawRecord{.file_id = be32(*node, start + kHfsAttributeKeyFileIdOffset),
                                      .name = name,
                                      .bytes = node->mid(start, end - start)});
        }
        return records;
    }

    [[nodiscard]] std::optional<QByteArray> buildAttributesLeafNodeBytes(
        const HfsBTreeHeader& tree,
        uint32_t forwardLink,
        uint32_t backwardLink,
        const QVector<HfsAttributeRawRecord>& records) const {
        QByteArray node(tree.node_size, '\0');
        writeBe32(&node, kBTreeNodeForwardLinkOffset, forwardLink);
        writeBe32(&node, kBTreeNodeBackwardLinkOffset, backwardLink);
        node[kBTreeKindOffset] = static_cast<char>(kBTreeLeafNode);
        node[kBTreeHeightOffset] = static_cast<char>(kHfsLeafNodeHeight);
        writeBe16(&node, kBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));
        QVector<qsizetype> offsets;
        offsets.reserve(records.size());
        qsizetype cursor = kBTreeNodeDescriptorBytes;
        for (const auto& record : records) {
            if (!appendCatalogRecordToNode(&node, record.bytes, &offsets, &cursor)) {
                return std::nullopt;
            }
        }
        writeCatalogNodeOffsets(&node, offsets, cursor);
        return node;
    }

    [[nodiscard]] std::optional<HfsBTreeHeaderNodeContext> loadAttributesHeaderNodeForMutation(
        const HfsBTreeHeader& tree, QStringList* blockers) {
        const auto node = readAttributesNode(tree, 0);
        if (!node.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return parseBTreeHeaderNodeForMutation(*node, QStringLiteral("attributes"), blockers);
    }

    [[nodiscard]] std::optional<QVector<HfsAttributeRawRecord>> collectAttributesRecordsForInsert(
        const HfsBTreeHeader& tree,
        bool treeIsEmpty,
        const HfsAttributeRawRecord& insertion,
        QStringList* blockers) {
        QVector<HfsAttributeRawRecord> records;
        if (!treeIsEmpty) {
            const auto existing =
                loadSingleAttributesLeafRecords(tree, tree.first_leaf_node, blockers);
            if (!existing.has_value()) {
                return std::nullopt;
            }
            records = *existing;
        }
        for (const auto& existing : records) {
            if (compareAttributeRawKeys(
                    existing.file_id, existing.name, insertion.file_id, insertion.name) == 0) {
                blockers->append(QStringLiteral("HFS+ attribute already exists"));
                return std::nullopt;
            }
        }
        records.append(insertion);
        std::sort(records.begin(),
                  records.end(),
                  [](const HfsAttributeRawRecord& left, const HfsAttributeRawRecord& right) {
                      return compareAttributeRawKeys(
                                 left.file_id, left.name, right.file_id, right.name) < 0;
                  });
        return records;
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> prepareAttributesTreeInsert(
        const HfsBTreeHeader& tree, const HfsAttributeRawRecord& insertion, QStringList* blockers) {
        const QStringList saved = m_blockers;
        QStringList firstAttempt;
        auto mutation = prepareAttributesTreeInsertOnce(tree, insertion, &firstAttempt);
        if (mutation.has_value()) {
            return mutation;
        }
        if (!blockersIndicateNodeExhaustion(firstAttempt)) {
            blockers->append(firstAttempt);
            return std::nullopt;
        }
        m_blockers = saved;
        if (!ensureAttributesFreeNodes(tree,
                                       static_cast<uint32_t>(tree.tree_depth) +
                                           kHfsNodePoolGrowthReserve,
                                       blockers)) {
            return std::nullopt;
        }
        HfsBTreeHeader grown;
        if (!loadAttributeHeader(&grown)) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return prepareAttributesTreeInsertOnce(grown, insertion, blockers);
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> prepareAttributesTreeInsertOnce(
        const HfsBTreeHeader& tree, const HfsAttributeRawRecord& insertion, QStringList* blockers) {
        const bool treeIsEmpty = tree.leaf_records == 0 || tree.first_leaf_node == 0;
        if (!treeIsEmpty && (tree.tree_depth != 1 || tree.first_leaf_node != tree.last_leaf_node ||
                             tree.root_node != tree.first_leaf_node)) {
            blockers->append(
                QStringLiteral("HFS+ attribute create currently requires an empty or single-leaf "
                               "attributes B-tree"));
            return std::nullopt;
        }
        const auto collected =
            collectAttributesRecordsForInsert(tree, treeIsEmpty, insertion, blockers);
        if (!collected.has_value()) {
            return std::nullopt;
        }
        const QVector<HfsAttributeRawRecord>& records = *collected;

        auto header = loadAttributesHeaderNodeForMutation(tree, blockers);
        if (!header.has_value()) {
            return std::nullopt;
        }
        HfsExtentsTreeMutation mutation;
        mutation.updated_header = tree;

        if (treeIsEmpty) {
            return buildAttributesTreeMaterialize(
                tree, *header, records, std::move(mutation), blockers);
        }

        const auto singleLeaf = buildAttributesLeafNodeBytes(tree, 0, 0, records);
        if (singleLeaf.has_value()) {
            HfsBTreeHeaderNodeContext updated = *header;
            writeBe32(&updated.node,
                      kBTreeHeaderRecordOffset + kBTreeHeaderLeafRecordsOffset,
                      static_cast<uint32_t>(records.size()));
            mutation.leaf_rewrite = HfsBTreeNodeWrite{tree.first_leaf_node, *singleLeaf};
            mutation.header_node = updated.node;
            mutation.updated_header.leaf_records = static_cast<uint32_t>(records.size());
            return mutation;
        }
        return buildAttributesTreeRootLeafSplit(
            tree, *header, records, std::move(mutation), blockers);
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> buildAttributesTreeMaterialize(
        const HfsBTreeHeader& tree,
        const HfsBTreeHeaderNodeContext& header,
        const QVector<HfsAttributeRawRecord>& records,
        HfsExtentsTreeMutation mutation,
        QStringList* blockers) const {
        const auto allocated =
            allocateBTreeNodesFromHeaderMap({.header = &header,
                                             .tree = &tree,
                                             .must_be_allocated = {0},
                                             .count = 1,
                                             .label = QStringLiteral("attributes")},
                                            blockers);
        if (!allocated.has_value()) {
            return std::nullopt;
        }
        const uint32_t leafNode = allocated->first();
        const auto leafBytes = buildAttributesLeafNodeBytes(tree, 0, 0, records);
        if (!leafBytes.has_value()) {
            blockers->append(
                QStringLiteral("HFS+ attribute records do not fit a single new leaf node"));
            return std::nullopt;
        }
        HfsBTreeHeaderNodeContext updated = header;
        const qsizetype headerRecord = kBTreeHeaderRecordOffset;
        writeBe16(&updated.node, headerRecord + kBTreeHeaderTreeDepthOffset, 1);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderRootNodeOffset, leafNode);
        writeBe32(&updated.node,
                  headerRecord + kBTreeHeaderLeafRecordsOffset,
                  static_cast<uint32_t>(records.size()));
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFirstLeafNodeOffset, leafNode);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderLastLeafNodeOffset, leafNode);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFreeNodesOffset, tree.free_nodes - 1);
        writeHeaderMapBit(&updated, leafNode, true);
        mutation.materialize = true;
        mutation.node_writes.append(HfsBTreeNodeWrite{leafNode, *leafBytes});
        mutation.header_node = updated.node;
        mutation.updated_header.tree_depth = 1;
        mutation.updated_header.root_node = leafNode;
        mutation.updated_header.leaf_records = static_cast<uint32_t>(records.size());
        mutation.updated_header.first_leaf_node = leafNode;
        mutation.updated_header.last_leaf_node = leafNode;
        mutation.updated_header.free_nodes = tree.free_nodes - 1;
        return mutation;
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> buildAttributesTreeRootLeafSplit(
        const HfsBTreeHeader& tree,
        const HfsBTreeHeaderNodeContext& header,
        const QVector<HfsAttributeRawRecord>& records,
        HfsExtentsTreeMutation mutation,
        QStringList* blockers) {
        if ((tree.attributes_mask & kBTreeBigKeysMask) == 0 ||
            (tree.attributes_mask & kBTreeVariableIndexKeysMask) == 0) {
            blockers->append(QStringLiteral(
                "HFS+ attributes B-tree split requires big keys and variable-length index keys"));
            return std::nullopt;
        }
        HfsBTreeHeaderNodeContext working = header;
        const auto allocated =
            allocateBTreeNodesFromHeaderMap({.header = &working,
                                             .tree = &tree,
                                             .must_be_allocated = {0, tree.first_leaf_node},
                                             .count = kHfsRootLeafSplitNodesNeeded,
                                             .label = QStringLiteral("attributes")},
                                            blockers);
        if (!allocated.has_value()) {
            return std::nullopt;
        }
        const uint32_t leftNode = allocated->at(kHfsSplitLeftNodeSlot);
        const uint32_t rightNode = allocated->at(kHfsSplitRightNodeSlot);
        const uint32_t indexNode = allocated->at(kHfsSplitIndexNodeSlot);
        const int splitIndex = std::max(1,
                                        static_cast<int>(records.size()) / kHfsRootLeafSplitHalves);
        const QVector<HfsAttributeRawRecord> leftRecords = records.mid(0, splitIndex);
        const QVector<HfsAttributeRawRecord> rightRecords = records.mid(splitIndex);
        const auto leftBytes = buildAttributesLeafNodeBytes(tree, rightNode, 0, leftRecords);
        const auto rightBytes = buildAttributesLeafNodeBytes(tree, 0, leftNode, rightRecords);
        if (!leftBytes.has_value() || !rightBytes.has_value()) {
            blockers->append(QStringLiteral(
                "HFS+ attributes root-leaf split could not fit records into two leaf nodes"));
            return std::nullopt;
        }
        const auto leftKey = rawRecordKeyBytes(leftRecords.first().bytes);
        const auto rightKey = rawRecordKeyBytes(rightRecords.first().bytes);
        if (!leftKey.has_value() || !rightKey.has_value()) {
            blockers->append(QStringLiteral("HFS+ attributes split index keys are invalid"));
            return std::nullopt;
        }
        const auto indexBytes = buildIndexNodeBytes(tree.node_size,
                                                    {{*leftKey, leftNode}, {*rightKey, rightNode}},
                                                    kHfsRootIndexNodeHeight,
                                                    QStringLiteral("attributes"),
                                                    blockers);
        if (!indexBytes.has_value()) {
            return std::nullopt;
        }
        HfsBTreeHeaderNodeContext updated = working;
        const qsizetype headerRecord = kBTreeHeaderRecordOffset;
        writeBe16(&updated.node,
                  headerRecord + kBTreeHeaderTreeDepthOffset,
                  kHfsRootLeafSplitTreeDepth);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderRootNodeOffset, indexNode);
        writeBe32(&updated.node,
                  headerRecord + kBTreeHeaderLeafRecordsOffset,
                  static_cast<uint32_t>(records.size()));
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFirstLeafNodeOffset, leftNode);
        writeBe32(&updated.node, headerRecord + kBTreeHeaderLastLeafNodeOffset, rightNode);
        const uint32_t newFreeNodes =
            tree.free_nodes -
            std::min<uint32_t>(tree.free_nodes,
                               static_cast<uint32_t>(-kHfsRootLeafSplitFreeNodeDelta));
        writeBe32(&updated.node, headerRecord + kBTreeHeaderFreeNodesOffset, newFreeNodes);
        writeHeaderMapBit(&updated, tree.first_leaf_node, false);
        mutation.split = true;
        mutation.node_writes.append(HfsBTreeNodeWrite{leftNode, *leftBytes});
        mutation.node_writes.append(HfsBTreeNodeWrite{rightNode, *rightBytes});
        mutation.node_writes.append(HfsBTreeNodeWrite{indexNode, *indexBytes});
        mutation.post_commit_writes.append(
            HfsBTreeNodeWrite{tree.first_leaf_node, QByteArray(tree.node_size, '\0')});
        mutation.header_node = updated.node;
        mutation.updated_header.tree_depth = kHfsRootLeafSplitTreeDepth;
        mutation.updated_header.root_node = indexNode;
        mutation.updated_header.leaf_records = static_cast<uint32_t>(records.size());
        mutation.updated_header.first_leaf_node = leftNode;
        mutation.updated_header.last_leaf_node = rightNode;
        mutation.updated_header.free_nodes = newFreeNodes;
        return mutation;
    }

    [[nodiscard]] std::optional<HfsRawCatalogRecord> findCatalogModelFileOrFolderRecord(
        const HfsCatalogTreeModel& model, uint32_t fileId) const {
        for (const auto& leaf : model.leaves) {
            for (const auto& record : leaf.records) {
                if (record.catalog_id == fileId &&
                    (record.record_type == kHfsCatalogFileRecord ||
                     record.record_type == kHfsCatalogFolderRecord)) {
                    return record;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<int> setCatalogRecordAttributesFlag(uint32_t fileId,
                                                                    bool set,
                                                                    QStringList* blockers,
                                                                    QStringList* warnings) {
        auto model = loadCatalogTreeForMutation(blockers);
        if (!model.has_value()) {
            return std::nullopt;
        }
        const auto record = findCatalogModelFileOrFolderRecord(*model, fileId);
        if (!record.has_value()) {
            blockers->append(QStringLiteral("HFS+ catalog ID for attribute create was not found"));
            return std::nullopt;
        }
        const uint16_t keyLength = be16(record->bytes, 0);
        const qsizetype flagsOffset = kUint16Size + keyLength + kHfsCatalogRecordFlagsOffset;
        if (!hasBytes(record->bytes, flagsOffset, kUint16Size)) {
            blockers->append(QStringLiteral("HFS+ catalog record flags field is unavailable"));
            return std::nullopt;
        }
        const uint16_t flags = be16(record->bytes, flagsOffset);
        if (((flags & kHfsCatalogHasAttributesMask) != 0) == set) {
            return 0;
        }
        HfsRawCatalogRecord replacement = *record;
        writeBe16(&replacement.bytes,
                  flagsOffset,
                  set ? flags | kHfsCatalogHasAttributesMask
                      : flags & ~kHfsCatalogHasAttributesMask);
        HfsCatalogTreeEdit edit;
        edit.replacements.append(replacement);
        edit.read_back_parent_id = record->parent_id;
        edit.read_back_name = record->name;
        const auto mutation = finishCatalogTreeMutation(&*model, edit, blockers, warnings);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        return applyCatalogTreeMutation(*mutation,
                                        HfsCatalogCounterUpdate{0, 0, 0, m_volume.next_catalog_id},
                                        blockers);
    }

    [[nodiscard]] std::optional<int> applyAttributesTreeMutation(
        const HfsBTreeHeader& tree, const HfsExtentsTreeMutation& mutation, QStringList* blockers) {
        int chunks = 0;
        for (const auto& nodeWrite : mutation.node_writes) {
            const auto nodeChunks =
                writeAttributesNode(tree, nodeWrite.node_number, nodeWrite.bytes);
            if (!nodeChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *nodeChunks;
        }
        if (mutation.leaf_rewrite.has_value()) {
            const auto leafChunks = writeAttributesNode(tree,
                                                        mutation.leaf_rewrite->node_number,
                                                        mutation.leaf_rewrite->bytes);
            if (!leafChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *leafChunks;
        }
        const auto headerChunks = writeAttributesNode(tree, 0, mutation.header_node);
        if (!headerChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        chunks += *headerChunks;
        for (const auto& nodeWrite : mutation.post_commit_writes) {
            const auto eraseChunks =
                writeAttributesNode(tree, nodeWrite.node_number, nodeWrite.bytes);
            if (!eraseChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *eraseChunks;
        }
        return chunks;
    }

private:
    void appendAttributeCreateBlockers(uint32_t fileId,
                                       const QString& name,
                                       const QByteArray& data,
                                       const PartitionHfsFileWriteOptions& options,
                                       QStringList* blockers) {
        appendWriterOptionBlockers(data, options, blockers);
        appendAttributeWriteRequestBlockers(fileId, name, data, options, blockers);
        if (!options.allow_compressed_file_mutation &&
            name.trimmed() == QLatin1String(kHfsDecmpfsAttributeName)) {
            blockers->append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
        if (m_volume.attributes_fork.logical_size == 0 ||
            m_volume.attributes_fork.extents.isEmpty()) {
            blockers->append(
                QStringLiteral("HFS+ volume has no attributes B-tree for attribute create"));
        }
    }

    [[nodiscard]] bool attributeCreateTargetExists(uint32_t fileId, QStringList* blockers) {
        auto catalogModel = loadCatalogTreeForMutation(blockers);
        if (!catalogModel.has_value()) {
            return false;
        }
        const auto flattened = flattenedCatalogModelRecords(*catalogModel);
        const bool targetExists = std::any_of(
            flattened.cbegin(), flattened.cend(), [fileId](const HfsRawCatalogRecord& record) {
                return record.catalog_id == fileId &&
                       (record.record_type == kHfsCatalogFileRecord ||
                        record.record_type == kHfsCatalogFolderRecord);
            });
        if (!targetExists) {
            blockers->append(QStringLiteral("HFS+ catalog ID for attribute create was not found"));
        }
        return targetExists;
    }

public:
    [[nodiscard]] PartitionHfsAttributeWriteResult createInlineAttributeValue(
        uint32_t fileId,
        const QString& name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options) {
        PartitionHfsAttributeWriteResult result;
        result.file_system = m_volume.file_system;
        result.file_id = fileId;
        result.attribute_name = name.trimmed();
        result.evidence_id = options.evidence_id;
        appendAttributeCreateBlockers(fileId, name, data, options, &result.blockers);
        if (!result.blockers.isEmpty()) {
            return result;
        }
        if (fileHasAttribute(fileId, result.attribute_name)) {
            result.blockers.append(QStringLiteral("HFS+ attribute already exists"));
            return result;
        }
        if (!attributeCreateTargetExists(fileId, &result.blockers)) {
            return result;
        }
        HfsBTreeHeader tree;
        if (!loadAttributeHeader(&tree)) {
            result.blockers.append(m_blockers);
            return result;
        }
        const HfsAttributeRawRecord insertion{
            .file_id = fileId,
            .name = result.attribute_name,
            .bytes = inlineAttributeRecordBytes(fileId, result.attribute_name, data)};
        const auto mutation = prepareAttributesTreeInsert(tree, insertion, &result.blockers);
        if (!mutation.has_value()) {
            return result;
        }
        result.before_sha256 = sha256Hex(QByteArray());
        const auto chunks = applyAttributesTreeMutation(tree, *mutation, &result.blockers);
        if (!chunks.has_value()) {
            return result;
        }
        const auto flagChunks =
            setCatalogRecordAttributesFlag(fileId, true, &result.blockers, &result.warnings);
        if (!flagChunks.has_value()) {
            return result;
        }
        finishAttributeCreateResult(data, options, *mutation, *chunks + *flagChunks, &result);
        return result;
    }

private:
    void finishAttributeCreateResult(const QByteArray& data,
                                     const PartitionHfsFileWriteOptions& options,
                                     const HfsExtentsTreeMutation& mutation,
                                     int chunks,
                                     PartitionHfsAttributeWriteResult* result) {
        const auto readBack =
            readAttributeValue(result->file_id, result->attribute_name, options.max_write_bytes);
        if (!readBack.ok || readBack.storage != QStringLiteral("inline") || readBack.data != data) {
            result->blockers.append(
                QStringLiteral("HFS+ attribute create read-back verification failed"));
            result->blockers.append(readBack.blockers);
            result->blockers.append(m_blockers);
            return;
        }
        result->after_sha256 = sha256Hex(readBack.data);
        result->bytes_written = static_cast<uint64_t>(data.size());
        result->chunks_written = chunks;
        result->warnings = m_warnings;
        result->warnings.append(
            mutation.materialize
                ? QStringLiteral("HFS+ inline attribute created in a new attributes B-tree leaf")
            : mutation.split
                ? QStringLiteral("HFS+ inline attribute created with an attributes root-leaf split")
                : QStringLiteral("HFS+ inline attribute created"));
        syncAlternateVolumeHeader(result);
        result->ok = result->blockers.isEmpty();
    }

    [[nodiscard]] static QByteArray forkAttributeRecordBytes(uint32_t fileId,
                                                             const QString& name,
                                                             const HfsForkData& fork) {
        QByteArray record = attributeKeyBytes(fileId, name);
        QByteArray payload(kHfsAttributeForkDataOffset + kHfsForkDataBytes, '\0');
        writeBe32(&payload, 0, kHfsAttributeForkDataRecord);
        const QByteArray forkBytes = hfsForkDataRecordBytes(fork);
        payload.replace(kHfsAttributeForkDataOffset, forkBytes.size(), forkBytes);
        record.append(payload);
        return record;
    }

    struct HfsForkAttributeCreatePlan {
        HfsForkData fork;
        QVector<HfsAllocationBitmapByte> allocation_bytes;
        uint32_t allocated_blocks{0};
        HfsExtentsTreeMutation tree_mutation;
    };

    [[nodiscard]] std::optional<HfsForkAttributeCreatePlan> prepareForkAttributeCreate(
        const HfsBTreeHeader& tree,
        uint32_t fileId,
        const QString& name,
        const QByteArray& data,
        QStringList* blockers) {
        appendAllocationForkGrowthBlockers(blockers);
        const auto requiredBlocks = requiredAllocationBlocksForBytes(data, blockers);
        if (!requiredBlocks.has_value()) {
            return std::nullopt;
        }
        if (*requiredBlocks > m_volume.free_blocks) {
            blockers->append(QStringLiteral("HFS+ volume does not have enough free blocks"));
            return std::nullopt;
        }
        const auto extents = findFreeAllocationExtents(*requiredBlocks,
                                                       QStringLiteral("created attribute fork"),
                                                       blockers);
        if (!extents.has_value()) {
            return std::nullopt;
        }
        if (extents->size() > kHfsExtentCount) {
            blockers->append(QStringLiteral(
                "HFS+ attribute fork would need extents-overflow records; create is blocked"));
            return std::nullopt;
        }
        const auto allocationBytes = prepareAllocationBitmapSet(*extents, blockers);
        if (!allocationBytes.has_value()) {
            return std::nullopt;
        }
        HfsForkAttributeCreatePlan plan;
        plan.fork.logical_size = static_cast<uint64_t>(data.size());
        plan.fork.total_blocks = *requiredBlocks;
        plan.fork.extents = *extents;
        plan.allocation_bytes = *allocationBytes;
        plan.allocated_blocks = *requiredBlocks;
        const HfsAttributeRawRecord insertion{
            .file_id = fileId,
            .name = name,
            .bytes = forkAttributeRecordBytes(fileId, name, plan.fork)};
        const auto mutation = prepareAttributesTreeInsert(tree, insertion, blockers);
        if (!mutation.has_value()) {
            return std::nullopt;
        }
        plan.tree_mutation = *mutation;
        return plan;
    }

    [[nodiscard]] std::optional<int> applyForkAttributeCreate(
        const HfsBTreeHeader& tree,
        const HfsForkAttributeCreatePlan& plan,
        const QByteArray& data,
        QStringList* blockers) {
        const auto dataChunks = writeForkBytesWithinAllocated(
            plan.fork, kHfsAttributesFileId, kHfsDataForkType, 0, data);
        if (!dataChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto slackChunks =
            zeroForkAllocationSlack(plan.fork, kHfsAttributesFileId, kHfsDataForkType, data.size());
        if (!slackChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto bitmapChunks = writeUpdatedAllocationBitmapBytes(plan.allocation_bytes);
        if (!bitmapChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto treeChunks = applyAttributesTreeMutation(tree, plan.tree_mutation, blockers);
        if (!treeChunks.has_value()) {
            return std::nullopt;
        }
        const auto freeBlockChunks =
            writeVolumeHeaderCounter(kHfsFreeBlocksOffset,
                                     m_volume.free_blocks,
                                     -static_cast<int>(plan.allocated_blocks),
                                     blockers);
        if (!freeBlockChunks.has_value()) {
            return std::nullopt;
        }
        m_volume.free_blocks -= plan.allocated_blocks;
        return *dataChunks + *slackChunks + *bitmapChunks + *treeChunks + *freeBlockChunks;
    }

public:
    [[nodiscard]] PartitionHfsAttributeWriteResult createForkAttributeValue(
        uint32_t fileId,
        const QString& name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options) {
        PartitionHfsAttributeWriteResult result;
        result.file_system = m_volume.file_system;
        result.file_id = fileId;
        result.attribute_name = name.trimmed();
        result.evidence_id = options.evidence_id;
        appendAttributeCreateBlockers(fileId, name, data, options, &result.blockers);
        if (data.isEmpty()) {
            result.blockers.append(
                QStringLiteral("HFS+ fork-backed attribute create requires a nonempty payload"));
        }
        if (!result.blockers.isEmpty()) {
            return result;
        }
        if (fileHasAttribute(fileId, result.attribute_name)) {
            result.blockers.append(QStringLiteral("HFS+ attribute already exists"));
            return result;
        }
        if (!attributeCreateTargetExists(fileId, &result.blockers)) {
            return result;
        }
        HfsBTreeHeader tree;
        if (!loadAttributeHeader(&tree)) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto plan =
            prepareForkAttributeCreate(tree, fileId, result.attribute_name, data, &result.blockers);
        if (!plan.has_value()) {
            return result;
        }
        result.before_sha256 = sha256Hex(QByteArray());
        const auto chunks = applyForkAttributeCreate(tree, *plan, data, &result.blockers);
        if (!chunks.has_value()) {
            return result;
        }
        const auto flagChunks =
            setCatalogRecordAttributesFlag(fileId, true, &result.blockers, &result.warnings);
        if (!flagChunks.has_value()) {
            return result;
        }
        if (!verifyAttributeCreateReadBack(QStringLiteral("fork"), data, options, &result)) {
            return result;
        }
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *chunks + *flagChunks;
        result.warnings = m_warnings;
        result.warnings.append(
            QStringLiteral("HFS+ fork-backed attribute created with %1 allocated blocks")
                .arg(plan->allocated_blocks));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

private:
    [[nodiscard]] bool verifyAttributeCreateReadBack(const QString& expectedStorage,
                                                     const QByteArray& data,
                                                     const PartitionHfsFileWriteOptions& options,
                                                     PartitionHfsAttributeWriteResult* result) {
        const auto readBack =
            readAttributeValue(result->file_id, result->attribute_name, options.max_write_bytes);
        if (!readBack.ok || readBack.storage != expectedStorage || readBack.data != data) {
            result->blockers.append(
                QStringLiteral("HFS+ attribute create read-back verification failed"));
            result->blockers.append(readBack.blockers);
            result->blockers.append(m_blockers);
            return false;
        }
        result->after_sha256 = sha256Hex(readBack.data);
        return true;
    }

public:
private:
    struct HfsAttributeDeleteSelection {
        QVector<HfsAttributeRawRecord> remaining;
        QByteArray removed_bytes;
        HfsForkData removed_fork;
        bool fork_backed{false};
        bool last_attribute_for_file{false};
    };

    [[nodiscard]] bool partitionAttributeRecordsForDelete(
        const QVector<HfsAttributeRawRecord>& records,
        uint32_t fileId,
        const QString& name,
        HfsAttributeDeleteSelection* selection) const {
        bool found = false;
        int remainingForFile = 0;
        for (const auto& record : records) {
            if (compareAttributeRawKeys(record.file_id, record.name, fileId, name) == 0) {
                selection->removed_bytes = record.bytes;
                found = true;
                continue;
            }
            if (record.file_id == fileId) {
                ++remainingForFile;
            }
            selection->remaining.append(record);
        }
        selection->last_attribute_for_file = remainingForFile == 0;
        return found;
    }

    [[nodiscard]] bool parseDeletedAttributePayload(HfsAttributeDeleteSelection* selection,
                                                    QStringList* blockers) const {
        const uint16_t keyLength = be16(selection->removed_bytes, 0);
        const qsizetype typeOffset = kUint16Size + keyLength;
        if (!hasBytes(selection->removed_bytes, typeOffset, kUint32Size)) {
            blockers->append(QStringLiteral("HFS+ attribute record for delete is invalid"));
            return false;
        }
        const uint32_t recordType = be32(selection->removed_bytes, typeOffset);
        if (recordType == kHfsAttributeForkDataRecord) {
            const qsizetype forkOffset = typeOffset + kHfsAttributeForkDataOffset;
            if (!hasBytes(selection->removed_bytes, forkOffset, kHfsForkDataBytes)) {
                blockers->append(QStringLiteral("HFS+ attribute fork data for delete is invalid"));
                return false;
            }
            selection->fork_backed = true;
            selection->removed_fork = parseFork(selection->removed_bytes, forkOffset);
            return true;
        }
        if (recordType != kHfsAttributeInlineDataRecord) {
            blockers->append(QStringLiteral(
                "HFS+ attribute delete supports only inline and fork-backed records"));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<HfsAttributeDeleteSelection> selectAttributeForDelete(
        const HfsBTreeHeader& tree, uint32_t fileId, const QString& name, QStringList* blockers) {
        if (tree.leaf_records == 0 || tree.first_leaf_node == 0 || tree.tree_depth != 1 ||
            tree.first_leaf_node != tree.last_leaf_node || tree.root_node != tree.first_leaf_node) {
            blockers->append(QStringLiteral(
                "HFS+ attribute delete currently requires a single-leaf attributes B-tree"));
            return std::nullopt;
        }
        const auto records = loadSingleAttributesLeafRecords(tree, tree.first_leaf_node, blockers);
        if (!records.has_value()) {
            return std::nullopt;
        }
        HfsAttributeDeleteSelection selection;
        if (!partitionAttributeRecordsForDelete(*records, fileId, name, &selection)) {
            blockers->append(QStringLiteral("HFS+ attribute for delete was not found"));
            return std::nullopt;
        }
        if (!parseDeletedAttributePayload(&selection, blockers)) {
            return std::nullopt;
        }
        return selection;
    }

    [[nodiscard]] std::optional<HfsExtentsTreeMutation> prepareAttributesTreeRemove(
        const HfsBTreeHeader& tree,
        const QVector<HfsAttributeRawRecord>& remaining,
        QStringList* blockers) {
        auto header = loadAttributesHeaderNodeForMutation(tree, blockers);
        if (!header.has_value()) {
            return std::nullopt;
        }
        HfsExtentsTreeMutation mutation;
        mutation.updated_header = tree;
        const qsizetype headerRecord = kBTreeHeaderRecordOffset;
        if (remaining.isEmpty()) {
            HfsBTreeHeaderNodeContext updated = *header;
            writeBe16(&updated.node, headerRecord + kBTreeHeaderTreeDepthOffset, 0);
            writeBe32(&updated.node, headerRecord + kBTreeHeaderRootNodeOffset, 0);
            writeBe32(&updated.node, headerRecord + kBTreeHeaderLeafRecordsOffset, 0);
            writeBe32(&updated.node, headerRecord + kBTreeHeaderFirstLeafNodeOffset, 0);
            writeBe32(&updated.node, headerRecord + kBTreeHeaderLastLeafNodeOffset, 0);
            writeBe32(&updated.node,
                      headerRecord + kBTreeHeaderFreeNodesOffset,
                      tree.free_nodes + 1);
            writeHeaderMapBit(&updated, tree.first_leaf_node, false);
            mutation.free_tree = true;
            mutation.post_commit_writes.append(
                HfsBTreeNodeWrite{tree.first_leaf_node, QByteArray(tree.node_size, '\0')});
            mutation.header_node = updated.node;
            mutation.updated_header.tree_depth = 0;
            mutation.updated_header.root_node = 0;
            mutation.updated_header.leaf_records = 0;
            mutation.updated_header.first_leaf_node = 0;
            mutation.updated_header.last_leaf_node = 0;
            mutation.updated_header.free_nodes = tree.free_nodes + 1;
            return mutation;
        }
        const auto leafBytes = buildAttributesLeafNodeBytes(tree, 0, 0, remaining);
        if (!leafBytes.has_value()) {
            blockers->append(QStringLiteral("HFS+ attributes leaf rebuild failed"));
            return std::nullopt;
        }
        HfsBTreeHeaderNodeContext updated = *header;
        writeBe32(&updated.node,
                  headerRecord + kBTreeHeaderLeafRecordsOffset,
                  static_cast<uint32_t>(remaining.size()));
        mutation.leaf_rewrite = HfsBTreeNodeWrite{tree.first_leaf_node, *leafBytes};
        mutation.header_node = updated.node;
        mutation.updated_header.leaf_records = static_cast<uint32_t>(remaining.size());
        return mutation;
    }

public:
    [[nodiscard]] PartitionHfsAttributeWriteResult deleteAttributeValue(
        uint32_t fileId, const QString& name, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsAttributeWriteResult result;
        result.file_system = m_volume.file_system;
        result.file_id = fileId;
        result.attribute_name = name.trimmed();
        result.evidence_id = options.evidence_id;
        if (!beginAttributeDelete(options, &result)) {
            return result;
        }
        HfsBTreeHeader tree;
        if (!loadAttributeHeader(&tree)) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto selection =
            selectAttributeForDelete(tree, fileId, result.attribute_name, &result.blockers);
        if (!selection.has_value()) {
            return result;
        }
        const auto mutation =
            prepareAttributesTreeRemove(tree, selection->remaining, &result.blockers);
        if (!mutation.has_value()) {
            return result;
        }
        std::optional<QVector<HfsAllocationBitmapByte>> releaseBytes;
        if (selection->fork_backed) {
            releaseBytes = prepareAllocationBitmapClear(selection->removed_fork.extents,
                                                        &result.blockers);
            if (!releaseBytes.has_value()) {
                return result;
            }
        }
        const auto chunks = applyAttributesTreeMutation(tree, *mutation, &result.blockers);
        if (!chunks.has_value()) {
            return result;
        }
        const auto releaseChunks =
            applyDeletedForkRelease(*selection, releaseBytes, &result.blockers);
        if (!releaseChunks.has_value()) {
            return result;
        }
        finishAttributeDeleteResult(
            *selection, mutation->free_tree, *chunks + *releaseChunks, &result);
        return result;
    }

private:
    [[nodiscard]] bool beginAttributeDelete(const PartitionHfsFileWriteOptions& options,
                                            PartitionHfsAttributeWriteResult* result) {
        appendAttributeWriteRequestBlockers(
            result->file_id, result->attribute_name, QByteArray(), options, &result->blockers);
        appendWriterActivationBlockers(options, &result->blockers);
        appendWriterDeviceBlockers(&result->blockers);
        if (!options.allow_compressed_file_mutation &&
            result->attribute_name == QLatin1String(kHfsDecmpfsAttributeName)) {
            result->blockers.append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
        if (!result->blockers.isEmpty()) {
            return false;
        }
        const auto before =
            readAttributeValue(result->file_id, result->attribute_name, options.max_write_bytes);
        if (!before.ok) {
            result->blockers.append(QStringLiteral("HFS+ attribute for delete was not found"));
            result->blockers.append(before.blockers);
            return false;
        }
        result->before_sha256 = sha256Hex(before.data);
        return true;
    }

    [[nodiscard]] std::optional<int> applyDeletedForkRelease(
        const HfsAttributeDeleteSelection& selection,
        const std::optional<QVector<HfsAllocationBitmapByte>>& releaseBytes,
        QStringList* blockers) {
        if (!selection.fork_backed) {
            return 0;
        }
        const auto bitmapChunks = writeUpdatedAllocationBitmapBytes(*releaseBytes);
        if (!bitmapChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto freeBlockChunks =
            writeVolumeHeaderCounter(kHfsFreeBlocksOffset,
                                     m_volume.free_blocks,
                                     static_cast<int>(selection.removed_fork.total_blocks),
                                     blockers);
        if (!freeBlockChunks.has_value()) {
            return std::nullopt;
        }
        m_volume.free_blocks += selection.removed_fork.total_blocks;
        return *bitmapChunks + *freeBlockChunks;
    }

    void finishAttributeDeleteResult(const HfsAttributeDeleteSelection& selection,
                                     bool treeFreed,
                                     int chunks,
                                     PartitionHfsAttributeWriteResult* result) {
        int flagChunks = 0;
        if (selection.last_attribute_for_file) {
            const auto cleared = setCatalogRecordAttributesFlag(
                result->file_id, false, &result->blockers, &result->warnings);
            if (!cleared.has_value()) {
                return;
            }
            flagChunks = *cleared;
        }
        if (fileHasAttribute(result->file_id, result->attribute_name)) {
            result->blockers.append(
                QStringLiteral("HFS+ attribute delete read-back verification failed"));
            result->blockers.append(m_blockers);
            return;
        }
        result->after_sha256 = sha256Hex(QByteArray());
        result->bytes_written = 0;
        result->chunks_written = chunks + flagChunks;
        result->warnings = m_warnings;
        result->warnings.append(
            treeFreed
                ? QStringLiteral("HFS+ attribute deleted; empty attributes B-tree leaf was freed")
                : QStringLiteral("HFS+ attribute deleted"));
        if (selection.fork_backed) {
            result->warnings.append(
                QStringLiteral("HFS+ attribute fork released %1 allocation blocks")
                    .arg(selection.removed_fork.total_blocks));
        }
        syncAlternateVolumeHeader(result);
        result->ok = result->blockers.isEmpty();
    }

public:
private:
private:
    // ---- HFS+ journal replay (little-endian journals, in-filesystem only) ----

    [[nodiscard]] static uint32_t hfsJournalChecksum(const QByteArray& data) {
        uint32_t cksum = 0;
        for (const char byte : data) {
            cksum = (cksum << kBitsPerByte) ^ (cksum + static_cast<uint8_t>(byte));
        }
        return ~cksum;
    }

    struct HfsJournalRegion {
        uint64_t journal_offset{0};
        uint64_t header_bytes{0};
        uint64_t size{0};
    };

    [[nodiscard]] static uint64_t journalAdvance(const HfsJournalRegion& region,
                                                 uint64_t position,
                                                 uint64_t delta) {
        uint64_t next = position + delta;
        const uint64_t dataBytes = region.size - region.header_bytes;
        while (next >= region.size) {
            next = region.header_bytes + (next - region.size);
            if (dataBytes == 0) {
                break;
            }
        }
        return next;
    }

    [[nodiscard]] std::optional<QByteArray> readJournalCircular(const HfsJournalRegion& region,
                                                                uint64_t position,
                                                                uint64_t length) {
        QByteArray output;
        output.reserve(static_cast<qsizetype>(length));
        uint64_t cursor = position;
        uint64_t remaining = length;
        while (remaining > 0) {
            const uint64_t available = region.size - cursor;
            const uint64_t take = std::min(remaining, available);
            const auto bytes = readAt(m_volume.volume_offset + region.journal_offset + cursor,
                                      take);
            if (!bytes.has_value()) {
                return std::nullopt;
            }
            output.append(*bytes);
            remaining -= take;
            cursor = take == available ? region.header_bytes : cursor + take;
        }
        return output;
    }

    struct HfsJournalState {
        HfsJournalRegion region;
        uint64_t start{0};
        uint64_t end{0};
        uint32_t blhdr_size{0};
        QByteArray header;
        bool needs_init{false};
    };

    [[nodiscard]] std::optional<HfsJournalState> loadJournalInfoBlock(QStringList* blockers) {
        if ((m_volume.attributes & kHfsVolumeJournaledMask) == 0) {
            blockers->append(QStringLiteral("HFS+ volume is not journaled"));
            return std::nullopt;
        }
        const auto volumeHeader = readVolumeHeaderAt(m_volume.volume_offset);
        if (!volumeHeader.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const uint32_t jibBlock = be32(*volumeHeader, kHfsVolumeJournalInfoBlockOffset);
        uint64_t jibOffset = 0;
        if (jibBlock == 0 || !checkedMul(jibBlock, m_volume.block_size, &jibOffset)) {
            blockers->append(QStringLiteral("HFS+ journal info block is unavailable"));
            return std::nullopt;
        }
        const auto jib = readAt(m_volume.volume_offset + jibOffset,
                                kHfsJournalInfoSizeField + kUint64Size);
        if (!jib.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return journalStateFromInfoBlock(*jib, blockers);
    }

    [[nodiscard]] std::optional<HfsJournalState> journalStateFromInfoBlock(
        const QByteArray& jib, QStringList* blockers) const {
        const uint32_t flags = be32(jib, 0);
        if ((flags & kHfsJournalInFsMask) == 0 || (flags & kHfsJournalOnOtherDeviceMask) != 0) {
            blockers->append(
                QStringLiteral("HFS+ journal replay requires an in-filesystem journal"));
            return std::nullopt;
        }
        HfsJournalState state;
        if ((flags & kHfsJournalNeedInitMask) != 0) {
            // A journal awaiting initialization has never logged a transaction.
            state.needs_init = true;
            return state;
        }
        state.region.journal_offset = be64(jib, kHfsJournalInfoOffsetField);
        state.region.size = be64(jib, kHfsJournalInfoSizeField);
        uint64_t totalBytes = 0;
        uint64_t journalEnd = 0;
        if (!checkedMul(m_volume.total_blocks, m_volume.block_size, &totalBytes) ||
            !checkedAdd(state.region.journal_offset, state.region.size, &journalEnd) ||
            state.region.size < kHfsJournalHeaderBytes || journalEnd > totalBytes) {
            blockers->append(QStringLiteral("HFS+ journal location is invalid"));
            return std::nullopt;
        }
        return state;
    }

    [[nodiscard]] bool parseJournalHeaderState(HfsJournalState* state, QStringList* blockers) {
        const auto header = readAt(m_volume.volume_offset + state->region.journal_offset,
                                   kHfsJournalHeaderBytes);
        if (!header.has_value()) {
            blockers->append(m_blockers);
            return false;
        }
        if (le32(*header, 0) != kHfsJournalHeaderMagic) {
            blockers->append(QStringLiteral("HFS+ journal header magic is invalid"));
            return false;
        }
        if (le32(*header, kUint32Size) != kHfsJournalEndianMagic) {
            blockers->append(
                QStringLiteral("HFS+ big-endian journals are not supported for replay"));
            return false;
        }
        QByteArray checksumInput = *header;
        writeLe32(&checksumInput, kHfsJournalHeaderChecksumField, 0);
        if (hfsJournalChecksum(checksumInput) != le32(*header, kHfsJournalHeaderChecksumField)) {
            blockers->append(QStringLiteral("HFS+ journal header checksum is invalid"));
            return false;
        }
        state->start = le64(*header, kHfsJournalHeaderStartField);
        state->end = le64(*header, kHfsJournalHeaderEndField);
        state->blhdr_size = le32(*header, kHfsJournalHeaderBlhdrSizeField);
        state->region.header_bytes = le32(*header, kHfsJournalHeaderJhdrSizeField);
        state->header = *header;
        return validateJournalHeaderGeometry(*state, blockers);
    }

    [[nodiscard]] static bool journalPointerInRange(const HfsJournalState& state,
                                                    uint64_t pointer) {
        return pointer >= state.region.header_bytes && pointer <= state.region.size;
    }

    [[nodiscard]] bool validateJournalHeaderGeometry(const HfsJournalState& state,
                                                     QStringList* blockers) const {
        if (le64(state.header, kHfsJournalHeaderSizeField) != state.region.size ||
            state.region.header_bytes < kHfsJournalHeaderBytes ||
            state.region.header_bytes >= state.region.size ||
            state.blhdr_size < kHfsJournalBlockInfoOffset + kHfsJournalBlockInfoBytes ||
            !journalPointerInRange(state, state.start) ||
            !journalPointerInRange(state, state.end)) {
            blockers->append(QStringLiteral("HFS+ journal header geometry is invalid"));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<HfsJournalState> loadJournalState(QStringList* blockers) {
        auto state = loadJournalInfoBlock(blockers);
        if (!state.has_value() || state->needs_init) {
            return state;
        }
        if (!parseJournalHeaderState(&*state, blockers)) {
            return std::nullopt;
        }
        return state;
    }

    struct HfsJournalReplayProgress {
        uint64_t position{0};
        uint64_t written{0};
        uint64_t volume_bytes{0};
        uint64_t max_write_bytes{0};
        int blocks{0};
    };

    [[nodiscard]] std::optional<QByteArray> readJournalTransactionHeader(
        const HfsJournalState& state, uint64_t position, QStringList* blockers) {
        const auto blhdr = readJournalCircular(state.region, position, state.blhdr_size);
        if (!blhdr.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        QByteArray checksumInput = blhdr->left(kHfsJournalBlhdrChecksumBytes);
        writeLe32(&checksumInput, kHfsJournalBlhdrChecksumField, 0);
        if (hfsJournalChecksum(checksumInput) != le32(*blhdr, kHfsJournalBlhdrChecksumField)) {
            blockers->append(QStringLiteral("HFS+ journal transaction checksum is invalid"));
            return std::nullopt;
        }
        const uint16_t numBlocks = le16(*blhdr, kHfsJournalBlhdrNumBlocksField);
        const uint32_t bytesUsed = le32(*blhdr, kHfsJournalBlhdrBytesUsedField);
        const qsizetype infoCapacity =
            (static_cast<qsizetype>(state.blhdr_size) - kHfsJournalBlockInfoOffset) /
            kHfsJournalBlockInfoBytes;
        if (numBlocks == 0 || numBlocks > infoCapacity ||
            numBlocks > kHfsJournalMaxBlocksPerTransaction || bytesUsed < state.blhdr_size) {
            blockers->append(QStringLiteral("HFS+ journal transaction header is invalid"));
            return std::nullopt;
        }
        return blhdr;
    }

    [[nodiscard]] bool replayJournalBlock(const QByteArray& data,
                                          uint64_t blockNumber,
                                          HfsJournalReplayProgress* progress,
                                          QStringList* blockers) {
        uint64_t destination = 0;
        uint64_t destinationEnd = 0;
        if (!checkedMul(blockNumber, kHfsJournalSectorBytes, &destination) ||
            !checkedAdd(destination, static_cast<uint64_t>(data.size()), &destinationEnd) ||
            destinationEnd > progress->volume_bytes) {
            blockers->append(
                QStringLiteral("HFS+ journal transaction targets blocks outside the volume"));
            return false;
        }
        progress->written += static_cast<uint64_t>(data.size());
        if (progress->written > progress->max_write_bytes) {
            blockers->append(
                QStringLiteral("HFS+ journal replay exceeds the configured write cap"));
            return false;
        }
        if (!writeAt(m_volume.volume_offset + destination,
                     data.constData(),
                     static_cast<uint64_t>(data.size()))) {
            blockers->append(m_blockers);
            return false;
        }
        ++progress->blocks;
        return true;
    }

    [[nodiscard]] bool replayJournalTransaction(const HfsJournalState& state,
                                                const QByteArray& blhdr,
                                                HfsJournalReplayProgress* progress,
                                                QStringList* blockers) {
        const uint16_t numBlocks = le16(blhdr, kHfsJournalBlhdrNumBlocksField);
        uint64_t dataPosition = journalAdvance(state.region, progress->position, state.blhdr_size);
        for (uint16_t index = 1; index < numBlocks; ++index) {
            const qsizetype info = kHfsJournalBlockInfoOffset + index * kHfsJournalBlockInfoBytes;
            const uint64_t blockNumber = le64(blhdr, info);
            const uint32_t blockSize = le32(blhdr, info + kHfsJournalBlockInfoSizeField);
            if (blockSize == 0) {
                continue;
            }
            const auto data = readJournalCircular(state.region, dataPosition, blockSize);
            if (!data.has_value()) {
                blockers->append(m_blockers);
                return false;
            }
            dataPosition = journalAdvance(state.region, dataPosition, blockSize);
            if (blockNumber == std::numeric_limits<uint64_t>::max()) {
                continue;
            }
            if (!replayJournalBlock(*data, blockNumber, progress, blockers)) {
                return false;
            }
        }
        progress->position = journalAdvance(state.region,
                                            progress->position,
                                            le32(blhdr, kHfsJournalBlhdrBytesUsedField));
        return true;
    }

    [[nodiscard]] std::optional<QPair<int, int>> replayJournalTransactions(
        const HfsJournalState& state, uint64_t maxWriteBytes, QStringList* blockers) {
        HfsJournalReplayProgress progress;
        progress.position = state.start;
        progress.max_write_bytes = maxWriteBytes;
        if (!checkedMul(m_volume.total_blocks, m_volume.block_size, &progress.volume_bytes)) {
            blockers->append(QStringLiteral("HFS+ volume size overflow"));
            return std::nullopt;
        }
        int transactions = 0;
        while (progress.position != state.end) {
            if (++transactions > kHfsJournalMaxTransactions) {
                blockers->append(QStringLiteral("HFS+ journal transaction limit exceeded"));
                return std::nullopt;
            }
            const auto blhdr = readJournalTransactionHeader(state, progress.position, blockers);
            if (!blhdr.has_value()) {
                return std::nullopt;
            }
            if (!replayJournalTransaction(state, *blhdr, &progress, blockers)) {
                return std::nullopt;
            }
        }
        return QPair<int, int>{transactions, progress.blocks};
    }

    [[nodiscard]] bool writeCleanJournalHeader(const HfsJournalState& state,
                                               QStringList* blockers) {
        QByteArray updatedHeader = state.header;
        qToLittleEndian<quint64>(state.end, updatedHeader.data() + kHfsJournalHeaderStartField);
        writeLe32(&updatedHeader, kHfsJournalHeaderChecksumField, 0);
        writeLe32(&updatedHeader,
                  kHfsJournalHeaderChecksumField,
                  hfsJournalChecksum(updatedHeader));
        if (!writeAt(m_volume.volume_offset + state.region.journal_offset,
                     updatedHeader.constData(),
                     static_cast<uint64_t>(updatedHeader.size()))) {
            blockers->append(m_blockers);
            return false;
        }
        const auto verify = readAt(m_volume.volume_offset + state.region.journal_offset,
                                   kHfsJournalHeaderBytes);
        if (!verify.has_value() || le64(*verify, kHfsJournalHeaderStartField) !=
                                       le64(*verify, kHfsJournalHeaderEndField)) {
            blockers->append(QStringLiteral("HFS+ journal replay read-back verification failed"));
            return false;
        }
        return true;
    }

public:
    [[nodiscard]] PartitionHfsFileWriteResult replayJournal(
        const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = QStringLiteral("(journal)");
        result.evidence_id = options.evidence_id;
        appendWriterActivationBlockers(options, &result.blockers);
        appendWriterDeviceBlockers(&result.blockers);
        if (options.max_write_bytes == 0) {
            result.blockers.append(
                QStringLiteral("HFS+ journal replay requires a nonzero write cap"));
        }
        if (!result.blockers.isEmpty()) {
            return result;
        }
        const auto state = loadJournalState(&result.blockers);
        if (!state.has_value()) {
            if (result.blockers.isEmpty()) {
                result.blockers.append(QStringLiteral("HFS+ journal read failed"));
            }
            return result;
        }
        if (state->needs_init) {
            result.warnings.append(
                QStringLiteral("HFS+ journal awaits initialization; nothing to replay"));
            result.ok = true;
            return result;
        }
        if (state->start == state->end) {
            result.warnings.append(QStringLiteral("HFS+ journal is clean; nothing to replay"));
            result.ok = true;
            return result;
        }
        const auto replayed =
            replayJournalTransactions(*state, options.max_write_bytes, &result.blockers);
        if (!replayed.has_value()) {
            if (result.blockers.isEmpty()) {
                result.blockers.append(QStringLiteral("HFS+ journal replay failed"));
            }
            return result;
        }
        if (!writeCleanJournalHeader(*state, &result.blockers)) {
            return result;
        }
        result.chunks_written = replayed->second + 1;
        result.warnings.append(QStringLiteral("HFS+ journal replayed: %1 transactions, %2 blocks")
                                   .arg(replayed->first)
                                   .arg(replayed->second));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

private:
    [[nodiscard]] std::optional<int> writeExtentsNode(uint32_t nodeNumber, const QByteArray& node) {
        uint64_t offset = 0;
        if (!checkedMul(nodeNumber, m_extents.node_size, &offset)) {
            m_blockers.append(QStringLiteral("HFS+ extents overflow node offset overflow"));
            return std::nullopt;
        }
        return writeForkBytes(
            m_volume.extents_fork, kHfsExtentsFileId, kHfsDataForkType, offset, node);
    }

    [[nodiscard]] std::optional<int> applyExtentsTreeMutation(
        const HfsExtentsTreeMutation& mutation, QStringList* blockers) {
        int chunks = 0;
        for (const auto& nodeWrite : mutation.node_writes) {
            const auto nodeChunks = writeExtentsNode(nodeWrite.node_number, nodeWrite.bytes);
            if (!nodeChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *nodeChunks;
        }
        if (mutation.leaf_rewrite.has_value()) {
            const auto leafChunks = writeExtentsNode(mutation.leaf_rewrite->node_number,
                                                     mutation.leaf_rewrite->bytes);
            if (!leafChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *leafChunks;
        }
        if (!mutation.header_node.isEmpty()) {
            const auto headerChunks = writeExtentsNode(0, mutation.header_node);
            if (!headerChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *headerChunks;
        }
        m_extents = mutation.updated_header;
        m_overflow_extents = mutation.final_records;
        for (const auto& nodeWrite : mutation.post_commit_writes) {
            const auto eraseChunks = writeExtentsNode(nodeWrite.node_number, nodeWrite.bytes);
            if (!eraseChunks.has_value()) {
                blockers->append(m_blockers);
                return std::nullopt;
            }
            chunks += *eraseChunks;
        }
        return chunks;
    }

    template <typename ResultType>
    void syncAlternateVolumeHeader(ResultType* result) {
        const auto primary = readAt(m_volume.volume_offset + kHfsVolumeHeaderOffset,
                                    kHfsVolumeHeaderSize);
        uint64_t totalBytes = 0;
        if (!primary.has_value() ||
            !checkedMul(m_volume.total_blocks, m_volume.block_size, &totalBytes) ||
            totalBytes < kHfsMinimumVolumeBytesForAlternateHeader) {
            result->warnings.append(
                QStringLiteral("HFS+ alternate volume header synchronization failed"));
            return;
        }
        const uint64_t alternateOffset = m_volume.volume_offset + totalBytes -
                                         kHfsVolumeHeaderOffset;
        if (!writeAt(
                alternateOffset, primary->constData(), static_cast<uint64_t>(primary->size()))) {
            result->warnings.append(
                QStringLiteral("HFS+ alternate volume header synchronization failed"));
            return;
        }
        result->chunks_written += 1;
        result->warnings.append(QStringLiteral("HFS+ alternate volume header synchronized"));
    }

    [[nodiscard]] std::optional<int> writeVolumeHeaderCounter(qsizetype offset,
                                                              uint32_t current,
                                                              int delta,
                                                              QStringList* blockers) {
        if (delta > 0 &&
            static_cast<uint32_t>(delta) > std::numeric_limits<uint32_t>::max() - current) {
            blockers->append(QStringLiteral("HFS+ volume header counter overflow"));
            return std::nullopt;
        }
        const uint32_t updated =
            delta < 0 ? current - std::min<uint32_t>(current, static_cast<uint32_t>(-delta))
                      : current + static_cast<uint32_t>(delta);
        const auto chunks = writeVolumeHeaderU32(offset, updated);
        if (!chunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return chunks;
    }

    [[nodiscard]] std::optional<int> writeCatalogLeafNode(uint32_t nodeNumber,
                                                          const QByteArray& node) {
        uint64_t offset = 0;
        if (!checkedMul(nodeNumber, m_catalog.node_size, &offset)) {
            m_blockers.append(QStringLiteral("HFS+ catalog node offset overflow"));
            return std::nullopt;
        }
        return writeForkBytes(
            m_volume.catalog_fork, kHfsCatalogFileId, kHfsDataForkType, offset, node);
    }

    [[nodiscard]] std::optional<int> writeVolumeHeaderU32(qsizetype fieldOffset, uint32_t value) {
        uint64_t headerOffset = 0;
        if (!checkedAdd(m_volume.volume_offset, kHfsVolumeHeaderOffset, &headerOffset) ||
            !checkedAdd(headerOffset, static_cast<uint64_t>(fieldOffset), &headerOffset)) {
            m_blockers.append(QStringLiteral("HFS+ volume header field offset overflow"));
            return std::nullopt;
        }
        const QByteArray bytes = be32Bytes(value);
        if (!writeAt(headerOffset, bytes.constData(), bytes.size())) {
            return std::nullopt;
        }
        if (fieldOffset == kHfsFileCountOffset) {
            m_volume.file_count = value;
        } else if (fieldOffset == kHfsFolderCountOffset) {
            m_volume.folder_count = value;
        } else if (fieldOffset == kHfsNextCatalogIdOffset) {
            m_volume.next_catalog_id = value;
        } else if (fieldOffset == kHfsFreeBlocksOffset) {
            m_volume.free_blocks = value;
        }
        return 1;
    }

    [[nodiscard]] PartitionHfsFileWriteResult replaceCatalogForkWithinAllocatedBlocks(
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options,
        HfsForkSelector selector) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.catalog_id = record->catalog_id;
        appendWriterResizeRecordBlockers(*record, data, options, selector, &result.blockers);
        if (!result.blockers.isEmpty()) {
            result.blockers.append(m_blockers);
            return result;
        }

        const auto& fork = catalogForkFor(*record, selector);
        const uint8_t forkType = catalogForkType(selector);
        const uint64_t forkSize = catalogForkSize(*record, selector);
        const auto before = readForkBytes(fork, record->catalog_id, forkType, 0, forkSize);
        if (!before.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.before_sha256 = sha256Hex(*before);

        const auto dataChunks =
            writeForkBytesWithinAllocated(fork, record->catalog_id, forkType, 0, data);
        if (!dataChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto zeroChunks =
            zeroStaleTail(*record, selector, static_cast<uint64_t>(data.size()));
        if (!zeroChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto metadataChunks =
            updateCatalogFileForkLogicalSize(*record, selector, static_cast<uint64_t>(data.size()));
        if (!metadataChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }

        HfsForkData resizedFork = fork;
        resizedFork.logical_size = static_cast<uint64_t>(data.size());
        const auto after =
            readForkBytes(resizedFork, record->catalog_id, forkType, 0, resizedFork.logical_size);
        if (!after.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        if (*after != data) {
            result.blockers.append(
                QStringLiteral("HFS+ resize write read-back verification failed"));
            result.blockers.append(m_blockers);
            return result;
        }

        result.after_sha256 = sha256Hex(*after);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *dataChunks + *zeroChunks + *metadataChunks;
        result.warnings = m_warnings;
        result.warnings.append(allocatedForkResizeWarning(selector));
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsFileWriteResult replaceCatalogForkWithAllocationGrowth(
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options,
        HfsForkSelector selector) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.catalog_id = record->catalog_id;
        const auto plan =
            prepareForkAllocationGrowth(*record, data, options, selector, &result.blockers);
        if (!plan.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }

        const auto before = readCatalogForkBytes(*record, selector, &result.blockers);
        if (!before.has_value()) {
            return result;
        }
        result.before_sha256 = sha256Hex(*before);

        const auto chunksWritten =
            writeForkAllocationGrowthPlan(*record, selector, data, *plan, &result.blockers);
        if (!chunksWritten.has_value()) {
            return result;
        }

        const auto after =
            readBackForkAllocationGrowth(*record, selector, data, *plan, &result.blockers);
        if (!after.has_value()) {
            return result;
        }
        if (!appendForkGrowthCounterReadBack(plan->allocated_blocks, &result)) {
            return result;
        }

        result.after_sha256 = sha256Hex(*after);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *chunksWritten;
        result.warnings = m_warnings;
        result.warnings.append(
            plan->extents_mutation.has_value()
                ? QStringLiteral("HFS+ %1 grew by allocating %2 new block(s) with %3 "
                                 "extents-overflow record(s)")
                      .arg(hfsForkLabel(selector))
                      .arg(plan->allocated_blocks)
                      .arg(plan->overflow_records.size())
                : QStringLiteral(
                      "HFS+ %1 grew by allocating %2 new block(s) inside initial extent records")
                      .arg(hfsForkLabel(selector))
                      .arg(plan->allocated_blocks));
        syncAlternateVolumeHeader(&result);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] std::optional<QByteArray> readCatalogForkBytes(const HfsCatalogRecord& record,
                                                                 HfsForkSelector selector,
                                                                 QStringList* blockers) {
        const auto& fork = catalogForkFor(record, selector);
        const uint8_t forkType = catalogForkType(selector);
        const uint64_t size = catalogForkSize(record, selector);
        const auto bytes = readForkBytes(fork, record.catalog_id, forkType, 0, size);
        if (!bytes.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return bytes;
    }

    [[nodiscard]] std::optional<int> writeForkAllocationGrowthPlan(
        const HfsCatalogRecord& record,
        HfsForkSelector selector,
        const QByteArray& data,
        const HfsForkAllocationGrowthPlan& plan,
        QStringList* blockers) {
        const uint8_t forkType = catalogForkType(selector);
        const auto dataChunks =
            writeForkBytesWithinAllocated(plan.new_fork, record.catalog_id, forkType, 0, data);
        if (!dataChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto slackChunks = zeroForkAllocationSlack(
            plan.new_fork, record.catalog_id, forkType, static_cast<uint64_t>(data.size()));
        if (!slackChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto bitmapChunks = writeUpdatedAllocationBitmapBytes(plan.allocation_bytes);
        if (!bitmapChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        int extentsChunks = 0;
        if (plan.extents_mutation.has_value()) {
            const auto treeChunks = applyExtentsTreeMutation(*plan.extents_mutation, blockers);
            if (!treeChunks.has_value()) {
                return std::nullopt;
            }
            extentsChunks = *treeChunks;
        }
        const auto metadataChunks = updateCatalogFileForkData(record, selector, plan.new_fork);
        if (!metadataChunks.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        const auto freeBlockChunks =
            writeVolumeHeaderCounter(kHfsFreeBlocksOffset,
                                     m_volume.free_blocks,
                                     -static_cast<int>(plan.allocated_blocks),
                                     blockers);
        if (!freeBlockChunks.has_value()) {
            return std::nullopt;
        }
        return *dataChunks + *slackChunks + *bitmapChunks + extentsChunks + *metadataChunks +
               *freeBlockChunks;
    }

    [[nodiscard]] std::optional<QByteArray> readBackForkAllocationGrowth(
        const HfsCatalogRecord& record,
        HfsForkSelector selector,
        const QByteArray& expected,
        const HfsForkAllocationGrowthPlan& plan,
        QStringList* blockers) {
        const uint8_t forkType = catalogForkType(selector);
        const auto after = readForkBytes(
            plan.new_fork, record.catalog_id, forkType, 0, plan.new_fork.logical_size);
        if (!after.has_value()) {
            blockers->append(m_blockers);
            return std::nullopt;
        }
        if (*after != expected) {
            blockers->append(
                QStringLiteral("HFS+ allocation-growth read-back verification failed"));
            blockers->append(m_blockers);
            return std::nullopt;
        }
        return after;
    }

    [[nodiscard]] PartitionHfsFileWriteResult truncateCatalogForkWithinAllocatedBlocks(
        const QString& path,
        const PartitionHfsFileWriteOptions& options,
        HfsForkSelector selector) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendTruncateOptionBlockers(options, &result.blockers);
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.catalog_id = record->catalog_id;
        appendWriterResizeRecordBlockers(
            *record, QByteArray(), options, selector, &result.blockers);
        if (!result.blockers.isEmpty()) {
            result.blockers.append(m_blockers);
            return result;
        }

        const auto& fork = catalogForkFor(*record, selector);
        const uint8_t forkType = catalogForkType(selector);
        const uint64_t forkSize = catalogForkSize(*record, selector);
        const auto before = readForkBytes(fork, record->catalog_id, forkType, 0, forkSize);
        if (!before.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        result.before_sha256 = sha256Hex(*before);
        const auto zeroChunks = zeroStaleTail(*record, selector, 0);
        if (!zeroChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }
        const auto metadataChunks = updateCatalogFileForkLogicalSize(*record, selector, 0);
        if (!metadataChunks.has_value()) {
            result.blockers.append(m_blockers);
            return result;
        }

        HfsForkData truncatedFork = fork;
        truncatedFork.logical_size = 0;
        const auto after = readForkBytes(truncatedFork, record->catalog_id, forkType, 0, 0);
        if (!after.has_value() || !after->isEmpty()) {
            result.blockers.append(QStringLiteral("HFS+ truncate read-back verification failed"));
            result.blockers.append(m_blockers);
            return result;
        }
        result.after_sha256 = sha256Hex(*after);
        result.chunks_written = *zeroChunks + *metadataChunks;
        result.warnings = m_warnings;
        result.warnings.append(
            QStringLiteral("HFS+ %1 truncated to zero bytes inside already allocated blocks")
                .arg(hfsForkLabel(selector)));
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsAttributeReadResult attributeBaseResult(uint32_t fileId,
                                                                      const QString& name) const {
        PartitionHfsAttributeReadResult result;
        result.file_system = m_volume.file_system;
        result.warnings = m_warnings;
        result.file_id = fileId;
        result.attribute_name = name.trimmed();
        return result;
    }

    void appendAttributeReadRequestBlockers(uint32_t fileId,
                                            const QString& name,
                                            uint64_t maxBytes,
                                            QStringList* blockers) const {
        if (fileId == 0) {
            blockers->append(QStringLiteral("HFS+ attribute file ID is required"));
        }
        const QString trimmed = name.trimmed();
        if (trimmed.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ attribute name is required"));
        }
        if (trimmed.size() > kHfsMaximumAttributeNameChars || trimmed.contains(QChar::Null)) {
            blockers->append(QStringLiteral("Unsupported HFS+ attribute name"));
        }
        if (maxBytes == 0 || maxBytes > kMaxForkReadBytes) {
            blockers->append(QStringLiteral("HFS+ attribute read cap is invalid"));
        }
    }

    void appendAttributeWriteRequestBlockers(uint32_t fileId,
                                             const QString& name,
                                             const QByteArray& data,
                                             const PartitionHfsFileWriteOptions& options,
                                             QStringList* blockers) const {
        appendAttributeReadRequestBlockers(fileId, name, options.max_write_bytes, blockers);
        if (static_cast<uint64_t>(data.size()) > options.max_write_bytes ||
            options.max_write_bytes == 0) {
            blockers->append(
                QStringLiteral("HFS+ attribute write payload exceeds configured write cap"));
        }
    }

    [[nodiscard]] std::optional<HfsDecmpfsHeader> readDecmpfsHeaderForRecord(
        const HfsCatalogRecord& record,
        uint64_t maxBytes,
        QByteArray* attributeData,
        QStringList* blockers) {
        const auto attribute = readAttributeValue(record.catalog_id,
                                                  QLatin1String(kHfsDecmpfsAttributeName),
                                                  kMaxForkReadBytes);
        if (!attribute.ok) {
            blockers->append(QStringLiteral("HFS+ decmpfs attribute read failed"));
            blockers->append(attribute.blockers);
            return std::nullopt;
        }
        const auto header = parseDecmpfsHeader(attribute.data);
        if (!header.has_value()) {
            blockers->append(QStringLiteral("HFS+ decmpfs attribute header is invalid"));
            return std::nullopt;
        }
        if (header->uncompressed_size > maxBytes) {
            blockers->append(
                QStringLiteral(
                    "Selected HFS+ compressed file is larger than the read cap (%1 > %2 bytes)")
                    .arg(header->uncompressed_size)
                    .arg(maxBytes));
            return std::nullopt;
        }
        if (header->uncompressed_size > kHfsDecmpfsMaxSupportedBytes) {
            blockers->append(QStringLiteral("HFS+ decmpfs payload exceeds supported size"));
            return std::nullopt;
        }
        if (attributeData) {
            *attributeData = attribute.data;
        }
        return header;
    }

    [[nodiscard]] std::optional<QByteArray> decodeCompressedInlineFile(
        const HfsDecmpfsHeader& header,
        const QByteArray& attributeData,
        PartitionHfsFileReadResult* result) {
        const QByteArray payload = attributeData.mid(kHfsDecmpfsHeaderBytes);
        const auto decoded = decodeDecmpfsCodecChunk(
            header.compression_type, payload, static_cast<uint32_t>(header.uncompressed_size));
        if (!decoded.has_value()) {
            result->blockers.append(
                QStringLiteral("HFS+ decmpfs inline payload failed to decompress"));
            return std::nullopt;
        }
        result->warnings.append(
            QStringLiteral("HFS+ decmpfs type-%1 compressed file was decompressed (%2 bytes)")
                .arg(header.compression_type)
                .arg(header.uncompressed_size));
        return decoded;
    }

    [[nodiscard]] std::optional<QByteArray> decodeCompressedResourceFile(
        const HfsCatalogRecord& record,
        const HfsDecmpfsHeader& header,
        PartitionHfsFileReadResult* result) {
        const auto fork =
            readCatalogForkBytes(record, HfsForkSelector::Resource, &result->blockers);
        if (!fork.has_value()) {
            return std::nullopt;
        }
        QString decodeError;
        const auto decoded =
            decmpfsTypeUsesChunkedOffsetTable(header.compression_type)
                ? decodeDecmpfsChunkedResourceFork(
                      header.compression_type, *fork, header.uncompressed_size, &decodeError)
                : decodeDecmpfsResourceFork(*fork, header.uncompressed_size, &decodeError);
        if (!decoded.has_value()) {
            result->blockers.append(QStringLiteral("HFS+ %1").arg(decodeError));
            return std::nullopt;
        }
        result->warnings.append(
            QStringLiteral("HFS+ decmpfs type-%1 compressed file was decompressed (%2 bytes)")
                .arg(header.compression_type)
                .arg(header.uncompressed_size));
        return decoded;
    }

    [[nodiscard]] bool maybeReadCompressedCatalogFile(const HfsCatalogRecord& record,
                                                      HfsForkSelector selector,
                                                      uint64_t maxBytes,
                                                      PartitionHfsFileReadResult* result) {
        if (selector != HfsForkSelector::Data || record.data_size != 0 ||
            record.data_fork.total_blocks != 0) {
            return false;
        }
        const auto compressed = tryReadCompressedFile(record, maxBytes, result);
        if (compressed.has_value()) {
            result->data = *compressed;
            result->blockers.append(m_blockers);
            result->ok = result->blockers.isEmpty();
            return true;
        }
        if (!result->blockers.isEmpty()) {
            result->blockers.append(m_blockers);
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<QByteArray> tryReadCompressedFile(
        const HfsCatalogRecord& record, uint64_t maxBytes, PartitionHfsFileReadResult* result) {
        if (!fileHasAttribute(record.catalog_id, QLatin1String(kHfsDecmpfsAttributeName))) {
            return std::nullopt;
        }
        QByteArray attributeData;
        const auto header =
            readDecmpfsHeaderForRecord(record, maxBytes, &attributeData, &result->blockers);
        if (!header.has_value()) {
            return std::nullopt;
        }
        if (header->compression_type == kHfsDecmpfsTypeZlibInline ||
            header->compression_type == kHfsDecmpfsTypeLzvnInline ||
            header->compression_type == kHfsDecmpfsTypeLzfseInline) {
            return decodeCompressedInlineFile(*header, attributeData, result);
        }
        if (header->compression_type == kHfsDecmpfsTypeZlibResource ||
            header->compression_type == kHfsDecmpfsTypeLzvnResource ||
            header->compression_type == kHfsDecmpfsTypeLzfseResource) {
            return decodeCompressedResourceFile(record, *header, result);
        }
        result->blockers.append(QStringLiteral("HFS+ decmpfs compression type %1 is not supported")
                                    .arg(header->compression_type));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<HfsCatalogRecord> resolveCompressedReplaceTarget(
        const QString& path, PartitionHfsFileWriteResult* result) {
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            result->blockers.append(m_blockers);
            return std::nullopt;
        }
        result->catalog_id = record->catalog_id;
        if (!record->regularFile() || record->data_size != 0 ||
            record->data_fork.total_blocks != 0) {
            result->blockers.append(QStringLiteral(
                "HFS+ compressed-file replacement requires a decmpfs-compressed file"));
            return std::nullopt;
        }
        PartitionHfsFileReadResult before = baseResult();
        const auto beforeData =
            tryReadCompressedFile(*record, kHfsDecmpfsMaxSupportedBytes, &before);
        if (!beforeData.has_value()) {
            result->blockers.append(QStringLiteral(
                "HFS+ compressed-file replacement requires a readable supported decmpfs payload"));
            result->blockers.append(before.blockers);
            return std::nullopt;
        }
        result->before_sha256 = sha256Hex(*beforeData);
        return record;
    }

    struct HfsCompressedReplaceRequest {
        QString path;
        const HfsCatalogRecord* record{nullptr};
        const QByteArray* data{nullptr};
        const PartitionHfsFileWriteOptions* options{nullptr};
    };

    [[nodiscard]] std::optional<int> writeCompressedInlineReplacement(
        uint32_t compressionType,
        const HfsCatalogRecord& record,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options,
        QStringList* blockers) {
        QByteArray newAttribute = decmpfsHeaderBytes(compressionType,
                                                     static_cast<uint64_t>(data.size()));
        newAttribute.append(encodeDecmpfsCodecChunk(compressionType, data));
        const auto attrWrite = replaceInlineAttributeValue(
            record.catalog_id, QLatin1String(kHfsDecmpfsAttributeName), newAttribute, options);
        if (!attrWrite.ok) {
            blockers->append(attrWrite.blockers);
            return std::nullopt;
        }
        return attrWrite.chunks_written;
    }

    [[nodiscard]] std::optional<int> writeCompressedResourceReplacement(
        uint32_t compressionType,
        const HfsCompressedReplaceRequest& request,
        QStringList* blockers) {
        const QByteArray& data = *request.data;
        const QByteArray forkBytes = decmpfsTypeUsesChunkedOffsetTable(compressionType)
                                         ? buildDecmpfsChunkedResourceFork(compressionType, data)
                                         : buildDecmpfsResourceFork(data);
        const auto allocatedBytes = forkAllocatedBytes(request.record->resource_fork);
        if (!allocatedBytes.has_value()) {
            blockers->append(
                QStringLiteral("HFS+ compressed resource fork allocation is unavailable"));
            return std::nullopt;
        }
        const auto forkWrite =
            static_cast<uint64_t>(forkBytes.size()) <= *allocatedBytes
                ? replaceCatalogForkWithinAllocatedBlocks(
                      request.path, forkBytes, *request.options, HfsForkSelector::Resource)
                : replaceCatalogForkWithAllocationGrowth(
                      request.path, forkBytes, *request.options, HfsForkSelector::Resource);
        if (!forkWrite.ok) {
            blockers->append(forkWrite.blockers);
            return std::nullopt;
        }
        const QByteArray newAttribute = decmpfsHeaderBytes(compressionType,
                                                           static_cast<uint64_t>(data.size()));
        const auto attrWrite = replaceInlineAttributeValue(request.record->catalog_id,
                                                           QLatin1String(kHfsDecmpfsAttributeName),
                                                           newAttribute,
                                                           *request.options);
        if (!attrWrite.ok) {
            blockers->append(attrWrite.blockers);
            return std::nullopt;
        }
        return forkWrite.chunks_written + attrWrite.chunks_written;
    }

    [[nodiscard]] std::optional<int> writeCompressedReplacementForType(
        uint32_t compressionType,
        const HfsCompressedReplaceRequest& request,
        QStringList* blockers) {
        if (compressionType == kHfsDecmpfsTypeZlibInline ||
            compressionType == kHfsDecmpfsTypeLzvnInline ||
            compressionType == kHfsDecmpfsTypeLzfseInline) {
            return writeCompressedInlineReplacement(
                compressionType, *request.record, *request.data, *request.options, blockers);
        }
        if (compressionType == kHfsDecmpfsTypeZlibResource ||
            compressionType == kHfsDecmpfsTypeLzvnResource ||
            compressionType == kHfsDecmpfsTypeLzfseResource) {
            return writeCompressedResourceReplacement(compressionType, request, blockers);
        }
        blockers->append(QStringLiteral("HFS+ decmpfs compression type %1 is not supported")
                             .arg(compressionType));
        return std::nullopt;
    }

public:
    [[nodiscard]] PartitionHfsFileWriteResult replaceCompressedFileContent(
        const QString& path, const QByteArray& data, const PartitionHfsFileWriteOptions& options) {
        PartitionHfsFileWriteResult result;
        result.file_system = m_volume.file_system;
        result.path = normalizedDisplayPath(path);
        result.evidence_id = options.evidence_id;
        appendWriterOptionBlockers(data, options, &result.blockers);
        if (!options.allow_compressed_file_mutation) {
            result.blockers.append(
                QStringLiteral("HFS+ compressed-file replacement requires explicit compressed-file "
                               "mutation opt-in"));
        }
        if (static_cast<uint64_t>(data.size()) > kHfsDecmpfsMaxSupportedBytes) {
            result.blockers.append(QStringLiteral("HFS+ decmpfs payload exceeds supported size"));
        }
        if (!result.blockers.isEmpty()) {
            return result;
        }
        const auto record = resolveCompressedReplaceTarget(path, &result);
        if (!record.has_value()) {
            return result;
        }
        QByteArray attributeData;
        const auto header = readDecmpfsHeaderForRecord(
            *record, kHfsDecmpfsMaxSupportedBytes, &attributeData, &result.blockers);
        if (!header.has_value()) {
            return result;
        }
        const auto chunks = writeCompressedReplacementForType(
            header->compression_type,
            {.path = path, .record = &*record, .data = &data, .options = &options},
            &result.blockers);
        if (!chunks.has_value()) {
            return result;
        }

        const auto readBack = readCatalogFileFork(
            result.path,
            std::max<uint64_t>(options.max_write_bytes, static_cast<uint64_t>(data.size())),
            HfsForkSelector::Data);
        if (!readBack.ok || readBack.data != data) {
            result.blockers.append(
                QStringLiteral("HFS+ compressed-file replacement read-back verification failed"));
            result.blockers.append(readBack.blockers);
            result.blockers.append(m_blockers);
            return result;
        }
        result.after_sha256 = sha256Hex(readBack.data);
        result.bytes_written = static_cast<uint64_t>(data.size());
        result.chunks_written = *chunks;
        result.warnings.append(m_warnings);
        result.warnings.append(QStringLiteral("HFS+ decmpfs type-%1 compressed file content was "
                                              "replaced with read-back decompression proof")
                                   .arg(header->compression_type));
        result.ok = result.blockers.isEmpty();
        return result;
    }

private:
    [[nodiscard]] PartitionHfsFileReadResult readCatalogFileFork(const QString& path,
                                                                 uint64_t maxBytes,
                                                                 HfsForkSelector selector) {
        PartitionHfsFileReadResult result = baseResult();
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        if (!record->regularFile()) {
            result.blockers.append(QStringLiteral("Selected HFS+ path is not a regular file"));
            result.blockers.append(m_blockers);
            return result;
        }
        if (maybeReadCompressedCatalogFile(*record, selector, maxBytes, &result)) {
            return result;
        }
        const bool resource = selector == HfsForkSelector::Resource;
        const uint64_t forkSize = resource ? record->resource_size : record->data_size;
        if (forkSize > maxBytes) {
            result.blockers.append(
                QStringLiteral("Selected HFS+ %1 is larger than the read cap (%2 > %3 bytes)")
                    .arg(resource ? QStringLiteral("resource fork") : QStringLiteral("file"))
                    .arg(forkSize)
                    .arg(maxBytes));
            result.blockers.append(m_blockers);
            return result;
        }
        const auto bytes = readForkBytes(resource ? record->resource_fork : record->data_fork,
                                         record->catalog_id,
                                         resource ? kHfsResourceForkType : kHfsDataForkType,
                                         0,
                                         forkSize);
        if (!bytes.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        result.data = *bytes;
        result.blockers = m_blockers;
        result.warnings.append(m_warnings);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    void appendWriterOptionBlockers(const QByteArray& data,
                                    const PartitionHfsFileWriteOptions& options,
                                    QStringList* blockers) const {
        appendWriterActivationBlockers(options, blockers);
        appendWriterDeviceBlockers(blockers);
        appendWriterPayloadBlockers(data, options, blockers);
        appendWriterVolumeBlockers(options, blockers);
    }

    void appendTruncateOptionBlockers(const PartitionHfsFileWriteOptions& options,
                                      QStringList* blockers) const {
        appendWriterActivationBlockers(options, blockers);
        appendWriterDeviceBlockers(blockers);
        if (options.max_write_bytes == 0) {
            blockers->append(QStringLiteral("HFS+ truncate requires a nonzero write cap"));
        }
        appendWriterVolumeBlockers(options, blockers);
    }

    static void appendWriterActivationBlockers(const PartitionHfsFileWriteOptions& options,
                                               QStringList* blockers) {
        if (!options.enable_writer) {
            blockers->append(QStringLiteral("HFS+ writer is not enabled"));
        }
        if (!options.target_write_confirmed) {
            blockers->append(QStringLiteral("HFS+ write requires explicit target confirmation"));
        }
        if (options.evidence_id.trimmed().isEmpty()) {
            blockers->append(QStringLiteral("HFS+ write requires certification evidence ID"));
        }
    }

    void appendWriterDeviceBlockers(QStringList* blockers) const {
        if (!m_device || !m_device->isWritable()) {
            blockers->append(QStringLiteral("Writable HFS+ device or image is required"));
        }
    }

    static void appendWriterPayloadBlockers(const QByteArray& data,
                                            const PartitionHfsFileWriteOptions& options,
                                            QStringList* blockers) {
        if (data.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ write payload is empty"));
        }
        if (static_cast<uint64_t>(data.size()) > options.max_write_bytes ||
            options.max_write_bytes == 0) {
            blockers->append(QStringLiteral("HFS+ write payload exceeds configured write cap"));
        }
    }

    void appendWriterVolumeBlockers(const PartitionHfsFileWriteOptions& options,
                                    QStringList* blockers) const {
        if ((m_volume.attributes & kHfsVolumeJournaledMask) != 0 &&
            !options.allow_journaled_volume) {
            blockers->append(
                QStringLiteral("HFS+ journaled volume write requires explicit journal override"));
        }
        if ((m_volume.attributes & kHfsVolumeInconsistentMask) != 0) {
            blockers->append(QStringLiteral("HFS+ inconsistent volume writes are blocked"));
        }
        if (m_volume.wrapped && !options.allow_wrapped_volume) {
            blockers->append(QStringLiteral("Wrapped HFS+ volume writes are blocked by default"));
        }
    }

    [[nodiscard]] static const HfsForkData& catalogForkFor(const HfsCatalogRecord& record,
                                                           HfsForkSelector selector) {
        return selector == HfsForkSelector::Resource ? record.resource_fork : record.data_fork;
    }

    [[nodiscard]] static uint64_t catalogForkSize(const HfsCatalogRecord& record,
                                                  HfsForkSelector selector) {
        return selector == HfsForkSelector::Resource ? record.resource_size : record.data_size;
    }

    [[nodiscard]] static uint8_t catalogForkType(HfsForkSelector selector) {
        return selector == HfsForkSelector::Resource ? kHfsResourceForkType : kHfsDataForkType;
    }

    [[nodiscard]] static qsizetype catalogForkRecordOffset(HfsForkSelector selector) {
        return selector == HfsForkSelector::Resource ? kHfsCatalogFileResourceForkOffset
                                                     : kHfsCatalogFileDataForkOffset;
    }

    [[nodiscard]] static QString hfsForkLabel(HfsForkSelector selector) {
        return selector == HfsForkSelector::Resource ? QStringLiteral("resource fork")
                                                     : QStringLiteral("data fork");
    }

    [[nodiscard]] static QString allocatedForkResizeWarning(HfsForkSelector selector) {
        return QStringLiteral("HFS+ %1 length changed only within already allocated blocks")
            .arg(hfsForkLabel(selector));
    }

    void appendWriterRecordBlockers(const HfsCatalogRecord& record,
                                    const QByteArray& data,
                                    const PartitionHfsFileWriteOptions& options,
                                    QStringList* blockers) {
        if (!record.regularFile()) {
            blockers->append(QStringLiteral("Selected HFS+ path is not a regular file"));
            return;
        }
        if (record.data_size != static_cast<uint64_t>(data.size())) {
            blockers->append(QStringLiteral(
                "HFS+ arbitrary write currently requires exact same-size data-fork replacement"));
        }
        if (record.data_fork.logical_size != record.data_size ||
            record.data_fork.logical_size == 0 || record.data_fork.extents.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ data fork is not writable in-place"));
        }
        if (!forkCoveredByInitialExtents(record.data_fork) && m_overflow_extents.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ overflow extents are required for this file"));
        }
        if (!options.allow_compressed_file_mutation &&
            fileHasAttribute(record.catalog_id, QStringLiteral("com.apple.decmpfs"))) {
            blockers->append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
    }

    void appendWriterResizeRecordBlockers(const HfsCatalogRecord& record,
                                          const QByteArray& data,
                                          const PartitionHfsFileWriteOptions& options,
                                          HfsForkSelector selector,
                                          QStringList* blockers) {
        appendWriterRecordSharedBlockers(record, options, selector, blockers);
        if (!blockers->isEmpty()) {
            return;
        }
        const uint64_t payloadBytes = static_cast<uint64_t>(data.size());
        const auto& fork = catalogForkFor(record, selector);
        const uint64_t forkSize = catalogForkSize(record, selector);
        const QString forkLabel = hfsForkLabel(selector);
        const auto allocatedBytes = forkAllocatedBytes(fork);
        if (!allocatedBytes.has_value() || *allocatedBytes == 0) {
            blockers->append(QStringLiteral("HFS+ %1 allocation is not writable").arg(forkLabel));
            return;
        }
        if (payloadBytes > *allocatedBytes) {
            blockers->append(
                QStringLiteral(
                    "HFS+ %1 allocated-block replacement cannot grow beyond existing allocation")
                    .arg(forkLabel));
        }
        if (record.catalog_data_offset == 0) {
            blockers->append(QStringLiteral("HFS+ catalog file record location is unavailable"));
        }
        if (payloadBytes < forkSize && forkSize - payloadBytes > options.max_write_bytes) {
            blockers->append(
                QStringLiteral("HFS+ stale-tail zeroing exceeds configured write cap"));
        }
    }

    void appendWriterRecordSharedBlockers(const HfsCatalogRecord& record,
                                          const PartitionHfsFileWriteOptions& options,
                                          HfsForkSelector selector,
                                          QStringList* blockers) {
        if (!record.regularFile()) {
            blockers->append(QStringLiteral("Selected HFS+ path is not a regular file"));
            return;
        }
        const auto& fork = catalogForkFor(record, selector);
        const QString forkLabel = hfsForkLabel(selector);
        if (fork.logical_size == 0 || fork.extents.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ %1 is not writable in-place").arg(forkLabel));
        }
        if (!forkCoveredByInitialExtents(fork) && m_overflow_extents.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ overflow extents are required for this file"));
        }
        if (!options.allow_compressed_file_mutation &&
            fileHasAttribute(record.catalog_id, QStringLiteral("com.apple.decmpfs"))) {
            blockers->append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
    }

    [[nodiscard]] std::optional<QVector<HfsExtent>> writableForkExtentsForGrowth(
        const HfsCatalogRecord& record, HfsForkSelector selector, QStringList* blockers) const {
        const auto& fork = catalogForkFor(record, selector);
        if (fork.total_blocks == 0) {
            return QVector<HfsExtent>();
        }
        const auto materialized =
            materializedForkExtents(fork, record.catalog_id, catalogForkType(selector), blockers);
        if (!materialized.has_value()) {
            return std::nullopt;
        }
        for (const auto& extent : *materialized) {
            if (!forkExtentLooksWritable(extent, hfsForkLabel(selector), blockers)) {
                return std::nullopt;
            }
        }
        return materialized;
    }

    [[nodiscard]] static QVector<HfsOverflowExtentRecord> overflowRecordsForCombinedExtents(
        const QVector<HfsExtent>& combinedExtents, uint32_t fileId, uint8_t forkType) {
        QVector<HfsOverflowExtentRecord> records;
        uint32_t coveredBlocks = 0;
        for (int index = 0; index < combinedExtents.size(); ++index) {
            if (index >= kHfsExtentCount) {
                const int groupOffset = (index - kHfsExtentCount) % kHfsExtentCount;
                if (groupOffset == 0) {
                    records.append(HfsOverflowExtentRecord{.fork_type = forkType,
                                                           .file_id = fileId,
                                                           .start_block = coveredBlocks,
                                                           .extents = {}});
                }
                records.last().extents.append(combinedExtents.at(index));
            }
            coveredBlocks += combinedExtents.at(index).block_count;
        }
        return records;
    }

    [[nodiscard]] std::optional<HfsForkAllocationGrowthPlan> prepareForkAllocationGrowth(
        const HfsCatalogRecord& record,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options,
        HfsForkSelector selector,
        QStringList* blockers) {
        appendWriterAllocationGrowthRecordBlockers(record, data, options, selector, blockers);
        if (!blockers->isEmpty()) {
            return std::nullopt;
        }
        const auto oldExtents = writableForkExtentsForGrowth(record, selector, blockers);
        const auto requiredBlocks = requiredAllocationBlocksForBytes(data, blockers);
        if (!oldExtents.has_value() || !requiredBlocks.has_value()) {
            return std::nullopt;
        }
        const uint32_t oldBlocks = catalogForkFor(record, selector).total_blocks;
        const uint32_t newBlocks = *requiredBlocks - oldBlocks;
        const auto newExtents =
            findFreeAllocationExtents(newBlocks, hfsForkLabel(selector), blockers);
        if (!newExtents.has_value()) {
            return std::nullopt;
        }
        auto combinedExtents = *oldExtents;
        appendExtentsCoalesced(&combinedExtents, *newExtents);
        const auto allocationBytes = prepareAllocationBitmapSet(*newExtents, blockers);
        if (!allocationBytes.has_value()) {
            return std::nullopt;
        }
        HfsForkAllocationGrowthPlan plan{
            .record = record,
            .new_fork = HfsForkData{.logical_size = static_cast<uint64_t>(data.size()),
                                    .total_blocks = *requiredBlocks,
                                    .extents = combinedExtents},
            .allocated_extents = *newExtents,
            .allocation_bytes = *allocationBytes,
            .allocated_blocks = newBlocks,
            .allocated_bytes = static_cast<uint64_t>(newBlocks) * m_volume.block_size,
            .overflow_records = {},
            .extents_mutation = std::nullopt};
        if (combinedExtents.size() > kHfsExtentCount) {
            plan.overflow_records = overflowRecordsForCombinedExtents(combinedExtents,
                                                                      record.catalog_id,
                                                                      catalogForkType(selector));
            const QVector<QPair<uint32_t, uint8_t>> removals{
                {record.catalog_id, catalogForkType(selector)}};
            const auto mutation =
                prepareExtentsTreeMutation(removals, plan.overflow_records, blockers);
            if (!mutation.has_value()) {
                return std::nullopt;
            }
            plan.extents_mutation = *mutation;
        }
        return plan;
    }

    void appendWriterAllocationGrowthRecordBlockers(const HfsCatalogRecord& record,
                                                    const QByteArray& data,
                                                    const PartitionHfsFileWriteOptions& options,
                                                    HfsForkSelector selector,
                                                    QStringList* blockers) {
        if (!record.regularFile()) {
            blockers->append(QStringLiteral("Selected HFS+ path is not a regular file"));
            return;
        }
        const auto& fork = catalogForkFor(record, selector);
        if (record.catalog_data_offset == 0) {
            blockers->append(QStringLiteral("HFS+ catalog file record location is unavailable"));
        }
        appendAllocationForkGrowthBlockers(blockers);
        appendRequiredAllocationGrowthBlockers(fork, data, selector, blockers);
        appendInitialExtentGrowthBlockers(fork, selector, blockers);
        appendCompressedFileGrowthBlocker(record, options, blockers);
    }

    void appendRequiredAllocationGrowthBlockers(const HfsForkData& fork,
                                                const QByteArray& data,
                                                HfsForkSelector selector,
                                                QStringList* blockers) const {
        const auto requiredBlocks = requiredAllocationBlocksForBytes(data, blockers);
        if (!requiredBlocks.has_value()) {
            return;
        }
        if (*requiredBlocks <= fork.total_blocks) {
            blockers->append(
                QStringLiteral("HFS+ %1 replacement does not require allocation growth")
                    .arg(hfsForkLabel(selector)));
            return;
        }
        if (*requiredBlocks - fork.total_blocks > m_volume.free_blocks) {
            blockers->append(QStringLiteral("HFS+ volume does not have enough free blocks"));
        }
    }

    void appendInitialExtentGrowthBlockers(const HfsForkData& fork,
                                           HfsForkSelector selector,
                                           QStringList* blockers) const {
        if (fork.total_blocks > 0 && fork.extents.isEmpty()) {
            blockers->append(QStringLiteral("HFS+ existing %1 extents are not available")
                                 .arg(hfsForkLabel(selector)));
        }
    }

    void appendCompressedFileGrowthBlocker(const HfsCatalogRecord& record,
                                           const PartitionHfsFileWriteOptions& options,
                                           QStringList* blockers) {
        if (!options.allow_compressed_file_mutation &&
            fileHasAttribute(record.catalog_id, QStringLiteral("com.apple.decmpfs"))) {
            blockers->append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
    }

    void appendAllocationForkGrowthBlockers(QStringList* blockers) const {
        if (m_volume.allocation_fork.logical_size == 0 ||
            m_volume.allocation_fork.total_blocks == 0 ||
            m_volume.allocation_fork.extents.isEmpty()) {
            blockers->append(
                QStringLiteral("HFS+ allocation bitmap fork is required for allocation growth"));
        }
    }

    [[nodiscard]] std::optional<uint32_t> requiredAllocationBlocksForBytes(
        const QByteArray& data, QStringList* blockers) const {
        uint64_t rounded = 0;
        if (m_volume.block_size == 0 ||
            !checkedAdd(static_cast<uint64_t>(data.size()), m_volume.block_size - 1, &rounded)) {
            blockers->append(QStringLiteral("HFS+ allocation-growth size overflow"));
            return std::nullopt;
        }
        const uint64_t blocks = rounded / m_volume.block_size;
        if (blocks == 0 || blocks > std::numeric_limits<uint32_t>::max()) {
            blockers->append(QStringLiteral("HFS+ allocation-growth block count is unsupported"));
            return std::nullopt;
        }
        return static_cast<uint32_t>(blocks);
    }

    [[nodiscard]] std::optional<QVector<HfsExtent>> initialForkExtentsForAllocatedBlocks(
        const HfsForkData& fork, const QString& label, QStringList* blockers) const {
        QVector<HfsExtent> extents;
        if (fork.total_blocks == 0) {
            return extents;
        }
        uint32_t covered = 0;
        for (const auto& extent : fork.extents) {
            if (covered >= fork.total_blocks || extent.block_count == 0) {
                continue;
            }
            const uint32_t blockCount = std::min(extent.block_count, fork.total_blocks - covered);
            HfsExtent trimmed{extent.start_block, blockCount};
            if (!forkExtentLooksWritable(trimmed, label, blockers)) {
                return std::nullopt;
            }
            extents.append(trimmed);
            covered += blockCount;
        }
        if (covered != fork.total_blocks) {
            blockers->append(QStringLiteral("HFS+ %1 allocation growth requires existing "
                                            "allocation in initial extent records")
                                 .arg(label));
            return std::nullopt;
        }
        return extents;
    }

    [[nodiscard]] bool forkExtentLooksWritable(const HfsExtent& extent,
                                               const QString& label,
                                               QStringList* blockers) const {
        uint64_t endBlock = 0;
        if (extent.block_count == 0 ||
            !checkedAdd(extent.start_block, extent.block_count, &endBlock) ||
            endBlock > m_volume.total_blocks) {
            blockers->append(QStringLiteral("HFS+ %1 extent is out of range").arg(label));
            return false;
        }
        if (extent.start_block == 0) {
            blockers->append(
                QStringLiteral(
                    "HFS+ %1 allocation growth blocks extents that include allocation block 0")
                    .arg(label));
            return false;
        }
        if (extentOverlapsMetadata(extent)) {
            blockers->append(
                QStringLiteral(
                    "HFS+ %1 allocation growth refuses extents that overlap metadata forks")
                    .arg(label));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<QVector<HfsExtent>> findFreeAllocationExtents(
        uint32_t blocksNeeded, const QString& label, QStringList* blockers) {
        HfsFreeExtentScanState scan{
            .blocks_needed = blocksNeeded,
            .found_blocks = 0,
            .extents = {},
            .blockers = blockers,
        };
        uint64_t bitmapOffset = 0;
        const uint64_t bitmapBytes = std::min<uint64_t>(m_volume.allocation_fork.logical_size,
                                                        (m_volume.total_blocks + 7ULL) / 8ULL);
        while (bitmapOffset < bitmapBytes && scan.found_blocks < scan.blocks_needed) {
            const uint64_t chunkSize = std::min<uint64_t>(kHfsAllocationBitmapScanChunkBytes,
                                                          bitmapBytes - bitmapOffset);
            const auto bytes = readForkBytes(m_volume.allocation_fork,
                                             kHfsAllocationFileId,
                                             kHfsDataForkType,
                                             bitmapOffset,
                                             chunkSize);
            if (!bytes.has_value()) {
                blockers->append(QStringLiteral("Unable to scan HFS+ allocation bitmap"));
                blockers->append(m_blockers);
                return std::nullopt;
            }
            appendFreeExtentsFromBitmapChunk(*bytes, bitmapOffset, label, &scan);
            if (!blockers->isEmpty()) {
                return std::nullopt;
            }
            bitmapOffset += chunkSize;
        }
        if (scan.found_blocks != scan.blocks_needed) {
            blockers->append(
                QStringLiteral("HFS+ allocation bitmap does not contain enough free blocks"));
            return std::nullopt;
        }
        return scan.extents;
    }

    void appendFreeExtentsFromBitmapChunk(const QByteArray& bytes,
                                          uint64_t bitmapOffset,
                                          const QString& label,
                                          HfsFreeExtentScanState* scan) const {
        for (qsizetype byteIndex = 0;
             byteIndex < bytes.size() && scan->found_blocks < scan->blocks_needed;
             ++byteIndex) {
            const auto value = static_cast<quint8>(bytes.at(byteIndex));
            const uint64_t byteBlockBase = (bitmapOffset + static_cast<uint64_t>(byteIndex)) * 8ULL;
            for (uint32_t bit = 0; bit < kBitsPerByte && scan->found_blocks < scan->blocks_needed;
                 ++bit) {
                const uint64_t block = byteBlockBase + bit;
                if (block >= m_volume.total_blocks) {
                    return;
                }
                const quint8 mask = static_cast<quint8>(0x80U >> bit);
                if (block == 0 || (value & mask) != 0) {
                    continue;
                }
                const HfsExtent candidate{static_cast<uint32_t>(block), 1};
                if (!forkExtentLooksWritable(candidate, label, scan->blockers)) {
                    continue;
                }
                appendExtentCoalesced(&scan->extents, candidate);
                ++scan->found_blocks;
            }
        }
    }

    static void appendExtentsCoalesced(QVector<HfsExtent>* target,
                                       const QVector<HfsExtent>& extents) {
        for (const auto& extent : extents) {
            appendExtentCoalesced(target, extent);
        }
    }

    static void appendExtentCoalesced(QVector<HfsExtent>* target, const HfsExtent& extent) {
        if (extent.block_count == 0) {
            return;
        }
        if (!target->isEmpty()) {
            auto& last = target->last();
            const uint64_t lastEnd = static_cast<uint64_t>(last.start_block) + last.block_count;
            if (lastEnd == extent.start_block) {
                last.block_count += extent.block_count;
                return;
            }
        }
        target->append(extent);
    }

    [[nodiscard]] std::optional<QVector<HfsAllocationBitmapByte>> prepareAllocationBitmapSet(
        const QVector<HfsExtent>& extents, QStringList* blockers) {
        const auto bytes = loadAllocationBitmapBytes(extents, blockers);
        if (!bytes.has_value()) {
            return std::nullopt;
        }
        auto updatedBytes = *bytes;
        if (!setAllocationBits(extents, &updatedBytes, blockers)) {
            return std::nullopt;
        }
        return updatedBytes;
    }

    [[nodiscard]] bool setAllocationBits(const QVector<HfsExtent>& extents,
                                         QVector<HfsAllocationBitmapByte>* bytes,
                                         QStringList* blockers) const {
        for (const auto& extent : extents) {
            if (!setAllocationExtentBits(extent, bytes, blockers)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool setAllocationExtentBits(const HfsExtent& extent,
                                               QVector<HfsAllocationBitmapByte>* bytes,
                                               QStringList* blockers) const {
        for (uint32_t block = 0; block < extent.block_count; ++block) {
            const uint64_t allocationBlock = static_cast<uint64_t>(extent.start_block) + block;
            if (!setAllocationBlockBit(allocationBlock, bytes, blockers)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool setAllocationBlockBit(uint64_t allocationBlock,
                                             QVector<HfsAllocationBitmapByte>* bytes,
                                             QStringList* blockers) const {
        const uint64_t byteOffset = allocationBlock / 8U;
        const quint8 mask = static_cast<quint8>(0x80U >> (allocationBlock % 8U));
        auto* byte = allocationBitmapByte(bytes, byteOffset);
        if (!byte) {
            blockers->append(QStringLiteral("HFS+ allocation bitmap byte was not staged"));
            return false;
        }
        if ((byte->updated_value & mask) != 0) {
            blockers->append(QStringLiteral("HFS+ allocation bitmap block is already allocated"));
            return false;
        }
        byte->updated_value = static_cast<quint8>(byte->updated_value | mask);
        return true;
    }

    [[nodiscard]] bool fileHasAttribute(uint32_t fileId, const QString& name) {
        if (m_volume.attributes_fork.logical_size == 0 ||
            m_volume.attributes_fork.extents.isEmpty()) {
            return false;
        }
        const auto attributes = scanAttributeRecords(kPartitionHfsDefaultCheckRecordLimit);
        if (!attributes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to scan HFS+ attributes before write"));
            return true;
        }
        return std::any_of(attributes->parsed_records.cbegin(),
                           attributes->parsed_records.cend(),
                           [&](const HfsAttributeRecord& record) {
                               return record.file_id == fileId && record.name == name;
                           });
    }

    [[nodiscard]] bool fileHasAnyAttribute(uint32_t fileId) {
        if (m_volume.attributes_fork.logical_size == 0 ||
            m_volume.attributes_fork.extents.isEmpty()) {
            return false;
        }
        const auto attributes = scanAttributeRecords(kPartitionHfsDefaultCheckRecordLimit);
        if (!attributes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to scan HFS+ attributes before delete"));
            return true;
        }
        return std::any_of(attributes->parsed_records.cbegin(),
                           attributes->parsed_records.cend(),
                           [fileId](const HfsAttributeRecord& record) {
                               return record.file_id == fileId;
                           });
    }

    [[nodiscard]] std::optional<int> writeForkBytes(const HfsForkData& fork,
                                                    uint32_t fileId,
                                                    uint8_t forkType,
                                                    uint64_t offset,
                                                    const QByteArray& data) {
        if (!forkReadRequestIsValid(fork, offset, static_cast<uint64_t>(data.size()))) {
            m_blockers.append(QStringLiteral("Requested HFS+ fork write is out of range"));
            return std::nullopt;
        }

        int chunks = 0;
        uint64_t remaining = static_cast<uint64_t>(data.size());
        uint64_t cursor = offset;
        qsizetype dataOffset = 0;
        while (remaining > 0) {
            const uint64_t withinBlock = cursor % m_volume.block_size;
            const uint64_t chunkSize = std::min<uint64_t>(remaining,
                                                          m_volume.block_size - withinBlock);
            const auto deviceOffset =
                deviceOffsetForForkCursor(fork, fileId, forkType, cursor, withinBlock);
            if (!deviceOffset.has_value() ||
                !writeAt(*deviceOffset, data.constData() + dataOffset, chunkSize)) {
                return std::nullopt;
            }
            cursor += chunkSize;
            remaining -= chunkSize;
            dataOffset += static_cast<qsizetype>(chunkSize);
            ++chunks;
        }
        return chunks;
    }

    [[nodiscard]] std::optional<int> writeForkBytesWithinAllocated(const HfsForkData& fork,
                                                                   uint32_t fileId,
                                                                   uint8_t forkType,
                                                                   uint64_t offset,
                                                                   const QByteArray& data) {
        if (!forkWriteRequestFitsAllocation(fork, offset, static_cast<uint64_t>(data.size()))) {
            m_blockers.append(QStringLiteral("Requested HFS+ fork write exceeds allocation"));
            return std::nullopt;
        }

        int chunks = 0;
        uint64_t remaining = static_cast<uint64_t>(data.size());
        uint64_t cursor = offset;
        qsizetype dataOffset = 0;
        while (remaining > 0) {
            const uint64_t withinBlock = cursor % m_volume.block_size;
            const uint64_t chunkSize = std::min<uint64_t>(remaining,
                                                          m_volume.block_size - withinBlock);
            const auto deviceOffset =
                deviceOffsetForForkCursor(fork, fileId, forkType, cursor, withinBlock);
            if (!deviceOffset.has_value() ||
                !writeAt(*deviceOffset, data.constData() + dataOffset, chunkSize)) {
                return std::nullopt;
            }
            cursor += chunkSize;
            remaining -= chunkSize;
            dataOffset += static_cast<qsizetype>(chunkSize);
            ++chunks;
        }
        return chunks;
    }

    [[nodiscard]] std::optional<int> zeroStaleTail(const HfsCatalogRecord& record,
                                                   HfsForkSelector selector,
                                                   uint64_t newSize) {
        const uint64_t forkSize = catalogForkSize(record, selector);
        if (newSize >= forkSize) {
            return 0;
        }
        const uint64_t staleBytes = forkSize - newSize;
        if (staleBytes > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
            m_blockers.append(QStringLiteral("HFS+ stale-tail zeroing is too large"));
            return std::nullopt;
        }
        const QByteArray zeros(static_cast<qsizetype>(staleBytes), '\0');
        return writeForkBytes(catalogForkFor(record, selector),
                              record.catalog_id,
                              catalogForkType(selector),
                              newSize,
                              zeros);
    }

    [[nodiscard]] std::optional<int> updateCatalogFileForkLogicalSize(
        const HfsCatalogRecord& record, HfsForkSelector selector, uint64_t newSize) {
        uint64_t sizeFieldOffset = 0;
        if (!checkedAdd(record.catalog_data_offset,
                        catalogForkRecordOffset(selector) + kHfsForkLogicalSizeOffset,
                        &sizeFieldOffset)) {
            m_blockers.append(QStringLiteral("HFS+ catalog size-field offset overflow"));
            return std::nullopt;
        }
        return writeForkBytes(m_volume.catalog_fork,
                              kHfsCatalogFileId,
                              kHfsDataForkType,
                              sizeFieldOffset,
                              be64Bytes(newSize));
    }

    [[nodiscard]] std::optional<int> updateCatalogFileForkData(const HfsCatalogRecord& record,
                                                               HfsForkSelector selector,
                                                               const HfsForkData& fork) {
        uint64_t forkBaseOffset = 0;
        if (!checkedAdd(
                record.catalog_data_offset, catalogForkRecordOffset(selector), &forkBaseOffset)) {
            m_blockers.append(QStringLiteral("HFS+ catalog fork-data offset overflow"));
            return std::nullopt;
        }
        int chunks = 0;
        const auto logicalSizeChunks = writeForkBytes(m_volume.catalog_fork,
                                                      kHfsCatalogFileId,
                                                      kHfsDataForkType,
                                                      forkBaseOffset + kHfsForkLogicalSizeOffset,
                                                      be64Bytes(fork.logical_size));
        if (!logicalSizeChunks.has_value()) {
            return std::nullopt;
        }
        chunks += *logicalSizeChunks;
        const auto totalBlockChunks = writeForkBytes(m_volume.catalog_fork,
                                                     kHfsCatalogFileId,
                                                     kHfsDataForkType,
                                                     forkBaseOffset + kHfsForkTotalBlocksOffset,
                                                     be32Bytes(fork.total_blocks));
        if (!totalBlockChunks.has_value()) {
            return std::nullopt;
        }
        chunks += *totalBlockChunks;
        const auto extentChunks = writeForkBytes(m_volume.catalog_fork,
                                                 kHfsCatalogFileId,
                                                 kHfsDataForkType,
                                                 forkBaseOffset + kHfsForkExtentsOffset,
                                                 hfsForkExtentBytes(fork.extents));
        if (!extentChunks.has_value()) {
            return std::nullopt;
        }
        return chunks + *extentChunks;
    }

    [[nodiscard]] static QByteArray hfsForkExtentBytes(const QVector<HfsExtent>& extents) {
        QByteArray bytes(kHfsExtentCount * kHfsExtentBytes, '\0');
        const int count = std::min<int>(static_cast<int>(extents.size()), kHfsExtentCount);
        for (int index = 0; index < count; ++index) {
            const qsizetype offset = index * kHfsExtentBytes;
            const auto& extent = extents.at(index);
            writeBe32(&bytes, offset + kHfsExtentStartBlockOffset, extent.start_block);
            writeBe32(&bytes, offset + kHfsExtentBlockCountOffset, extent.block_count);
        }
        return bytes;
    }

    [[nodiscard]] std::optional<int> zeroForkAllocationSlack(const HfsForkData& fork,
                                                             uint32_t fileId,
                                                             uint8_t forkType,
                                                             uint64_t logicalSize) {
        const auto allocatedBytes = forkAllocatedBytes(fork);
        if (!allocatedBytes.has_value()) {
            m_blockers.append(QStringLiteral("HFS+ allocation-growth byte count overflow"));
            return std::nullopt;
        }
        if (logicalSize >= *allocatedBytes) {
            return 0;
        }
        const uint64_t slackBytes = *allocatedBytes - logicalSize;
        if (slackBytes > m_volume.block_size ||
            slackBytes > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
            m_blockers.append(QStringLiteral("HFS+ allocation-growth slack zeroing is too large"));
            return std::nullopt;
        }
        const QByteArray zeros(static_cast<qsizetype>(slackBytes), '\0');
        return writeForkBytesWithinAllocated(fork, fileId, forkType, logicalSize, zeros);
    }

    bool appendForkGrowthCounterReadBack(uint32_t allocatedBlocks,
                                         PartitionHfsFileWriteResult* result) {
        const auto header = readVolumeHeaderAt(m_volume.volume_offset);
        if (!header.has_value() || be32(*header, kHfsFreeBlocksOffset) != m_volume.free_blocks ||
            m_volume.free_blocks + allocatedBlocks > m_volume.total_blocks) {
            result->blockers.append(
                QStringLiteral("HFS+ allocation-growth free-block counter read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        return true;
    }

    bool appendForkGrowthCounterReadBack(uint32_t allocatedBlocks,
                                         PartitionHfsAttributeWriteResult* result) {
        const auto header = readVolumeHeaderAt(m_volume.volume_offset);
        if (!header.has_value() || be32(*header, kHfsFreeBlocksOffset) != m_volume.free_blocks ||
            m_volume.free_blocks + allocatedBlocks > m_volume.total_blocks) {
            result->blockers.append(
                QStringLiteral("HFS+ allocation-growth free-block counter read-back failed"));
            result->blockers.append(m_blockers);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool writeAt(uint64_t offset, const char* data, uint64_t length) {
        if (!data || length > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) ||
            offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
            m_blockers.append(QStringLiteral("HFS+ write request is out of range"));
            return false;
        }
        if (!m_device->seek(static_cast<qint64>(offset))) {
            m_blockers.append(
                QStringLiteral("Unable to seek HFS+ fork data for write at byte %1: %2")
                    .arg(offset)
                    .arg(m_device->errorString()));
            return false;
        }
        const qint64 written = m_device->write(data, static_cast<qint64>(length));
        if (written != static_cast<qint64>(length)) {
            m_blockers.append(QStringLiteral("Unable to write HFS+ fork data"));
            return false;
        }
        return true;
    }

    [[nodiscard]] static QString sha256Hex(const QByteArray& data) {
        return QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    }

    [[nodiscard]] PartitionHfsFileReadResult baseResult() const {
        PartitionHfsFileReadResult result;
        result.file_system = m_volume.file_system;
        result.warnings = m_warnings;
        return result;
    }

    [[nodiscard]] bool loadDirectVolumeHeader() {
        const auto bytes = readAt(kHfsVolumeHeaderOffset, kHfsVolumeHeaderSize);
        if (!bytes.has_value() || hfsFamilyName(*bytes, 0).isEmpty()) {
            return false;
        }
        parseVolumeHeader(*bytes, 0, false);
        return true;
    }

    [[nodiscard]] bool loadWrappedVolumeHeader() {
        const auto mdb = readAt(kHfsWrapperMdbOffset, kHfsWrapperMdbSize);
        if (!mdb.has_value()) {
            return false;
        }

        const auto embeddedOffset = wrappedVolumeOffset(*mdb);
        if (!embeddedOffset.has_value()) {
            return false;
        }
        const auto header = readVolumeHeaderAt(*embeddedOffset);
        if (!header.has_value()) {
            return false;
        }
        parseVolumeHeader(*header, *embeddedOffset, true);
        return true;
    }

    [[nodiscard]] std::optional<uint64_t> wrappedVolumeOffset(const QByteArray& mdb) const {
        if (!wrapperHeaderLooksLikeHfsPlus(mdb)) {
            return std::nullopt;
        }
        const uint32_t allocationBlockSize = be32(mdb, kHfsWrapperAllocationBlockSizeOffset);
        const uint16_t allocationStartSector = be16(mdb, kHfsWrapperAllocationBlockStartOffset);
        const uint16_t extentStartBlock = be16(mdb, kHfsWrapperEmbeddedExtentStartOffset);
        const uint16_t extentBlockCount = be16(mdb, kHfsWrapperEmbeddedExtentCountOffset);
        if (!wrapperGeometryLooksValid(allocationBlockSize, extentBlockCount)) {
            return std::nullopt;
        }
        return wrapperEmbeddedOffset(allocationStartSector, extentStartBlock, allocationBlockSize);
    }

    [[nodiscard]] bool wrapperHeaderLooksLikeHfsPlus(const QByteArray& mdb) const {
        return matchesBytes(mdb, 0, "BD", kHfsSignatureSize) &&
               !hfsFamilyName(mdb, kHfsWrapperEmbeddedSignatureOffset).isEmpty();
    }

    [[nodiscard]] bool wrapperGeometryLooksValid(uint32_t allocationBlockSize,
                                                 uint16_t extentBlockCount) const {
        return extentBlockCount != 0 && allocationBlockSize >= kMinimumHfsBlockSize &&
               isPowerOfTwo(allocationBlockSize);
    }

    [[nodiscard]] std::optional<uint64_t> wrapperEmbeddedOffset(
        uint16_t allocationStartSector,
        uint16_t extentStartBlock,
        uint32_t allocationBlockSize) const {
        uint64_t allocationStartBytes = 0;
        uint64_t extentStartBytes = 0;
        uint64_t embeddedOffset = 0;
        if (!checkedMul(allocationStartSector, kHfsWrapperSectorBytes, &allocationStartBytes) ||
            !checkedMul(extentStartBlock, allocationBlockSize, &extentStartBytes) ||
            !checkedAdd(allocationStartBytes, extentStartBytes, &embeddedOffset)) {
            return std::nullopt;
        }
        return embeddedOffset;
    }

    [[nodiscard]] std::optional<QByteArray> readVolumeHeaderAt(uint64_t volumeOffset) {
        uint64_t headerOffset = 0;
        if (!checkedAdd(volumeOffset, kHfsVolumeHeaderOffset, &headerOffset)) {
            return std::nullopt;
        }
        const auto header = readAt(headerOffset, kHfsVolumeHeaderSize);
        if (!header.has_value() || hfsFamilyName(*header, 0).isEmpty()) {
            return std::nullopt;
        }
        return header;
    }

    void parseVolumeHeader(const QByteArray& bytes, uint64_t volumeOffset, bool wrapped) {
        m_volume.file_system = hfsFamilyName(bytes, 0);
        m_volume.volume_offset = volumeOffset;
        m_volume.wrapped = wrapped;
        m_volume.attributes = be32(bytes, kHfsAttributesOffset);
        m_volume.file_count = be32(bytes, kHfsFileCountOffset);
        m_volume.folder_count = be32(bytes, kHfsFolderCountOffset);
        m_volume.next_catalog_id = be32(bytes, kHfsNextCatalogIdOffset);
        m_volume.block_size = be32(bytes, kHfsBlockSizeOffset);
        m_volume.total_blocks = be32(bytes, kHfsTotalBlocksOffset);
        m_volume.free_blocks = be32(bytes, kHfsFreeBlocksOffset);
        m_volume.allocation_fork = parseFork(bytes, kHfsAllocationForkOffset);
        m_volume.extents_fork = parseFork(bytes, kHfsExtentsForkOffset);
        m_volume.catalog_fork = parseFork(bytes, kHfsCatalogForkOffset);
        m_volume.attributes_fork = parseFork(bytes, kHfsAttributesForkOffset);

        const uint16_t version = be16(bytes, kHfsVersionOffset);
        if (m_volume.file_system == QStringLiteral("HFS+") && version != kHfsPlusVersion) {
            m_blockers.append(QStringLiteral("Unsupported HFS+ version"));
        }
        if (m_volume.file_system == QStringLiteral("HFSX") && version < kHfsXMinimumVersion) {
            m_blockers.append(QStringLiteral("Unsupported HFSX version"));
        }
    }

    [[nodiscard]] HfsForkData parseFork(const QByteArray& bytes, qsizetype offset) const {
        HfsForkData fork;
        fork.logical_size = be64(bytes, offset + kHfsForkLogicalSizeOffset);
        fork.total_blocks = be32(bytes, offset + kHfsForkTotalBlocksOffset);
        for (int index = 0; index < kHfsExtentCount; ++index) {
            const qsizetype extentOffset = offset + kHfsForkExtentsOffset + index * kHfsExtentBytes;
            const uint32_t start = be32(bytes, extentOffset + kHfsExtentStartBlockOffset);
            const uint32_t count = be32(bytes, extentOffset + kHfsExtentBlockCountOffset);
            if (count == 0) {
                continue;
            }
            fork.extents.append(HfsExtent{.start_block = start, .block_count = count});
        }
        return fork;
    }

    void validateVolume() {
        appendVolumeGeometryBlockers();
        appendCatalogForkBlockers();
        appendVolumeWarnings();
    }

    void appendVolumeGeometryBlockers() {
        if (m_volume.block_size < kMinimumHfsBlockSize ||
            m_volume.block_size > kMaximumHfsBlockSize || !isPowerOfTwo(m_volume.block_size) ||
            m_volume.total_blocks == 0 || m_volume.free_blocks > m_volume.total_blocks) {
            m_blockers.append(QStringLiteral("Unsupported HFS+ volume geometry"));
        }
    }

    void appendCatalogForkBlockers() {
        if (m_volume.catalog_fork.logical_size == 0 || m_volume.catalog_fork.extents.isEmpty()) {
            m_blockers.append(QStringLiteral("HFS+ catalog fork is not available"));
        }
        if (forkNeedsOverflow(m_volume.catalog_fork) &&
            (m_volume.extents_fork.logical_size == 0 || m_volume.extents_fork.extents.isEmpty())) {
            m_blockers.append(QStringLiteral(
                "HFS+ catalog overflow extents require a readable extents overflow file"));
        }
        if (m_volume.extents_fork.logical_size > 0 &&
            !forkCoveredByInitialExtents(m_volume.extents_fork)) {
            m_blockers.append(QStringLiteral(
                "HFS+ extents overflow file recursive overflow extents are not supported"));
        }
    }

    void appendVolumeWarnings() {
        if ((m_volume.attributes & kHfsVolumeJournaledMask) != 0) {
            m_warnings.append(
                QStringLiteral("HFS+ journal replay is not performed in read-only browser mode"));
        }
        if ((m_volume.attributes & kHfsVolumeInconsistentMask) != 0) {
            m_warnings.append(QStringLiteral("HFS+ volume is marked inconsistent"));
        }
        if (m_volume.wrapped) {
            m_warnings.append(QStringLiteral("HFS wrapper volume was detected"));
        }
    }

    [[nodiscard]] bool forkCoveredByInitialExtents(const HfsForkData& fork) const {
        return fork.logical_size <= extentCoverageBytes(fork, m_volume.block_size);
    }

    [[nodiscard]] bool forkNeedsOverflow(const HfsForkData& fork) const {
        return fork.logical_size > extentCoverageBytes(fork, m_volume.block_size);
    }

    void loadExtentsOverflowRecords() {
        if (m_volume.extents_fork.logical_size == 0 || m_volume.extents_fork.extents.isEmpty()) {
            return;
        }

        const auto bytes =
            readForkBytesInitialExtents(m_volume.extents_fork, 0, kBTreeHeaderMinimumReadBytes);
        if (!bytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read HFS+ extents overflow B-tree header"));
            return;
        }
        if (!parseBTreeHeader(*bytes, QStringLiteral("extents overflow"), &m_extents)) {
            return;
        }
        validateExtentsHeader();
        if (!m_blockers.isEmpty() || m_extents.leaf_records == 0) {
            return;
        }
        loadExtentsOverflowLeafRecords();
    }

    void loadExtentsOverflowLeafRecords() {
        uint32_t nodeNumber = m_extents.first_leaf_node;
        int visited = 0;
        while (nodeNumber != 0 && visited < kMaxCatalogLeafNodesToScan) {
            const auto node = readExtentsNode(nodeNumber);
            if (!node.has_value()) {
                return;
            }
            if (static_cast<int8_t>(node->at(kBTreeKindOffset)) != kBTreeLeafNode ||
                static_cast<uint8_t>(node->at(kBTreeHeightOffset)) != 1) {
                m_blockers.append(QStringLiteral("Invalid HFS+ extents overflow leaf node"));
                return;
            }
            appendExtentsOverflowRecords(*node);
            if (!m_blockers.isEmpty()) {
                return;
            }
            nodeNumber = be32(*node, 0);
            ++visited;
        }
        if (visited >= kMaxCatalogLeafNodesToScan) {
            m_blockers.append(QStringLiteral("HFS+ extents overflow scan limit reached"));
        }
    }

    void loadCatalogHeader() {
        const auto bytes = readForkBytes(m_volume.catalog_fork,
                                         kHfsCatalogFileId,
                                         kHfsDataForkType,
                                         0,
                                         kBTreeHeaderMinimumReadBytes);
        if (!bytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read HFS+ catalog B-tree header"));
            return;
        }
        if (!parseBTreeHeader(*bytes, QStringLiteral("catalog"), &m_catalog)) {
            return;
        }
        validateCatalogHeader();
    }

    [[nodiscard]] bool parseBTreeHeader(const QByteArray& bytes,
                                        const QString& label,
                                        HfsBTreeHeader* headerRecord) {
        if (!headerRecord) {
            return false;
        }
        if (static_cast<int8_t>(bytes.at(kBTreeKindOffset)) != kBTreeHeaderNode) {
            m_blockers.append(
                QStringLiteral("HFS+ %1 B-tree header node is invalid (kind=%2, bytes=%3)")
                    .arg(label)
                    .arg(static_cast<int>(static_cast<uint8_t>(bytes.at(kBTreeKindOffset))))
                    .arg(QString::fromLatin1(bytes.left(16).toHex())));
            return false;
        }
        const qsizetype header = kBTreeHeaderRecordOffset;
        headerRecord->tree_depth = be16(bytes, header + kBTreeHeaderTreeDepthOffset);
        headerRecord->root_node = be32(bytes, header + kBTreeHeaderRootNodeOffset);
        headerRecord->leaf_records = be32(bytes, header + kBTreeHeaderLeafRecordsOffset);
        headerRecord->first_leaf_node = be32(bytes, header + kBTreeHeaderFirstLeafNodeOffset);
        headerRecord->last_leaf_node = be32(bytes, header + kBTreeHeaderLastLeafNodeOffset);
        headerRecord->node_size = be16(bytes, header + kBTreeHeaderNodeSizeOffset);
        headerRecord->max_key_length = be16(bytes, header + kBTreeHeaderMaxKeyLengthOffset);
        headerRecord->total_nodes = be32(bytes, header + kBTreeHeaderTotalNodesOffset);
        headerRecord->free_nodes = be32(bytes, header + kBTreeHeaderFreeNodesOffset);
        headerRecord->key_compare_type =
            static_cast<uint8_t>(bytes.at(header + kBTreeHeaderKeyCompareTypeOffset));
        headerRecord->attributes_mask = be32(bytes, header + kBTreeHeaderAttributesOffset);
        return true;
    }

    void validateCatalogHeader() {
        if (m_catalog.node_size < kMinimumHfsCatalogNodeSize ||
            m_catalog.node_size > kMaximumHfsCatalogNodeSize ||
            !isPowerOfTwo(m_catalog.node_size)) {
            m_blockers.append(QStringLiteral("Unsupported HFS+ catalog node size"));
        }
        if (m_catalog.total_nodes == 0 ||
            (m_catalog.leaf_records > 0 && m_catalog.first_leaf_node == 0) ||
            m_catalog.first_leaf_node >= m_catalog.total_nodes ||
            m_catalog.last_leaf_node >= m_catalog.total_nodes ||
            m_catalog.root_node >= m_catalog.total_nodes) {
            m_blockers.append(QStringLiteral("Invalid HFS+ catalog B-tree geometry"));
        }
    }

    void validateExtentsHeader() {
        if (m_extents.node_size < kMinimumHfsBTreeNodeSize ||
            m_extents.node_size > kMaximumHfsCatalogNodeSize ||
            !isPowerOfTwo(m_extents.node_size)) {
            m_blockers.append(QStringLiteral("Unsupported HFS+ extents overflow node size"));
        }
        if (extentsHeaderGeometryInvalid()) {
            m_blockers.append(QStringLiteral("Invalid HFS+ extents overflow B-tree geometry"));
        }
    }

    [[nodiscard]] bool extentsHeaderGeometryInvalid() const {
        return m_extents.total_nodes == 0 || extentsLeafRangeInvalid() || extentsRootNodeInvalid();
    }

    [[nodiscard]] bool extentsLeafRangeInvalid() const {
        return (m_extents.leaf_records > 0 && m_extents.first_leaf_node == 0) ||
               extentsNodeNumberOutOfRange(m_extents.first_leaf_node) ||
               extentsNodeNumberOutOfRange(m_extents.last_leaf_node);
    }

    [[nodiscard]] bool extentsRootNodeInvalid() const {
        return m_extents.root_node >= m_extents.total_nodes &&
               !(m_extents.leaf_records == 0 && m_extents.root_node == 0);
    }

    [[nodiscard]] bool extentsNodeNumberOutOfRange(uint32_t nodeNumber) const {
        return nodeNumber != 0 && nodeNumber >= m_extents.total_nodes;
    }

    [[nodiscard]] std::optional<QByteArray> readAt(uint64_t offset, qsizetype length) {
        if (length < 0 || offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
            return std::nullopt;
        }
        if (!m_device->seek(static_cast<qint64>(offset))) {
            return std::nullopt;
        }
        QByteArray bytes = m_device->read(length);
        if (bytes.size() != length) {
            return std::nullopt;
        }
        return bytes;
    }

    [[nodiscard]] std::optional<QByteArray> readForkBytes(const HfsForkData& fork,
                                                          uint32_t fileId,
                                                          uint8_t forkType,
                                                          uint64_t offset,
                                                          uint64_t length) {
        if (!forkReadRequestIsValid(fork, offset, length)) {
            m_blockers.append(QStringLiteral("Requested HFS+ fork read is out of range"));
            return std::nullopt;
        }

        QByteArray output;
        output.reserve(static_cast<qsizetype>(length));
        uint64_t remaining = length;
        uint64_t cursor = offset;
        while (remaining > 0) {
            const auto chunk = readForkChunk(fork, fileId, forkType, cursor, remaining);
            if (!chunk.has_value()) {
                return std::nullopt;
            }
            output.append(chunk->bytes);
            cursor += chunk->size;
            remaining -= chunk->size;
        }
        return output;
    }

    [[nodiscard]] std::optional<QByteArray> readForkBytesInitialExtents(const HfsForkData& fork,
                                                                        uint64_t offset,
                                                                        uint64_t length) {
        if (!forkReadRequestIsValid(fork, offset, length)) {
            m_blockers.append(QStringLiteral("Requested HFS+ fork read is out of range"));
            return std::nullopt;
        }

        QByteArray output;
        output.reserve(static_cast<qsizetype>(length));
        uint64_t remaining = length;
        uint64_t cursor = offset;
        while (remaining > 0) {
            const auto chunk = readForkChunkInitialExtents(fork, cursor, remaining);
            if (!chunk.has_value()) {
                return std::nullopt;
            }
            output.append(chunk->bytes);
            cursor += chunk->size;
            remaining -= chunk->size;
        }
        return output;
    }

    [[nodiscard]] bool forkReadRequestIsValid(const HfsForkData& fork,
                                              uint64_t offset,
                                              uint64_t length) const {
        return length <= kMaxForkReadBytes &&
               length <= static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()) &&
               offset <= fork.logical_size && length <= fork.logical_size - offset;
    }

    [[nodiscard]] bool forkWriteRequestFitsAllocation(const HfsForkData& fork,
                                                      uint64_t offset,
                                                      uint64_t length) const {
        const auto allocatedBytes = forkAllocatedBytes(fork);
        return allocatedBytes.has_value() && length <= kMaxForkReadBytes &&
               length <= static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()) &&
               offset <= *allocatedBytes && length <= *allocatedBytes - offset;
    }

    [[nodiscard]] std::optional<uint64_t> forkAllocatedBytes(const HfsForkData& fork) const {
        uint64_t bytes = 0;
        if (!checkedMul(fork.total_blocks, m_volume.block_size, &bytes)) {
            return std::nullopt;
        }
        return bytes;
    }

    [[nodiscard]] std::optional<HfsForkReadChunk> readForkChunk(const HfsForkData& fork,
                                                                uint32_t fileId,
                                                                uint8_t forkType,
                                                                uint64_t cursor,
                                                                uint64_t remaining) {
        const uint64_t withinBlock = cursor % m_volume.block_size;
        const uint64_t chunkSize = std::min<uint64_t>(remaining, m_volume.block_size - withinBlock);
        const auto deviceOffset =
            deviceOffsetForForkCursor(fork, fileId, forkType, cursor, withinBlock);
        if (!deviceOffset.has_value()) {
            return std::nullopt;
        }
        const auto bytes = readAt(*deviceOffset, static_cast<qsizetype>(chunkSize));
        if (!bytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read HFS+ fork data"));
            return std::nullopt;
        }
        return HfsForkReadChunk{.bytes = *bytes, .size = chunkSize};
    }

    [[nodiscard]] std::optional<HfsForkReadChunk> readForkChunkInitialExtents(
        const HfsForkData& fork, uint64_t cursor, uint64_t remaining) {
        const uint64_t withinBlock = cursor % m_volume.block_size;
        const uint64_t chunkSize = std::min<uint64_t>(remaining, m_volume.block_size - withinBlock);
        const auto deviceOffset = deviceOffsetForInitialForkCursor(fork, cursor, withinBlock);
        if (!deviceOffset.has_value()) {
            return std::nullopt;
        }
        const auto bytes = readAt(*deviceOffset, static_cast<qsizetype>(chunkSize));
        if (!bytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read HFS+ fork data"));
            return std::nullopt;
        }
        return HfsForkReadChunk{.bytes = *bytes, .size = chunkSize};
    }

    [[nodiscard]] std::optional<uint64_t> deviceOffsetForForkCursor(const HfsForkData& fork,
                                                                    uint32_t fileId,
                                                                    uint8_t forkType,
                                                                    uint64_t cursor,
                                                                    uint64_t withinBlock) {
        const auto block = physicalBlockForForkOffset(fork, fileId, forkType, cursor);
        if (!block.has_value()) {
            return std::nullopt;
        }
        return deviceOffsetForPhysicalBlock(*block, withinBlock);
    }

    [[nodiscard]] std::optional<uint64_t> deviceOffsetForInitialForkCursor(const HfsForkData& fork,
                                                                           uint64_t cursor,
                                                                           uint64_t withinBlock) {
        const uint64_t logicalBlock = cursor / m_volume.block_size;
        const auto block = physicalBlockInExtents(fork.extents, 0, logicalBlock);
        if (!block.has_value()) {
            m_blockers.append(
                QStringLiteral("HFS+ initial extents do not cover requested fork read"));
            return std::nullopt;
        }
        return deviceOffsetForPhysicalBlock(*block, withinBlock);
    }

    [[nodiscard]] std::optional<uint64_t> deviceOffsetForPhysicalBlock(uint64_t block,
                                                                       uint64_t withinBlock) {
        uint64_t deviceOffset = 0;
        uint64_t blockOffset = 0;
        if (!checkedMul(block, m_volume.block_size, &blockOffset) ||
            !checkedAdd(m_volume.volume_offset, blockOffset, &deviceOffset) ||
            !checkedAdd(deviceOffset, withinBlock, &deviceOffset)) {
            m_blockers.append(QStringLiteral("HFS+ device offset overflow"));
            return std::nullopt;
        }
        return deviceOffset;
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockForForkOffset(const HfsForkData& fork,
                                                                     uint32_t fileId,
                                                                     uint8_t forkType,
                                                                     uint64_t offset) {
        const uint64_t logicalBlock = offset / m_volume.block_size;
        const auto initialBlock = physicalBlockInExtents(fork.extents, 0, logicalBlock);
        if (initialBlock.has_value()) {
            return initialBlock;
        }
        const auto overflowBlock = physicalBlockFromOverflowExtents(fileId, forkType, logicalBlock);
        if (overflowBlock.has_value()) {
            return overflowBlock;
        }
        m_blockers.append(QStringLiteral("HFS+ overflow extent record was not found"));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockInExtents(const QVector<HfsExtent>& extents,
                                                                 uint64_t logicalBase,
                                                                 uint64_t logicalBlock) const {
        for (const auto& extent : extents) {
            uint64_t extentEnd = 0;
            if (!checkedAdd(logicalBase, extent.block_count, &extentEnd)) {
                return std::nullopt;
            }
            if (logicalBlock >= logicalBase && logicalBlock < extentEnd) {
                return static_cast<uint64_t>(extent.start_block) + (logicalBlock - logicalBase);
            }
            logicalBase = extentEnd;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockFromOverflowExtents(
        uint32_t fileId, uint8_t forkType, uint64_t logicalBlock) const {
        const HfsOverflowExtentRecord* best = nullptr;
        for (const auto& record : m_overflow_extents) {
            if (record.file_id != fileId || record.fork_type != forkType ||
                record.start_block > logicalBlock) {
                continue;
            }
            if (!best || record.start_block > best->start_block) {
                best = &record;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        return physicalBlockInExtents(best->extents, best->start_block, logicalBlock);
    }

    [[nodiscard]] std::optional<QByteArray> readCatalogNode(uint32_t nodeNumber) {
        if (nodeNumber >= m_catalog.total_nodes) {
            m_blockers.append(QStringLiteral("HFS+ catalog node is out of range"));
            return std::nullopt;
        }
        uint64_t offset = 0;
        if (!checkedMul(nodeNumber, m_catalog.node_size, &offset)) {
            m_blockers.append(QStringLiteral("HFS+ catalog node offset overflow"));
            return std::nullopt;
        }
        return readForkBytes(m_volume.catalog_fork,
                             kHfsCatalogFileId,
                             kHfsDataForkType,
                             offset,
                             m_catalog.node_size);
    }

    [[nodiscard]] std::optional<HfsCatalogScanSummary> scanCatalogRecords(int maxRecords) {
        HfsCatalogScanSummary summary;
        uint32_t nodeNumber = m_catalog.first_leaf_node;
        int visited = 0;
        while (nodeNumber != 0 && visited < kMaxCatalogLeafNodesToScan) {
            const auto node = readCatalogNode(nodeNumber);
            if (!node.has_value() ||
                !scanCatalogLeafNode(*node, nodeNumber, maxRecords, &summary)) {
                return std::nullopt;
            }
            nodeNumber = be32(*node, 0);
            ++visited;
        }
        summary.leaf_nodes = visited;
        appendCatalogScanWarnings(summary);
        if (visited >= kMaxCatalogLeafNodesToScan) {
            m_blockers.append(QStringLiteral("HFS+ catalog scan limit reached"));
            return std::nullopt;
        }
        return m_blockers.isEmpty() ? std::optional<HfsCatalogScanSummary>(summary) : std::nullopt;
    }

    [[nodiscard]] bool scanCatalogLeafNode(const QByteArray& node,
                                           uint32_t nodeNumber,
                                           int maxRecords,
                                           HfsCatalogScanSummary* summary) {
        if (static_cast<int8_t>(node.at(kBTreeKindOffset)) != kBTreeLeafNode ||
            static_cast<uint8_t>(node.at(kBTreeHeightOffset)) != 1) {
            m_blockers.append(QStringLiteral("Invalid HFS+ catalog leaf node"));
            return false;
        }
        const uint16_t numRecords = be16(node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(node, numRecords);
        if (!offsets.has_value()) {
            return false;
        }
        for (int index = 0; index < offsets->size(); ++index) {
            if (summary->records >= maxRecords) {
                m_blockers.append(QStringLiteral("HFS+ catalog consistency record cap reached"));
                return false;
            }
            if (!scanCatalogRecord(node, nodeNumber, *offsets, index, summary)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool scanCatalogRecord(const QByteArray& node,
                                         uint32_t nodeNumber,
                                         const QVector<qsizetype>& offsets,
                                         int index,
                                         HfsCatalogScanSummary* summary) const {
        const qsizetype start = offsets.at(index);
        const qsizetype end = recordEnd(node, offsets, index);
        if (end <= start || end > node.size()) {
            ++summary->invalid_records;
            return true;
        }
        const auto record = parseCatalogRecord(node, start, end, nodeNumber);
        if (!record.has_value()) {
            ++summary->invalid_records;
            return true;
        }
        ++summary->records;
        appendCatalogRecordSummary(*record, summary);
        return true;
    }

    void appendCatalogRecordSummary(const HfsCatalogRecord& record,
                                    HfsCatalogScanSummary* summary) const {
        if (record.directory()) {
            ++summary->directories;
        } else if (record.regularFile()) {
            ++summary->files;
        } else if (record.record_type == kHfsCatalogFolderThreadRecord ||
                   record.record_type == kHfsCatalogFileThreadRecord) {
            ++summary->threads;
        } else {
            ++summary->other_records;
        }
    }

    void appendCatalogScanWarnings(const HfsCatalogScanSummary& summary) {
        const int scannedRecords = summary.records + summary.invalid_records;
        if (scannedRecords != static_cast<int>(m_catalog.leaf_records)) {
            m_warnings.append(
                QStringLiteral("HFS+ catalog leaf record count mismatch: header=%1 scanned=%2")
                    .arg(m_catalog.leaf_records)
                    .arg(scannedRecords));
        }
        if (summary.invalid_records > 0) {
            m_warnings.append(QStringLiteral("Skipped %1 invalid HFS+ catalog records")
                                  .arg(summary.invalid_records));
        }
    }

    void appendConsistencyDetails(const HfsCatalogScanSummary& summary,
                                  QStringList* details) const {
        details->append(QStringLiteral("Volume header: valid %1").arg(m_volume.file_system));
        details->append(QStringLiteral("Volume offset: %1").arg(m_volume.volume_offset));
        details->append(QStringLiteral("Block size: %1").arg(m_volume.block_size));
        details->append(QStringLiteral("Total blocks: %1").arg(m_volume.total_blocks));
        details->append(QStringLiteral("Free blocks: %1").arg(m_volume.free_blocks));
        details->append(QStringLiteral("Catalog node size: %1").arg(m_catalog.node_size));
        details->append(
            QStringLiteral("Catalog leaf records expected: %1").arg(m_catalog.leaf_records));
        details->append(QStringLiteral("Catalog records scanned: %1").arg(summary.records));
        details->append(
            QStringLiteral("Invalid catalog records skipped: %1").arg(summary.invalid_records));
        details->append(QStringLiteral("Catalog leaf nodes scanned: %1").arg(summary.leaf_nodes));
        details->append(QStringLiteral("Directories: %1").arg(summary.directories));
        details->append(QStringLiteral("Files: %1").arg(summary.files));
        details->append(QStringLiteral("Thread records: %1").arg(summary.threads));
        details->append(QStringLiteral("Other records: %1").arg(summary.other_records));
    }

    [[nodiscard]] std::optional<HfsAttributeScanSummary> scanAttributeRecords(int maxRecords) {
        HfsAttributeScanSummary summary;
        if (m_volume.attributes_fork.logical_size == 0 ||
            m_volume.attributes_fork.extents.isEmpty()) {
            return summary;
        }
        summary.present = true;

        HfsBTreeHeader attributes;
        if (!loadAttributeHeader(&attributes)) {
            return std::nullopt;
        }
        uint32_t nodeNumber = attributes.first_leaf_node;
        int visited = 0;
        while (nodeNumber != 0 && visited < kMaxCatalogLeafNodesToScan) {
            const auto node = readAttributeNode(attributes, nodeNumber);
            if (!node.has_value() ||
                !scanAttributeLeafNode(
                    *node, attributes.node_size, nodeNumber, maxRecords, &summary)) {
                return std::nullopt;
            }
            nodeNumber = be32(*node, 0);
            ++visited;
        }
        summary.leaf_nodes = visited;
        if (visited >= kMaxCatalogLeafNodesToScan) {
            m_blockers.append(QStringLiteral("HFS+ attributes B-tree scan limit reached"));
            return std::nullopt;
        }
        return summary;
    }

    [[nodiscard]] bool loadAttributeHeader(HfsBTreeHeader* attributes) {
        const auto bytes = readForkBytes(m_volume.attributes_fork,
                                         kHfsAttributesFileId,
                                         kHfsDataForkType,
                                         0,
                                         kBTreeHeaderMinimumReadBytes);
        if (!bytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read HFS+ attributes B-tree header"));
            return false;
        }
        if (!parseBTreeHeader(*bytes, QStringLiteral("attributes"), attributes)) {
            return false;
        }
        return validateAttributeHeader(*attributes);
    }

    [[nodiscard]] bool validateAttributeHeader(const HfsBTreeHeader& attributes) {
        if (attributes.node_size < kMinimumHfsCatalogNodeSize ||
            attributes.node_size > kMaximumHfsCatalogNodeSize ||
            !isPowerOfTwo(attributes.node_size)) {
            m_blockers.append(QStringLiteral("Unsupported HFS+ attributes node size"));
            return false;
        }
        if (attributes.total_nodes == 0 ||
            (attributes.leaf_records > 0 && attributes.first_leaf_node == 0) ||
            attributes.first_leaf_node >= attributes.total_nodes ||
            attributes.last_leaf_node >= attributes.total_nodes ||
            attributes.root_node >= attributes.total_nodes) {
            m_blockers.append(QStringLiteral("Invalid HFS+ attributes B-tree geometry"));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<QByteArray> readAttributeNode(const HfsBTreeHeader& attributes,
                                                              uint32_t nodeNumber) {
        if (nodeNumber >= attributes.total_nodes) {
            m_blockers.append(QStringLiteral("HFS+ attributes node is out of range"));
            return std::nullopt;
        }
        uint64_t offset = 0;
        if (!checkedMul(nodeNumber, attributes.node_size, &offset)) {
            m_blockers.append(QStringLiteral("HFS+ attributes node offset overflow"));
            return std::nullopt;
        }
        return readForkBytes(m_volume.attributes_fork,
                             kHfsAttributesFileId,
                             kHfsDataForkType,
                             offset,
                             attributes.node_size);
    }

    [[nodiscard]] bool scanAttributeLeafNode(const QByteArray& node,
                                             uint16_t nodeSize,
                                             uint32_t nodeNumber,
                                             int maxRecords,
                                             HfsAttributeScanSummary* summary) {
        if (static_cast<int8_t>(node.at(kBTreeKindOffset)) != kBTreeLeafNode ||
            static_cast<uint8_t>(node.at(kBTreeHeightOffset)) != 1) {
            m_blockers.append(QStringLiteral("Invalid HFS+ attributes leaf node"));
            return false;
        }
        const uint16_t numRecords = be16(node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(node, numRecords);
        if (!offsets.has_value()) {
            return false;
        }
        for (int index = 0; index < offsets->size(); ++index) {
            if (summary->records >= maxRecords) {
                m_blockers.append(QStringLiteral("HFS+ attributes consistency record cap reached"));
                return false;
            }
            const HfsAttributeLeafContext context{.node = node,
                                                  .offsets = *offsets,
                                                  .node_size = nodeSize,
                                                  .node_number = nodeNumber};
            scanAttributeRecord(context, index, summary);
            if (!m_blockers.isEmpty()) {
                return false;
            }
        }
        return true;
    }

    void scanAttributeRecord(const HfsAttributeLeafContext& context,
                             int index,
                             HfsAttributeScanSummary* summary) {
        const qsizetype start = context.offsets.at(index);
        const qsizetype end = recordEnd(context.node, context.offsets, index);
        if (end <= start || end > context.node.size()) {
            m_blockers.append(QStringLiteral("Invalid HFS+ attributes record length"));
            return;
        }
        const auto record =
            parseAttributeRecord(context.node,
                                 HfsAttributeRecordBounds{.offset = start,
                                                          .end = end,
                                                          .node_size = context.node_size,
                                                          .node_number = context.node_number});
        ++summary->records;
        if (!record.has_value()) {
            ++summary->other_records;
            m_warnings.append(QStringLiteral(
                "Skipped unsupported HFS+ attributes record key while scanning read-only"));
            return;
        }
        summary->parsed_records.append(*record);
        appendAttributeRecordSummary(*record, summary);
    }

    [[nodiscard]] std::optional<HfsAttributeRecord> parseAttributeRecord(
        const QByteArray& node, const HfsAttributeRecordBounds& bounds) const {
        const auto key = parseAttributeKey(node, bounds.offset, bounds.end);
        if (!key.has_value()) {
            return std::nullopt;
        }

        HfsAttributeRecord record;
        record.file_id = key->file_id;
        record.start_block = key->start_block;
        if (!attributeDataOffset(bounds, key->data_offset, &record.attribute_data_offset)) {
            return std::nullopt;
        }
        record.record_type = be32(node, key->data_offset);
        record.name = key->name;
        parseAttributePayload(node, key->data_offset, bounds.end, &record);
        return record;
    }

    [[nodiscard]] std::optional<HfsAttributeKeyInfo> parseAttributeKey(const QByteArray& node,
                                                                       qsizetype offset,
                                                                       qsizetype end) const {
        if (!hasBytes(node, offset, kUint16Size)) {
            return std::nullopt;
        }
        const uint16_t keyLength = be16(node, offset);
        if (keyLength < kHfsAttributeMinimumKeyBytes || keyLength > kHfsAttributeMaximumKeyBytes ||
            !hasBytes(node, offset, kUint16Size + keyLength)) {
            return std::nullopt;
        }
        const qsizetype dataOffset = offset + kUint16Size + keyLength;
        if (dataOffset + kHfsAttributeRecordTypeBytes > end) {
            return std::nullopt;
        }
        const uint16_t nameLength = be16(node, offset + kHfsAttributeKeyNameLengthOffset);
        if (nameLength > kHfsMaximumNameChars) {
            return std::nullopt;
        }
        const qsizetype nameBytes = static_cast<qsizetype>(nameLength) * kUint16Size;
        const qsizetype nameOffset = offset + kHfsAttributeKeyNameOffset;
        if (nameOffset > dataOffset || dataOffset - nameOffset < nameBytes) {
            return std::nullopt;
        }

        HfsAttributeKeyInfo key;
        key.file_id = be32(node, offset + kHfsAttributeKeyFileIdOffset);
        key.start_block = be32(node, offset + kHfsAttributeKeyStartBlockOffset);
        key.data_offset = dataOffset;
        key.name = unicodeField(node, nameOffset, nameLength);
        if (key.name.contains(QChar::Null)) {
            return std::nullopt;
        }
        return key;
    }

    [[nodiscard]] bool attributeDataOffset(const HfsAttributeRecordBounds& bounds,
                                           qsizetype dataOffset,
                                           uint64_t* output) const {
        uint64_t nodeBase = 0;
        return dataOffset >= 0 && checkedMul(bounds.node_number, bounds.node_size, &nodeBase) &&
               checkedAdd(nodeBase, static_cast<uint64_t>(dataOffset), output);
    }

    void parseAttributePayload(const QByteArray& node,
                               qsizetype dataOffset,
                               qsizetype end,
                               HfsAttributeRecord* record) const {
        if (record->record_type == kHfsAttributeInlineDataRecord) {
            parseInlineAttributePayload(node, dataOffset, end, record);
            return;
        }

        const qsizetype typedPayloadOffset = dataOffset + kHfsAttributeForkDataOffset;
        if (record->record_type == kHfsAttributeForkDataRecord) {
            if (!hasBytes(node, typedPayloadOffset, kHfsForkDataBytes) ||
                typedPayloadOffset + kHfsForkDataBytes > end) {
                record->payload_complete = false;
                return;
            }
            const HfsForkData fork = parseFork(node, typedPayloadOffset);
            record->has_fork_data = true;
            record->fork_data = fork;
            record->fork_logical_size = fork.logical_size;
            record->fork_total_blocks = fork.total_blocks;
            record->fork_extent_count = fork.extents.size();
            return;
        }

        if (record->record_type == kHfsAttributeExtentsRecord) {
            if (!hasBytes(node, typedPayloadOffset, kHfsExtentsRecordBytes) ||
                typedPayloadOffset + kHfsExtentsRecordBytes > end) {
                record->payload_complete = false;
                return;
            }
            record->has_extent_data = true;
            record->extent_count = countExtents(node, typedPayloadOffset);
            record->extents = parseExtents(node, typedPayloadOffset);
        }
    }

    void parseInlineAttributePayload(const QByteArray& node,
                                     qsizetype dataOffset,
                                     qsizetype end,
                                     HfsAttributeRecord* record) const {
        const qsizetype sizeOffset = dataOffset + kHfsAttributeInlineSizeOffset;
        const qsizetype valueOffset = dataOffset + kHfsAttributeInlineDataOffset;
        if (!hasBytes(node, sizeOffset, kUint32Size) || valueOffset > end) {
            record->payload_complete = false;
            record->inline_available_bytes =
                std::max<qsizetype>(0, end - (dataOffset + kHfsAttributeRecordTypeBytes));
            return;
        }
        record->inline_size = be32(node, sizeOffset);
        if (!checkedAdd(record->attribute_data_offset,
                        kHfsAttributeInlineSizeOffset,
                        &record->inline_size_offset) ||
            !checkedAdd(record->attribute_data_offset,
                        kHfsAttributeInlineDataOffset,
                        &record->inline_data_offset)) {
            record->payload_complete = false;
            return;
        }
        record->inline_available_bytes = std::max<qsizetype>(0, end - valueOffset);
        if (record->inline_size > static_cast<uint32_t>(record->inline_available_bytes)) {
            record->payload_complete = false;
            return;
        }
        record->has_inline_data = true;
        record->inline_data = node.mid(valueOffset, static_cast<qsizetype>(record->inline_size));
    }

    [[nodiscard]] int countExtents(const QByteArray& node, qsizetype offset) const {
        return parseExtents(node, offset).size();
    }

    [[nodiscard]] QVector<HfsExtent> parseExtents(const QByteArray& node, qsizetype offset) const {
        QVector<HfsExtent> extents;
        for (int index = 0; index < kHfsExtentCount; ++index) {
            const qsizetype extentOffset = offset + index * kHfsExtentBytes;
            const uint32_t start = be32(node, extentOffset + kHfsExtentStartBlockOffset);
            const uint32_t blockCount = be32(node, extentOffset + kHfsExtentBlockCountOffset);
            if (blockCount != 0) {
                extents.append(HfsExtent{.start_block = start, .block_count = blockCount});
            }
        }
        return extents;
    }

    [[nodiscard]] QString unicodeField(const QByteArray& node,
                                       qsizetype offset,
                                       uint16_t length) const {
        QString value;
        value.reserve(length);
        for (uint16_t index = 0; index < length; ++index) {
            value.append(QChar(be16(node, offset + index * kUint16Size)));
        }
        return value;
    }

    void appendAttributeRecordSummary(const HfsAttributeRecord& record,
                                      HfsAttributeScanSummary* summary) const {
        if (record.record_type == kHfsAttributeInlineDataRecord) {
            ++summary->inline_records;
        } else if (record.record_type == kHfsAttributeForkDataRecord) {
            ++summary->fork_records;
        } else if (record.record_type == kHfsAttributeExtentsRecord) {
            ++summary->extent_records;
        } else {
            ++summary->other_records;
        }
        const QString name = record.name.trimmed();
        if (!name.isEmpty() && !summary->names.contains(name)) {
            summary->names.append(name);
        }
        const QString metadata = attributeRecordMetadata(record);
        if (!metadata.isEmpty()) {
            summary->metadata.append(metadata);
        }
    }

    [[nodiscard]] QString attributeRecordMetadata(const HfsAttributeRecord& record) const {
        const QString label =
            record.name.trimmed().isEmpty()
                ? QStringLiteral("file-id %1").arg(record.file_id)
                : QStringLiteral("%1 on file-id %2").arg(record.name.trimmed()).arg(record.file_id);
        if (!record.payload_complete) {
            return QStringLiteral("%1: record metadata truncated").arg(label);
        }
        if (record.record_type == kHfsAttributeInlineDataRecord) {
            if (!record.payload_complete) {
                return QStringLiteral("%1: inline attribute metadata truncated").arg(label);
            }
            return QStringLiteral("%1: inline attribute size %2 bytes")
                .arg(label)
                .arg(record.inline_size);
        }
        if (record.record_type == kHfsAttributeForkDataRecord && record.has_fork_data) {
            return QStringLiteral("%1: fork attribute size %2 bytes, blocks %3, extents %4")
                .arg(label)
                .arg(record.fork_logical_size)
                .arg(record.fork_total_blocks)
                .arg(record.fork_extent_count);
        }
        if (record.record_type == kHfsAttributeExtentsRecord && record.has_extent_data) {
            return QStringLiteral("%1: extent attribute records %2")
                .arg(label)
                .arg(record.extent_count);
        }
        return {};
    }

    [[nodiscard]] QVector<PartitionHfsAttributeMetadata> attributeMetadataRecords(
        const HfsAttributeScanSummary& summary) const {
        QVector<PartitionHfsAttributeMetadata> records;
        records.reserve(summary.parsed_records.size());
        for (const auto& record : summary.parsed_records) {
            PartitionHfsAttributeMetadata metadata;
            metadata.file_id = record.file_id;
            metadata.start_block = record.start_block;
            metadata.name = record.name;
            if (record.record_type == kHfsAttributeInlineDataRecord) {
                metadata.storage = QStringLiteral("inline");
                metadata.size_bytes = record.inline_size;
                metadata.readable = record.payload_complete && record.has_inline_data;
            } else if (record.record_type == kHfsAttributeForkDataRecord) {
                metadata.storage = QStringLiteral("fork");
                metadata.size_bytes = record.fork_logical_size;
                metadata.extent_count = record.fork_extent_count;
                metadata.readable = record.payload_complete && record.has_fork_data;
            } else if (record.record_type == kHfsAttributeExtentsRecord) {
                metadata.storage = QStringLiteral("extents");
                metadata.extent_count = record.extent_count;
            } else {
                metadata.storage = QStringLiteral("other");
            }
            records.append(metadata);
        }
        return records;
    }

    void appendAttributeDetails(const HfsAttributeScanSummary& summary,
                                QStringList* details) const {
        details->append(
            QStringLiteral("Attributes file: %1")
                .arg(summary.present ? QStringLiteral("present") : QStringLiteral("not present")));
        if (!summary.present) {
            return;
        }
        details->append(QStringLiteral("Attribute records scanned: %1").arg(summary.records));
        details->append(QStringLiteral("Attribute leaf nodes scanned: %1").arg(summary.leaf_nodes));
        details->append(QStringLiteral("Inline attribute records: %1").arg(summary.inline_records));
        details->append(QStringLiteral("Fork attribute records: %1").arg(summary.fork_records));
        details->append(QStringLiteral("Extent attribute records: %1").arg(summary.extent_records));
        details->append(QStringLiteral("Other attribute records: %1").arg(summary.other_records));
        if (!summary.names.isEmpty()) {
            details->append(QStringLiteral("Attribute names: %1")
                                .arg(summary.names.join(QStringLiteral(", "))));
        }
        if (!summary.metadata.isEmpty()) {
            details->append(QStringLiteral("Attribute metadata: %1")
                                .arg(summary.metadata.join(QStringLiteral("; "))));
        }
    }

    [[nodiscard]] std::optional<HfsAttributeRecord> findReadableAttribute(
        const HfsAttributeScanSummary& summary, uint32_t fileId, const QString& name) const {
        const QString trimmed = name.trimmed();
        for (const auto& record : summary.parsed_records) {
            if (record.file_id == fileId && record.name == trimmed &&
                (record.record_type == kHfsAttributeInlineDataRecord ||
                 record.record_type == kHfsAttributeForkDataRecord)) {
                return record;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<HfsAttributeRecord> findAttributeByIdAndName(
        const HfsAttributeScanSummary& summary, uint32_t fileId, const QString& name) const {
        for (const auto& record : summary.parsed_records) {
            if (record.file_id == fileId && record.name == name) {
                return record;
            }
        }
        return std::nullopt;
    }

    void appendInlineAttributeRecordBlockers(const HfsAttributeRecord& record,
                                             const QByteArray& data,
                                             const PartitionHfsFileWriteOptions& options,
                                             QStringList* blockers) const {
        if (record.record_type != kHfsAttributeInlineDataRecord || !record.payload_complete ||
            !record.has_inline_data) {
            blockers->append(
                QStringLiteral("Only complete inline HFS+ attribute records are writable"));
            return;
        }
        if (!options.allow_compressed_file_mutation &&
            record.name == QStringLiteral("com.apple.decmpfs")) {
            blockers->append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
        if (static_cast<uint64_t>(data.size()) >
            static_cast<uint64_t>(record.inline_available_bytes)) {
            blockers->append(QStringLiteral(
                "HFS+ inline attribute replacement exceeds existing record capacity"));
        }
        if (record.inline_size_offset == 0 || record.inline_data_offset == 0) {
            blockers->append(
                QStringLiteral("HFS+ inline attribute record location is unavailable"));
        }
    }

    [[nodiscard]] std::optional<int> zeroInlineAttributeTail(const HfsAttributeRecord& record,
                                                             uint64_t newSize) {
        if (newSize >= record.inline_size) {
            return 0;
        }
        const uint64_t staleBytes = record.inline_size - newSize;
        if (staleBytes > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
            m_blockers.append(
                QStringLiteral("HFS+ inline attribute stale-tail zeroing is too large"));
            return std::nullopt;
        }
        const QByteArray zeros(static_cast<qsizetype>(staleBytes), '\0');
        return writeForkBytes(m_volume.attributes_fork,
                              kHfsAttributesFileId,
                              kHfsDataForkType,
                              record.inline_data_offset + newSize,
                              zeros);
    }

    void appendForkAttributeRecordBlockers(const HfsAttributeRecord& record,
                                           const QVector<HfsAttributeRecord>& allRecords,
                                           const QByteArray& data,
                                           const PartitionHfsFileWriteOptions& options,
                                           QStringList* blockers) {
        appendForkAttributeTypeBlockers(record, options, blockers);
        if (!blockers->isEmpty()) {
            return;
        }
        appendForkAttributeSizeBlockers(record, data, options, blockers);
        const uint64_t bytesToCover = std::max(static_cast<uint64_t>(data.size()),
                                               record.fork_logical_size);
        appendAttributeForkPhysicalMapBlockers(record, allRecords, bytesToCover, blockers);
    }

    void appendForkAttributeTypeBlockers(const HfsAttributeRecord& record,
                                         const PartitionHfsFileWriteOptions& options,
                                         QStringList* blockers) const {
        if (record.record_type != kHfsAttributeForkDataRecord || !record.payload_complete ||
            !record.has_fork_data) {
            blockers->append(
                QStringLiteral("Only complete fork-backed HFS+ attribute records are writable"));
            return;
        }
        if (!options.allow_compressed_file_mutation &&
            record.name == QStringLiteral("com.apple.decmpfs")) {
            blockers->append(QStringLiteral("HFS+ compressed-file mutation is blocked"));
        }
    }

    void appendForkAttributeSizeBlockers(const HfsAttributeRecord& record,
                                         const QByteArray& data,
                                         const PartitionHfsFileWriteOptions& options,
                                         QStringList* blockers) const {
        const uint64_t payloadBytes = static_cast<uint64_t>(data.size());
        const auto allocatedBytes = forkAllocatedBytes(record.fork_data);
        if (!allocatedBytes.has_value() || *allocatedBytes == 0) {
            blockers->append(QStringLiteral("HFS+ fork attribute allocation is not writable"));
            return;
        }
        if (payloadBytes > *allocatedBytes) {
            blockers->append(QStringLiteral(
                "HFS+ fork attribute replacement cannot grow beyond existing allocation"));
        }
        if (record.attribute_data_offset == 0) {
            blockers->append(QStringLiteral("HFS+ fork attribute record location is unavailable"));
        }
        if (record.fork_logical_size > options.max_write_bytes) {
            blockers->append(
                QStringLiteral("HFS+ fork attribute existing value exceeds configured write cap"));
        }
        if (payloadBytes < record.fork_logical_size &&
            record.fork_logical_size - payloadBytes > options.max_write_bytes) {
            blockers->append(QStringLiteral(
                "HFS+ fork attribute stale-tail zeroing exceeds configured write cap"));
        }
    }

    void appendAttributeForkPhysicalMapBlockers(const HfsAttributeRecord& record,
                                                const QVector<HfsAttributeRecord>& allRecords,
                                                uint64_t bytesToCover,
                                                QStringList* blockers) {
        if (bytesToCover == 0) {
            return;
        }
        uint64_t logicalBlocks = bytesToCover / m_volume.block_size;
        if ((bytesToCover % m_volume.block_size) != 0) {
            ++logicalBlocks;
        }
        QSet<uint64_t> seenBlocks;
        for (uint64_t logicalBlock = 0; logicalBlock < logicalBlocks; ++logicalBlock) {
            const auto physicalBlock =
                physicalBlockForAttributeLogicalBlock(record, allRecords, logicalBlock);
            if (!physicalBlock.has_value()) {
                blockers->append(QStringLiteral(
                    "HFS+ fork attribute extent map does not cover existing allocation"));
                return;
            }
            if (!attributePhysicalBlockIsWritable(*physicalBlock, &seenBlocks, blockers)) {
                return;
            }
        }
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockForAttributeLogicalBlock(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t logicalBlock) const {
        const auto initialBlock = physicalBlockInExtents(record.fork_data.extents, 0, logicalBlock);
        if (initialBlock.has_value()) {
            return initialBlock;
        }
        return physicalBlockFromAttributeExtents(record, allRecords, logicalBlock);
    }

    [[nodiscard]] bool attributePhysicalBlockIsWritable(uint64_t physicalBlock,
                                                        QSet<uint64_t>* seenBlocks,
                                                        QStringList* blockers) {
        if (physicalBlock == 0 || physicalBlock >= m_volume.total_blocks ||
            physicalBlock > std::numeric_limits<uint32_t>::max()) {
            blockers->append(QStringLiteral("HFS+ fork attribute extent is out of range"));
            return false;
        }
        const HfsExtent oneBlock{.start_block = static_cast<uint32_t>(physicalBlock),
                                 .block_count = 1};
        if (extentOverlapsMetadata(oneBlock)) {
            blockers->append(QStringLiteral(
                "HFS+ fork attribute write refuses extents that overlap metadata forks"));
            return false;
        }
        if (seenBlocks->contains(physicalBlock)) {
            blockers->append(
                QStringLiteral("HFS+ fork attribute write refuses overlapping attribute extents"));
            return false;
        }
        seenBlocks->insert(physicalBlock);
        return allocationBlockIsMarked(physicalBlock, blockers);
    }

    [[nodiscard]] bool allocationBlockIsMarked(uint64_t physicalBlock, QStringList* blockers) {
        const uint64_t byteOffset = physicalBlock / 8U;
        if (byteOffset >= m_volume.allocation_fork.logical_size) {
            blockers->append(
                QStringLiteral("HFS+ allocation bitmap does not cover attribute extent"));
            return false;
        }
        const auto byte = readForkBytes(
            m_volume.allocation_fork, kHfsAllocationFileId, kHfsDataForkType, byteOffset, 1);
        if (!byte.has_value() || byte->size() != 1) {
            blockers->append(QStringLiteral("Unable to read HFS+ allocation bitmap byte"));
            blockers->append(m_blockers);
            return false;
        }
        const quint8 mask = static_cast<quint8>(0x80U >> (physicalBlock % 8U));
        if ((static_cast<quint8>(byte->at(0)) & mask) == 0) {
            blockers->append(
                QStringLiteral("HFS+ allocation bitmap did not mark attribute block allocated"));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<int> writeAttributeForkBytesWithinAllocated(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t offset,
        const QByteArray& data) {
        if (!forkWriteRequestFitsAllocation(
                record.fork_data, offset, static_cast<uint64_t>(data.size()))) {
            m_blockers.append(QStringLiteral("Requested HFS+ attribute write exceeds allocation"));
            return std::nullopt;
        }

        int chunks = 0;
        uint64_t remaining = static_cast<uint64_t>(data.size());
        uint64_t cursor = offset;
        qsizetype dataOffset = 0;
        while (remaining > 0) {
            const uint64_t withinBlock = cursor % m_volume.block_size;
            const uint64_t chunkSize = std::min<uint64_t>(remaining,
                                                          m_volume.block_size - withinBlock);
            const auto deviceOffset =
                deviceOffsetForAttributeForkCursor(record, allRecords, cursor, withinBlock);
            if (!deviceOffset.has_value() ||
                !writeAt(*deviceOffset, data.constData() + dataOffset, chunkSize)) {
                return std::nullopt;
            }
            cursor += chunkSize;
            remaining -= chunkSize;
            dataOffset += static_cast<qsizetype>(chunkSize);
            ++chunks;
        }
        return chunks;
    }

    [[nodiscard]] std::optional<int> zeroForkAttributeTail(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t newSize) {
        if (newSize >= record.fork_logical_size) {
            return 0;
        }
        const uint64_t staleBytes = record.fork_logical_size - newSize;
        if (staleBytes > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
            m_blockers.append(
                QStringLiteral("HFS+ fork attribute stale-tail zeroing is too large"));
            return std::nullopt;
        }
        const QByteArray zeros(static_cast<qsizetype>(staleBytes), '\0');
        return writeAttributeForkBytesWithinAllocated(record, allRecords, newSize, zeros);
    }

    [[nodiscard]] std::optional<int> updateForkAttributeLogicalSize(
        const HfsAttributeRecord& record, uint64_t newSize) {
        uint64_t sizeFieldOffset = 0;
        if (!checkedAdd(record.attribute_data_offset,
                        kHfsAttributeForkDataOffset + kHfsForkLogicalSizeOffset,
                        &sizeFieldOffset)) {
            m_blockers.append(QStringLiteral("HFS+ fork attribute size-field offset overflow"));
            return std::nullopt;
        }
        return writeForkBytes(m_volume.attributes_fork,
                              kHfsAttributesFileId,
                              kHfsDataForkType,
                              sizeFieldOffset,
                              be64Bytes(newSize));
    }

    [[nodiscard]] PartitionHfsAttributeReadResult readAttributeRecordValue(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t maxBytes,
        PartitionHfsAttributeReadResult result) {
        result.warnings = m_warnings;
        if (record.record_type == kHfsAttributeInlineDataRecord) {
            return readInlineAttributeValue(record, maxBytes, std::move(result));
        }
        if (record.record_type == kHfsAttributeForkDataRecord) {
            return readForkAttributeValue(record, allRecords, maxBytes, std::move(result));
        }
        result.blockers.append(QStringLiteral("Unsupported HFS+ attribute record type"));
        result.blockers.append(m_blockers);
        return result;
    }

    [[nodiscard]] PartitionHfsAttributeReadResult readInlineAttributeValue(
        const HfsAttributeRecord& record,
        uint64_t maxBytes,
        PartitionHfsAttributeReadResult result) const {
        if (!record.payload_complete || !record.has_inline_data) {
            result.blockers.append(QStringLiteral("HFS+ inline attribute payload is incomplete"));
            return result;
        }
        if (record.inline_size > maxBytes) {
            result.blockers.append(
                QStringLiteral(
                    "Selected HFS+ attribute is larger than the read cap (%1 > %2 bytes)")
                    .arg(record.inline_size)
                    .arg(maxBytes));
            return result;
        }
        result.storage = QStringLiteral("inline");
        result.data = record.inline_data;
        result.blockers = m_blockers;
        result.warnings = m_warnings;
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionHfsAttributeReadResult readForkAttributeValue(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t maxBytes,
        PartitionHfsAttributeReadResult result) {
        if (!record.payload_complete || !record.has_fork_data) {
            result.blockers.append(QStringLiteral("HFS+ fork attribute payload is incomplete"));
            return result;
        }
        if (record.fork_logical_size > maxBytes) {
            result.blockers.append(
                QStringLiteral(
                    "Selected HFS+ attribute is larger than the read cap (%1 > %2 bytes)")
                    .arg(record.fork_logical_size)
                    .arg(maxBytes));
            return result;
        }
        const auto bytes = readAttributeForkBytes(record, allRecords, 0, record.fork_logical_size);
        if (!bytes.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        result.storage = QStringLiteral("fork");
        result.data = *bytes;
        result.blockers = m_blockers;
        result.warnings = m_warnings;
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] std::optional<QByteArray> readAttributeForkBytes(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t offset,
        uint64_t length) {
        if (!forkReadRequestIsValid(record.fork_data, offset, length)) {
            m_blockers.append(QStringLiteral("Requested HFS+ attribute read is out of range"));
            return std::nullopt;
        }

        QByteArray output;
        output.reserve(static_cast<qsizetype>(length));
        uint64_t remaining = length;
        uint64_t cursor = offset;
        while (remaining > 0) {
            const auto chunk = readAttributeForkChunk(record, allRecords, cursor, remaining);
            if (!chunk.has_value()) {
                return std::nullopt;
            }
            output.append(chunk->bytes);
            cursor += chunk->size;
            remaining -= chunk->size;
        }
        return output;
    }

    [[nodiscard]] std::optional<HfsForkReadChunk> readAttributeForkChunk(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t cursor,
        uint64_t remaining) {
        const uint64_t withinBlock = cursor % m_volume.block_size;
        const uint64_t chunkSize = std::min<uint64_t>(remaining, m_volume.block_size - withinBlock);
        const auto deviceOffset =
            deviceOffsetForAttributeForkCursor(record, allRecords, cursor, withinBlock);
        if (!deviceOffset.has_value()) {
            return std::nullopt;
        }
        const auto bytes = readAt(*deviceOffset, static_cast<qsizetype>(chunkSize));
        if (!bytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read HFS+ attribute data"));
            return std::nullopt;
        }
        return HfsForkReadChunk{.bytes = *bytes, .size = chunkSize};
    }

    [[nodiscard]] std::optional<uint64_t> deviceOffsetForAttributeForkCursor(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t cursor,
        uint64_t withinBlock) {
        const auto block = physicalBlockForAttributeForkOffset(record, allRecords, cursor);
        if (!block.has_value()) {
            return std::nullopt;
        }
        return deviceOffsetForPhysicalBlock(*block, withinBlock);
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockForAttributeForkOffset(
        const HfsAttributeRecord& record,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t offset) {
        const uint64_t logicalBlock = offset / m_volume.block_size;
        const auto initialBlock = physicalBlockInExtents(record.fork_data.extents, 0, logicalBlock);
        if (initialBlock.has_value()) {
            return initialBlock;
        }
        const auto overflowBlock =
            physicalBlockFromAttributeExtents(record, allRecords, logicalBlock);
        if (overflowBlock.has_value()) {
            return overflowBlock;
        }
        m_blockers.append(QStringLiteral("HFS+ attribute overflow extent record was not found"));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockFromAttributeExtents(
        const HfsAttributeRecord& owner,
        const QVector<HfsAttributeRecord>& allRecords,
        uint64_t logicalBlock) const {
        const HfsAttributeRecord* best = nullptr;
        for (const auto& record : allRecords) {
            if (record.record_type != kHfsAttributeExtentsRecord || !record.has_extent_data ||
                record.file_id != owner.file_id || record.name != owner.name ||
                record.start_block > logicalBlock) {
                continue;
            }
            if (!best || record.start_block > best->start_block) {
                best = &record;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        return physicalBlockInExtents(best->extents, best->start_block, logicalBlock);
    }

    [[nodiscard]] std::optional<QByteArray> readExtentsNode(uint32_t nodeNumber) {
        if (nodeNumber >= m_extents.total_nodes) {
            m_blockers.append(QStringLiteral("HFS+ extents overflow node is out of range"));
            return std::nullopt;
        }
        uint64_t offset = 0;
        if (!checkedMul(nodeNumber, m_extents.node_size, &offset)) {
            m_blockers.append(QStringLiteral("HFS+ extents overflow node offset overflow"));
            return std::nullopt;
        }
        return readForkBytesInitialExtents(m_volume.extents_fork, offset, m_extents.node_size);
    }

    [[nodiscard]] std::optional<QVector<qsizetype>> recordOffsets(const QByteArray& node,
                                                                  uint16_t numRecords) {
        const qsizetype nodeSize = node.size();
        if (numRecords == 0 || nodeSize < kBTreeNodeDescriptorBytes + (numRecords * kUint16Size)) {
            return QVector<qsizetype>();
        }

        QVector<qsizetype> offsets;
        offsets.reserve(numRecords);
        for (uint16_t index = 0; index < numRecords; ++index) {
            const qsizetype tableOffset = nodeSize - ((index + 1) * kUint16Size);
            const uint16_t offset = be16(node, tableOffset);
            if (offset < kBTreeNodeDescriptorBytes || offset >= nodeSize) {
                m_blockers.append(QStringLiteral("Invalid HFS+ B-tree record offset"));
                return std::nullopt;
            }
            offsets.append(offset);
        }
        if (!std::is_sorted(offsets.cbegin(), offsets.cend())) {
            m_blockers.append(QStringLiteral("Unsorted HFS+ B-tree record offsets"));
            return std::nullopt;
        }
        return offsets;
    }

    [[nodiscard]] qsizetype recordEnd(const QByteArray& node,
                                      const QVector<qsizetype>& offsets,
                                      int index) const {
        if (index + 1 < offsets.size()) {
            return offsets.at(index + 1);
        }
        const qsizetype offsetTableStart = node.size() - ((offsets.size() + 1) * kUint16Size);
        const uint16_t freeOffset = be16(node, offsetTableStart);
        if (freeOffset > offsets.at(index) && freeOffset <= offsetTableStart) {
            return freeOffset;
        }
        return node.size() - (offsets.size() * kUint16Size);
    }

    void appendExtentsOverflowRecords(const QByteArray& node) {
        const uint16_t numRecords = be16(node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(node, numRecords);
        if (!offsets.has_value()) {
            return;
        }
        for (int index = 0; index < offsets->size(); ++index) {
            const qsizetype start = offsets->at(index);
            const qsizetype end = recordEnd(node, *offsets, index);
            if (end <= start || end > node.size()) {
                m_blockers.append(QStringLiteral("Invalid HFS+ extents overflow record length"));
                return;
            }
            const auto record = parseOverflowExtentRecord(node, start, end);
            if (record.has_value() && !record->extents.isEmpty()) {
                m_overflow_extents.append(*record);
            }
        }
    }

    [[nodiscard]] std::optional<HfsOverflowExtentRecord> parseOverflowExtentRecord(
        const QByteArray& node, qsizetype offset, qsizetype end) const {
        const uint16_t keyLength = be16(node, offset);
        if (keyLength != kHfsExtentsKeyLength || !hasBytes(node, offset, kUint16Size + keyLength)) {
            return std::nullopt;
        }

        const qsizetype dataOffset = offset + kUint16Size + keyLength;
        if (dataOffset > end || end - dataOffset < kHfsExtentsRecordBytes) {
            return std::nullopt;
        }
        const uint8_t forkType =
            static_cast<uint8_t>(node.at(offset + kHfsExtentsKeyForkTypeOffset));
        if (forkType != kHfsDataForkType && forkType != kHfsResourceForkType) {
            return std::nullopt;
        }

        HfsOverflowExtentRecord record;
        record.fork_type = forkType;
        record.file_id = be32(node, offset + kHfsExtentsKeyFileIdOffset);
        record.start_block = be32(node, offset + kHfsExtentsKeyStartBlockOffset);
        for (int index = 0; index < kHfsExtentCount; ++index) {
            const qsizetype extentOffset = dataOffset + index * kHfsExtentBytes;
            const uint32_t start = be32(node, extentOffset + kHfsExtentStartBlockOffset);
            const uint32_t count = be32(node, extentOffset + kHfsExtentBlockCountOffset);
            if (count == 0) {
                continue;
            }
            record.extents.append(HfsExtent{.start_block = start, .block_count = count});
        }
        return record;
    }

    [[nodiscard]] std::optional<QString> catalogName(const QByteArray& node,
                                                     qsizetype offset,
                                                     qsizetype end) const {
        if (!hasBytes(node, offset + kHfsCatalogKeyNameLengthOffset, kUint16Size)) {
            return std::nullopt;
        }
        const uint16_t length = be16(node, offset + kHfsCatalogKeyNameLengthOffset);
        if (length > kHfsMaximumNameChars) {
            return std::nullopt;
        }
        const qsizetype nameBytes = static_cast<qsizetype>(length) * kUint16Size;
        const qsizetype nameOffset = offset + kHfsCatalogKeyNameOffset;
        if (nameOffset > end || end - nameOffset < nameBytes) {
            return std::nullopt;
        }

        QString name;
        name.reserve(length);
        for (uint16_t index = 0; index < length; ++index) {
            name.append(QChar(be16(node, nameOffset + index * kUint16Size)));
        }
        if (name.contains(QChar::Null) || name.contains(QLatin1Char('/'))) {
            return std::nullopt;
        }
        return name;
    }

    [[nodiscard]] std::optional<HfsCatalogRecord> parseCatalogRecord(const QByteArray& node,
                                                                     qsizetype offset,
                                                                     qsizetype end,
                                                                     uint32_t nodeNumber) const {
        const auto key = parseCatalogKey(node, offset, end, nodeNumber);
        if (!key.has_value()) {
            return std::nullopt;
        }
        return parseCatalogDataRecord(node, *key);
    }

    [[nodiscard]] std::optional<HfsCatalogKeyInfo> parseCatalogKey(const QByteArray& node,
                                                                   qsizetype offset,
                                                                   qsizetype end,
                                                                   uint32_t nodeNumber) const {
        const uint16_t keyLength = be16(node, offset);
        if (keyLength < kHfsCatalogMinimumKeyBytes || keyLength > kHfsCatalogMaximumKeyBytes ||
            !hasBytes(node, offset, kUint16Size + keyLength)) {
            return std::nullopt;
        }

        const qsizetype dataOffset = offset + kUint16Size + keyLength;
        if (dataOffset + kUint16Size > end) {
            return std::nullopt;
        }
        const auto name = catalogName(node, offset, dataOffset);
        if (!name.has_value()) {
            return std::nullopt;
        }
        uint64_t nodeBase = 0;
        uint64_t catalogDataOffset = 0;
        if (!checkedMul(nodeNumber, m_catalog.node_size, &nodeBase) ||
            !checkedAdd(nodeBase, static_cast<uint64_t>(dataOffset), &catalogDataOffset)) {
            return std::nullopt;
        }
        return HfsCatalogKeyInfo{.parent_id = be32(node, offset + kHfsCatalogKeyParentIdOffset),
                                 .name = *name,
                                 .data_offset = dataOffset,
                                 .catalog_data_offset = catalogDataOffset};
    }

    [[nodiscard]] std::optional<HfsCatalogRecord> parseCatalogDataRecord(
        const QByteArray& node, const HfsCatalogKeyInfo& key) const {
        HfsCatalogRecord record;
        record.parent_id = key.parent_id;
        record.name = key.name;
        record.catalog_data_offset = key.catalog_data_offset;
        record.record_type = be16(node, key.data_offset + kHfsCatalogRecordTypeOffset);
        if (record.record_type == kHfsCatalogFolderThreadRecord ||
            record.record_type == kHfsCatalogFileThreadRecord) {
            return record;
        }
        if (!catalogRecordTypeIsPayload(record.record_type)) {
            return std::nullopt;
        }
        if (!hasBytes(node, key.data_offset + kHfsCatalogRecordIdOffset, kUint32Size)) {
            return std::nullopt;
        }
        record.catalog_id = be32(node, key.data_offset + kHfsCatalogRecordIdOffset);
        if (record.regularFile()) {
            const qsizetype dataForkOffset = key.data_offset + kHfsCatalogFileDataForkOffset;
            const qsizetype resourceForkOffset = key.data_offset +
                                                 kHfsCatalogFileResourceForkOffset;
            if (!hasBytes(node, dataForkOffset, kHfsForkDataBytes) ||
                !hasBytes(node, resourceForkOffset, kHfsForkDataBytes)) {
                return std::nullopt;
            }
            record.data_fork = parseFork(node, dataForkOffset);
            record.data_size = record.data_fork.logical_size;
            record.resource_fork = parseFork(node, resourceForkOffset);
            record.resource_size = record.resource_fork.logical_size;
        }
        return record;
    }

    [[nodiscard]] bool catalogRecordTypeIsPayload(uint16_t recordType) const {
        return recordType == kHfsCatalogFolderRecord || recordType == kHfsCatalogFileRecord;
    }

    [[nodiscard]] std::optional<QVector<HfsCatalogRecord>> childRecords(uint32_t folderId,
                                                                        int maxEntries) {
        QVector<HfsCatalogRecord> records;
        uint32_t nodeNumber = m_catalog.first_leaf_node;
        int visited = 0;
        while (nodeNumber != 0 && visited < kMaxCatalogLeafNodesToScan) {
            const auto node = readCatalogNode(nodeNumber);
            if (!node.has_value()) {
                return std::nullopt;
            }
            if (static_cast<int8_t>(node->at(kBTreeKindOffset)) != kBTreeLeafNode ||
                static_cast<uint8_t>(node->at(kBTreeHeightOffset)) != 1) {
                m_blockers.append(QStringLiteral("Invalid HFS+ catalog leaf node"));
                return std::nullopt;
            }
            appendChildRecords(*node, nodeNumber, folderId, maxEntries, &records);
            if (!m_blockers.isEmpty() || records.size() >= maxEntries) {
                break;
            }
            nodeNumber = be32(*node, 0);
            ++visited;
        }
        if (visited >= kMaxCatalogLeafNodesToScan) {
            m_blockers.append(QStringLiteral("HFS+ catalog scan limit reached"));
            return std::nullopt;
        }
        return m_blockers.isEmpty() ? std::optional<QVector<HfsCatalogRecord>>(records)
                                    : std::nullopt;
    }

    void appendChildRecords(const QByteArray& node,
                            uint32_t nodeNumber,
                            uint32_t folderId,
                            int maxEntries,
                            QVector<HfsCatalogRecord>* records) {
        const uint16_t numRecords = be16(node, kBTreeNumRecordsOffset);
        const auto offsets = recordOffsets(node, numRecords);
        if (!offsets.has_value()) {
            return;
        }
        for (int index = 0; index < offsets->size() && records->size() < maxEntries; ++index) {
            const qsizetype start = offsets->at(index);
            const qsizetype end = recordEnd(node, *offsets, index);
            if (end <= start || end > node.size()) {
                m_blockers.append(QStringLiteral("Invalid HFS+ catalog record length"));
                return;
            }
            const auto record = parseCatalogRecord(node, start, end, nodeNumber);
            if (!record.has_value() || record->record_type == kHfsCatalogFolderThreadRecord ||
                record->record_type == kHfsCatalogFileThreadRecord) {
                continue;
            }
            if (record->parent_id == folderId) {
                records->append(*record);
            }
        }
    }

    [[nodiscard]] std::optional<uint32_t> resolveFolderPath(const QString& path) {
        const auto record = resolveCatalogPath(path);
        if (!record.has_value()) {
            if (normalizedDisplayPath(path) == QStringLiteral("/")) {
                return kHfsRootFolderId;
            }
            return std::nullopt;
        }
        if (!record->directory()) {
            m_blockers.append(QStringLiteral("Selected HFS+ path is not a directory"));
            return std::nullopt;
        }
        return record->catalog_id;
    }

    [[nodiscard]] std::optional<HfsCatalogRecord> resolveCatalogPath(const QString& path) {
        QStringList blockers;
        const QStringList parts = hfsPathParts(path, &blockers);
        if (!blockers.isEmpty()) {
            m_blockers.append(blockers);
            return std::nullopt;
        }
        if (parts.isEmpty()) {
            return HfsCatalogRecord{.parent_id = kHfsRootParentId,
                                    .name = QStringLiteral("/"),
                                    .record_type = kHfsCatalogFolderRecord,
                                    .catalog_id = kHfsRootFolderId};
        }

        uint32_t folderId = kHfsRootFolderId;
        HfsCatalogRecord current;
        for (int index = 0; index < parts.size(); ++index) {
            const auto match = findChild(folderId, parts.at(index));
            if (!match.has_value()) {
                m_blockers.append(QStringLiteral("HFS+ path not found: %1").arg(parts.at(index)));
                return std::nullopt;
            }
            current = *match;
            if (index + 1 < parts.size()) {
                if (!current.directory()) {
                    m_blockers.append(QStringLiteral("HFS+ path component is not a directory: %1")
                                          .arg(parts.at(index)));
                    return std::nullopt;
                }
                folderId = current.catalog_id;
            }
        }
        return current;
    }

    [[nodiscard]] std::optional<HfsCatalogRecord> findChild(uint32_t folderId,
                                                            const QString& name) {
        const auto records = childRecords(folderId, kDefaultPathResolveEntryLimit);
        if (!records.has_value()) {
            return std::nullopt;
        }
        for (const auto& record : *records) {
            if (record.name == name) {
                return record;
            }
        }
        if (m_catalog.key_compare_type == kHfsBinaryCompare) {
            return std::nullopt;
        }
        for (const auto& record : *records) {
            if (QString::compare(record.name, name, Qt::CaseInsensitive) == 0) {
                return record;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] QString normalizedDisplayPath(const QString& path) const {
        QStringList blockers;
        const QStringList parts = hfsPathParts(path, &blockers);
        if (parts.isEmpty()) {
            return QStringLiteral("/");
        }
        return QStringLiteral("/%1").arg(parts.join(QLatin1Char('/')));
    }

    [[nodiscard]] PartitionHfsFileEntry entryFor(const HfsCatalogRecord& record,
                                                 const QString& parentPath) const {
        return {.path = makeEntryPath(parentPath, record.name),
                .name = record.name,
                .type = typeNameForRecord(record.record_type),
                .catalog_id = record.catalog_id,
                .size_bytes = record.data_size,
                .resource_fork_size_bytes = record.resource_size,
                .directory = record.directory(),
                .regular_file = record.regularFile()};
    }

    QIODevice* m_device{nullptr};
    HfsVolume m_volume;
    HfsBTreeHeader m_catalog;
    HfsBTreeHeader m_extents;
    QVector<HfsOverflowExtentRecord> m_overflow_extents;
    QStringList m_blockers;
    QStringList m_warnings;
};

}  // namespace


}  // namespace sak
