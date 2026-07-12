// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_command_bar.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QFrame>
#include <QHBoxLayout>

namespace sak {

namespace {

QPushButton* makeCommandButton(QWidget* parent,
                               const char* object_name,
                               const FileExplorerCommandId command,
                               const QString& accessible_name,
                               const QString& tool_tip) {
    auto* button = new QPushButton(parent);
    button->setObjectName(QString::fromLatin1(object_name));
    button->setIcon(FileExplorerIconRegistry::iconForCommand(command));
    button->setAccessibleName(accessible_name);
    button->setToolTip(tool_tip);
    return button;
}

QFrame* makeSeparator(QWidget* parent) {
    auto* separator = new QFrame(parent);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Plain);
    return separator;
}

}  // namespace

FileExplorerCommandBar::FileExplorerCommandBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerCommandBar"));
    auto* commandRow = new QHBoxLayout(this);
    commandRow->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    commandRow->setSpacing(ui::kSpacingTight);

    createCreationButtons(commandRow);
    commandRow->addWidget(makeSeparator(this));
    createItemButtons(commandRow);
    commandRow->addStretch(1);
    createViewButtons(commandRow);
}

void FileExplorerCommandBar::createCreationButtons(QHBoxLayout* commandRow) {
    m_new_folder_button = makeCommandButton(this,
                                            "fileExplorerNewFolderButton",
                                            FileExplorerCommandId::NewFolder,
                                            tr("Create folder in selected target"),
                                            tr("Create a folder in the current target path"));
    m_new_folder_button->setText(tr("New Folder"));
    commandRow->addWidget(m_new_folder_button);

    m_write_file_button = makeCommandButton(this,
                                            "fileExplorerWriteFileButton",
                                            FileExplorerCommandId::WriteFile,
                                            tr("Write file to selected target"),
                                            tr("Copy a local file into the current target path"));
    m_write_file_button->setText(tr("Write File"));
    commandRow->addWidget(m_write_file_button);
}

void FileExplorerCommandBar::createItemButtons(QHBoxLayout* commandRow) {
    m_open_button = makeCommandButton(this,
                                      "fileExplorerOpenButton",
                                      FileExplorerCommandId::Open,
                                      tr("Open selected explorer item"),
                                      tr("Open the selected item"));
    commandRow->addWidget(m_open_button);

    m_copy_path_button = makeCommandButton(this,
                                           "fileExplorerCopyPathButton",
                                           FileExplorerCommandId::CopyItemPath,
                                           tr("Copy selected explorer path"),
                                           tr("Copy the selected item path"));
    commandRow->addWidget(m_copy_path_button);

    m_rename_button = makeCommandButton(this,
                                        "fileExplorerRenameButton",
                                        FileExplorerCommandId::Rename,
                                        tr("Rename selected explorer item"),
                                        tr("Rename or move the selected item where supported"));
    commandRow->addWidget(m_rename_button);

    m_delete_button = makeCommandButton(this,
                                        "fileExplorerDeleteButton",
                                        FileExplorerCommandId::Delete,
                                        tr("Delete selected explorer item"),
                                        tr("Delete the selected item from the target"));
    commandRow->addWidget(m_delete_button);
}

void FileExplorerCommandBar::createViewButtons(QHBoxLayout* commandRow) {
    m_view_button = new QToolButton(this);
    m_view_button->setObjectName(QStringLiteral("fileExplorerViewButton"));
    m_view_button->setText(tr("View"));
    m_view_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_view_button->setPopupMode(QToolButton::InstantPopup);
    m_view_button->setIcon(
        FileExplorerIconRegistry::iconForCommand(FileExplorerCommandId::ViewDetails));
    m_view_button->setAccessibleName(tr("Change File Explorer view layout"));
    m_view_button->setToolTip(tr("Change layout, hidden items, extensions, and pane options"));
    commandRow->addWidget(m_view_button);

    m_details_toggle_button =
        makeCommandButton(this,
                          "fileExplorerDetailsToggleButton",
                          FileExplorerCommandId::TogglePreviewPane,
                          tr("Toggle File Explorer details pane"),
                          tr("Show or hide preview, properties, safety, and evidence"));
    commandRow->addWidget(m_details_toggle_button);
}

QPushButton* FileExplorerCommandBar::newFolderButton() const {
    return m_new_folder_button;
}

QPushButton* FileExplorerCommandBar::writeFileButton() const {
    return m_write_file_button;
}

QPushButton* FileExplorerCommandBar::openButton() const {
    return m_open_button;
}

QPushButton* FileExplorerCommandBar::copyPathButton() const {
    return m_copy_path_button;
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
