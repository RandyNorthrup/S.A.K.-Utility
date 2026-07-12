// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_command_bar.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>

namespace sak {

namespace {

// Files Toolbar.xaml: primary command buttons are icon-only with labels on
// flyout groups only; the toolbar card uses an 8px radius (styled via QSS).
QPushButton* makeItemCommandButton(QWidget* parent,
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

QToolButton* makeFlyoutButton(QWidget* parent,
                              const char* object_name,
                              const QIcon& icon,
                              const QString& accessible_name,
                              const QString& tool_tip) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QString::fromLatin1(object_name));
    button->setIcon(icon);
    button->setPopupMode(QToolButton::InstantPopup);
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

    createLeftCommands(commandRow);
    commandRow->addStretch(1);
    createRightCommands(commandRow);
}

void FileExplorerCommandBar::createLeftCommands(QHBoxLayout* commandRow) {
    // Files ToolbarSections.cs AlwaysVisible order: New (labeled group
    // flyout), separator, then icon-only Cut/Copy/Paste/Rename/Share/
    // Delete/Properties. Cut and Share are documented C-track deviations
    // (Cut lands with the C3 transfer engine; Share is EXCLUDED).
    m_new_button = makeFlyoutButton(this,
                                    "fileExplorerNewButton",
                                    FileExplorerIconRegistry::iconForKey(QStringLiteral("plus")),
                                    tr("New item"),
                                    tr("Create a folder or write a file into the current path"));
    m_new_button->setText(tr("New"));
    m_new_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_new_menu = new QMenu(m_new_button);
    m_new_menu->setObjectName(QStringLiteral("fileExplorerNewMenu"));
    m_new_button->setMenu(m_new_menu);
    commandRow->addWidget(m_new_button);

    commandRow->addWidget(makeSeparator(this));

    m_copy_button = makeItemCommandButton(this,
                                          "fileExplorerCopyButton",
                                          FileExplorerCommandId::CopyItems,
                                          tr("Copy selected items"),
                                          tr("Copy the selected items to the clipboard"));
    commandRow->addWidget(m_copy_button);

    m_paste_button = makeItemCommandButton(this,
                                           "fileExplorerPasteButton",
                                           FileExplorerCommandId::Paste,
                                           tr("Paste"),
                                           tr("Paste clipboard files into the current path"));
    commandRow->addWidget(m_paste_button);

    m_rename_button = makeItemCommandButton(this,
                                            "fileExplorerRenameButton",
                                            FileExplorerCommandId::Rename,
                                            tr("Rename selected explorer item"),
                                            tr("Rename or move the selected item"));
    commandRow->addWidget(m_rename_button);

    m_delete_button = makeItemCommandButton(this,
                                            "fileExplorerDeleteButton",
                                            FileExplorerCommandId::Delete,
                                            tr("Delete selected explorer item"),
                                            tr("Delete the selected item from the target"));
    commandRow->addWidget(m_delete_button);

    m_properties_button = makeItemCommandButton(this,
                                                "fileExplorerPropertiesButton",
                                                FileExplorerCommandId::Properties,
                                                tr("Item properties"),
                                                tr("Show properties for the selected item"));
    commandRow->addWidget(m_properties_button);
}

void FileExplorerCommandBar::createRightCommands(QHBoxLayout* commandRow) {
    // Files base command bar: selection options, sort (with group-by once
    // grouping lands in C5), layout, info-pane toggle - all icon-only.
    m_selection_button =
        makeFlyoutButton(this,
                         "fileExplorerSelectionButton",
                         FileExplorerIconRegistry::iconForKey(QStringLiteral("select-mode")),
                         tr("Selection options"),
                         tr("Select all, invert selection, or clear selection"));
    m_selection_menu = new QMenu(m_selection_button);
    m_selection_menu->setObjectName(QStringLiteral("fileExplorerSelectionMenu"));
    m_selection_button->setMenu(m_selection_menu);
    commandRow->addWidget(m_selection_button);

    m_sort_button = makeFlyoutButton(this,
                                     "fileExplorerSortButton",
                                     FileExplorerIconRegistry::iconForKey(QStringLiteral("sort")),
                                     tr("Sort options"),
                                     tr("Sort the current folder"));
    m_sort_menu = new QMenu(m_sort_button);
    m_sort_menu->setObjectName(QStringLiteral("fileExplorerSortMenu"));
    m_sort_button->setMenu(m_sort_menu);
    commandRow->addWidget(m_sort_button);

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
        makeItemCommandButton(this,
                              "fileExplorerDetailsToggleButton",
                              FileExplorerCommandId::TogglePreviewPane,
                              tr("Toggle File Explorer details pane"),
                              tr("Show or hide preview, properties, safety, and evidence"));
    commandRow->addWidget(m_details_toggle_button);
}

QToolButton* FileExplorerCommandBar::newButton() const {
    return m_new_button;
}

QMenu* FileExplorerCommandBar::newMenu() const {
    return m_new_menu;
}

QPushButton* FileExplorerCommandBar::copyButton() const {
    return m_copy_button;
}

QPushButton* FileExplorerCommandBar::pasteButton() const {
    return m_paste_button;
}

QPushButton* FileExplorerCommandBar::renameButton() const {
    return m_rename_button;
}

QPushButton* FileExplorerCommandBar::deleteButton() const {
    return m_delete_button;
}

QPushButton* FileExplorerCommandBar::propertiesButton() const {
    return m_properties_button;
}

QToolButton* FileExplorerCommandBar::selectionButton() const {
    return m_selection_button;
}

QMenu* FileExplorerCommandBar::selectionMenu() const {
    return m_selection_menu;
}

QToolButton* FileExplorerCommandBar::sortButton() const {
    return m_sort_button;
}

QMenu* FileExplorerCommandBar::sortMenu() const {
    return m_sort_menu;
}

QToolButton* FileExplorerCommandBar::viewButton() const {
    return m_view_button;
}

QPushButton* FileExplorerCommandBar::detailsToggleButton() const {
    return m_details_toggle_button;
}

}  // namespace sak
