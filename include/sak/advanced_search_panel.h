// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_panel.h
/// @brief Three-panel Advanced Search UI with file explorer, results tree,
///        and preview pane
///
/// Provides enterprise-grade grep-style file content searching with regex,
/// metadata, archive, and binary search modes within a unified three-panel
/// interface matching the SAK UI theme.

#pragma once

#include "sak/advanced_search_types.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QTreeWidget>
#include <QWidget>

#include <memory>
#include <type_traits>

class QVBoxLayout;
class QHBoxLayout;

namespace sak {

class AdvancedSearchController;
class LogToggleSwitch;

/// @brief Advanced Search Panel -- three-panel grep-style file search UI
///
/// Layout: Search Bar -> [File Explorer | Results Tree | Preview Pane]
///
/// Supports text content, regex, image metadata, file metadata, archive
/// content, and binary/hex search modes with real-time results and
/// match highlighting in the preview pane.
class AdvancedSearchPanel : public QWidget {
    Q_OBJECT

public:
    explicit AdvancedSearchPanel(QWidget* parent = nullptr);
    ~AdvancedSearchPanel() override;

    // Disable copy/move
    AdvancedSearchPanel(const AdvancedSearchPanel&) = delete;
    AdvancedSearchPanel& operator=(const AdvancedSearchPanel&) = delete;
    AdvancedSearchPanel(AdvancedSearchPanel&&) = delete;
    AdvancedSearchPanel& operator=(AdvancedSearchPanel&&) = delete;

    /// @brief Access the log toggle switch for MainWindow connection
    LogToggleSwitch* logToggle() const { return m_log_toggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Search controls
    void onSearchClicked();
    void onStopClicked();
    void onRegexPatternsClicked();
    void onPreferencesClicked();

    // Controller signals
    void onSearchStarted(const QString& pattern);
    void onResultsReceived(QVector<sak::SearchMatch> matches);
    void onSearchFinished(int totalMatches, int totalFiles);
    void onSearchFailed(const QString& error);
    void onSearchCancelled();

    // File explorer
    void onFileExplorerItemClicked(QTreeWidgetItem* item, int column);
    void onFileExplorerItemExpanded(QTreeWidgetItem* item);
    void onFileExplorerContextMenu(const QPoint& pos);

    // Results tree
    void onResultItemClicked(QTreeWidgetItem* item, int column);
    void onResultItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onResultContextMenu(const QPoint& pos);
    void onSortChanged(int index);

    // Preview navigation
    void onPreviousMatch();
    void onNextMatch();

private:
    // -- UI Setup --
    void setupUi();
    void createSearchBar(QVBoxLayout* layout);
    void createThreePanelSplitter(QVBoxLayout* layout);
    void createFileExplorer();
    void createResultsTree();
    void createPreviewPane();
    void createStatusBar(QVBoxLayout* layout);
    void createRegexPatternMenu();

    // -- File Explorer Helpers --
    void populateFileExplorerRoot();
    void populateDirectoryChildren(QTreeWidgetItem* parentItem, const QString& dirPath);
    void addPlaceholderChild(QTreeWidgetItem* parentItem);
    void removePlaceholderChildren(QTreeWidgetItem* parentItem);

    // -- Results Helpers --
    void clearResults();
    void sortResults();

    // -- Preview Helpers --
    void showFilePreview(const QString& filePath, const QVector<SearchMatch>& matches);
    void showMetadataPreview(const QString& filePath, const QVector<SearchMatch>& matches);
    void highlightMatches();
    void navigateToMatch(int matchIndex);
    void updateMatchCounter();

    // -- Utility --
    void setSearchRunning(bool running);
    void logMessage(const QString& message);
    void updateRegexPatternsButton();
    [[nodiscard]] SearchConfig buildSearchConfig() const;

    // -- Controller --
    std::unique_ptr<AdvancedSearchController> m_controller;

    // -- Search Bar Widgets --
    QComboBox* m_search_combo{nullptr};
    QPushButton* m_search_button{nullptr};
    QPushButton* m_stop_button{nullptr};
    QComboBox* m_context_lines_combo{nullptr};
    QPushButton* m_regex_patterns_button{nullptr};
    QPushButton* m_preferences_button{nullptr};
    QCheckBox* m_case_sensitive_check{nullptr};
    QCheckBox* m_whole_word_check{nullptr};
    QCheckBox* m_use_regex_check{nullptr};
    QCheckBox* m_image_metadata_check{nullptr};
    QLineEdit* m_extensions_edit{nullptr};
    QCheckBox* m_file_metadata_check{nullptr};
    QCheckBox* m_archive_search_check{nullptr};
    QCheckBox* m_binary_hex_check{nullptr};
    QMenu* m_regex_menu{nullptr};

    // -- Three-Panel Splitter --
    QSplitter* m_splitter{nullptr};

    // -- File Explorer (Left Panel) --
    QTreeWidget* m_file_explorer{nullptr};

    // -- Results Tree (Middle Panel) --
    QTreeWidget* m_results_tree{nullptr};
    QComboBox* m_sort_combo{nullptr};
    QLabel* m_results_count_label{nullptr};

    // -- Preview Pane (Right Panel) --
    QTextEdit* m_preview_edit{nullptr};
    QPushButton* m_prev_match_button{nullptr};
    QPushButton* m_next_match_button{nullptr};
    QLabel* m_match_counter_label{nullptr};
    QLabel* m_preview_header_label{nullptr};

    // -- Status Bar (progress routed via statusMessage/progressUpdate signals) --

    // -- Preview State --
    QString m_current_preview_file;
    QVector<SearchMatch> m_current_matches;
    int m_current_match_index = -1;

    // -- All results (for sorting) --
    QMap<QString, QVector<SearchMatch>> m_all_results;

    // -- Log toggle switch --
    LogToggleSwitch* m_log_toggle{nullptr};

    // -- File Explorer placeholder sentinel --
    static constexpr const char* kPlaceholderText = "Loading...";
};

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

/// AdvancedSearchPanel must inherit QWidget.
static_assert(std::is_base_of_v<QWidget, AdvancedSearchPanel>,
              "AdvancedSearchPanel must inherit QWidget.");

/// AdvancedSearchPanel must not be copyable.
static_assert(!std::is_copy_constructible_v<AdvancedSearchPanel>,
              "AdvancedSearchPanel must not be copy-constructible.");

}  // namespace sak
