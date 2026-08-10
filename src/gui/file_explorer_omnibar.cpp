// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_omnibar.h"

#include "sak/file_explorer_breadcrumb.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/file_explorer_status_center_widget.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace sak {

namespace {

constexpr int kSearchBoxMinWidth = 170;
constexpr int kSearchBoxMaxWidth = 260;
// Files NavigationToolbar.xaml metrics.
constexpr int kAddressBarHeight = 48;
constexpr int kAddressBarSidePadding = 4;
constexpr int kAddressBarSpacing = 4;
constexpr int kAddressButtonWidth = 36;
constexpr int kAddressButtonHeight = 32;
// Files Omnibar.xaml AutoSuggestListMaxHeight-ish cap for the popup.
constexpr int kSuggestionPopupMaxHeight = 320;

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
    button->setFixedSize(kAddressButtonWidth, kAddressButtonHeight);
    return button;
}

}  // namespace

FileExplorerOmnibar::FileExplorerOmnibar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerOmnibar"));
    // Files address bar: 48px row, 4px side padding, 4px spacing, 36x32
    // buttons (NavigationToolbar.xaml root Grid + AddressToolbarButtonStyle).
    setFixedHeight(kAddressBarHeight);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(kAddressBarSidePadding, 0, kAddressBarSidePadding, 0);
    row->setSpacing(kAddressBarSpacing);

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
    // Right-click shows the back-history flyout (Files NavigationToolbar.xaml
    // BackHistoryFlyout); the panel populates the menu from the pane state.
    m_back_button->setContextMenuPolicy(Qt::CustomContextMenu);
    row->addWidget(m_back_button);

    // Narrow-window overflow ("see more", Files NavigationStates): holds
    // Forward/Up/Refresh when the shell is too narrow for the full cluster.
    m_nav_overflow_button = makeIconButton(this,
                                           "fileExplorerNavOverflowButton",
                                           "more",
                                           tr("More navigation actions"),
                                           tr("Forward, parent directory, and refresh"));
    auto* overflow_menu = new QMenu(m_nav_overflow_button);
    m_overflow_forward_action = overflow_menu->addAction(
        FileExplorerIconRegistry::iconForKey(QStringLiteral("nav-forward")), tr("Forward"));
    m_overflow_up_action = overflow_menu->addAction(
        FileExplorerIconRegistry::iconForKey(QStringLiteral("nav-up")), tr("Up"));
    m_overflow_refresh_action = overflow_menu->addAction(
        FileExplorerIconRegistry::iconForKey(QStringLiteral("refresh")), tr("Refresh"));
    m_nav_overflow_button->setMenu(overflow_menu);
    m_nav_overflow_button->setVisible(false);
    row->addWidget(m_nav_overflow_button);

    m_forward_button = makeIconButton(this,
                                      "fileExplorerForwardButton",
                                      "nav-forward",
                                      tr("Go forward"),
                                      tr("Go to next explorer location"));
    m_forward_button->setContextMenuPolicy(Qt::CustomContextMenu);
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

    connect(overflow_menu, &QMenu::aboutToShow, this, [this]() {
        m_overflow_forward_action->setEnabled(m_forward_button->isEnabled());
        m_overflow_up_action->setEnabled(m_up_button->isEnabled());
        m_overflow_refresh_action->setEnabled(m_refresh_button->isEnabled());
    });
    connect(m_overflow_forward_action, &QAction::triggered, m_forward_button, &QPushButton::click);
    connect(m_overflow_up_action, &QAction::triggered, m_up_button, &QPushButton::click);
    connect(m_overflow_refresh_action, &QAction::triggered, m_refresh_button, &QPushButton::click);
}

void FileExplorerOmnibar::setNarrowMode(const bool narrow) {
    if (m_nav_overflow_button) {
        m_nav_overflow_button->setVisible(narrow);
    }
    for (QPushButton* button : {m_forward_button, m_up_button, m_refresh_button}) {
        if (button) {
            button->setVisible(!narrow);
        }
    }
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
    m_path_edit->setPlaceholderText(tr("Enter a path to navigate to..."));
    m_path_edit->installEventFilter(this);
    m_address_stack->addWidget(m_path_edit);
    row->addWidget(m_address_stack, 1);

    // Palette/search text drives the suggestion population in the panel;
    // path-mode edits stay silent (the breadcrumb binding below covers them).
    connect(m_path_edit, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (m_mode != FileExplorerOmnibarMode::Path) {
            Q_EMIT queryTextEdited(text);
        }
    });

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

    // Files NavigationToolbar ShowStatusCenterButton: rightmost slot of the
    // address row, hosting the progress ring and InfoBadge overlays.
    m_status_center_button = new FileExplorerStatusCenterButton(this);
    m_status_center_button->setFixedSize(kAddressButtonWidth, kAddressButtonHeight);
    row->addWidget(m_status_center_button);
}

