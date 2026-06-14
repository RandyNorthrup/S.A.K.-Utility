// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_omnibar.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QHBoxLayout>
#include <QStyle>

namespace sak {

FileExplorerOmnibar::FileExplorerOmnibar(QWidget* parent) : QWidget(parent) {
    auto* pathRow = new QHBoxLayout(this);
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(ui::kSpacingSmall);

    m_back_button = new QPushButton(this);
    m_back_button->setObjectName(QStringLiteral("fileExplorerBackButton"));
    m_back_button->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_back_button->setAccessibleName(tr("Go back"));
    m_back_button->setToolTip(tr("Go to previous explorer location"));
    m_back_button->setStyleSheet(ui::kSecondaryButtonStyle);
    pathRow->addWidget(m_back_button);

    m_forward_button = new QPushButton(this);
    m_forward_button->setObjectName(QStringLiteral("fileExplorerForwardButton"));
    m_forward_button->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    m_forward_button->setAccessibleName(tr("Go forward"));
    m_forward_button->setToolTip(tr("Go to next explorer location"));
    m_forward_button->setStyleSheet(ui::kSecondaryButtonStyle);
    pathRow->addWidget(m_forward_button);

    m_up_button = new QPushButton(this);
    m_up_button->setObjectName(QStringLiteral("fileExplorerUpButton"));
    m_up_button->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_up_button->setAccessibleName(tr("Go to parent directory"));
    m_up_button->setToolTip(tr("Go to parent directory"));
    m_up_button->setStyleSheet(ui::kSecondaryButtonStyle);
    pathRow->addWidget(m_up_button);

    m_path_edit = new QLineEdit(this);
    m_path_edit->setObjectName(QStringLiteral("fileExplorerPathEdit"));
    m_path_edit->setAccessibleName(tr("Explorer omnibar path"));
    m_path_edit->setToolTip(tr("Path inside the selected target. Press Enter to navigate."));
    pathRow->addWidget(m_path_edit, 1);

    m_search_button = new QPushButton(this);
    m_search_button->setObjectName(QStringLiteral("fileExplorerSearchButton"));
    m_search_button->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
    m_search_button->setAccessibleName(tr("Search current File Explorer location"));
    m_search_button->setToolTip(tr("Filter the current folder by name, type, or path"));
    m_search_button->setStyleSheet(ui::kSecondaryButtonStyle);
    pathRow->addWidget(m_search_button);

    m_command_button = new QPushButton(this);
    m_command_button->setObjectName(QStringLiteral("fileExplorerCommandButton"));
    m_command_button->setIcon(FileExplorerIconRegistry::iconForKey(QStringLiteral("more")));
    m_command_button->setAccessibleName(tr("Open File Explorer command palette"));
    m_command_button->setToolTip(tr("Search and run File Explorer commands"));
    m_command_button->setStyleSheet(ui::kSecondaryButtonStyle);
    pathRow->addWidget(m_command_button);

    m_open_button = new QPushButton(tr("Open"), this);
    m_open_button->setObjectName(QStringLiteral("fileExplorerOpenButton"));
    m_open_button->setIcon(FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::Open));
    m_open_button->setAccessibleName(tr("Open selected explorer item"));
    m_open_button->setStyleSheet(ui::kPrimaryButtonStyle);
    pathRow->addWidget(m_open_button);

    m_copy_path_button = new QPushButton(tr("Copy Path"), this);
    m_copy_path_button->setObjectName(QStringLiteral("fileExplorerCopyPathButton"));
    m_copy_path_button->setIcon(
        FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::CopyItemPath));
    m_copy_path_button->setAccessibleName(tr("Copy selected explorer path"));
    m_copy_path_button->setStyleSheet(ui::kSecondaryButtonStyle);
    pathRow->addWidget(m_copy_path_button);
}

QPushButton* FileExplorerOmnibar::backButton() const {
    return m_back_button;
}

QPushButton* FileExplorerOmnibar::forwardButton() const {
    return m_forward_button;
}

QPushButton* FileExplorerOmnibar::upButton() const {
    return m_up_button;
}

QLineEdit* FileExplorerOmnibar::pathEdit() const {
    return m_path_edit;
}

QPushButton* FileExplorerOmnibar::searchButton() const {
    return m_search_button;
}

QPushButton* FileExplorerOmnibar::commandButton() const {
    return m_command_button;
}

QPushButton* FileExplorerOmnibar::openButton() const {
    return m_open_button;
}

QPushButton* FileExplorerOmnibar::copyPathButton() const {
    return m_copy_path_button;
}

}  // namespace sak
