// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_ext_file_system_reader.h
/// @brief Read-only ext2/ext3/ext4 file browser for Partition Manager.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

class QIODevice;

namespace sak {

inline constexpr int kPartitionExtDefaultBrowseEntryLimit = 1000;

struct PartitionExtFileEntry {
    QString path;
    QString name;
    QString type;
    uint32_t inode{0};
    uint64_t size_bytes{0};
    QString symlink_target;
    bool directory{false};
    bool regular_file{false};
    bool symlink{false};
};

struct PartitionExtFileReadResult {
    bool ok{false};
    QString file_system;
    QStringList blockers;
    QStringList warnings;
    QVector<PartitionExtFileEntry> entries;
    QByteArray data;
};

struct PartitionExtDirectoryExportResult {
    bool ok{false};
    QStringList blockers;
    QStringList warnings;
    int files_exported{0};
    int directories_exported{0};
    int symlinks_exported{0};
    int entries_scanned{0};
    uint64_t bytes_exported{0};
};

struct PartitionExtDirectoryExportOptions {
    int max_entries{kPartitionExtDefaultBrowseEntryLimit};
    uint64_t max_file_bytes{0};
    uint64_t max_total_bytes{0};
};

class PartitionExtFileSystemReader {
public:
    [[nodiscard]] static PartitionExtFileReadResult listDirectory(
        QIODevice* device,
        const QString& path = {},
        int max_entries = kPartitionExtDefaultBrowseEntryLimit);
    [[nodiscard]] static PartitionExtFileReadResult listDirectoryFromImage(
        const QString& image_path,
        const QString& path = {},
        int max_entries = kPartitionExtDefaultBrowseEntryLimit);
    [[nodiscard]] static PartitionExtFileReadResult readFile(QIODevice* device,
                                                             const QString& path,
                                                             uint64_t max_bytes);
    [[nodiscard]] static PartitionExtFileReadResult readFileFromImage(const QString& image_path,
                                                                      const QString& path,
                                                                      uint64_t max_bytes);
    [[nodiscard]] static PartitionExtDirectoryExportResult exportDirectoryFromImage(
        const QString& image_path,
        const QString& source_path,
        const QString& output_directory,
        const PartitionExtDirectoryExportOptions& options);

    /// @brief True when @p child resolves (junctions/symlinks followed) to a real path at or
    ///        under the canonical @p canonical_root. Pure seam for the export junction/symlink
    ///        TOCTOU guard, matching the APFS and HFS+ readers.
    ///
    /// All three route to the one definition in sak/partition_export_containment.h. Each keeps
    /// its own seam so the shared test can prove that every reader actually reaches it -- a
    /// reader that quietly kept a local copy would still pass a test that only exercised the
    /// shared helper directly. ext was the copy with no seam and no coverage (R5-P4-44).
    [[nodiscard]] static bool exportPathWithinRootForTesting(const QString& canonical_root,
                                                             const QString& child);
};

}  // namespace sak
