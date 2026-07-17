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
    void rebuildGroups();
    void resetWithGroups();

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
};

}  // namespace sak
