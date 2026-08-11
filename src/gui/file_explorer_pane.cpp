// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_pane.h"

#include "sak/file_explorer_layout_metrics.h"
#include "sak/file_explorer_name_delegate.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QVBoxLayout>

#include <utility>

namespace sak {

FileExplorerPane::FileExplorerPane(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    layout->setSpacing(ui::kSpacingSmall);

    buildStateLabel(layout);
    buildModels();
    buildItemViews();
    applyLayoutSizes();
    layout->addWidget(m_view_stack, 1);
    buildStatusLabel(layout);
    connectSignals();
}

void FileExplorerPane::buildStateLabel(QVBoxLayout* layout) {
    m_state_label = new QLabel(this);
    m_state_label->setObjectName(QStringLiteral("fileExplorerStateLabel"));
    m_state_label->setAccessibleName(tr("Explorer loading, empty, and error state"));
    m_state_label->setWordWrap(true);
    m_state_label->setVisible(false);
    layout->addWidget(m_state_label);
}

void FileExplorerPane::buildModels() {
    m_item_model = new FileExplorerItemModel(this);
    m_sort_filter_model = new FileExplorerSortFilterModel(this);
    m_sort_filter_model->setSourceModel(m_item_model);
    // Files SortOption default: every pane starts sorted by Name ascending
    // instead of raw enumeration order.
    m_sort_filter_model->sort(FileExplorerItemModel::NameColumn, Qt::AscendingOrder);
    // Group-header proxy on top of the sorted rows (Files grouped
    // collections); transparent identity mapping while grouping is off.
    m_group_proxy = new FileExplorerGroupProxyModel(this);
    m_group_proxy->setSourceModel(m_sort_filter_model);
    m_columns_preview_model = new FileExplorerItemModel(this);
    m_columns_preview_proxy = new FileExplorerSortFilterModel(this);
    m_columns_preview_proxy->setSourceModel(m_columns_preview_model);
    m_columns_preview_proxy->sort(FileExplorerItemModel::NameColumn, Qt::AscendingOrder);
    m_selection_model = new QItemSelectionModel(m_group_proxy, this);
    connect(m_group_proxy, &QAbstractItemModel::modelReset, this, [this]() {
        applyGroupHeaderSpans();
    });
    connect(m_group_proxy, &QAbstractItemModel::layoutChanged, this, [this]() {
        applyGroupHeaderSpans();
    });
}

void FileExplorerPane::buildItemViews() {
    m_view_stack = new QStackedWidget(this);
    m_details_view = new FileExplorerDetailsView(this);
    m_details_view->setModel(m_group_proxy);
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
    auto* columns_layout = new QHBoxLayout(m_columns_container);
    columns_layout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    columns_layout->setSpacing(ui::kSpacingSmall);
    m_columns_view = new QListView(m_columns_container);
    configureListView(m_columns_view,
                      QStringLiteral("fileExplorerColumnsView"),
                      QListView::ListMode);
    m_columns_preview_view = new QListView(m_columns_container);
    configureColumnsPreviewView(m_columns_preview_view);
    columns_layout->addWidget(m_columns_view, 1);
    columns_layout->addWidget(m_columns_preview_view, 1);
    m_view_stack->addWidget(m_columns_container);

    // Inline rename (Files BaseLayoutPage): a name-column delegate per view
    // (sharing one instance would cross-wire commitData between views). Cards
    // paints the Files card cell; the columns blades add the folder chevron
    // (the preview blade is read-only but still shows it).
    m_details_view->setItemDelegateForColumn(FileExplorerItemModel::NameColumn,
                                             new FileExplorerNameDelegate(m_details_view));
    for (QListView* view : {m_list_view, m_grid_view}) {
        view->setItemDelegate(new FileExplorerNameDelegate(view));
    }
    m_cards_view->setItemDelegate(new FileExplorerCardDelegate(m_cards_view));
    m_columns_view->setItemDelegate(
        new FileExplorerNameDelegate(m_columns_view, /*folder_chevron=*/true));
    m_columns_preview_view->setItemDelegate(
        new FileExplorerNameDelegate(m_columns_preview_view, /*folder_chevron=*/true));

    // Drag and drop (Files BaseItemsLayoutPage): rows drag out through the
    // model's payload provider; drops are filtered at the viewport by the
    // panel, which routes them through the transfer kernel. The view supplies
    // only Qt's drag plumbing and the drop indicator.
    for (QAbstractItemView* view : itemViews()) {
        view->setDragEnabled(true);
        view->setAcceptDrops(true);
        view->viewport()->setAcceptDrops(true);
        view->setDropIndicatorShown(true);
        view->setDragDropMode(QAbstractItemView::DragDrop);
        view->setDefaultDropAction(Qt::MoveAction);
    }
}

void FileExplorerPane::buildStatusLabel(QVBoxLayout* layout) {
    m_status_label = new QLabel(tr("No target selected"), this);
    m_status_label->setObjectName(QStringLiteral("fileExplorerStatusLabel"));
    m_status_label->setAccessibleName(tr("Explorer status"));
    // Status text carries command blockers and target/selection descriptions, which embed paths
    // and names read off the mounted medium; show them verbatim, never as markup.
    m_status_label->setTextFormat(Qt::PlainText);
    m_status_label->setWordWrap(true);
    layout->addWidget(m_status_label);
}

void FileExplorerPane::connectSignals() {
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

FileExplorerItemModel* FileExplorerPane::columnsPreviewModel() const {
    return m_columns_preview_model;
}

FileExplorerSortFilterModel* FileExplorerPane::sortFilterModel() const {
    return m_sort_filter_model;
}

FileExplorerGroupProxyModel* FileExplorerPane::groupProxyModel() const {
    return m_group_proxy;
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
    if (m_view_stack == nullptr) {
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
    const QModelIndex group_index = m_group_proxy->index(row, FileExplorerItemModel::NameColumn);
    const QModelIndex proxy_index = m_group_proxy->mapToSource(group_index);
    const QModelIndex source_index = m_sort_filter_model->mapToSource(proxy_index);
    return m_item_model->entryAt(source_index.row());
}

bool FileExplorerPane::hasViewEntry(const int row) const {
    return (m_group_proxy != nullptr) && (m_sort_filter_model != nullptr) &&
           (m_item_model != nullptr) && row >= 0 && row < m_group_proxy->rowCount() &&
           !m_group_proxy->isHeaderRow(row);
}

FileExplorerViewMode FileExplorerPane::viewMode() const {
    return m_view_mode;
}

FileExplorerLayoutSizes FileExplorerPane::layoutSizes() const {
    return m_layout_sizes;
}

bool FileExplorerPane::showHiddenItems() const {
    return (m_sort_filter_model != nullptr) && m_sort_filter_model->showHiddenItems();
}

bool FileExplorerPane::showFileExtensions() const {
    return (m_item_model != nullptr) && m_item_model->showFileExtensions();
}

void FileExplorerPane::setViewMode(const FileExplorerViewMode mode) {
    m_view_mode = mode;
    if (m_view_stack == nullptr) {
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

void FileExplorerPane::setGrouping(const FileExplorerGroupOption option,
                                   const FileExplorerGroupDateUnit date_unit,
                                   const Qt::SortOrder direction) {
    if (m_group_proxy != nullptr) {
        m_group_proxy->setGrouping(option, date_unit, direction);
    }
}

void FileExplorerPane::setLayoutSizes(const FileExplorerLayoutSizes& sizes) {
    FileExplorerLayoutSizes clamped = sizes;
    clampFileExplorerLayoutSizes(clamped);
    if (m_layout_sizes == clamped) {
        return;
    }
    m_layout_sizes = clamped;
    applyLayoutSizes();
}

void FileExplorerPane::setShowHiddenItems(const bool show) {
    if (m_sort_filter_model != nullptr) {
        m_sort_filter_model->setShowHiddenItems(show);
    }
    if (m_columns_preview_proxy != nullptr) {
        m_columns_preview_proxy->setShowHiddenItems(show);
    }
}

void FileExplorerPane::setShowFileExtensions(const bool show) {
    if (m_item_model != nullptr) {
        m_item_model->setShowFileExtensions(show);
    }
    if (m_columns_preview_model != nullptr) {
        m_columns_preview_model->setShowFileExtensions(show);
    }
}

void FileExplorerPane::setColumnsPreviewEntries(const QString& path,
                                                QVector<FileManagementEntry> entries) {
    m_columns_preview_path = path;
    if (m_columns_preview_model != nullptr) {
        m_columns_preview_model->setEntries(std::move(entries));
    }
}

void FileExplorerPane::clearColumnsPreview() {
    m_columns_preview_path.clear();
    if (m_columns_preview_model != nullptr) {
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
    if (m_state_label == nullptr) {
        return;
    }
    m_state_label->setText(message);
    m_state_label->setVisible(visible);
}

void FileExplorerPane::configureListView(QListView* view,
                                         const QString& object_name,
                                         const QListView::ViewMode mode) {
    if (view == nullptr) {
        return;
    }
    view->setObjectName(object_name);
    view->setModel(m_group_proxy);
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
    if (view == nullptr) {
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

void FileExplorerPane::applyLayoutSizes() {
    // Files LayoutSizeKindHelper: each layout's size kind drives its row
    // height (or grid cell) and icon edge; kinds are independent per layout.
    if (m_details_view != nullptr) {
        const int icon = fileExplorerIconSize(FileExplorerViewMode::Details,
                                              m_layout_sizes.details);
        m_details_view->setIconSize(QSize(icon, icon));
        m_details_view->verticalHeader()->setDefaultSectionSize(
            fileExplorerRowHeight(FileExplorerViewMode::Details, m_layout_sizes.details));
    }
    if (m_list_view != nullptr) {
        const int icon = fileExplorerIconSize(FileExplorerViewMode::List, m_layout_sizes.list);
        m_list_view->setIconSize(QSize(icon, icon));
        m_list_view->setGridSize(
            QSize(kFileExplorerListCellW,
                  fileExplorerRowHeight(FileExplorerViewMode::List, m_layout_sizes.list)));
    }
    if (m_grid_view != nullptr) {
        const int icon = fileExplorerIconSize(FileExplorerViewMode::Grid, m_layout_sizes.grid);
        m_grid_view->setIconSize(QSize(icon, icon));
        m_grid_view->setGridSize(
            QSize(fileExplorerGridItemWidth(m_layout_sizes.grid),
                  fileExplorerRowHeight(FileExplorerViewMode::Grid, m_layout_sizes.grid)));
    }
    if (m_cards_view != nullptr) {
        const int icon = fileExplorerIconSize(FileExplorerViewMode::Cards, m_layout_sizes.cards);
        m_cards_view->setIconSize(QSize(icon, icon));
        m_cards_view->setGridSize(
            QSize(kFileExplorerCardsCellW,
                  fileExplorerRowHeight(FileExplorerViewMode::Cards, m_layout_sizes.cards)));
    }
    const int columns_icon = fileExplorerIconSize(FileExplorerViewMode::Columns,
                                                  m_layout_sizes.columns);
    const QSize columns_cell(kFileExplorerColumnsMinCellW,
                             fileExplorerRowHeight(FileExplorerViewMode::Columns,
                                                   m_layout_sizes.columns));
    if (m_columns_view != nullptr) {
        m_columns_view->setIconSize(QSize(columns_icon, columns_icon));
        m_columns_view->setGridSize(columns_cell);
    }
    if (m_columns_preview_view != nullptr) {
        m_columns_preview_view->setIconSize(QSize(columns_icon, columns_icon));
        m_columns_preview_view->setGridSize(columns_cell);
    }
}

void FileExplorerPane::applyGroupHeaderSpans() {
    // Files renders group headers full-width above each section; in the
    // details table the injected header rows span every column.
    if ((m_details_view == nullptr) || (m_group_proxy == nullptr)) {
        return;
    }
    m_details_view->clearSpans();
    const int columns = m_group_proxy->columnCount();
    if (columns <= 1) {
        return;
    }
    const QVector<int> header_rows = m_group_proxy->headerRows();
    for (const int row : header_rows) {
        m_details_view->setSpan(row, 0, 1, columns);
    }
}

void FileExplorerPane::updateColumnsPreviewRequest() {
    if (m_view_mode != FileExplorerViewMode::Columns || (m_selection_model == nullptr)) {
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
    if ((m_columns_preview_proxy == nullptr) || (m_columns_preview_model == nullptr) || row < 0 ||
        row >= m_columns_preview_proxy->rowCount()) {
        return {};
    }
    const QModelIndex proxy_index =
        m_columns_preview_proxy->index(row, FileExplorerItemModel::NameColumn);
    const QModelIndex source_index = m_columns_preview_proxy->mapToSource(proxy_index);
    return m_columns_preview_model->entryAt(source_index.row());
}

}  // namespace sak
