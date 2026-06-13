// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_details_view.h
/// @brief Details table view for File Explorer rows.

#pragma once

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

protected:
    void showEvent(QShowEvent* event) override;

private:
    bool m_save_connections_installed{false};
};

}  // namespace sak
