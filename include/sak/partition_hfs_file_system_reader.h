// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_hfs_file_system_reader.h
/// @brief Read-only HFS+/HFSX file browser for Partition Manager.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

class QIODevice;

namespace sak {

inline constexpr int kPartitionHfsDefaultBrowseEntryLimit = 1000;
inline constexpr int kPartitionHfsDefaultCheckRecordLimit = 100000;
inline constexpr uint64_t kPartitionHfsBytesPerKiB = 1024ULL;
inline constexpr uint64_t kPartitionHfsBytesPerMiB = kPartitionHfsBytesPerKiB * kPartitionHfsBytesPerKiB;
inline constexpr uint64_t kPartitionHfsDefaultMaxWriteMiB = 64ULL;
inline constexpr uint64_t kPartitionHfsDefaultMaxWriteBytes =
    kPartitionHfsDefaultMaxWriteMiB * kPartitionHfsBytesPerMiB;

struct PartitionHfsFileEntry {
    QString path;
    QString name;
    QString type;
    uint32_t catalog_id{0};
    uint64_t size_bytes{0};
    uint64_t resource_fork_size_bytes{0};
    bool directory{false};
    bool regular_file{false};
};

struct PartitionHfsFileReadResult {
    bool ok{false};
    QString file_system;
    QStringList blockers;
    QStringList warnings;
    QVector<PartitionHfsFileEntry> entries;
    QByteArray data;
};

struct PartitionHfsAttributeReadResult {
    bool ok{false};
    QString file_system;
    QStringList blockers;
    QStringList warnings;
    uint32_t file_id{0};
    QString attribute_name;
    QString storage;
    QByteArray data;
};

struct PartitionHfsAttributeMetadata {
    uint32_t file_id{0};
    uint32_t start_block{0};
    QString name;
    QString storage;
    uint64_t size_bytes{0};
    int extent_count{0};
    bool readable{false};
};

struct PartitionHfsDirectoryExportResult {
    bool ok{false};
    QStringList blockers;
    QStringList warnings;
    int files_exported{0};
    int resource_forks_exported{0};
    int directories_exported{0};
    int entries_scanned{0};
    uint64_t bytes_exported{0};
};

struct PartitionHfsDirectoryExportOptions {
    int max_entries{kPartitionHfsDefaultBrowseEntryLimit};
    uint64_t max_file_bytes{0};
    uint64_t max_total_bytes{0};
};

struct PartitionHfsFileWriteOptions {
    bool enable_writer{false};
    bool target_write_confirmed{false};
    bool image_only{true};
    bool allow_journaled_volume{false};
    bool allow_wrapped_volume{false};
    bool allow_compressed_file_mutation{false};
    bool secure_wipe_deleted_blocks{false};
    QString evidence_id;
    uint64_t max_write_bytes{kPartitionHfsDefaultMaxWriteBytes};
};

struct PartitionHfsFileWriteResult {
    bool ok{false};
    QString file_system;
    QStringList blockers;
    QStringList warnings;
    QString path;
    uint32_t catalog_id{0};
    uint64_t bytes_written{0};
    int chunks_written{0};
    QString before_sha256;
    QString after_sha256;
    QString evidence_id;
};

struct PartitionHfsAttributeWriteResult {
    bool ok{false};
    QString file_system;
    QStringList blockers;
    QStringList warnings;
    uint32_t file_id{0};
    QString attribute_name;
    uint64_t bytes_written{0};
    int chunks_written{0};
    QString before_sha256;
    QString after_sha256;
    QString evidence_id;
};

struct PartitionHfsConsistencyCheckResult {
    bool ok{false};
    QString file_system;
    QStringList blockers;
    QStringList warnings;
    QStringList details;
    QStringList attribute_names;
    QStringList attribute_metadata;
    QVector<PartitionHfsAttributeMetadata> attribute_records;
    int records_scanned{0};
    int directories{0};
    int files{0};
    int threads{0};
    int other_records{0};
    int invalid_records_skipped{0};
    bool attributes_present{false};
    int attribute_records_scanned{0};
    int inline_attribute_records{0};
    int fork_attribute_records{0};
    int extent_attribute_records{0};
    int other_attribute_records{0};
};

class PartitionHfsFileSystemReader {
public:
    [[nodiscard]] static PartitionHfsConsistencyCheckResult checkConsistency(
        QIODevice* device,
        int max_records = kPartitionHfsDefaultCheckRecordLimit);
    [[nodiscard]] static PartitionHfsConsistencyCheckResult checkConsistencyFromImage(
        const QString& image_path,
        int max_records = kPartitionHfsDefaultCheckRecordLimit);
    [[nodiscard]] static PartitionHfsFileReadResult listDirectory(
        QIODevice* device,
        const QString& path = {},
        int max_entries = kPartitionHfsDefaultBrowseEntryLimit);
    [[nodiscard]] static PartitionHfsFileReadResult listDirectoryFromImage(
        const QString& image_path,
        const QString& path = {},
        int max_entries = kPartitionHfsDefaultBrowseEntryLimit);
    [[nodiscard]] static PartitionHfsFileReadResult readFile(QIODevice* device,
                                                             const QString& path,
                                                             uint64_t max_bytes);
    [[nodiscard]] static PartitionHfsFileReadResult readFileFromImage(
        const QString& image_path,
        const QString& path,
        uint64_t max_bytes);
    [[nodiscard]] static PartitionHfsFileReadResult readResourceFork(QIODevice* device,
                                                                     const QString& path,
                                                                     uint64_t max_bytes);
    [[nodiscard]] static PartitionHfsFileReadResult readResourceForkFromImage(
        const QString& image_path,
        const QString& path,
        uint64_t max_bytes);
    [[nodiscard]] static PartitionHfsAttributeReadResult readAttributeValue(
        QIODevice* device,
        uint32_t file_id,
        const QString& attribute_name,
        uint64_t max_bytes);
    [[nodiscard]] static PartitionHfsAttributeReadResult readAttributeValueFromImage(
        const QString& image_path,
        uint32_t file_id,
        const QString& attribute_name,
        uint64_t max_bytes);
    [[nodiscard]] static PartitionHfsDirectoryExportResult exportDirectoryFromImage(
        const QString& image_path,
        const QString& source_path,
        const QString& output_directory,
        const PartitionHfsDirectoryExportOptions& options);
};

class PartitionHfsFileSystemWriter {
public:
    [[nodiscard]] static PartitionHfsFileWriteResult overwriteFileSameSize(
        QIODevice* device,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult overwriteFileSameSizeFromImage(
        const QString& image_path,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceFileWithinAllocatedBlocks(
        QIODevice* device,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceFileWithinAllocatedBlocksFromImage(
        const QString& image_path,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult truncateFileWithinAllocatedBlocks(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult truncateFileWithinAllocatedBlocksFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceResourceForkWithinAllocatedBlocks(
        QIODevice* device,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceResourceForkWithinAllocatedBlocksFromImage(
        const QString& image_path,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceFileWithAllocationGrowth(
        QIODevice* device,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceFileWithAllocationGrowthFromImage(
        const QString& image_path,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceResourceForkWithAllocationGrowth(
        QIODevice* device,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceResourceForkWithAllocationGrowthFromImage(
        const QString& image_path,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult truncateResourceForkWithinAllocatedBlocks(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult truncateResourceForkWithinAllocatedBlocksFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceCompressedFileContent(
        QIODevice* device,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replaceCompressedFileContentFromImage(
        const QString& image_path,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult createEmptyFile(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult createEmptyFileFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult createFileWithData(
        QIODevice* device,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult createFileWithDataFromImage(
        const QString& image_path,
        const QString& path,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteEmptyFile(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteEmptyFileFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteFileAndReleaseAllocatedBlocks(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteFileAndReleaseAllocatedBlocksFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteFolderTreeAndReleaseAllocatedBlocks(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult renameOrMoveCatalogEntry(
        QIODevice* device,
        const QString& source_path,
        const QString& destination_path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult renameOrMoveCatalogEntryFromImage(
        const QString& image_path,
        const QString& source_path,
        const QString& destination_path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult createEmptyFolder(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult createEmptyFolderFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteEmptyFolder(
        QIODevice* device,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult deleteEmptyFolderFromImage(
        const QString& image_path,
        const QString& path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult createInlineAttributeValue(
        QIODevice* device,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult createInlineAttributeValueFromImage(
        const QString& image_path,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replayJournal(
        QIODevice* device,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsFileWriteResult replayJournalFromImage(
        const QString& image_path,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult createForkAttributeValue(
        QIODevice* device,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult createForkAttributeValueFromImage(
        const QString& image_path,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult deleteAttributeValue(
        QIODevice* device,
        uint32_t file_id,
        const QString& attribute_name,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult deleteAttributeValueFromImage(
        const QString& image_path,
        uint32_t file_id,
        const QString& attribute_name,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult replaceInlineAttributeValue(
        QIODevice* device,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult replaceInlineAttributeValueFromImage(
        const QString& image_path,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult replaceForkAttributeValueWithinAllocatedBlocks(
        QIODevice* device,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult
    replaceForkAttributeValueWithinAllocatedBlocksFromImage(
        const QString& image_path,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult
    replaceForkAttributeValueWithAllocationGrowth(
        QIODevice* device,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
    [[nodiscard]] static PartitionHfsAttributeWriteResult
    replaceForkAttributeValueWithAllocationGrowthFromImage(
        const QString& image_path,
        uint32_t file_id,
        const QString& attribute_name,
        const QByteArray& data,
        const PartitionHfsFileWriteOptions& options);
};

}  // namespace sak
