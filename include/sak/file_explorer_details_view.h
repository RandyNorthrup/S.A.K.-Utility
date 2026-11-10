// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_details_view.h
/// @brief Details table view for File Explorer rows.

#pragma once

#include <QSet>
#include <QTableView>

namespace sak {

class FileExplorerDetailsView : public QTableView {
    Q_OBJECT

public:
    explicit FileExplorerDetailsView(QWidget* parent = nullptr);

    void setModel(QAbstractItemModel* model) override;
    void configureExplorerColumns();
    void saveColumnState() const;
    void restoreColumnState();
    /// Files AutoFitColumns: size every visible column to its content.
    void autoFitAllColumns();

    /// Name takes the room the other columns leave, never less than a readable minimum; when the
    /// view is too narrow for that, the least needed columns are dropped until it is wide again.
    void fitNameColumn();
    /// The columns dropped for room, shown again as soon as the view can hold them.
    [[nodiscard]] QSet<int> columnsDroppedForRoom() const { return m_dropped_for_room; }

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    /// Header context menu with one checkable action per optional column.
    void showColumnMenu(const QPoint& position);

    /// Shows the columns dropped for room, then drops again, least needed first, what does not fit
    /// beside a readable Name. Returns the width the kept columns other than Name take.
    int dropColumnsForRoom(int room);

    bool m_save_connections_installed{false};
    bool m_fitting_name{false};
    QSet<int> m_dropped_for_room;
};

}  // namespace sak