FileExplorerStatusCenterButton* FileExplorerOmnibar::statusCenterButton() const {
    return m_status_center_button;
}

void FileExplorerOmnibar::setAddressEditMode(const bool edit) {
    if (!m_address_stack) {
        return;
    }
    // Editing the address is a path operation. If a palette/search mode is active, leave it
    // first (Files EditPathAction) so the address text is treated as a path and Enter
    // navigates instead of submitting a search or running the selected command.
    if (edit && m_mode != FileExplorerOmnibarMode::Path) {
        setMode(FileExplorerOmnibarMode::Path);
    }
    m_address_stack->setCurrentWidget(edit ? static_cast<QWidget*>(m_path_edit)
                                           : static_cast<QWidget*>(m_breadcrumb));
    if (edit) {
        // Remember the committed path so an Escape-cancel can revert an uncommitted edit.
        m_path_edit_snapshot = m_path_edit->text();
        m_path_edit->setFocus(Qt::MouseFocusReason);
        m_path_edit->selectAll();
    }
}

bool FileExplorerOmnibar::addressEditMode() const {
    return m_address_stack && m_address_stack->currentWidget() == m_path_edit;
}

void FileExplorerOmnibar::setMode(const FileExplorerOmnibarMode mode) {
    if (m_mode == mode) {
        return;
    }
    const FileExplorerOmnibarMode previous = m_mode;
    m_mode = mode;
    if (mode == FileExplorerOmnibarMode::Path) {
        // Files Omnibar_IsFocusedChanged: leaving palette/search restores the
        // path text and reverts to the breadcrumb.
        setSuggestionsVisible(false);
        m_path_edit->setText(m_path_text_backup);
        m_path_edit->setPlaceholderText(tr("Enter a path to navigate to..."));
        setAddressEditMode(false);
        Q_EMIT modeChanged(mode);
        return;
    }
    if (previous == FileExplorerOmnibarMode::Path) {
        m_path_text_backup = m_path_edit->text();
    }
    // Files SwitchToCommandPaletteMode/SwitchToSearchMode: text clears, the
    // mode placeholder appears, and the box focuses (IsAutoFocusEnabled).
    m_path_edit->setPlaceholderText(mode == FileExplorerOmnibarMode::Palette
                                        ? tr("Find features and commands...")
                                        : tr("Search for files and folders..."));
    m_address_stack->setCurrentWidget(m_path_edit);
    m_path_edit->clear();
    m_path_edit->setFocus(Qt::ShortcutFocusReason);
    Q_EMIT modeChanged(mode);
    Q_EMIT queryTextEdited(QString());
}

FileExplorerOmnibarMode FileExplorerOmnibar::mode() const {
    return m_mode;
}

void FileExplorerOmnibar::ensureSuggestionPopup() {
    if (m_suggestion_frame) {
        return;
    }
    // A plain child frame of the shell window (not Qt::Popup): it never takes
    // focus away from the address box, and it behaves identically offscreen.
    m_suggestion_frame = new QFrame(window());
    m_suggestion_frame->setObjectName(QStringLiteral("fileExplorerOmnibarSuggestionFrame"));
    m_suggestion_frame->setFrameShape(QFrame::StyledPanel);
    m_suggestion_frame->setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(m_suggestion_frame);
    layout->setContentsMargins(1, 1, 1, 1);
    m_suggestion_list = new QListWidget(m_suggestion_frame);
    m_suggestion_list->setObjectName(QStringLiteral("fileExplorerOmnibarSuggestions"));
    m_suggestion_list->setAccessibleName(tr("Omnibar suggestions"));
    m_suggestion_list->setFocusPolicy(Qt::NoFocus);
    m_suggestion_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_suggestion_list);
    m_suggestion_frame->hide();
    connect(m_suggestion_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        Q_EMIT suggestionActivated(item);
    });
}

