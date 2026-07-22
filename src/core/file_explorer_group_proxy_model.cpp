// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_group_proxy_model.cpp
/// @brief Group-header proxy: Files-style group sections over sorted rows.

#include "sak/file_explorer_group_proxy_model.h"

#include "sak/file_explorer_item_model.h"

#include <QFont>

#include <algorithm>

namespace sak {

namespace {

// One group section while rebuilding: the header info plus the source rows
// that fall into it, in source (already sorted) order.
struct GroupBucket {
    FileExplorerGroupInfo info;
    QVector<int> source_rows;
};

bool bucketBefore(const GroupBucket& lhs, const GroupBucket& rhs) {
    if (lhs.info.sort_index != rhs.info.sort_index) {
        return lhs.info.sort_index < rhs.info.sort_index;
    }
    return QString::localeAwareCompare(lhs.info.text, rhs.info.text) < 0;
}

}  // namespace

FileExplorerGroupProxyModel::FileExplorerGroupProxyModel(QObject* parent)
    : QAbstractProxyModel(parent) {}

void FileExplorerGroupProxyModel::setSourceModel(QAbstractItemModel* source_model) {
    beginResetModel();
    if (sourceModel()) {
        disconnect(sourceModel(), nullptr, this, nullptr);
    }
    QAbstractProxyModel::setSourceModel(source_model);
    if (source_model) {
        connectSourceSignals(source_model);
    }
    rebuildGroups();
    endResetModel();
}

void FileExplorerGroupProxyModel::connectSourceSignals(QAbstractItemModel* source_model) {
    // Structure changes regroup via a full reset: group membership can move
    // arbitrarily, and the explorer reloads listings through model resets
    // anyway, so surgical row moves would add complexity without a consumer.
    connect(source_model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        beginResetModel();
    });
    connect(source_model, &QAbstractItemModel::modelReset, this, [this]() {
        rebuildGroups();
        endResetModel();
    });
    connect(source_model, &QAbstractItemModel::rowsAboutToBeInserted, this, [this]() {
        beginResetModel();
    });
    connect(source_model, &QAbstractItemModel::rowsInserted, this, [this]() {
        rebuildGroups();
        endResetModel();
    });
    connect(source_model, &QAbstractItemModel::rowsAboutToBeRemoved, this, [this]() {
        beginResetModel();
    });
    connect(source_model, &QAbstractItemModel::rowsRemoved, this, [this]() {
        rebuildGroups();
        endResetModel();
    });
    connect(source_model,
            &QAbstractItemModel::layoutAboutToBeChanged,
            this,
            &FileExplorerGroupProxyModel::onSourceLayoutAboutToBeChanged);
    connect(source_model,
            &QAbstractItemModel::layoutChanged,
            this,
            &FileExplorerGroupProxyModel::onSourceLayoutChanged);
    connect(source_model,
            &QAbstractItemModel::dataChanged,
            this,
            [this](const QModelIndex& top_left,
                   const QModelIndex& bottom_right,
                   const QList<int>& roles) {
                // Group membership only changes with re-sorts or resets; a
                // plain data repaint (cut dimming, tag refresh) maps through.
                if (!top_left.isValid() || !bottom_right.isValid()) {
                    return;
                }
                for (int row = top_left.row(); row <= bottom_right.row(); ++row) {
                    const QModelIndex proxy_top =
                        mapFromSource(sourceModel()->index(row, top_left.column()));
                    const QModelIndex proxy_bottom =
                        mapFromSource(sourceModel()->index(row, bottom_right.column()));
                    if (proxy_top.isValid() && proxy_bottom.isValid()) {
                        Q_EMIT dataChanged(proxy_top, proxy_bottom, roles);
                    }
                }
            });
}

void FileExplorerGroupProxyModel::onSourceLayoutAboutToBeChanged() {
    // Snapshot the proxy->source mapping NOW, while m_rows still matches the
    // source's pre-sort order. Wrapping each source index in a persistent index
    // lets the source track it across its own re-sort; doing this in
    // layoutChanged instead would read stale rows and remap to wrong items.
    m_layout_proxy_indexes = persistentIndexList();
    m_layout_source_indexes.clear();
    m_layout_source_indexes.reserve(m_layout_proxy_indexes.size());
    for (const QModelIndex& proxy_index : m_layout_proxy_indexes) {
        m_layout_source_indexes.append(QPersistentModelIndex(mapToSource(proxy_index)));
    }
    Q_EMIT layoutAboutToBeChanged();
}

