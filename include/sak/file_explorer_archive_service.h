// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_archive_service.h
/// @brief Zip compress/extract engine for the File Management Explorer.

#pragma once

#include <QString>
#include <QStringList>

namespace sak {

/// Result of one archive operation (compress or extract).
struct FileExplorerArchiveResult {
    bool ok{false};
    QString output_path;  ///< The written archive, or the extraction destination.
    int entries{0};       ///< Files written into or out of the archive.
    QStringList blockers;
    QStringList warnings;
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
    /// Files smart-extract probe: true when every entry lives under one
    /// top-level folder; @p root_name receives that folder's name.
    [[nodiscard]] static bool hasSingleTopLevelRoot(const QString& zip_path, QString* root_name);
};

}  // namespace sak
