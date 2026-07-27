// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_archive_service.h
/// @brief Zip compress/extract engine for the File Management Explorer.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace sak {

/// Result of one archive operation (compress or extract).
struct FileExplorerArchiveResult {
    bool ok{false};
    QString output_path;  ///< The written archive, or the extraction destination.
    int entries{0};       ///< Files written into or out of the archive.
    QStringList blockers;
    QStringList warnings;
};

/// One entry in an archive's central directory (see listEntries).
struct ArchiveEntryInfo {
    QString path;        ///< Entry path within the archive.
    qint64 size{0};      ///< Uncompressed size in bytes (0 for a directory entry).
    bool is_dir{false};  ///< True for a directory entry.
};

/// Result of a read-only archive LISTING (no extraction, no writes).
struct ArchiveListing {
    bool ok{false};
    int total_entries{0};               ///< Entries in the archive's central directory.
    qint64 total_uncompressed_bytes{0};
    QVector<ArchiveEntryInfo> entries;  ///< Bounded to the entry cap; truncation reported via ok.
    QStringList blockers;
};

/// Zip archive engine mirroring the Files StorageArchiveService semantics the
/// explorer clones: archive naming from the selection, "smart" single-root
/// detection, and recursive folder compression. Host (local) paths only - raw
/// sources and destinations stage through the bridge at the panel layer.
class FileExplorerArchiveService {
public:
    /// Files GenerateArchiveNameFromItems: one item names the archive after
    /// itself, several items name it after their parent folder.
    [[nodiscard]] static QString archiveBaseName(const QStringList& item_names,
                                                 const QString& parent_name);
    /// True when @p name has the .zip extension (Files IsZipFile).
    [[nodiscard]] static bool isZipName(const QString& name);
    /// Compress @p source_paths (files or folders, recursed) into @p zip_path.
    [[nodiscard]] static FileExplorerArchiveResult compressToZip(const QString& zip_path,
                                                                 const QStringList& source_paths);
    /// Extract the whole archive into @p destination_dir (created if missing).
    [[nodiscard]] static FileExplorerArchiveResult extractZip(const QString& zip_path,
                                                              const QString& destination_dir);
    /// List an archive's entries WITHOUT extracting (reads only the central directory; writes
    /// nothing). Fails closed (ok=false + a blocker) on an unreadable archive or one whose entry
    /// count exceeds the shared cap.
    [[nodiscard]] static ArchiveListing listEntries(const QString& zip_path);
    /// Files smart-extract probe: true when every entry lives under one
    /// top-level folder; @p root_name receives that folder's name.
    [[nodiscard]] static bool hasSingleTopLevelRoot(const QString& zip_path, QString* root_name);
};

}  // namespace sak
