// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_tag_store.h
/// @brief App-level File Explorer item tags keyed by stable target identity + path.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

class QSettings;

namespace sak {

/// One tagged item: the stable target id plus the item path it was tagged on.
struct FileExplorerTaggedItem {
    QString target_id;
    QString path;
    QStringList tags;
};

/// Stores File Explorer tags as S.A.K. app metadata only. Tags are keyed by stable
/// target identity plus item path and never written into raw HFS/APFS metadata.
/// Each item's record lives under a content-addressed sub-group so arbitrary path
/// characters are safe as QSettings keys.
class FileExplorerTagStore {
public:
    /// Tags currently set on (target_id, path), sorted and de-duplicated.
    [[nodiscard]] static QStringList tagsFor(QSettings& settings,
                                             const QString& group,
                                             const QString& target_id,
                                             const QString& path);

    /// Replace the tags on (target_id, path). An empty/whitespace-only list removes
    /// the record entirely. Tags are trimmed, de-duplicated, and sorted.
    static void setTags(QSettings& settings,
                        const QString& group,
                        const QString& target_id,
                        const QString& path,
                        const QStringList& tags);

    /// Union of every tag in use, sorted and de-duplicated (drives the sidebar Tags group).
    [[nodiscard]] static QStringList allTags(QSettings& settings, const QString& group);

    /// Every item carrying @p tag.
    [[nodiscard]] static QVector<FileExplorerTaggedItem> itemsWithTag(QSettings& settings,
                                                                      const QString& group,
                                                                      const QString& tag);
};

}  // namespace sak
