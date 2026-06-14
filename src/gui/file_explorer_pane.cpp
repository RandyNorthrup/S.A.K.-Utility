// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_pane.h"

#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace sak {

FileExplorerPane::FileExplorerPane(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kSpacingSmall);

    m_state_label = new QLabel(this);
    m_state_label->setObjectName(QStringLiteral("fileExplorerStateLabel"));
    m_state_label->setAccessibleName(tr("Explorer loading, empty, and error state"));
    m_state_label->setWordWrap(true);
    m_state_label->setVisible(false);
    m_state_label->setStyleSheet(ui::paddedStatusTextStyle(ui::kColorTextMuted, ui::kFontSizeNote));
    layout->addWidget(m_state_label);

    m_item_model = new FileExplorerItemModel(this);
    m_sort_filter_model = new FileExplorerSortFilterModel(this);
    m_sort_filter_model->setSourceModel(m_item_model);
    m_columns_preview_model = new FileExplorerItemModel(this);
    m_columns_preview_proxy = new FileExplorerSortFilterModel(this);
    m_columns_preview_proxy->setSourceModel(m_columns_preview_model);
    m_selection_model = new QItemSelectionModel(m_sort_filter_model, this);

    m_view_stack = new QStackedWidget(this);
    m_details_view = new FileExplorerDetailsView(this);
    m_details_view->setModel(m_sort_filter_model);
    m_details_view->setSelectionModel(m_selection_model);
    m_view_stack->addWidget(m_details_view);

    m_list_view = new QListView(this);
    configureListView(m_list_view, QStringLiteral("fileExplorerListView"), QListView::ListMode);
    m_view_stack->addWidget(m_list_view);

    m_grid_view = new QListView(this);
    configureListView(m_grid_view, QStringLiteral("fileExplorerGridView"), QListView::IconMode);
    m_view_stack->addWidget(m_grid_view);

    m_cards_view = new QListView(this);
    configureListView(m_cards_view, QStringLiteral("fileExplorerCardsView"), QListView::ListMode);
    m_view_stack->addWidget(m_cards_view);

    m_columns_container = new QWidget(this);
    m_columns_container->setObjectName(QStringLiteral("fileExplorerColumnsContainer"));
    auto* columnsLayout = new QHBoxLayout(m_columns_container);
    columnsLayout->setContentsMargins(0, 0, 0, 0);
    columnsLayout->setSpacing(ui::kSpacingSmall);
    m_columns_view = new QListView(m_columns_container);
    configureListView(m_columns_view,
                      QStringLiteral("fileExplorerColumnsView"),
                      QListView::ListMode);
    m_columns_preview_view = new QListView(m_columns_container);
    configureColumnsPreviewView(m_columns_preview_view);
    columnsLayout->addWidget(m_columns_view, 1);
    columnsLayout->addWidget(m_columns_preview_view, 1);
    m_view_stack->addWidget(m_columns_container);
    applyItemSize();
    layout->addWidget(m_view_stack, 1);

    m_status_label = new QLabel(tr("No target selected"), this);
    m_status_label->setObjectName(QStringLiteral("fileExplorerStatusLabel"));
    m_status_label->setAccessibleName(tr("Explorer status"));
    m_status_label->setWordWrap(true);
    m_status_label->setStyleSheet(
        ui::paddedStatusTextStyle(ui::kColorTextMuted, ui::kFontSizeNote));
    layout->addWidget(m_status_label);

    connect(m_selection_model,
            &QItemSelectionModel::selectionChanged,
            this,
            &FileExplorerPane::updateColumnsPreviewRequest);
    connect(m_columns_preview_view,
            &QAbstractItemView::doubleClicked,
            this,
            [this](const QModelIndex& index) {
                const FileManagementEntry entry = columnsPreviewEntryAtRow(index.row());
                if (!entry.path.trimmed().isEmpty()) {
                    Q_EMIT columnsChildActivated(entry.path);
                }
            });
}

FileExplorerItemModel* FileExplorerPane::itemModel() const {
    return m_item_model;
}

FileExplorerSortFilterModel* FileExplorerPane::sortFilterModel() const {
    return m_sort_filter_model;
}

QTableView* FileExplorerPane::tableView() const {
    return m_details_view;
}

QListView* FileExplorerPane::listView() const {
    return m_list_view;
}

QListView* FileExplorerPane::gridView() const {
    return m_grid_view;
}

QListView* FileExplorerPane::cardsView() const {
    return m_cards_view;
}

QListView* FileExplorerPane::columnsView() const {
    return m_columns_view;
}

