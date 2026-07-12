// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_omnibar.h
/// @brief File Explorer navigation and address row (Files-style toolbar).

#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

class QHBoxLayout;

namespace sak {

/// Top navigation row: sidebar toggle, back/forward/up/refresh, address path,
/// search box, and the command palette trigger.
class FileExplorerOmnibar : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerOmnibar(QWidget* parent = nullptr);

    [[nodiscard]] QPushButton* sidebarToggleButton() const;
    [[nodiscard]] QPushButton* backButton() const;
    [[nodiscard]] QPushButton* forwardButton() const;
    [[nodiscard]] QPushButton* upButton() const;
    [[nodiscard]] QPushButton* refreshButton() const;
    [[nodiscard]] QLineEdit* pathEdit() const;
    [[nodiscard]] QLineEdit* searchBox() const;
    [[nodiscard]] QPushButton* searchButton() const;
    [[nodiscard]] QPushButton* commandButton() const;

private:
    void createNavigationButtons(QHBoxLayout* row);
    void createAddressAndSearch(QHBoxLayout* row);

    QPushButton* m_sidebar_toggle_button{nullptr};
    QPushButton* m_back_button{nullptr};
    QPushButton* m_forward_button{nullptr};
    QPushButton* m_up_button{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QLineEdit* m_path_edit{nullptr};
    QLineEdit* m_search_box{nullptr};
    QPushButton* m_search_button{nullptr};
    QPushButton* m_command_button{nullptr};
};

}  // namespace sak
