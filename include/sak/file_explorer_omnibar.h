// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_omnibar.h
/// @brief File Explorer navigation and address row (Files-style toolbar).

#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

class QAction;
class QFrame;
class QHBoxLayout;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;

namespace sak {

class FileExplorerBreadcrumb;

/// Files Omnibar modes (Files.App.Controls.Omnibar): the address text box
/// serves path editing (default), the command palette, and search.
enum class FileExplorerOmnibarMode {
    Path,
    Palette,
    Search,
};

/// Top navigation row: sidebar toggle, back/forward/up/refresh, breadcrumb
/// address (with inline path-edit, palette, and search modes plus an anchored
/// suggestion popup), search box, and the command palette trigger.
class FileExplorerOmnibar : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerOmnibar(QWidget* parent = nullptr);

    [[nodiscard]] QPushButton* sidebarToggleButton() const;
    [[nodiscard]] QPushButton* backButton() const;
    [[nodiscard]] QPushButton* forwardButton() const;
    [[nodiscard]] QPushButton* upButton() const;
    [[nodiscard]] QPushButton* refreshButton() const;
    [[nodiscard]] QLineEdit* pathEdit() const;
    [[nodiscard]] FileExplorerBreadcrumb* breadcrumb() const;
    [[nodiscard]] QLineEdit* searchBox() const;
    [[nodiscard]] QPushButton* searchButton() const;
    [[nodiscard]] QPushButton* commandButton() const;

    /// Switches the address slot between the breadcrumb (false) and the
    /// editable path line (true, focused with the text selected).
    void setAddressEditMode(bool edit);
    [[nodiscard]] bool addressEditMode() const;

    /// Files Omnibar mode switch: Palette/Search take over the address text
    /// box (text cleared, mode placeholder, focused); returning to Path
    /// restores the path text and the breadcrumb.
    void setMode(FileExplorerOmnibarMode mode);
    [[nodiscard]] FileExplorerOmnibarMode mode() const;

    /// Anchored suggestion popup under the address box (Files
    /// PART_SuggestionsPopup). The list never takes focus; the panel fills it.
    [[nodiscard]] QListWidget* suggestionList() const;
    void setSuggestionsVisible(bool visible);
    [[nodiscard]] bool suggestionsVisible() const;

    /// Narrow shells collapse Forward/Up/Refresh into an overflow menu
    /// (Files NavigationToolbar adaptive states).
    void setNarrowMode(bool narrow);

Q_SIGNALS:
    void modeChanged(FileExplorerOmnibarMode mode);
    /// Text edits while in Palette/Search mode (never in Path mode).
    void queryTextEdited(const QString& text);
    /// Enter in Palette/Search mode; the current suggestion (if any) is the
    /// chosen item.
    void querySubmitted(const QString& text);
    /// A suggestion row was clicked.
    void suggestionActivated(QListWidgetItem* item);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void createNavigationButtons(QHBoxLayout* row);
    void createAddressAndSearch(QHBoxLayout* row);
    void ensureSuggestionPopup();
    void positionSuggestionPopup();
    [[nodiscard]] bool claimShortcutOverride(QKeyEvent* key_event) const;
    [[nodiscard]] bool handleQueryModeKey(QKeyEvent* key_event);
    [[nodiscard]] bool filterQueryModeEvent(QEvent* event);
    [[nodiscard]] bool filterPathModeEvent(QEvent* event);
    void moveSuggestionSelection(int delta);

    QPushButton* m_sidebar_toggle_button{nullptr};
    QPushButton* m_back_button{nullptr};
    QPushButton* m_nav_overflow_button{nullptr};
    QAction* m_overflow_forward_action{nullptr};
    QAction* m_overflow_up_action{nullptr};
    QAction* m_overflow_refresh_action{nullptr};
    QPushButton* m_forward_button{nullptr};
    QPushButton* m_up_button{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QStackedWidget* m_address_stack{nullptr};
    FileExplorerBreadcrumb* m_breadcrumb{nullptr};
    QLineEdit* m_path_edit{nullptr};
    QLineEdit* m_search_box{nullptr};
    QPushButton* m_search_button{nullptr};
    QPushButton* m_command_button{nullptr};
    QFrame* m_suggestion_frame{nullptr};
    QListWidget* m_suggestion_list{nullptr};
    FileExplorerOmnibarMode m_mode{FileExplorerOmnibarMode::Path};
    QString m_path_text_backup;
};

}  // namespace sak