QListView* FileExplorerPane::columnsPreviewView() const {
    return m_columns_preview_view;
}

QVector<QAbstractItemView*> FileExplorerPane::itemViews() const {
    return {m_details_view, m_list_view, m_grid_view, m_cards_view, m_columns_view};
}

QAbstractItemView* FileExplorerPane::activeItemView() const {
    if (!m_view_stack) {
        return nullptr;
    }
    if (m_view_mode == FileExplorerViewMode::Columns) {
        return m_columns_view;
    }
    return qobject_cast<QAbstractItemView*>(m_view_stack->currentWidget());
}

QItemSelectionModel* FileExplorerPane::sharedSelectionModel() const {
    return m_selection_model;
}

QLabel* FileExplorerPane::statusLabel() const {
    return m_status_label;
}

QLabel* FileExplorerPane::stateLabel() const {
    return m_state_label;
}

FileManagementEntry FileExplorerPane::entryAtViewRow(const int row) const {
    if (!hasViewEntry(row)) {
        return {};
    }
    const QModelIndex proxy_index = m_sort_filter_model->index(row,
                                                               FileExplorerItemModel::NameColumn);
    const QModelIndex source_index = m_sort_filter_model->mapToSource(proxy_index);
    return m_item_model->entryAt(source_index.row());
}

bool FileExplorerPane::hasViewEntry(const int row) const {
    return m_sort_filter_model && m_item_model && row >= 0 && row < m_sort_filter_model->rowCount();
}

FileExplorerViewMode FileExplorerPane::viewMode() const {
    return m_view_mode;
}

int FileExplorerPane::itemSizePx() const {
    return m_item_size_px;
}

bool FileExplorerPane::showHiddenItems() const {
    return m_sort_filter_model && m_sort_filter_model->showHiddenItems();
}

bool FileExplorerPane::showFileExtensions() const {
    return m_item_model && m_item_model->showFileExtensions();
}

void FileExplorerPane::setViewMode(const FileExplorerViewMode mode) {
    m_view_mode = mode;
    if (!m_view_stack) {
        return;
    }

    switch (mode) {
    case FileExplorerViewMode::Details:
        m_view_stack->setCurrentWidget(m_details_view);
        break;
    case FileExplorerViewMode::List:
        m_view_stack->setCurrentWidget(m_list_view);
        break;
    case FileExplorerViewMode::Grid:
    case FileExplorerViewMode::Adaptive:
        m_view_stack->setCurrentWidget(m_grid_view);
        break;
    case FileExplorerViewMode::Cards:
        m_view_stack->setCurrentWidget(m_cards_view);
        break;
    case FileExplorerViewMode::Columns:
        m_view_stack->setCurrentWidget(m_columns_container);
        updateColumnsPreviewRequest();
        break;
    }
}

void FileExplorerPane::setItemSizePx(const int item_size_px) {
    const int clamped =
        std::clamp(item_size_px, kFileExplorerItemSizeMin, kFileExplorerItemSizeMax);
    if (m_item_size_px == clamped) {
        return;
    }
    m_item_size_px = clamped;
    applyItemSize();
}

void FileExplorerPane::setShowHiddenItems(const bool show) {
    if (m_sort_filter_model) {
        m_sort_filter_model->setShowHiddenItems(show);
    }
    if (m_columns_preview_proxy) {
        m_columns_preview_proxy->setShowHiddenItems(show);
    }
}

void FileExplorerPane::setShowFileExtensions(const bool show) {
    if (m_item_model) {
        m_item_model->setShowFileExtensions(show);
    }
    if (m_columns_preview_model) {
        m_columns_preview_model->setShowFileExtensions(show);
    }
}

void FileExplorerPane::setColumnsPreviewEntries(const QString& path,
                                                QVector<FileManagementEntry> entries) {
    m_columns_preview_path = path;
    if (m_columns_preview_model) {
        m_columns_preview_model->setEntries(std::move(entries));
    }
}

void FileExplorerPane::clearColumnsPreview() {
    m_columns_preview_path.clear();
    if (m_columns_preview_model) {
        m_columns_preview_model->clear();
    }
}

void FileExplorerPane::showReadyState() {
    setStateMessage({}, false);
}

void FileExplorerPane::showLoadingState(const QString& message) {
    setStateMessage(message, true);
}

void FileExplorerPane::showEmptyState(const QString& message) {
    setStateMessage(message, true);
}

void FileExplorerPane::showErrorState(const QString& message) {
    setStateMessage(message, true);
}

