// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_item_model.h
/// @brief Model/view item model for the File Management Explorer.

#pragma once

#include "sak/file_management_file_system.h"

#include <QAbstractTableModel>
#include <QDateTime>
#include <QModelIndex>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>

class QMimeData;

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
        TagsColumn,
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
        EntryTagsRole,
    };

    /// Callback that returns the app-level tags for an entry path. Injected by the
    /// panel so the model stays decoupled from the tag store; unset = no Tags column data.
    using TagProvider = std::function<QStringList(const QString& path)>;

    /// Callback that returns the row icon for an entry. Injected by the panel so
    /// the model stays decoupled from the GUI icon registry; unset = no row icons.
    using IconProvider = std::function<QVariant(const FileManagementEntry& entry)>;

    /// Callback that builds the drag payload for a set of entry rows. Injected by
    /// the panel so the model stays decoupled from the clipboard/transfer plumbing;
    /// unset = rows are not draggable.
    using DragPayloadProvider = std::function<QMimeData*(const QList<int>& rows)>;

    explicit FileExplorerItemModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section,
                                      Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;

    void setEntries(QVector<FileManagementEntry> entries);
    void clear();
    void setShowFileExtensions(bool show);
    /// Mark clipboard-cut items (Files dims them at 0.4 opacity until the
    /// move-paste lands or a new transfer replaces the clipboard).
    void setCutPaths(const QSet<QString>& paths);
    [[nodiscard]] QSet<QString> cutPaths() const;
    /// Set the tag lookup used by the Tags column; pass a null function to disable it.
    void setTagProvider(TagProvider provider);
    /// Set the icon lookup used for the Name column; pass a null function to disable it.
    void setIconProvider(IconProvider provider);
    /// Set the drag payload builder; pass a null function to disable dragging.
    void setDragPayloadProvider(DragPayloadProvider provider);
    /// Notify views that tag data changed (e.g. after editing tags) so the column repaints.
    void refreshTags();

    [[nodiscard]] QVector<FileManagementEntry> entries() const;
    [[nodiscard]] FileManagementEntry entryAt(int row) const;
    [[nodiscard]] bool hasEntry(int row) const;
    [[nodiscard]] bool showFileExtensions() const;
    [[nodiscard]] static QString attributeSummary(const FileManagementEntry& entry);
    [[nodiscard]] static QString sizeText(uint64_t bytes);
    [[nodiscard]] static QString timeText(const QDateTime& time);

Q_SIGNALS:
    /// Inline rename commit: the model performs no mutation itself; the panel
    /// routes the request through the file-system bridge and reloads.
    void renameRequested(int row, const QString& new_name);

private:
    [[nodiscard]] QStringList tagsForEntry(const FileManagementEntry& entry) const;
    [[nodiscard]] QVariant displayForColumn(const FileManagementEntry& entry, int column) const;
    [[nodiscard]] QVariant editForColumn(const FileManagementEntry& entry, int column) const;
    [[nodiscard]] QVariant foregroundForEntry(const FileManagementEntry& entry) const;
    [[nodiscard]] QVariant decorationForColumn(const FileManagementEntry& entry, int column) const;
    [[nodiscard]] QString completedRenameName(const FileManagementEntry& entry,
                                              const QString& edited) const;

    QVector<FileManagementEntry> m_entries;
    bool m_show_file_extensions{true};
    QSet<QString> m_cut_paths;
    TagProvider m_tag_provider;
    IconProvider m_icon_provider;
    DragPayloadProvider m_drag_payload_provider;
};

}  // namespace sak
