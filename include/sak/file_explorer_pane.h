// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_pane.h
/// @brief File Explorer main item pane widget.

#pragma once

#include "sak/file_explorer_details_view.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_sort_filter_model.h"
#include "sak/file_explorer_types.h"
#include "sak/layout_constants.h"

#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QStackedWidget>
#include <QWidget>

class QVBoxLayout;

namespace sak {

class FileExplorerPane : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerPane(QWidget* parent = nullptr);

    [[nodiscard]] FileExplorerItemModel* itemModel() const;
    [[nodiscard]] FileExplorerSortFilterModel* sortFilterModel() const;
    [[nodiscard]] QTableView* tableView() const;
    [[nodiscard]] QListView* listView() const;
    [[nodiscard]] QListView* gridView() const;
    [[nodiscard]] QListView* cardsView() const;
    [[nodiscard]] QListView* columnsView() const;
    [[nodiscard]] QListView* columnsPreviewView() const;
    [[nodiscard]] QVector<QAbstractItemView*> itemViews() const;
    [[nodiscard]] QAbstractItemView* activeItemView() const;
    [[nodiscard]] QItemSelectionModel* sharedSelectionModel() const;
    [[nodiscard]] QLabel* statusLabel() const;
    [[nodiscard]] QLabel* stateLabel() const;
    [[nodiscard]] FileManagementEntry entryAtViewRow(int row) const;
    [[nodiscard]] bool hasViewEntry(int row) const;
    [[nodiscard]] FileExplorerViewMode viewMode() const;
    [[nodiscard]] int itemSizePx() const;
    [[nodiscard]] bool showHiddenItems() const;
    [[nodiscard]] bool showFileExtensions() const;

    void setViewMode(FileExplorerViewMode mode);
    void setItemSizePx(int item_size_px);
    void setShowHiddenItems(bool show);
    void setShowFileExtensions(bool show);
    void setColumnsPreviewEntries(const QString& path, QVector<FileManagementEntry> entries);
    void clearColumnsPreview();
    void showReadyState();
    void showLoadingState(const QString& message);
    void showEmptyState(const QString& message);
    void showErrorState(const QString& message);

private:
    void buildStateLabel(QVBoxLayout* layout);
    void buildModels();
    void buildItemViews();
    void buildStatusLabel(QVBoxLayout* layout);
    void connectSignals();
    void setStateMessage(const QString& message, bool visible);
    void configureListView(QListView* view, const QString& object_name, QListView::ViewMode mode);
    void configureColumnsPreviewView(QListView* view);
    void applyItemSize();
    void updateColumnsPreviewRequest();
    [[nodiscard]] FileManagementEntry columnsPreviewEntryAtRow(int row) const;

Q_SIGNALS:
    void columnsDirectoryPreviewRequested(const QString& path);
    void columnsChildActivated(const QString& path);

private:
    FileExplorerItemModel* m_item_model{nullptr};
    FileExplorerSortFilterModel* m_sort_filter_model{nullptr};
    FileExplorerItemModel* m_columns_preview_model{nullptr};
    FileExplorerSortFilterModel* m_columns_preview_proxy{nullptr};
    QItemSelectionModel* m_selection_model{nullptr};
    QStackedWidget* m_view_stack{nullptr};
    FileExplorerDetailsView* m_details_view{nullptr};
    QListView* m_list_view{nullptr};
    QListView* m_grid_view{nullptr};
    QListView* m_cards_view{nullptr};
    QListView* m_columns_view{nullptr};
    QListView* m_columns_preview_view{nullptr};
    QWidget* m_columns_container{nullptr};
    QLabel* m_state_label{nullptr};
    QLabel* m_status_label{nullptr};
    QString m_columns_preview_path;
    FileExplorerViewMode m_view_mode{FileExplorerViewMode::Details};
    int m_item_size_px{kFileExplorerItemSizeDefault};
};

}  // namespace sak
