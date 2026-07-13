// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_command_bar.h
/// @brief File Explorer command toolbar (clone of Files' Toolbar.xaml row).

#pragma once

#include <QPushButton>
#include <QToolButton>
#include <QWidget>

class QHBoxLayout;
class QMenu;

namespace sak {

/// Command toolbar inside the content column, mirroring Files
/// (src/Files.App/UserControls/Toolbar.xaml + ToolbarSections.cs):
/// left = New group + icon-only item commands; right = selection options,
/// sort, layout, and info-pane toggle. Flyout menus are owned here but
/// populated by the panel on aboutToShow so entries always reflect the
/// current command context.
class FileExplorerCommandBar : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerCommandBar(QWidget* parent = nullptr);

    [[nodiscard]] QToolButton* newButton() const;
    [[nodiscard]] QMenu* newMenu() const;
    [[nodiscard]] QPushButton* cutButton() const;
    [[nodiscard]] QPushButton* copyButton() const;
    [[nodiscard]] QPushButton* pasteButton() const;
    [[nodiscard]] QPushButton* renameButton() const;
    [[nodiscard]] QPushButton* deleteButton() const;
    [[nodiscard]] QPushButton* propertiesButton() const;
    [[nodiscard]] QToolButton* selectionButton() const;
    [[nodiscard]] QMenu* selectionMenu() const;
    [[nodiscard]] QToolButton* sortButton() const;
    [[nodiscard]] QMenu* sortMenu() const;
    [[nodiscard]] QToolButton* viewButton() const;
    [[nodiscard]] QPushButton* detailsToggleButton() const;

private:
    void createLeftCommands(QHBoxLayout* commandRow);
    void createRightCommands(QHBoxLayout* commandRow);

    QToolButton* m_new_button{nullptr};
    QMenu* m_new_menu{nullptr};
    QPushButton* m_cut_button{nullptr};
    QPushButton* m_copy_button{nullptr};
    QPushButton* m_paste_button{nullptr};
    QPushButton* m_rename_button{nullptr};
    QPushButton* m_delete_button{nullptr};
    QPushButton* m_properties_button{nullptr};
    QToolButton* m_selection_button{nullptr};
    QMenu* m_selection_menu{nullptr};
    QToolButton* m_sort_button{nullptr};
    QMenu* m_sort_menu{nullptr};
    QToolButton* m_view_button{nullptr};
    QPushButton* m_details_toggle_button{nullptr};
};

}  // namespace sak
