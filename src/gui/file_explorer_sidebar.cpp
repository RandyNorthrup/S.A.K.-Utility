// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_sidebar.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QHBoxLayout>
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
    layout->addWidget(m_target_list, 1);

    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("fileExplorerSidebarFooter"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    footer_layout->setSpacing(ui::kSpacingTight);

    m_scan_disks_button = new QPushButton(tr("Scan Disks"), footer);
    m_scan_disks_button->setObjectName(QStringLiteral("fileExplorerScanDisksButton"));
    m_scan_disks_button->setAccessibleName(tr("Scan disk and partition targets"));
    m_scan_disks_button->setToolTip(tr("Discover disk and partition targets"));
    m_scan_disks_button->setIcon(
        FileExplorerIconRegistry::iconForKey(QStringLiteral("scan-disks")));
    footer_layout->addWidget(m_scan_disks_button);

    m_add_manual_button = new QPushButton(tr("Add Raw/Image"), footer);
    m_add_manual_button->setObjectName(QStringLiteral("fileExplorerAddRawImageButton"));
    m_add_manual_button->setAccessibleName(tr("Add manual raw or image target"));
    m_add_manual_button->setToolTip(tr("Add a raw device or image file as a target"));
    m_add_manual_button->setIcon(
        FileExplorerIconRegistry::iconForKey(QStringLiteral("image-file")));
    footer_layout->addWidget(m_add_manual_button);
    footer_layout->addStretch(1);

    // Files MainPage SettingsButton (SidebarView.Footer): the gear opens the
    // explorer settings.
    m_settings_button = new QPushButton(footer);
    m_settings_button->setObjectName(QStringLiteral("fileExplorerSettingsButton"));
    m_settings_button->setAccessibleName(tr("Open File Explorer settings"));
    m_settings_button->setToolTip(tr("Explorer settings"));
    m_settings_button->setIcon(FileExplorerIconRegistry::iconForKey(QStringLiteral("more")));
    footer_layout->addWidget(m_settings_button);

    layout->addWidget(footer, 0);
}

QListWidget* FileExplorerSidebar::targetList() const {
    return m_target_list;
}

QPushButton* FileExplorerSidebar::scanDisksButton() const {
    return m_scan_disks_button;
}

QPushButton* FileExplorerSidebar::addManualButton() const {
    return m_add_manual_button;
}

QPushButton* FileExplorerSidebar::settingsButton() const {
    return m_settings_button;
}

}  // namespace sak
