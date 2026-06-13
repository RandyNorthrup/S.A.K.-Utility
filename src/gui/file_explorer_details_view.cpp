// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_details_view.h"

#include "sak/file_explorer_item_model.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QSettings>

namespace {

constexpr const char* kExplorerDetailsViewGroup = "FileExplorerDetailsView";
constexpr const char* kHeaderStateKey = "HeaderState";
constexpr const char* kColumnWidthsKey = "ColumnWidths";

}  // namespace

namespace sak {

FileExplorerDetailsView::FileExplorerDetailsView(QWidget* parent) : QTableView(parent) {
    setObjectName(QStringLiteral("fileExplorerTable"));
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAlternatingRowColors(true);
    setSortingEnabled(true);
    setAccessibleName(tr("File explorer directory listing"));
    setContextMenuPolicy(Qt::CustomContextMenu);
    horizontalHeader()->setSectionsMovable(true);
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
    const QByteArray state =
        settings.value(QString::fromLatin1(kHeaderStateKey)).toByteArray();
    const QVariantList widths =
        settings.value(QString::fromLatin1(kColumnWidthsKey)).toList();
    settings.endGroup();
    if (!state.isEmpty()) {
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
