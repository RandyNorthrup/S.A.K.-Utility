// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_item_model.h
/// @brief Model/view item model for the File Management Explorer.

#pragma once

#include "sak/file_management_file_system.h"

#include <QAbstractTableModel>
#include <QDateTime>
#include <QModelIndex>
#include <QVariant>
#include <QVector>

namespace sak {

class FileExplorerItemModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn = 0,
        TypeColumn,
        SizeColumn,
        ModifiedColumn,
        CreatedColumn,
        IdentifierColumn,
        AttributesColumn,
        PathColumn,
        ColumnCount,
    };

    enum ItemRole {
        EntryPathRole = Qt::UserRole + 1,
        EntryNameRole,
        EntryTypeRole,
        EntrySizeRole,
        EntryIdentifierRole,
        EntryDirectoryRole,
        EntryRegularFileRole,
        EntrySymlinkRole,
        EntryLinkTargetRole,
        EntryAttributeSummaryRole,
        EntryModifiedTimeRole,
        EntryCreatedTimeRole,
    };

    explicit FileExplorerItemModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section,
                                      Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    void setEntries(QVector<FileManagementEntry> entries);
    void clear();
    void setShowFileExtensions(bool show);

    [[nodiscard]] QVector<FileManagementEntry> entries() const;
    [[nodiscard]] FileManagementEntry entryAt(int row) const;
    [[nodiscard]] bool hasEntry(int row) const;
    [[nodiscard]] bool showFileExtensions() const;
    [[nodiscard]] static QString attributeSummary(const FileManagementEntry& entry);
    [[nodiscard]] static QString sizeText(uint64_t bytes);
    [[nodiscard]] static QString timeText(const QDateTime& time);

private:
    QVector<FileManagementEntry> m_entries;
    bool m_show_file_extensions{true};
};

}  // namespace sak