void FileExplorerOmnibar::positionSuggestionPopup() {
    if (!m_suggestion_frame || !m_address_stack) {
        return;
    }
    const QPoint anchor = m_address_stack->mapTo(window(), m_address_stack->rect().bottomLeft());
    m_suggestion_frame->setGeometry(
        anchor.x(), anchor.y() + 2, m_address_stack->width(), kSuggestionPopupMaxHeight);
}

QListWidget* FileExplorerOmnibar::suggestionList() const {
    const_cast<FileExplorerOmnibar*>(this)->ensureSuggestionPopup();
    return m_suggestion_list;
}

void FileExplorerOmnibar::setSuggestionsVisible(const bool visible) {
    ensureSuggestionPopup();
    if (!visible) {
        m_suggestion_frame->hide();
        return;
    }
    positionSuggestionPopup();
    m_suggestion_frame->show();
    m_suggestion_frame->raise();
}

bool FileExplorerOmnibar::suggestionsVisible() const {
    return m_suggestion_frame && m_suggestion_frame->isVisible();
}

void FileExplorerOmnibar::moveSuggestionSelection(const int delta) {
    if (!m_suggestion_list || m_suggestion_list->count() == 0) {
        return;
    }
    const int count = m_suggestion_list->count();
    const int current = m_suggestion_list->currentRow();
    const int next = qBound(0, current + delta, count - 1);
    m_suggestion_list->setCurrentRow(next);
}

// Keys the palette/search modes claim (Files Omnibar.Events AutoSuggestBox
// key handling): arrows drive the suggestion selection, Enter submits, and
// Escape closes the popup first and leaves the mode second.
bool FileExplorerOmnibar::handleQueryModeKey(QKeyEvent* key_event) {
    switch (key_event->key()) {
    case Qt::Key_Down:
        moveSuggestionSelection(1);
        return true;
    case Qt::Key_Up:
        moveSuggestionSelection(-1);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        Q_EMIT querySubmitted(m_path_edit->text());
        return true;
    case Qt::Key_Escape:
        if (suggestionsVisible()) {
            setSuggestionsVisible(false);
        } else {
            setMode(FileExplorerOmnibarMode::Path);
        }
        return true;
    default:
        return false;
    }
}

// Claim the keys the omnibar handles ahead of the panel-level ambient
// shortcuts (ClearSelection owns a bare Esc): accepting the ShortcutOverride
// delivers the real KeyPress to the address box.
bool FileExplorerOmnibar::claimShortcutOverride(QKeyEvent* key_event) const {
    const int key = key_event->key();
    if (key == Qt::Key_Escape) {
        key_event->accept();
        return true;
    }
    const bool query_mode = m_mode != FileExplorerOmnibarMode::Path;
    if (query_mode && (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Return ||
                       key == Qt::Key_Enter)) {
        key_event->accept();
        return true;
    }
    return false;
}

// Palette/search mode events: arrows/Enter/Escape belong to the popup, and a
// real focus loss reverts to path mode (the suggestion list never takes
// focus, Files Omnibar_IsFocusedChanged).
bool FileExplorerOmnibar::filterQueryModeEvent(QEvent* event) {
    if (event->type() == QEvent::KeyPress && handleQueryModeKey(static_cast<QKeyEvent*>(event))) {
        return true;
    }
    if (event->type() == QEvent::FocusOut) {
        setMode(FileExplorerOmnibarMode::Path);
    }
    return false;
}

// Path-edit mode events: leave edit mode when the user commits (Enter
// navigates via the panel), cancels with Escape, or clicks elsewhere.
bool FileExplorerOmnibar::filterPathModeEvent(QEvent* event) {
    if (event->type() == QEvent::FocusOut) {
        setAddressEditMode(false);
        return false;
    }
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        // Cancel the edit: restore the committed path so an uncommitted or malformed edit
        // never becomes the displayed current location (the breadcrumb is bound to this
        // text via textChanged, so this reverts it too). The real directory is unchanged.
        m_path_edit->setText(m_path_edit_snapshot);
        setAddressEditMode(false);
        return true;
    }
    return false;
}

bool FileExplorerOmnibar::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_path_edit || !event) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::ShortcutOverride &&
        claimShortcutOverride(static_cast<QKeyEvent*>(event))) {
        return true;
    }
    const bool handled = m_mode != FileExplorerOmnibarMode::Path ? filterQueryModeEvent(event)
                                                                 : filterPathModeEvent(event);
    if (handled) {
        return true;
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
