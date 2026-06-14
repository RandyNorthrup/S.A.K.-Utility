// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_sort_filter_model.h"

#include "sak/file_explorer_item_model.h"

#include <QDateTime>

#include <optional>

namespace sak {
namespace {

bool directoryAt(const QModelIndex& index) {
    return index.siblingAtColumn(FileExplorerItemModel::NameColumn)
        .data(FileExplorerItemModel::EntryDirectoryRole)
        .toBool();
}

QString nameAt(const QModelIndex& index) {
    return index.siblingAtColumn(FileExplorerItemModel::NameColumn)
        .data(FileExplorerItemModel::EntryNameRole)
        .toString();
}

std::optional<bool> sizeLessThan(const QModelIndex& left, const QModelIndex& right) {
    if (left.column() != FileExplorerItemModel::SizeColumn) {
        return std::nullopt;
    }
    const quint64 left_size = left.siblingAtColumn(FileExplorerItemModel::NameColumn)
                                  .data(FileExplorerItemModel::EntrySizeRole)
                                  .toULongLong();
    const quint64 right_size = right.siblingAtColumn(FileExplorerItemModel::NameColumn)
                                   .data(FileExplorerItemModel::EntrySizeRole)
                                   .toULongLong();
    if (left_size != right_size) {
        return left_size < right_size;
    }
    return std::nullopt;
}

std::optional<bool> dateTimeLessThan(const QModelIndex& left, const QModelIndex& right) {
    const int column = left.column();
    if (column != FileExplorerItemModel::ModifiedColumn &&
        column != FileExplorerItemModel::CreatedColumn) {
        return std::nullopt;
    }
    const int role = column == FileExplorerItemModel::ModifiedColumn
                         ? FileExplorerItemModel::EntryModifiedTimeRole
                         : FileExplorerItemModel::EntryCreatedTimeRole;
    const QDateTime left_time =
        left.siblingAtColumn(FileExplorerItemModel::NameColumn).data(role).toDateTime();
    const QDateTime right_time =
        right.siblingAtColumn(FileExplorerItemModel::NameColumn).data(role).toDateTime();
    if (left_time.isValid() != right_time.isValid()) {
        return left_time.isValid();
    }
    if (left_time != right_time) {
        return left_time < right_time;
    }
    return std::nullopt;
}

}  // namespace

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
    const bool left_directory = directoryAt(source_left);
    if (left_directory != directoryAt(source_right)) {
        return sortOrder() == Qt::DescendingOrder ? !left_directory : left_directory;
    }
    if (const auto by_size = sizeLessThan(source_left, source_right)) {
        return *by_size;
    }
    if (const auto by_time = dateTimeLessThan(source_left, source_right)) {
        return *by_time;
    }
    const int compared = QString::localeAwareCompare(source_left.data(Qt::DisplayRole).toString(),
                                                     source_right.data(Qt::DisplayRole).toString());
    if (compared != 0) {
        return compared < 0;
    }
    return QString::localeAwareCompare(nameAt(source_left), nameAt(source_right)) < 0;
}

bool FileExplorerSortFilterModel::filterAcceptsRow(const int source_row,
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
