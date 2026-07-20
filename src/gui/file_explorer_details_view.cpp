// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_details_view.h"

#include "sak/file_explorer_item_model.h"
#include "sak/widget_helpers.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QMenu>
#include <QSettings>

#include <array>

namespace {

constexpr const char* kExplorerDetailsViewGroup = "FileExplorerDetailsView";
constexpr const char* kHeaderStateKey = "HeaderState";
constexpr const char* kColumnWidthsKey = "ColumnWidths";

// Files-style default column set: Name/Type/Size/Modified/Tags visible, the
// power columns opt-in through the header context menu.
constexpr std::array kDefaultHiddenColumns = {
    static_cast<int>(sak::FileExplorerItemModel::CreatedColumn),
    static_cast<int>(sak::FileExplorerItemModel::IdentifierColumn),
    static_cast<int>(sak::FileExplorerItemModel::AttributesColumn),
    static_cast<int>(sak::FileExplorerItemModel::PathColumn),
};

constexpr std::array<std::pair<int, int>, 4> kDefaultColumnWidths = {{
    {static_cast<int>(sak::FileExplorerItemModel::NameColumn), 280},
    {static_cast<int>(sak::FileExplorerItemModel::TypeColumn), 110},
    {static_cast<int>(sak::FileExplorerItemModel::SizeColumn), 90},
    {static_cast<int>(sak::FileExplorerItemModel::ModifiedColumn), 140},
}};

}  // namespace

namespace sak {

FileExplorerDetailsView::FileExplorerDetailsView(QWidget* parent) : QTableView(parent) {
    setObjectName(QStringLiteral("fileExplorerTable"));
    configureStandardTable(this, QAbstractItemView::ExtendedSelection);
    setSortingEnabled(true);
    setAccessibleName(tr("File explorer directory listing"));
    setContextMenuPolicy(Qt::CustomContextMenu);
    setShowGrid(false);
    horizontalHeader()->setSectionsMovable(true);
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(horizontalHeader(),
            &QWidget::customContextMenuRequested,
            this,
            &FileExplorerDetailsView::showColumnMenu);
}

void FileExplorerDetailsView::showColumnMenu(const QPoint& position) {
    if (!model()) {
        return;
    }
    QMenu menu(this);
    for (int column = 0; column < model()->columnCount(); ++column) {
        if (column == FileExplorerItemModel::NameColumn) {
            continue;  // Name always stays visible.
        }
        const QString title = model()->headerData(column, Qt::Horizontal).toString();
        QAction* action = menu.addAction(title);
        action->setCheckable(true);
        action->setChecked(!isColumnHidden(column));
        connect(action, &QAction::toggled, this, [this, column](const bool visible) {
            setColumnHidden(column, !visible);
            saveColumnState();
        });
    }
    // Files DetailsLayoutPage header menu ends with the auto-fit command
    // (SizeAllColumnsToFit -> AutoFitColumnsAction).
    menu.addSeparator();
    QAction* fit = menu.addAction(tr("Size all columns to fit"));
    fit->setObjectName(QStringLiteral("fileExplorerSizeColumnsToFitAction"));
    connect(fit, &QAction::triggered, this, &FileExplorerDetailsView::autoFitAllColumns);
    menu.exec(horizontalHeader()->mapToGlobal(position));
}

void FileExplorerDetailsView::autoFitAllColumns() {
    if (!model()) {
        return;
    }
    // Files ResizeColumnToFit per column; the name column gets extra
    // headroom for the sort indicator and padding (+20/+36 in Files).
    constexpr int kNameHeadroomPx = 20;
    for (int column = 0; column < model()->columnCount(); ++column) {
        if (isColumnHidden(column) ||
            horizontalHeader()->sectionResizeMode(column) == QHeaderView::Stretch) {
            continue;
        }
        resizeColumnToContents(column);
        if (column == FileExplorerItemModel::NameColumn) {
            setColumnWidth(column, columnWidth(column) + kNameHeadroomPx);
        }
    }
    saveColumnState();
}

void FileExplorerDetailsView::setModel(QAbstractItemModel* model) {
    QTableView::setModel(model);
    configureExplorerColumns();
    restoreColumnState();
}

void FileExplorerDetailsView::showEvent(QShowEvent* event) {
    QTableView::showEvent(event);
    restoreColumnState();
    if (!m_save_connections_installed) {
        connect(horizontalHeader(), &QHeaderView::sectionResized, this, [this]() {
            saveColumnState();
        });
        connect(horizontalHeader(), &QHeaderView::sectionMoved, this, [this]() {
            saveColumnState();
        });
        m_save_connections_installed = true;
    }
}

void FileExplorerDetailsView::configureExplorerColumns() {
    if (!model()) {
        return;
    }
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::NameColumn,
                                             QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::TypeColumn,
                                             QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::SizeColumn,
                                             QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::ModifiedColumn,
                                             QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::CreatedColumn,
                                             QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::IdentifierColumn,
                                             QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::AttributesColumn,
                                             QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(FileExplorerItemModel::PathColumn,
                                             QHeaderView::Stretch);
}

void FileExplorerDetailsView::saveColumnState() const {
    if (!model()) {
        return;
    }
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerDetailsViewGroup));
    settings.setValue(QString::fromLatin1(kHeaderStateKey), horizontalHeader()->saveState());
    QVariantList widths;
    for (int column = 0; column < model()->columnCount(); ++column) {
        widths.append(columnWidth(column));
    }
    settings.setValue(QString::fromLatin1(kColumnWidthsKey), widths);
    settings.endGroup();
    settings.sync();
}

void FileExplorerDetailsView::restoreColumnState() {
    if (!model()) {
        return;
    }
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerDetailsViewGroup));
    const QByteArray state = settings.value(QString::fromLatin1(kHeaderStateKey)).toByteArray();
    const QVariantList widths = settings.value(QString::fromLatin1(kColumnWidthsKey)).toList();
    settings.endGroup();
    if (state.isEmpty()) {
        // First run (no saved layout): apply the Files-style default set.
        for (const int column : kDefaultHiddenColumns) {
            setColumnHidden(column, true);
        }
        for (const auto& [column, width] : kDefaultColumnWidths) {
            setColumnWidth(column, width);
        }
    } else {
        horizontalHeader()->restoreState(state);
    }
    for (int column = 0; column < widths.size() && column < model()->columnCount(); ++column) {
        const int width = widths.at(column).toInt();
        if (width > 0) {
            setColumnWidth(column, width);
        }
    }
}

}  // namespace sak
