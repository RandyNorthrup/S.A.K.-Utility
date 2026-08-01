// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_group_proxy_model.h
/// @brief Group-header proxy for the File Management Explorer: injects
///        Files-style group section headers above the sorted item rows.

#pragma once

#include "sak/file_explorer_grouping.h"

#include <QAbstractProxyModel>
#include <QVector>

namespace sak {

/// Sits between the sort/filter proxy and the item views. With grouping off
/// it is a transparent identity mapping; with grouping on it reorders rows
/// into group sections (Files GroupingHelper order: section rank, then the
/// source sort order within each section) and injects one non-selectable
/// header row per section.
class FileExplorerGroupProxyModel : public QAbstractProxyModel {
    Q_OBJECT

public:
    enum GroupRole {
        IsGroupHeaderRole = Qt::UserRole + 101,
        GroupTextRole,
    };

    explicit FileExplorerGroupProxyModel(QObject* parent = nullptr);

    void setSourceModel(QAbstractItemModel* source_model) override;

    /// Applies a grouping configuration; None restores identity mapping.
    void setGrouping(FileExplorerGroupOption option,
                     FileExplorerGroupDateUnit date_unit,
                     Qt::SortOrder direction);
    [[nodiscard]] FileExplorerGroupOption groupOption() const;
    [[nodiscard]] FileExplorerGroupDateUnit groupDateUnit() const;
    [[nodiscard]] Qt::SortOrder groupDirection() const;

    [[nodiscard]] bool isHeaderRow(int proxy_row) const;
    /// Proxy rows of every injected header (for details-view row spans).
    [[nodiscard]] QVector<int> headerRows() const;

    [[nodiscard]] QModelIndex mapToSource(const QModelIndex& proxy_index) const override;
    [[nodiscard]] QModelIndex mapFromSource(const QModelIndex& source_index) const override;
    [[nodiscard]] QModelIndex index(int row,
                                    int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section,
                                      Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    [[nodiscard]] bool grouped() const;
    void connectSourceSignals(QAbstractItemModel* source_model);
    void connectDataChangedSignal(QAbstractItemModel* source_model);
    void rebuildGroups();
    void resetWithGroups();

    // Source structural changes drive a full proxy reset. beginSourceReset /
    // endSourceReset depth-guard the bracket so overlapping source signals never
    // nest beginResetModel.
    void beginSourceReset();
    void endSourceReset();

    // Source re-sort (layoutChanged) handling, split so the persistent-index
    // mapping is snapshot before the source re-sorts and remapped after.
    void onSourceLayoutAboutToBeChanged();
    void onSourceLayoutChanged();

    // True when an in-place source dataChanged touched the role feeding the
    // current grouping key, so the affected rows may need to move sections.
    [[nodiscard]] bool dataChangeAffectsGrouping(const QList<int>& roles) const;

    // Grouped-mode row map: source_row >= 0 is an item; -1 is a header whose
    // text lives in header_text.
    struct ProxyRow {
        int source_row{-1};
        QString header_text;
    };

    FileExplorerGroupOption m_option{FileExplorerGroupOption::None};
    FileExplorerGroupDateUnit m_date_unit{FileExplorerGroupDateUnit::Year};
    Qt::SortOrder m_direction{Qt::AscendingOrder};
    QVector<ProxyRow> m_rows;
    QVector<int> m_source_to_proxy;

    // Carried from the source's layoutAboutToBeChanged to its layoutChanged so
    // the proxy->source mapping is captured BEFORE the source re-sorts (the
    // group rebuild afterwards would otherwise read stale rows and remap every
    // held selection/current index to the wrong item).
    QModelIndexList m_layout_proxy_indexes;
    QVector<QPersistentModelIndex> m_layout_source_indexes;

    // Depth of nested source reset brackets; only the outermost drives the proxy
    // reset (see beginSourceReset / endSourceReset).
    int m_reset_depth{0};
};

}  // namespace sak
