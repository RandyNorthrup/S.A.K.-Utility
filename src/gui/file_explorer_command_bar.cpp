// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_command_bar.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QHBoxLayout>
#include <QStyle>
#include <QToolButton>

namespace sak {

FileExplorerCommandBar::FileExplorerCommandBar(QWidget* parent) : QWidget(parent) {
    auto* commandRow = new QHBoxLayout(this);
    commandRow->setContentsMargins(0, 0, 0, 0);
    commandRow->setSpacing(ui::kSpacingSmall);

    m_sidebar_toggle_button = new QPushButton(this);
    m_sidebar_toggle_button->setObjectName(QStringLiteral("fileExplorerSidebarToggleButton"));
    m_sidebar_toggle_button->setIcon(FileExplorerIconRegistry::iconForKey(QStringLiteral("panel-left")));
    m_sidebar_toggle_button->setAccessibleName(tr("Toggle File Explorer sidebar"));
    m_sidebar_toggle_button->setToolTip(tr("Show or hide target navigation"));
    m_sidebar_toggle_button->setStyleSheet(ui::kSecondaryButtonStyle);
    commandRow->addWidget(m_sidebar_toggle_button);

    m_refresh_button = new QPushButton(tr("Refresh"), this);
    m_refresh_button->setObjectName(QStringLiteral("fileExplorerRefreshButton"));
    m_refresh_button->setAccessibleName(tr("Refresh mounted file targets"));
    m_refresh_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::Refresh));
    m_refresh_button->setStyleSheet(ui::kSecondaryButtonStyle);
    commandRow->addWidget(m_refresh_button);

    m_scan_disks_button = new QPushButton(tr("Scan Disks"), this);
    m_scan_disks_button->setObjectName(QStringLiteral("fileExplorerScanDisksButton"));
    m_scan_disks_button->setAccessibleName(tr("Scan disk and partition targets"));
    m_scan_disks_button->setIcon(style()->standardIcon(QStyle::SP_DriveHDIcon));
    m_scan_disks_button->setStyleSheet(ui::kPrimaryButtonStyle);
    commandRow->addWidget(m_scan_disks_button);

    m_add_manual_button = new QPushButton(tr("Add Raw/Image"), this);
    m_add_manual_button->setObjectName(QStringLiteral("fileExplorerAddRawImageButton"));
    m_add_manual_button->setAccessibleName(tr("Add manual raw or image target"));
    m_add_manual_button->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    m_add_manual_button->setStyleSheet(ui::kPrimaryButtonStyle);
    commandRow->addWidget(m_add_manual_button);

    commandRow->addSpacing(ui::kSpacingDefault);

    m_new_folder_button = new QPushButton(tr("New Folder"), this);
    m_new_folder_button->setObjectName(QStringLiteral("fileExplorerNewFolderButton"));
    m_new_folder_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::NewFolder));
    m_new_folder_button->setAccessibleName(tr("Create folder in selected target"));
    m_new_folder_button->setToolTip(tr("Create a folder in the current target path"));
    m_new_folder_button->setStyleSheet(ui::kSecondaryButtonStyle);
    commandRow->addWidget(m_new_folder_button);

    m_write_file_button = new QPushButton(tr("Write File"), this);
    m_write_file_button->setObjectName(QStringLiteral("fileExplorerWriteFileButton"));
    m_write_file_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::WriteFile));
    m_write_file_button->setAccessibleName(tr("Write file to selected target"));
    m_write_file_button->setToolTip(tr("Copy a local file into the current target path"));
    m_write_file_button->setStyleSheet(ui::kPrimaryButtonStyle);
    commandRow->addWidget(m_write_file_button);

    m_rename_button = new QPushButton(tr("Rename"), this);
    m_rename_button->setObjectName(QStringLiteral("fileExplorerRenameButton"));
    m_rename_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::Rename));
    m_rename_button->setAccessibleName(tr("Rename selected explorer item"));
    m_rename_button->setToolTip(tr("Rename or move the selected item where supported"));
    m_rename_button->setStyleSheet(ui::kSecondaryButtonStyle);
    commandRow->addWidget(m_rename_button);

    m_delete_button = new QPushButton(tr("Delete"), this);
    m_delete_button->setObjectName(QStringLiteral("fileExplorerDeleteButton"));
    m_delete_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::Delete));
    m_delete_button->setAccessibleName(tr("Delete selected explorer item"));
    m_delete_button->setToolTip(tr("Delete the selected item from the target"));
    m_delete_button->setStyleSheet(ui::kSecondaryButtonStyle);
    commandRow->addWidget(m_delete_button);
    commandRow->addStretch(1);

    m_view_button = new QToolButton(this);
    m_view_button->setObjectName(QStringLiteral("fileExplorerViewButton"));
    m_view_button->setText(tr("View"));
    m_view_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_view_button->setPopupMode(QToolButton::InstantPopup);
    m_view_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::ViewDetails));
    m_view_button->setAccessibleName(tr("Change File Explorer view layout"));
    m_view_button->setToolTip(tr("Change layout, hidden items, extensions, and pane options"));
    m_view_button->setStyleSheet(ui::kSecondaryButtonStyle);
    commandRow->addWidget(m_view_button);

    m_details_toggle_button = new QPushButton(this);
    m_details_toggle_button->setObjectName(QStringLiteral("fileExplorerDetailsToggleButton"));
    m_details_toggle_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::TogglePreviewPane));
    m_details_toggle_button->setAccessibleName(tr("Toggle File Explorer details pane"));
    m_details_toggle_button->setToolTip(tr("Show or hide preview, properties, safety, and evidence"));
    m_details_toggle_button->setStyleSheet(ui::kSecondaryButtonStyle);
    commandRow->addWidget(m_details_toggle_button);
}

QPushButton* FileExplorerCommandBar::sidebarToggleButton() const {
    return m_sidebar_toggle_button;
}

QPushButton* FileExplorerCommandBar::refreshButton() const {
    return m_refresh_button;
}

QPushButton* FileExplorerCommandBar::scanDisksButton() const {
    return m_scan_disks_button;
}

QPushButton* FileExplorerCommandBar::addManualButton() const {
    return m_add_manual_button;
}

QPushButton* FileExplorerCommandBar::newFolderButton() const {
    return m_new_folder_button;
}

QPushButton* FileExplorerCommandBar::writeFileButton() const {
    return m_write_file_button;
}

QPushButton* FileExplorerCommandBar::renameButton() const {
    return m_rename_button;
}

QPushButton* FileExplorerCommandBar::deleteButton() const {
    return m_delete_button;
}

QToolButton* FileExplorerCommandBar::viewButton() const {
    return m_view_button;
}

QPushButton* FileExplorerCommandBar::detailsToggleButton() const {
    return m_details_toggle_button;
}

}  // namespace sak
