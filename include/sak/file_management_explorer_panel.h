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
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QStringList>
#include <QUrl>
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
class QShortcut;
class QTemporaryDir;
class QAbstractItemView;
class QKeyEvent;
class QMouseEvent;
class QTimer;

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
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    void buildStatusRow(QVBoxLayout* root_layout);
    /// Right-click history flyout on Back/Forward (Files BackHistoryFlyout).
    void showHistoryMenu(bool back, const QPoint& global_pos);
    /// Steps the pane history several entries at once, then loads once.
    void jumpHistory(int steps, bool back);
    /// Sort flyout (Files ArrangementOptions): sort-by + direction.
    void rebuildSortMenu(QMenu* menu, bool include_grouping = true);
    void rebuildGroupMenu(QMenu* menu);
    void addGroupDirectionActions(QMenu* menu);
    void addGroupDateUnitActions(QMenu* menu);
    void applySortOrder(int column, Qt::SortOrder order);
    void applyGrouping(FileExplorerGroupOption option,
                       FileExplorerGroupDateUnit date_unit,
                       Qt::SortOrder order);
    /// Tab-actions flyout (Files TabStripHeader): split/arrange/close pane.
    void rebuildTabActionsMenu(QMenu* menu);
    void splitPane(Qt::Orientation orientation);
    /// Tab right-click menu (Files TabFlyout).
    void showTabContextMenu(const QPoint& point);
    /// direction: -1 close left of index, +1 close right, 0 close others.
    void closeTabsRelative(int index, int direction);
    /// Files status bar col0: "N items | N selected | size".
    void updateStatusCounts();
    /// Sidebar section visibility (Files SidebarContextMenu toggles).
    [[nodiscard]] bool sidebarSectionVisible(const QString& section_id) const;
    void setSidebarSectionVisible(const QString& section_id, bool visible);
    void addSidebarSectionToggleMenu(QMenu* parent_menu);
    void appendVisibleSidebarSections();
    void appendTagRows();
    void installAuxiliaryShortcuts();
    void installFilesAliasShortcuts();
    void installTabShortcuts();
    void installPaneShortcuts();
    QShortcut* addPanelShortcut(const QString& key_sequence);
    void cycleTab(int direction);
    void selectTabByNumber(int digit);
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
    /// One transferable item: a host (local) path, or a raw-target path with its listed
    /// size and directory flag.
    struct PasteItem {
        QString path;
        quint64 size_bytes{0};
        bool directory{false};
    };
    /// What (if anything) occupies a destination child name.
    enum class PasteEntryKind {
        None,
        File,
        Directory
    };
    /// Clipboard payload gathered for a paste: host (local) source paths, or raw-target
    /// source items tagged with the source target identity. `move` mirrors the
    /// Files DataPackage RequestedOperation (cut = Move, copy = Copy). `clipboard`
    /// is false for drag-drop payloads, whose completed move must not consume the
    /// user's clipboard.
    struct PasteSources {
        QStringList host_files;
        QString source_target_id;
        QList<PasteItem> raw_items;
        bool move{false};
        bool clipboard{true};
    };
    /// Files FileNameConflictResolveOptionType (GenerateNewName default).
    enum class PasteCollisionChoice {
        GenerateNew,
        Replace,
        Skip
    };
    struct PasteCollisionPolicy {
        PasteCollisionChoice choice{PasteCollisionChoice::GenerateNew};
        bool apply_to_all{false};
        bool cancelled{false};
    };
    /// Selection snapshot serialized for the clipboard payload.
    struct ClipboardBatch {
        QJsonArray items;
        QList<QUrl> urls;
        QStringList lines;
        int skipped{0};
    };
    static ClipboardBatch collectClipboardBatch(const FileExplorerSelection& selection,
                                                const FileManagementTarget& target);
    void copySelectionToClipboard(bool move = false);
    void pasteClipboardIntoCurrentFolder();
    void pasteClipboardIntoSelection();
    void pasteClipboardTo(const QString& destination_dir);
    bool pasteSameTargetMove(const FileManagementTarget& target,
                             const PasteSources& sources,
                             const QString& destination_dir);
    int moveEntriesWithinTarget(const FileManagementTarget& target,
                                const QList<PasteItem>& items,
                                const QString& destination_dir,
                                PasteCollisionPolicy* policy,
                                QStringList* blockers);
    PasteCollisionChoice resolvePasteCollision(const QString& name,
                                               bool multiple,
                                               PasteCollisionPolicy* policy);
    bool resolvePasteDestination(const FileManagementTarget& target,
                                 bool multiple,
                                 PasteCollisionPolicy* policy,
                                 QString* destination,
                                 QStringList* blockers);
    [[nodiscard]] QString uniqueChildName(const FileManagementTarget& target,
                                          const QString& directory,
                                          const QString& name) const;
    [[nodiscard]] QString availableChildName(const FileManagementTarget& target,
                                             const QString& directory,
                                             const QString& name) const;
    [[nodiscard]] PasteEntryKind destinationEntryKind(const FileManagementTarget& target,
                                                      const QString& directory,
                                                      const QString& name) const;
    [[nodiscard]] bool destinationOccupied(const FileManagementTarget& target,
                                           const QString& directory,
                                           const QString& name) const;
    void clearCutMarks();
    void finishMovePaste();
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
    [[nodiscard]] static bool mimeHasPasteableItems(const QMimeData* mime);
    [[nodiscard]] PasteSources collectPasteSources(const QMimeData* mime) const;
    QMimeData* buildDragMimeData(int pane_index, const QList<int>& rows);
    bool handleViewportDragEvent(QAbstractItemView* view, QEvent* event);
    void handleDropOnView(QAbstractItemView* view, QEvent* event);
    [[nodiscard]] Qt::DropAction dropActionFor(Qt::KeyboardModifiers modifiers,
                                               const QMimeData* mime) const;
    void performDrop(const PasteSources& sources, const QString& destination_dir);
    void armSpringOpen(const QString& directory_path);
    void cancelSpringOpen();
    [[nodiscard]] int paneIndexForView(const QAbstractItemView* view) const;
    static void appendPayloadItems(const QJsonArray& items,
                                   bool source_is_local,
                                   PasteSources& sources);
    [[nodiscard]] bool preparePasteDestination(const PasteSources& sources);
    void executePaste(const PasteSources& sources, const QString& destination_dir);
    [[nodiscard]] bool confirmTypedRawImport(const FileManagementTarget& target, int file_count);
    [[nodiscard]] static QList<PasteItem> pasteItemsFor(const PasteSources& sources);
    /// Shared context for one paste batch: the transfer endpoints plus flags
    /// computed once and applied to every item.
    struct PasteBatch {
        const FileManagementTarget& source_target;
        const FileManagementTarget& target;
        QString destination_dir;
        bool move{false};
        bool multiple{false};
        /// Source and destination paths share a namespace (both host paths, or the
        /// same raw target); only then can a folder contain its own destination.
        bool same_namespace{false};
    };
    int pasteItemsToFolder(const PasteBatch& batch,
                           const QList<PasteItem>& items,
                           PasteCollisionPolicy* policy,
                           QStringList* blockers);
    bool pasteOneItem(const PasteBatch& batch,
                      const PasteItem& item,
                      PasteCollisionPolicy* policy,
                      QStringList* blockers);
    bool transferEntry(const FileManagementTarget& source_target,
                       const PasteItem& item,
                       const FileManagementTarget& target,
                       const QString& destination,
                       QStringList* blockers);
    bool transferEntryFromHost(const PasteItem& item,
                               const FileManagementTarget& target,
                               const QString& destination,
                               QStringList* blockers);
    bool transferRawEntryToLocal(const FileManagementTarget& source_target,
                                 const PasteItem& item,
                                 const QString& destination,
                                 QStringList* blockers);
    bool transferRawEntryStaged(const FileManagementTarget& source_target,
                                const PasteItem& item,
                                const FileManagementTarget& target,
                                const QString& destination,
                                QStringList* blockers);
    bool deleteMoveSource(const FileManagementTarget& source_target,
                          const PasteItem& item,
                          QStringList* blockers);
    void showMutationResult(const QString& title, const FileManagementMutationResult& result);
    [[nodiscard]] FileExplorerSelection currentSelection() const;
    [[nodiscard]] FileExplorerCommandContext commandContext() const;
    [[nodiscard]] bool selectionHasTags(const FileExplorerSelection& selection) const;
    void buildItemContextMenu(QMenu* menu, const FileExplorerCommandContext& context);
    void buildBackgroundContextMenu(QMenu* menu, const FileExplorerCommandContext& context);
    void addTagsSubmenu(QMenu* menu, const FileExplorerCommandContext& context);
    void toggleTagOnSelection(const QString& tag, bool add);
    void commitPropertiesRename(const QString& original, const QString& edited);
    void removeTagsFromSelection();
    void createFolderWithSelection();
    void openTerminalHere();
    void editSelectionInNotepad();
    /// Files decompress legs: the Ctrl+E dialog, extract-here, smart (skip a
    /// redundant single top-level folder), and always-wrap subfolder.
    enum class ExtractMode {
        Dialog,
        Here,
        Smart,
        ChildFolder
    };
    [[nodiscard]] QString selectionArchiveBaseName() const;
    void compressSelectionToZip();
    QStringList compressSourcePaths(const FileManagementTarget& target,
                                    const QTemporaryDir& staging,
                                    QStringList* blockers);
    void extractSelection(ExtractMode mode);
    bool extractOneArchive(ExtractMode mode,
                           const FileManagementEntry& entry,
                           const QString& staging_dir,
                           QStringList* blockers);
    bool extractArchiveViaDialog(const FileManagementTarget& target,
                                 const FileManagementEntry& entry,
                                 const QString& host_zip,
                                 QStringList* blockers);
    [[nodiscard]] static bool extractionNeedsWrapFolder(ExtractMode mode, const QString& host_zip);
    QString stageEntryToHost(const FileManagementTarget& target,
                             const FileManagementEntry& entry,
                             const QString& staging_dir,
                             QStringList* blockers);
    bool deliverExtractedTree(const FileManagementTarget& target,
                              const QString& host_out_dir,
                              const QString& wrap_name,
                              QStringList* blockers);
    void addArchiveSubmenus(QMenu* menu, const FileExplorerCommandContext& context);
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
    bool dispatchCopyPathCommand(FileExplorerCommandId command);
    bool dispatchSelectionCommand(FileExplorerCommandId command);
    bool dispatchSelectionEditCommand(FileExplorerCommandId command);
    bool dispatchFileViewCommand(FileExplorerCommandId command);
    bool dispatchWriteCommand(FileExplorerCommandId command);
    bool dispatchSelectionToolCommand(FileExplorerCommandId command);
    bool dispatchOpenElsewhereCommand(FileExplorerCommandId command);
    void invertCurrentSelection();
    void toggleCurrentItemSelection();
    void stepLayoutSize(int direction);
    void performInlineRename(int row, const QString& new_name);
    void activatePaneForView(QAbstractItemView* view);
    bool handleViewKeyPress(QAbstractItemView* view, QKeyEvent* key);
    bool handleViewportMouseEvent(QAbstractItemView* view, QEvent* event);
    bool handleViewportMousePress(QAbstractItemView* view, const QMouseEvent* mouse);
    [[nodiscard]] bool doubleClickToGoUpEnabled() const;
    void openPathInNewTab(const QString& path);
    void openSelectedFoldersInNewTabs();
    void handleRenameTapEvent(QAbstractItemView* view, QEvent* event);
    void handleRenameTapRelease(QAbstractItemView* view, const QMouseEvent* mouse);
    void armRenameTapCandidate(QAbstractItemView* view, const QMouseEvent* mouse);
    void cancelRenameTap();
    void onRenameTapTimeout();
    void toggleHiddenItems();
    void toggleFileExtensions();
    void showSelectedItemProperties();
    void editSelectedItemTags();
    void applyTagFilter(const QString& tag);
    void clearCurrentTagFilter();
    [[nodiscard]] QStringList tagsForSelectedItem() const;
    [[nodiscard]] QStringList allKnownTags() const;
    void installTagProvider(FileExplorerItemModel* model);
    void installIconProvider(FileExplorerItemModel* model);
    void togglePreviewPane();
    void resetListingForUnavailableTarget(const QString& message, bool is_error);
    int deleteSelectedEntries(const FileManagementTarget& target,
                              const FileExplorerSelection& selection,
                              QStringList* blockers,
                              QStringList* warnings);
    int recycleSelectedEntries(const FileExplorerSelection& selection, QStringList* blockers);
    [[nodiscard]] QString deleteConfirmationText(bool recycle,
                                                 const FileManagementTarget& target,
                                                 const FileExplorerSelection& selection) const;
    void deleteSelectionWithConfirmation(bool permanent);
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
    void showExplorerSearchDialog(const QString& initial_query = {});
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
    QLineEdit* m_search_box{nullptr};
    QPushButton* m_search_button{nullptr};
    QPushButton* m_command_button{nullptr};
    QPushButton* m_rename_button{nullptr};
    QPushButton* m_delete_button{nullptr};
    QToolButton* m_view_button{nullptr};
    QLabel* m_summary_label{nullptr};
    QLabel* m_items_count_label{nullptr};
    QLabel* m_selection_count_label{nullptr};
    QLabel* m_status_label{nullptr};
    QPlainTextEdit* m_preview_text{nullptr};
    QPlainTextEdit* m_properties_text{nullptr};
    QPlainTextEdit* m_safety_text{nullptr};
    QPlainTextEdit* m_evidence_text{nullptr};
    // Slow-double-click inline rename (Files tapDebounceTimer, 1500 ms): a
    // second left-click on the already-selected item's name arms the timer;
    // opening (double-click) or selection churn cancels it.
    QTimer* m_rename_tap_timer{nullptr};
    QPersistentModelIndex m_rename_tap_candidate;
    QPointer<QAbstractItemView> m_rename_tap_view;
    // Drag spring-open (Files SpringLoaded timer, 1300 ms): hovering a folder
    // during a drag navigates into it; leaving or dropping cancels the timer.
    QTimer* m_spring_open_timer{nullptr};
    QString m_spring_open_path;
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
