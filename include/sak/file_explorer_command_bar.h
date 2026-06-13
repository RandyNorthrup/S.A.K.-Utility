// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_command_bar.h
/// @brief File Explorer command bar widget.

#pragma once

#include <QPushButton>
#include <QToolButton>
#include <QWidget>

namespace sak {

class FileExplorerCommandBar : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerCommandBar(QWidget* parent = nullptr);

    [[nodiscard]] QPushButton* sidebarToggleButton() const;
    [[nodiscard]] QPushButton* refreshButton() const;
    [[nodiscard]] QPushButton* scanDisksButton() const;
    [[nodiscard]] QPushButton* addManualButton() const;
    [[nodiscard]] QPushButton* newFolderButton() const;
    [[nodiscard]] QPushButton* writeFileButton() const;
    [[nodiscard]] QPushButton* renameButton() const;
    [[nodiscard]] QPushButton* deleteButton() const;
    [[nodiscard]] QToolButton* viewButton() const;
    [[nodiscard]] QPushButton* detailsToggleButton() const;

private:
    QPushButton* m_sidebar_toggle_button{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QPushButton* m_scan_disks_button{nullptr};
    QPushButton* m_add_manual_button{nullptr};
    QPushButton* m_new_folder_button{nullptr};
    QPushButton* m_write_file_button{nullptr};
    QPushButton* m_rename_button{nullptr};
    QPushButton* m_delete_button{nullptr};
    QToolButton* m_view_button{nullptr};
    QPushButton* m_details_toggle_button{nullptr};
};

}  // namespace sak