void FileExplorerGroupProxyModel::onSourceLayoutChanged() {
    // The sort proxy re-sorted: regroup, then remap each held persistent index
    // (selection, current) through its now-updated source index.
    rebuildGroups();
    for (int i = 0; i < m_layout_proxy_indexes.size(); ++i) {
        changePersistentIndex(m_layout_proxy_indexes.at(i),
                              mapFromSource(m_layout_source_indexes.at(i)));
    }
    m_layout_proxy_indexes.clear();
    m_layout_source_indexes.clear();
    Q_EMIT layoutChanged();
}

void FileExplorerGroupProxyModel::setGrouping(const FileExplorerGroupOption option,
                                              const FileExplorerGroupDateUnit date_unit,
                                              const Qt::SortOrder direction) {
    if (m_option == option && m_date_unit == date_unit && m_direction == direction) {
        return;
    }
    beginResetModel();
    m_option = option;
    m_date_unit = date_unit;
    m_direction = direction;
    rebuildGroups();
    endResetModel();
}

FileExplorerGroupOption FileExplorerGroupProxyModel::groupOption() const {
    return m_option;
}

FileExplorerGroupDateUnit FileExplorerGroupProxyModel::groupDateUnit() const {
    return m_date_unit;
}

Qt::SortOrder FileExplorerGroupProxyModel::groupDirection() const {
    return m_direction;
}

bool FileExplorerGroupProxyModel::grouped() const {
    return m_option != FileExplorerGroupOption::None && sourceModel();
}

bool FileExplorerGroupProxyModel::isHeaderRow(const int proxy_row) const {
    return grouped() && proxy_row >= 0 && proxy_row < m_rows.size() &&
           m_rows.at(proxy_row).source_row < 0;
}

QVector<int> FileExplorerGroupProxyModel::headerRows() const {
    QVector<int> rows;
    if (!grouped()) {
        return rows;
    }
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).source_row < 0) {
            rows.append(row);
        }
    }
    return rows;
}

void FileExplorerGroupProxyModel::rebuildGroups() {
    m_rows.clear();
    m_source_to_proxy.clear();
    if (!grouped()) {
        return;
    }
    const QAbstractItemModel* source = sourceModel();
    const int count = source->rowCount();
    const QDateTime now = QDateTime::currentDateTime();

    // Bucket the sorted source rows, keeping the source order inside each
    // bucket (Files: grouping and sorting are orthogonal).
    QVector<GroupBucket> buckets;
    for (int row = 0; row < count; ++row) {
        const QModelIndex index = source->index(row, FileExplorerItemModel::NameColumn);
        FileExplorerGroupSource group_source;
        group_source.name = index.data(FileExplorerItemModel::EntryNameRole).toString();
        group_source.type = index.data(FileExplorerItemModel::EntryTypeRole).toString();
        group_source.size_bytes = index.data(FileExplorerItemModel::EntrySizeRole).toULongLong();
        group_source.directory = index.data(FileExplorerItemModel::EntryDirectoryRole).toBool();
        group_source.modified_time =
            index.data(FileExplorerItemModel::EntryModifiedTimeRole).toDateTime();
        group_source.created_time =
            index.data(FileExplorerItemModel::EntryCreatedTimeRole).toDateTime();
        group_source.tags = index.data(FileExplorerItemModel::EntryTagsRole).toStringList();
        const FileExplorerGroupInfo info =
            fileExplorerGroupInfo(group_source, m_option, m_date_unit, now);
        auto it = std::find_if(buckets.begin(), buckets.end(), [&info](const GroupBucket& b) {
            return b.info == info;
        });
        if (it == buckets.end()) {
            buckets.append(GroupBucket{info, {}});
            it = buckets.end() - 1;
        }
        it->source_rows.append(row);
    }
    std::stable_sort(buckets.begin(), buckets.end(), bucketBefore);
    if (m_direction == Qt::DescendingOrder) {
        std::reverse(buckets.begin(), buckets.end());
    }

    m_source_to_proxy.resize(count);
    for (const GroupBucket& bucket : buckets) {
        m_rows.append(ProxyRow{-1, bucket.info.text});
        for (const int source_row : bucket.source_rows) {
            m_source_to_proxy[source_row] = m_rows.size();
            m_rows.append(ProxyRow{source_row, {}});
        }
    }
}

