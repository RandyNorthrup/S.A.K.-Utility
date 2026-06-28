// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_apfs_file_system_reader.h
/// @brief Read-only APFS file browser for Partition Manager.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

class QIODevice;

namespace sak {

inline constexpr int kPartitionApfsDefaultBrowseEntryLimit = 1000;

struct PartitionApfsFileEntry {
    QString path;
    QString name;
    QString type;
    uint64_t object_id{0};
    uint64_t size_bytes{0};
    bool directory{false};
    bool regular_file{false};
    bool symlink{false};
};

struct PartitionApfsFileReadResult {
    bool ok{false};
    QString file_system;
    QString volume_name;
    QStringList blockers;
    QStringList warnings;
    QVector<PartitionApfsFileEntry> entries;
    QByteArray data;
};

struct PartitionApfsDirectoryExportResult {
    bool ok{false};
    QStringList blockers;
    QStringList warnings;
    int files_exported{0};
    int directories_exported{0};
    int symlinks_skipped{0};
    int entries_scanned{0};
    uint64_t bytes_exported{0};
};

struct PartitionApfsDirectoryExportOptions {
    int max_entries{kPartitionApfsDefaultBrowseEntryLimit};
    uint64_t max_file_bytes{0};
    uint64_t max_total_bytes{0};
};

class PartitionApfsFileSystemReader {
public:
    /// @p credential (optional) is the FileVault volume password or personal
    /// recovery key used to unlock a software-encrypted volume; held in memory only.
    [[nodiscard]] static PartitionApfsFileReadResult listDirectory(
        QIODevice* device,
        const QString& path = {},
        int max_entries = kPartitionApfsDefaultBrowseEntryLimit,
        const QString& credential = {});
    [[nodiscard]] static PartitionApfsFileReadResult listDirectoryFromImage(
        const QString& image_path,
        const QString& path = {},
        int max_entries = kPartitionApfsDefaultBrowseEntryLimit,
        const QString& credential = {});
    [[nodiscard]] static PartitionApfsFileReadResult readFile(QIODevice* device,
                                                              const QString& path,
                                                              uint64_t max_bytes,
                                                              const QString& credential = {});
    [[nodiscard]] static PartitionApfsFileReadResult readFileFromImage(
        const QString& image_path,
        const QString& path,
        uint64_t max_bytes,
        const QString& credential = {});
    [[nodiscard]] static PartitionApfsDirectoryExportResult exportDirectoryFromImage(
        const QString& image_path,
        const QString& source_path,
        const QString& output_directory,
        const PartitionApfsDirectoryExportOptions& options);
};

}  // namespace sak
