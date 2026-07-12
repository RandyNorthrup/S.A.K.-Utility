// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_omnibar.h"

#include "sak/file_explorer_breadcrumb.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QStackedWidget>

namespace sak {

namespace {

constexpr int kSearchBoxMinWidth = 170;
constexpr int kSearchBoxMaxWidth = 260;

QPushButton* makeIconButton(QWidget* parent,
                            const char* object_name,
                            const char* icon_key,
                            const QString& accessible_name,
                            const QString& tool_tip) {
    auto* button = new QPushButton(parent);
    button->setObjectName(QString::fromLatin1(object_name));
    button->setIcon(FileExplorerIconRegistry::iconForKey(QString::fromLatin1(icon_key)));
    button->setAccessibleName(accessible_name);
    button->setToolTip(tool_tip);
    return button;
}

}  // namespace

FileExplorerOmnibar::FileExplorerOmnibar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerOmnibar"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    row->setSpacing(ui::kSpacingTight);

    createNavigationButtons(row);
    createAddressAndSearch(row);
}

void FileExplorerOmnibar::createNavigationButtons(QHBoxLayout* row) {
    m_sidebar_toggle_button = makeIconButton(this,
                                             "fileExplorerSidebarToggleButton",
                                             "panel-left",
                                             tr("Toggle File Explorer sidebar"),
                                             tr("Show or hide target navigation"));
    row->addWidget(m_sidebar_toggle_button);

    m_back_button = makeIconButton(this,
                                   "fileExplorerBackButton",
                                   "nav-back",
                                   tr("Go back"),
                                   tr("Go to previous explorer location"));
    row->addWidget(m_back_button);

    m_forward_button = makeIconButton(this,
                                      "fileExplorerForwardButton",
                                      "nav-forward",
                                      tr("Go forward"),
                                      tr("Go to next explorer location"));
    row->addWidget(m_forward_button);

    m_up_button = makeIconButton(this,
                                 "fileExplorerUpButton",
                                 "nav-up",
                                 tr("Go to parent directory"),
                                 tr("Go to parent directory"));
    row->addWidget(m_up_button);

    m_refresh_button = makeIconButton(this,
                                      "fileExplorerRefreshButton",
                                      "refresh",
                                      tr("Refresh mounted file targets"),
                                      tr("Reload targets and the current folder"));
    row->addWidget(m_refresh_button);
}

void FileExplorerOmnibar::createAddressAndSearch(QHBoxLayout* row) {
    m_address_stack = new QStackedWidget(this);
    m_address_stack->setObjectName(QStringLiteral("fileExplorerAddressStack"));

    m_breadcrumb = new FileExplorerBreadcrumb(m_address_stack);
    m_address_stack->addWidget(m_breadcrumb);

    m_path_edit = new QLineEdit(m_address_stack);
    m_path_edit->setObjectName(QStringLiteral("fileExplorerPathEdit"));
    m_path_edit->setAccessibleName(tr("Explorer omnibar path"));
    m_path_edit->setToolTip(tr("Path inside the selected target. Press Enter to navigate."));
    m_path_edit->installEventFilter(this);
    m_address_stack->addWidget(m_path_edit);
    row->addWidget(m_address_stack, 1);

    // Breadcrumb follows whatever the path line holds, so the panel keeps a
    // single source of truth for the current location.
    connect(m_path_edit, &QLineEdit::textChanged, m_breadcrumb, &FileExplorerBreadcrumb::setPath);
    connect(m_breadcrumb, &FileExplorerBreadcrumb::editRequested, this, [this]() {
        setAddressEditMode(true);
    });

    m_search_box = new QLineEdit(this);
    m_search_box->setObjectName(QStringLiteral("fileExplorerSearchBox"));
    m_search_box->setAccessibleName(tr("Search current File Explorer location"));
    m_search_box->setPlaceholderText(tr("Search"));
    m_search_box->setToolTip(tr("Search the current target. Press Enter to run."));
    m_search_box->setClearButtonEnabled(true);
    m_search_box->setMinimumWidth(kSearchBoxMinWidth);
    m_search_box->setMaximumWidth(kSearchBoxMaxWidth);
    row->addWidget(m_search_box);

    m_search_button = makeIconButton(this,
                                     "fileExplorerSearchButton",
                                     "search",
                                     tr("Search current File Explorer location"),
                                     tr("Search the current target by name, type, or path"));
    row->addWidget(m_search_button);

    m_command_button = makeIconButton(this,
                                      "fileExplorerCommandButton",
                                      "more",
                                      tr("Open File Explorer command palette"),
                                      tr("Search and run File Explorer commands"));
    row->addWidget(m_command_button);
}

void FileExplorerOmnibar::setAddressEditMode(const bool edit) {
    if (!m_address_stack) {
        return;
    }
    m_address_stack->setCurrentWidget(edit ? static_cast<QWidget*>(m_path_edit)
                                           : static_cast<QWidget*>(m_breadcrumb));
    if (edit) {
        m_path_edit->setFocus(Qt::MouseFocusReason);
        m_path_edit->selectAll();
    }
}

bool FileExplorerOmnibar::addressEditMode() const {
    return m_address_stack && m_address_stack->currentWidget() == m_path_edit;
}

bool FileExplorerOmnibar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_path_edit && event) {
        // Leave edit mode when the user commits (Enter navigates via the
        // panel), cancels with Escape, or clicks elsewhere.
        if (event->type() == QEvent::FocusOut) {
            setAddressEditMode(false);
        } else if (event->type() == QEvent::KeyPress) {
            const auto* key_event = static_cast<QKeyEvent*>(event);
            if (key_event->key() == Qt::Key_Escape) {
                setAddressEditMode(false);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QPushButton* FileExplorerOmnibar::sidebarToggleButton() const {
    return m_sidebar_toggle_button;
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

QPushButton* FileExplorerOmnibar::refreshButton() const {
    return m_refresh_button;
}

QLineEdit* FileExplorerOmnibar::pathEdit() const {
    return m_path_edit;
}

FileExplorerBreadcrumb* FileExplorerOmnibar::breadcrumb() const {
    return m_breadcrumb;
}

QLineEdit* FileExplorerOmnibar::searchBox() const {
    return m_search_box;
}

QPushButton* FileExplorerOmnibar::searchButton() const {
    return m_search_button;
}

QPushButton* FileExplorerOmnibar::commandButton() const {
    return m_command_button;
}

}  // namespace sak
