// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_explorer_panel.h
/// @brief File Management explorer tab with mounted and raw/image targets.

#pragma once

#include "sak/file_explorer_command_bar.h"
#include "sak/file_explorer_command_registry.h"
#include "sak/file_explorer_details_pane.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_omnibar.h"
#include "sak/file_explorer_pane.h"
#include "sak/file_explorer_sidebar.h"
#include "sak/file_management_file_system.h"

#include <QAbstractItemView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QWidget>

class QJsonArray;
class QComboBox;
class QDialog;
class QMenu;
class QMimeData;
class QPoint;
class QAction;
class QToolButton;
class QVBoxLayout;
class QImage;
class QTabBar;

namespace sak {

class AdvancedSearchWorker;

class FileManagementExplorerPanel : public QWidget {
    Q_OBJECT

public:
    explicit FileManagementExplorerPanel(QWidget* parent = nullptr);
    ~FileManagementExplorerPanel() override;

    /// Turn on cross-restart tab persistence and immediately restore any saved
    /// session. Off by default so headless/unit construction never reads or
    /// writes the shared session store. The owning panel calls this once.
    void enableTabSessionPersistence();

    /// Scan @p evidence_root for live-certification report JSONs whose target path
    /// matches @p target_root_path; returns the matching report file paths.
    [[nodiscard]] static QStringList evidenceReportsForTarget(const QString& evidence_root,
                                                              const QString& target_root_path);

protected:
    void resizeEvent(QResizeEvent* event) override;

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onRefreshMountedTargets();
    void onScanDiskTargets();
    void onAddManualTarget();
    void onTargetChanged(int index);
    void onPathReturnPressed();
    void onBackClicked();
    void onForwardClicked();
    void onUpClicked();
    void onOpenSelected();
    void onCopyPathClicked();
    void onNewFolderClicked();
    void onWriteFileClicked();
    void onRenameClicked();
    void onDeleteClicked();
    void onItemDoubleClicked(const QModelIndex& index);
    void onTableContextMenuRequested(const QPoint& position);
    void onTargetContextMenuRequested(const QPoint& position);

private:
    void setupUi();
    void buildCommandAndNavBars(QWidget* center, QVBoxLayout* center_layout);
    void buildContentArea(QWidget* center, QVBoxLayout* center_layout);
    void connectUiSignals();
    void connectToolbarSignals();
    void connectNavigationSignals();
    void connectPaneSignals(FileExplorerPane* pane, int pane_index);
    void installCommandShortcuts();
    void setTargets(QVector<FileManagementTarget> targets);
    void appendTarget(const FileManagementTarget& target);
    void rebuildTargetList(const QString& preferred_target_id = {});
    void appendSidebarHeader(const QString& text);
    void appendSidebarTarget(const FileManagementTarget& target, int target_index);
    void selectTargetById(const QString& target_id);
    void rememberRecentTarget(const QString& target_id);
    void loadSidebarState();
    void saveSidebarState() const;
    void applyViewSettings();
    void loadViewSettingsForCurrentLocation();
    void saveViewSettings() const;
    void setExplorerViewMode(FileExplorerViewMode mode);
    [[nodiscard]] QAbstractItemView* currentItemView() const;
    [[nodiscard]] FileManagementTarget currentTarget() const;
    [[nodiscard]] int targetIndexForId(const QString& target_id) const;
    [[nodiscard]] QString selectedPath() const;
    [[nodiscard]] bool selectedIsDirectory() const;
    [[nodiscard]] QString targetPathForName(const QString& name) const;
    [[nodiscard]] bool validateCurrentTargetIdentity(QString* blocker) const;
    void loadDirectory(const QString& path, bool add_history = true);
    void loadColumnsPreview(const QString& path);
    void populateTable(const FileManagementListResult& result);
    void selectPendingSearchResult();
    void previewSelectedFile();
    void hashSelectedFile();
    void copySelectedFileOut();
    /// Clipboard file payload gathered for a paste: host (local) source files, or raw-target
    /// source items (path + size) tagged with the source target identity.
    struct PasteSources {
        QStringList host_files;
        QString source_target_id;
        QList<QPair<QString, quint64>> raw_items;
    };
    void copySelectionToClipboard();
    void pasteClipboardIntoCurrentFolder();
    void exportSelectedDirectoryOut(const FileManagementEntry& entry);
    void crossPaneCopySelection();
    int crossPaneCopyEntries(const FileManagementTarget& source,
                             const FileManagementTarget& destination,
                             const QString& destination_dir,
                             QStringList* blockers);
    void comparePanes();
    void refreshOtherPane();
    [[nodiscard]] FileManagementTarget otherPaneTarget() const;
    [[nodiscard]] bool clipboardHasPasteableFiles() const;
    [[nodiscard]] PasteSources collectPasteSources(const QMimeData* mime) const;
    static void appendPayloadItems(const QJsonArray& items,
                                   bool source_is_local,
                                   PasteSources& sources);
    [[nodiscard]] bool preparePasteDestination(const PasteSources& sources);
    void executePaste(const PasteSources& sources);
    [[nodiscard]] bool confirmTypedRawImport(const FileManagementTarget& target, int file_count);
    [[nodiscard]] bool confirmPasteOverwrite(const QString& name);
    int pasteHostFiles(const FileManagementTarget& target,
                       const QStringList& source_paths,
                       QStringList* blockers);
    int pasteRawItemsToLocalFolder(const FileManagementTarget& source_target,
                                   const QList<QPair<QString, quint64>>& items,
                                   QStringList* blockers);
    void showMutationResult(const QString& title, const FileManagementMutationResult& result);
    [[nodiscard]] FileExplorerSelection currentSelection() const;
    [[nodiscard]] FileExplorerCommandContext commandContext() const;
    void applyCommandState(QPushButton* button,
                           FileExplorerCommandId command,
                           const FileExplorerCommandContext& context);
    QAction* addCommandMenuAction(QMenu* menu,
                                  FileExplorerCommandId command,
                                  const FileExplorerCommandContext& context);
    void rebuildViewMenu(const FileExplorerCommandContext& context);
    void appendItemSizeMenuRow(QMenu* menu);
    void executeCommand(FileExplorerCommandId command);
    bool dispatchNavigationCommand(FileExplorerCommandId command);
    bool dispatchSelectionCommand(FileExplorerCommandId command);
    bool dispatchSelectionEditCommand(FileExplorerCommandId command);
    bool dispatchFileViewCommand(FileExplorerCommandId command);
    bool dispatchWriteCommand(FileExplorerCommandId command);
    bool dispatchOpenElsewhereCommand(FileExplorerCommandId command);
    void invertCurrentSelection();
    void toggleHiddenItems();
    void toggleFileExtensions();
    void showSelectedItemProperties();
    void editSelectedItemTags();
    void applyTagFilter(const QString& tag);
    void clearCurrentTagFilter();
    [[nodiscard]] QStringList tagsForSelectedItem() const;
    [[nodiscard]] QStringList allKnownTags() const;
    void togglePreviewPane();
    void resetListingForUnavailableTarget(const QString& message, bool is_error);
    int deleteSelectedEntries(const FileManagementTarget& target,
                              const FileExplorerSelection& selection,
                              QStringList* blockers,
                              QStringList* warnings);
    [[nodiscard]] int resolveSidebarTargetIndex(QListWidgetItem* item) const;
    void appendSidebarTargetsWhere(const QString& title,
                                   bool (*predicate)(const FileManagementTarget&));
    void appendSidebarTargetsById(const QString& title,
                                  const QStringList& target_ids,
                                  bool warn_when_missing = false);
    void appendStaleFavoriteRow(const QString& target_id);
    bool showStaleFavoriteContextMenu(const QPoint& position);
    void promptCurrentFolderFilter();
    /// Widgets of the omnibar search dialog, shared between builder and wiring.
    struct SearchDialogUi {
        QComboBox* query{nullptr};
        QListWidget* results{nullptr};
        QLabel* status{nullptr};
        QPushButton* search{nullptr};
        QPushButton* clear{nullptr};
        QPushButton* open{nullptr};
        QPushButton* open_location{nullptr};
    };
    void showExplorerSearchDialog();
    [[nodiscard]] SearchDialogUi buildSearchDialogUi(QDialog* dialog,
                                                     const FileManagementTarget& target) const;
    void startExplorerSearch(const QString& query, QListWidget* results, QLabel* status);
    void stopExplorerSearch();
    void openSearchResult(const QString& path, bool location_only);
    [[nodiscard]] QStringList searchHistory() const;
    void rememberSearchQuery(const QString& query);
    void showCommandPalette();
    void updateDetailsPane();
    [[nodiscard]] QString composeStatusText(const FileManagementTarget& target,
                                            const FileExplorerSelection& selection) const;
    void updatePreviewPane(const FileManagementTarget& target,
                           const FileExplorerSelection& selection);
    void showPreviewHint(const QString& message);
    void renderPreviewForEntry(const FileManagementEntry& entry, const QByteArray& bytes);
    void showImagePreviewForEntry(const FileManagementEntry& entry, const QImage& image);
    [[nodiscard]] QStringList buildDetailsProperties(const FileManagementTarget& target,
                                                     const FileExplorerSelection& selection) const;
    [[nodiscard]] QStringList buildDetailsSafety(const FileManagementTarget& target) const;
    [[nodiscard]] QStringList commandAvailabilityLines() const;
    [[nodiscard]] QStringList buildDetailsEvidence(const FileManagementTarget& target) const;
    static void appendEvidenceReportLinks(const FileManagementTarget& target,
                                          QStringList* evidence);
    [[nodiscard]] int resolveContextMenuTargetIndex(const QPoint& position);
    [[nodiscard]] QString favoriteActionLabel(int target_index, bool has_target) const;
    void openTargetAtIndex(int target_index);
    void copyTargetRootAtIndex(int target_index);
    void toggleFavoriteAtIndex(int target_index);
    void moveFavoriteAtIndex(int target_index, int delta);
    void clearRecentTargets();
    [[nodiscard]] bool isFavoriteTargetIndex(int target_index) const;
    void showTargetPropertiesAtIndex(int target_index);
    void updateActionButtons();
    void logMessage(const QString& message);
    void buildTabBar(QVBoxLayout* center_layout);
    /// @brief Give the tab bar's auto-generated close buttons accessible names.
    void nameTabCloseButtons();
    void ensureSecondPane();
    void activatePane(int index);
    void toggleDualPane();
    void togglePaneOrientation();
    void openSelectionInSecondPane();
    void highlightActivePane();
    [[nodiscard]] FileExplorerTabState captureCurrentTab() const;
    [[nodiscard]] QString tabTitleForCurrentLocation() const;
    void restoreTab(const FileExplorerTabState& tab);
    void restoreSecondaryPane(const FileExplorerTabState& tab);
    void updateActiveTabLabel();
    void openCurrentLocationInNewTab();
    void duplicateCurrentTab();
    void reopenClosedTab();
    void onTabSwitched(int index);
    void onTabCloseRequested(int index);
    [[nodiscard]] int findTargetIndexById(const QString& target_id) const;
    void saveTabSession() const;
    void restoreTabSession();

