// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_grouping.h
/// @brief Files-style group-by options and group-key computation for the
///        File Management Explorer (clone of GroupOption.cs and
///        Utils/Storage/Collection/GroupingHelper.cs from the Files app).

#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace sak {

// Files Data/Enums/GroupOption.cs. SyncStatus, OriginalFolder, DateDeleted,
// and FolderPath are cloud-drive / recycle-bin / library specific in Files
// and have no S.A.K. equivalent surface, so only the folder-general options
// are cloned here.
enum class FileExplorerGroupOption {
    None = 0,
    Name,
    DateModified,
    DateCreated,
    Size,
    FileType,
    FileTag,
};

// Files GroupByDateUnit: how date groups bucket beyond the relative labels.
enum class FileExplorerGroupDateUnit {
    Year = 0,
    Month,
    Day,
};

/// The fields a group key depends on, decoupled from FileManagementEntry so
/// the proxy can feed role data straight from any source model.
struct FileExplorerGroupSource {
    QString name;
    QString type;
    quint64 size_bytes{0};
    bool directory{false};
    QDateTime modified_time;
    QDateTime created_time;
    QStringList tags;
};

/// A computed group header: display text plus the ordering rank used to sort
/// group sections (Files SortIndexOverride).
struct FileExplorerGroupInfo {
    QString text;
    int sort_index{0};

    [[nodiscard]] friend bool operator==(const FileExplorerGroupInfo& lhs,
                                         const FileExplorerGroupInfo& rhs) = default;
};

/// Computes the group header for one item, cloning the Files
/// GroupingHelper.GetItemGroupKeySelector / GetGroupInfoSelector semantics.
/// `now` anchors the relative date buckets so tests stay deterministic.
[[nodiscard]] FileExplorerGroupInfo fileExplorerGroupInfo(const FileExplorerGroupSource& source,
                                                          FileExplorerGroupOption option,
                                                          FileExplorerGroupDateUnit date_unit,
                                                          const QDateTime& now);

/// Settings-name round trip for persistence (mirrors the Files setting values).
[[nodiscard]] QString fileExplorerGroupOptionName(FileExplorerGroupOption option);
[[nodiscard]] FileExplorerGroupOption fileExplorerGroupOptionFromName(const QString& name);

}  // namespace sak