QModelIndex FileExplorerGroupProxyModel::mapToSource(const QModelIndex& proxy_index) const {
    if (!proxy_index.isValid() || !sourceModel()) {
        return {};
    }
    if (!grouped()) {
        return sourceModel()->index(proxy_index.row(), proxy_index.column());
    }
    if (proxy_index.row() >= m_rows.size() || m_rows.at(proxy_index.row()).source_row < 0) {
        return {};
    }
    return sourceModel()->index(m_rows.at(proxy_index.row()).source_row, proxy_index.column());
}

QModelIndex FileExplorerGroupProxyModel::mapFromSource(const QModelIndex& source_index) const {
    if (!source_index.isValid()) {
        return {};
    }
    if (!grouped()) {
        return createIndex(source_index.row(), source_index.column());
    }
    if (source_index.row() >= m_source_to_proxy.size()) {
        return {};
    }
    return createIndex(m_source_to_proxy.at(source_index.row()), source_index.column());
}

QModelIndex FileExplorerGroupProxyModel::index(const int row,
                                               const int column,
                                               const QModelIndex& parent) const {
    if (parent.isValid() || row < 0 || row >= rowCount() || column < 0 || column >= columnCount()) {
        return {};
    }
    return createIndex(row, column);
}

QModelIndex FileExplorerGroupProxyModel::parent(const QModelIndex& index) const {
    Q_UNUSED(index);
    return {};
}

int FileExplorerGroupProxyModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !sourceModel()) {
        return 0;
    }
    return grouped() ? m_rows.size() : sourceModel()->rowCount();
}

int FileExplorerGroupProxyModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid() || !sourceModel()) {
        return 0;
    }
    return sourceModel()->columnCount();
}

QVariant FileExplorerGroupProxyModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid()) {
        return {};
    }
    if (isHeaderRow(index.row())) {
        const QString text = m_rows.at(index.row()).header_text;
        switch (role) {
        case Qt::DisplayRole:
        case GroupTextRole:
            return index.column() == 0 ? QVariant(text) : QVariant();
        case IsGroupHeaderRole:
            return true;
        case Qt::FontRole: {
            // Files group headers use the SubtitleTextBlock style: bold and
            // visually separated from the item rows.
            QFont font;
            font.setBold(true);
            return font;
        }
        default:
            return {};
        }
    }
    if (role == IsGroupHeaderRole) {
        return false;
    }
    return QAbstractProxyModel::data(index, role);
}

QVariant FileExplorerGroupProxyModel::headerData(const int section,
                                                 const Qt::Orientation orientation,
                                                 const int role) const {
    // The base class maps the section through proxy row 0; when row 0 is an
    // injected group header that maps to nothing and the view headers blank
    // out. Columns are never rearranged here, so forward sections directly.
    if (!sourceModel() || orientation != Qt::Horizontal) {
        return {};
    }
    return sourceModel()->headerData(section, orientation, role);
}

Qt::ItemFlags FileExplorerGroupProxyModel::flags(const QModelIndex& index) const {
    if (isHeaderRow(index.row())) {
        return Qt::ItemIsEnabled | Qt::ItemNeverHasChildren;
    }
    return QAbstractProxyModel::flags(index);
}

void FileExplorerGroupProxyModel::sort(const int column, const Qt::SortOrder order) {
    // Header clicks in the details view sort the VIEW's model; forward to the
    // sort/filter proxy underneath (the base class silently drops sort()).
    if (sourceModel()) {
        sourceModel()->sort(column, order);
    }
}

}  // namespace sak