    FileExplorerSidebar* m_sidebar{nullptr};
    QTabBar* m_tab_bar{nullptr};
    FileExplorerCommandBar* m_command_bar{nullptr};
    FileExplorerOmnibar* m_omnibar{nullptr};
    FileExplorerPane* m_pane{nullptr};
    // Dual-pane: two physical panes in a splitter. m_pane always points at the ACTIVE pane
    // (m_pane_a or m_pane_b), so all existing single-pane logic operates on the active pane;
    // m_secondary_state holds the inactive pane's location/history/view, swapped on activation.
    FileExplorerPane* m_pane_a{nullptr};
    FileExplorerPane* m_pane_b{nullptr};
    QSplitter* m_pane_splitter{nullptr};
    FileExplorerPaneState m_secondary_state;
    int m_active_pane_index{0};
    bool m_dual_pane_enabled{false};
    bool m_tab_session_persistence{false};
    FileExplorerDetailsPane* m_details_pane{nullptr};
    QListWidget* m_target_list{nullptr};
    QSplitter* m_shell_splitter{nullptr};
    QPushButton* m_sidebar_toggle_button{nullptr};
    QPushButton* m_details_toggle_button{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QPushButton* m_scan_disks_button{nullptr};
    QPushButton* m_add_manual_button{nullptr};
    QLineEdit* m_path_edit{nullptr};
    QPushButton* m_back_button{nullptr};
    QPushButton* m_forward_button{nullptr};
    QPushButton* m_up_button{nullptr};
    QPushButton* m_search_button{nullptr};
    QPushButton* m_command_button{nullptr};
    QPushButton* m_open_button{nullptr};
    QPushButton* m_copy_path_button{nullptr};
    QPushButton* m_new_folder_button{nullptr};
    QPushButton* m_write_file_button{nullptr};
    QPushButton* m_rename_button{nullptr};
    QPushButton* m_delete_button{nullptr};
    QToolButton* m_view_button{nullptr};
    QLabel* m_summary_label{nullptr};
    QLabel* m_status_label{nullptr};
    QPlainTextEdit* m_preview_text{nullptr};
    QPlainTextEdit* m_properties_text{nullptr};
    QPlainTextEdit* m_safety_text{nullptr};
    QPlainTextEdit* m_evidence_text{nullptr};
    QTabWidget* m_details_tabs{nullptr};
    // Path of the file currently rendered in the preview pane, so selection churn does not
    // re-read the same file; empty when no single readable file is selected.
    QString m_last_preview_path;
    // Result of the most recent mutation, surfaced in the Evidence tab (path + hashes).
    FileManagementMutationResult m_last_mutation;
    // Most recent on-demand hash of a selected file, surfaced in the Evidence tab.
    QString m_last_hash_name;
    QString m_last_hash_sha256;
    bool m_last_hash_capped{false};
    // Live omnibar search worker (one per search; stopped on re-search/dialog close).
    AdvancedSearchWorker* m_search_worker{nullptr};
    // Entry name to select once the next listing arrives (open-search-result flow).
    QString m_pending_select_name;
    FileExplorerItemModel* m_item_model{nullptr};
    QVector<FileManagementTarget> m_targets;
    QString m_current_path{QStringLiteral("/")};
    FileExplorerPaneState m_pane_state;
    QStringList m_favorite_target_ids;
    QStringList m_recent_target_ids;
    QString m_last_target_id;
    quint64 m_listing_revision{0};
    quint64 m_columns_preview_revision{0};
    int m_current_target_index{-1};
    // Open explorer tabs; each carries an independent target+path+history+view via its primary
    // pane state. m_active_tab indexes the visible tab; m_restoring_tab suppresses the tab-switch
    // save/restore while a restore is already in flight.
    QVector<FileExplorerTabState> m_tabs;
    QVector<FileExplorerTabState> m_closed_tabs;  ///< LIFO stack of closed tabs for reopen.
    int m_active_tab{0};
    bool m_restoring_tab{false};
};

}  // namespace sak
