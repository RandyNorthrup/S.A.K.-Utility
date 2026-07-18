// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_sidebar.h
/// @brief File Explorer target navigation sidebar widget.

#pragma once

#include <QListWidget>
#include <QPushButton>
#include <QWidget>

namespace sak {

/// Left navigation: target list plus a footer with the target-discovery
/// actions (scan disks, add raw/image) and the settings gear, Files-style.
class FileExplorerSidebar : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerSidebar(QWidget* parent = nullptr);

    [[nodiscard]] QListWidget* targetList() const;
    [[nodiscard]] QPushButton* scanDisksButton() const;
    [[nodiscard]] QPushButton* addManualButton() const;
    [[nodiscard]] QPushButton* settingsButton() const;

private:
    QListWidget* m_target_list{nullptr};
    QPushButton* m_scan_disks_button{nullptr};
    QPushButton* m_add_manual_button{nullptr};
    QPushButton* m_settings_button{nullptr};
};

}  // namespace sak
