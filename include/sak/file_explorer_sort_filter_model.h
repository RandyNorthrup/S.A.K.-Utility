// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_sort_filter_model.h
/// @brief Sort/filter proxy for File Explorer item rows.

#pragma once

#include "sak/file_explorer_types.h"

#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>

namespace sak {

class FileExplorerSortFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit FileExplorerSortFilterModel(QObject* parent = nullptr);

    void setNameFilter(const QString& filter_text);
    void setShowHiddenItems(bool show);
    void setFolderSortPlacement(FileExplorerFolderSortPlacement placement);
    [[nodiscard]] QString nameFilter() const;
    [[nodiscard]] bool showHiddenItems() const;
    [[nodiscard]] FileExplorerFolderSortPlacement folderSortPlacement() const {
        return m_folder_placement;
    }

    /// Restrict visible rows to the given set of item paths (a tag filter). An
    /// empty active set hides everything; clearTagFilter() disables the filter.
    void setTagFilter(const QSet<QString>& paths);
    void clearTagFilter();
    [[nodiscard]] bool tagFilterActive() const;

protected:
    [[nodiscard]] bool lessThan(const QModelIndex& source_left,
                                const QModelIndex& source_right) const override;
    [[nodiscard]] bool filterAcceptsRow(int source_row,
                                        const QModelIndex& source_parent) const override;

private:
    QString m_name_filter;
    bool m_show_hidden_items{false};
    FileExplorerFolderSortPlacement m_folder_placement{
        FileExplorerFolderSortPlacement::FoldersFirst};
    QSet<QString> m_tag_filter_paths;
    bool m_tag_filter_active{false};
};

}  // namespace sak
