// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_sidebar.h"

#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QVBoxLayout>

namespace sak {

FileExplorerSidebar::FileExplorerSidebar(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(kFileExplorerSidebarMinW);
    setMaximumWidth(kFileExplorerSidebarMaxW);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    layout->setSpacing(ui::kSpacingNone);

    m_target_list = new QListWidget(this);
    m_target_list->setObjectName(QStringLiteral("fileExplorerTargetList"));
    m_target_list->setAccessibleName(tr("File explorer target navigation"));
    m_target_list->setToolTip(
        tr("Mounted volumes, scanned partitions, and manual raw/image targets"));
    m_target_list->setUniformItemSizes(true);
    m_target_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_target_list);
}

QListWidget* FileExplorerSidebar::targetList() const {
    return m_target_list;
}

}  // namespace sak
