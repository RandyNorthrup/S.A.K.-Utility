// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_sidebar.h
/// @brief File Explorer target navigation sidebar widget.

#pragma once

#include <QListWidget>
#include <QWidget>

namespace sak {

class FileExplorerSidebar : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerSidebar(QWidget* parent = nullptr);

    [[nodiscard]] QListWidget* targetList() const;

private:
    QListWidget* m_target_list{nullptr};
};

}  // namespace sak