void FileExplorerPane::setStateMessage(const QString& message, const bool visible) {
    if (!m_state_label) {
        return;
    }
    m_state_label->setText(message);
    m_state_label->setVisible(visible);
}

void FileExplorerPane::configureListView(QListView* view,
                                         const QString& object_name,
                                         const QListView::ViewMode mode) {
    if (!view) {
        return;
    }
    view->setObjectName(object_name);
    view->setModel(m_sort_filter_model);
    view->setSelectionModel(m_selection_model);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->setUniformItemSizes(true);
    view->setResizeMode(QListView::Adjust);
    view->setMovement(QListView::Static);
    view->setViewMode(mode);
    view->setTextElideMode(Qt::ElideMiddle);
    view->setAccessibleName(tr("File explorer %1 view").arg(object_name));
}

void FileExplorerPane::configureColumnsPreviewView(QListView* view) {
    if (!view) {
        return;
    }
    view->setObjectName(QStringLiteral("fileExplorerColumnsPreviewView"));
    view->setModel(m_columns_preview_proxy);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setContextMenuPolicy(Qt::NoContextMenu);
    view->setUniformItemSizes(true);
    view->setResizeMode(QListView::Adjust);
    view->setMovement(QListView::Static);
    view->setViewMode(QListView::ListMode);
    view->setTextElideMode(Qt::ElideMiddle);
    view->setAccessibleName(tr("File explorer columns child preview"));
}

void FileExplorerPane::applyItemSize() {
    const QSize iconSize(m_item_size_px, m_item_size_px);
    if (m_list_view) {
        m_list_view->setIconSize(QSize(kFileExplorerListIconSize, kFileExplorerListIconSize));
        m_list_view->setGridSize(QSize(kFileExplorerListCellW, kFileExplorerListCellH));
    }
    if (m_grid_view) {
        m_grid_view->setIconSize(iconSize);
        m_grid_view->setGridSize(QSize(
            std::max(kFileExplorerGridMinCellW, m_item_size_px + kFileExplorerGridCellExtraW),
            std::max(kFileExplorerGridMinCellH, m_item_size_px + kFileExplorerGridCellExtraH)));
    }
    if (m_cards_view) {
        const int cardsIconSize = std::max(kFileExplorerCardsMinIconSize, m_item_size_px / 2);
        m_cards_view->setIconSize(QSize(cardsIconSize, cardsIconSize));
        m_cards_view->setGridSize(QSize(kFileExplorerCardsCellW,
                                        std::max(kFileExplorerCardsMinCellH,
                                                 m_item_size_px + kFileExplorerCardsCellExtraH)));
    }
    if (m_columns_view) {
        m_columns_view->setIconSize(QSize(kFileExplorerListIconSize, kFileExplorerListIconSize));
        m_columns_view->setGridSize(QSize(std::max(kFileExplorerColumnsMinCellW,
                                                   m_item_size_px * kFileExplorerColumnsCellScale),
                                          kFileExplorerColumnsCellH));
    }
    if (m_columns_preview_view) {
        m_columns_preview_view->setIconSize(
            QSize(kFileExplorerListIconSize, kFileExplorerListIconSize));
        m_columns_preview_view->setGridSize(QSize(
            std::max(kFileExplorerColumnsMinCellW, m_item_size_px * kFileExplorerColumnsCellScale),
            kFileExplorerColumnsCellH));
    }
}

void FileExplorerPane::updateColumnsPreviewRequest() {
    if (m_view_mode != FileExplorerViewMode::Columns || !m_selection_model) {
        return;
    }
    const QModelIndexList rows = m_selection_model->selectedRows();
    if (rows.size() != 1) {
        clearColumnsPreview();
        return;
    }
    const FileManagementEntry entry = entryAtViewRow(rows.first().row());
    if (!entry.directory || entry.path.trimmed().isEmpty()) {
        clearColumnsPreview();
        return;
    }
    if (entry.path == m_columns_preview_path) {
        return;
    }
    clearColumnsPreview();
    Q_EMIT columnsDirectoryPreviewRequested(entry.path);
}

FileManagementEntry FileExplorerPane::columnsPreviewEntryAtRow(const int row) const {
    if (!m_columns_preview_proxy || !m_columns_preview_model || row < 0 ||
        row >= m_columns_preview_proxy->rowCount()) {
        return {};
    }
    const QModelIndex proxy_index =
        m_columns_preview_proxy->index(row, FileExplorerItemModel::NameColumn);
    const QModelIndex source_index = m_columns_preview_proxy->mapToSource(proxy_index);
    return m_columns_preview_model->entryAt(source_index.row());
}

}  // namespace sak
