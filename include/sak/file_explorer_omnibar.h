// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_omnibar.h
/// @brief File Explorer path, search, and command row widget.

#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

namespace sak {

class FileExplorerOmnibar : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerOmnibar(QWidget* parent = nullptr);

    [[nodiscard]] QPushButton* backButton() const;
    [[nodiscard]] QPushButton* forwardButton() const;
    [[nodiscard]] QPushButton* upButton() const;
    [[nodiscard]] QLineEdit* pathEdit() const;
    [[nodiscard]] QPushButton* searchButton() const;
    [[nodiscard]] QPushButton* commandButton() const;
    [[nodiscard]] QPushButton* openButton() const;
    [[nodiscard]] QPushButton* copyPathButton() const;

private:
    QPushButton* m_back_button{nullptr};
    QPushButton* m_forward_button{nullptr};
    QPushButton* m_up_button{nullptr};
    QLineEdit* m_path_edit{nullptr};
    QPushButton* m_search_button{nullptr};
    QPushButton* m_command_button{nullptr};
    QPushButton* m_open_button{nullptr};
    QPushButton* m_copy_path_button{nullptr};
};

}  // namespace sak
