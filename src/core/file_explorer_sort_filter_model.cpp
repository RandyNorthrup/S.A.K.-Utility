// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_sort_filter_model.h"

#include "sak/file_explorer_item_model.h"

#include <QDateTime>

namespace sak {

FileExplorerSortFilterModel::FileExplorerSortFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void FileExplorerSortFilterModel::setNameFilter(const QString& filter_text) {
    const QString clean = filter_text.trimmed();
    if (m_name_filter == clean) {
        return;
    }
    m_name_filter = clean;
    invalidateFilter();
}

QString FileExplorerSortFilterModel::nameFilter() const {
    return m_name_filter;
}

void FileExplorerSortFilterModel::setShowHiddenItems(const bool show) {
    if (m_show_hidden_items == show) {
        return;
    }
    m_show_hidden_items = show;
    invalidateFilter();
}

bool FileExplorerSortFilterModel::showHiddenItems() const {
    return m_show_hidden_items;
}

bool FileExplorerSortFilterModel::lessThan(const QModelIndex& source_left,
                                           const QModelIndex& source_right) const {
    const bool left_directory =
        source_left.siblingAtColumn(FileExplorerItemModel::NameColumn)
            .data(FileExplorerItemModel::EntryDirectoryRole)
            .toBool();
    const bool right_directory =
        source_right.siblingAtColumn(FileExplorerItemModel::NameColumn)
            .data(FileExplorerItemModel::EntryDirectoryRole)
            .toBool();
    if (left_directory != right_directory) {
        return sortOrder() == Qt::DescendingOrder ? !left_directory : left_directory;
    }

    if (source_left.column() == FileExplorerItemModel::SizeColumn) {
        const quint64 left_size =
            source_left.siblingAtColumn(FileExplorerItemModel::NameColumn)
                .data(FileExplorerItemModel::EntrySizeRole)
                .toULongLong();
        const quint64 right_size =
            source_right.siblingAtColumn(FileExplorerItemModel::NameColumn)
                .data(FileExplorerItemModel::EntrySizeRole)
                .toULongLong();
        if (left_size != right_size) {
            return left_size < right_size;
        }
    }
    if (source_left.column() == FileExplorerItemModel::ModifiedColumn ||
        source_left.column() == FileExplorerItemModel::CreatedColumn) {
        const int role = source_left.column() == FileExplorerItemModel::ModifiedColumn
                             ? FileExplorerItemModel::EntryModifiedTimeRole
                             : FileExplorerItemModel::EntryCreatedTimeRole;
        const QDateTime left_time =
            source_left.siblingAtColumn(FileExplorerItemModel::NameColumn).data(role).toDateTime();
        const QDateTime right_time =
            source_right.siblingAtColumn(FileExplorerItemModel::NameColumn).data(role).toDateTime();
        if (left_time.isValid() != right_time.isValid()) {
            return left_time.isValid();
        }
        if (left_time != right_time) {
            return left_time < right_time;
        }
    }

    const QString left_text = source_left.data(Qt::DisplayRole).toString();
    const QString right_text = source_right.data(Qt::DisplayRole).toString();
    const int compared = QString::localeAwareCompare(left_text, right_text);
    if (compared != 0) {
        return compared < 0;
    }
    return QString::localeAwareCompare(
               source_left.siblingAtColumn(FileExplorerItemModel::NameColumn)
                   .data(FileExplorerItemModel::EntryNameRole)
                   .toString(),
               source_right.siblingAtColumn(FileExplorerItemModel::NameColumn)
                   .data(FileExplorerItemModel::EntryNameRole)
                   .toString()) < 0;
}

bool FileExplorerSortFilterModel::filterAcceptsRow(
    const int source_row,
    const QModelIndex& source_parent) const {
    const QModelIndex name_index =
        sourceModel()->index(source_row, FileExplorerItemModel::NameColumn, source_parent);
    const QString entry_name = name_index.data(FileExplorerItemModel::EntryNameRole).toString();
    if (!m_show_hidden_items && entry_name.startsWith(QLatin1Char('.'))) {
        return false;
    }

    if (m_name_filter.isEmpty()) {
        return true;
    }

    const QString haystack =
        QStringList{entry_name,
                    name_index.data(FileExplorerItemModel::EntryTypeRole).toString(),
                    name_index.data(FileExplorerItemModel::EntryPathRole).toString()}
            .join(QLatin1Char('\n'));
    return haystack.contains(m_name_filter, Qt::CaseInsensitive);
}

}  // namespace sak
