// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_explorer_panel.h
/// @brief File Management explorer tab with mounted and raw/image targets.

#pragma once

#include "sak/advanced_search_types.h"
#include "sak/file_explorer_command_bar.h"
#include "sak/file_explorer_command_registry.h"
#include "sak/file_explorer_details_pane.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_omnibar.h"
#include "sak/file_explorer_pane.h"
#include "sak/file_explorer_sidebar.h"
#include "sak/file_explorer_status_center.h"
#include "sak/file_explorer_storage_history.h"
#include "sak/file_explorer_transfer_worker.h"
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
#include <QSet>
#include <QSplitter>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include <functional>

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
class FileExplorerArchiveWorker;
class FileExplorerStatusCenterFlyout;
struct FileExplorerArchiveExtractItem;
struct FileExplorerArchiveRequest;

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

    /// Files StatusCenterViewModel: operation cards + toolbar badge state.
    [[nodiscard]] FileExplorerStatusCenterModel* statusCenterModel() const {
        return m_status_center;
    }

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
    void onCreateFileClicked();
    void onWriteFileClicked();
    void onRenameClicked();
    void onDeleteClicked();
    void onItemDoubleClicked(const QModelIndex& index);
    void onTableContextMenuRequested(const QPoint& position);
    void onTargetContextMenuRequested(const QPoint& position);
    /// Files FlattenFolderAction (setting-gated, one folder, confirmation).
    void flattenSelectedFolder();

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
    /// Inline omnibar command palette (Files Omnibar palette mode).
    void populateOmnibarPalette(const QString& needle);
    void executePaletteSuggestion(QListWidgetItem* item, const QString& typed);
    void rebuildSortMenu(QMenu* menu, bool include_grouping = true);
    void rebuildGroupMenu(QMenu* menu);
    void addGroupDirectionActions(QMenu* menu);
    void addGroupDateUnitActions(QMenu* menu);
    void applySortOrder(int column, Qt::SortOrder order);
    void addSortPlacementActions(QMenu* menu);
    void applyFolderSortPlacement(FileExplorerFolderSortPlacement placement);
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
    void connectOmnibarModeSignals();
    /// Files ShowStatusCenterButton click: toggles the status-center flyout
    /// anchored to the button's bottom-right corner.
    void toggleStatusCenterFlyout();
    void syncStatusCenterButton();
    bool handleFilterBoxKeyEvent(QEvent* event);
    bool dispatchFilteredEvent(QObject* watched, QEvent* event);
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
    /// What (if anything) occupies a destination child name. Unknown means the
    /// listing was not authoritative (a raw list failed or was truncated), so
    /// callers must fail closed rather than assume the name is free.
    enum class PasteEntryKind {
        None,
        File,
        Directory,
        Unknown
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
    /// One resolved paste destination: the final path plus whether an existing
    /// occupant must still be removed (deferred to just before that item's own
    /// copy, so an earlier cancel or failure leaves it untouched).
    struct PasteDestination {
        QString path;
        bool replace{false};
    };
    bool resolvePasteDestination(const FileManagementTarget& target,
                                 bool multiple,
                                 PasteCollisionPolicy* policy,
                                 PasteDestination* destination,
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
    bool crossPaneCopyOneEntry(const FileManagementTarget& source,
                               const FileManagementTarget& destination,
                               const QString& destination_dir,
                               const FileManagementEntry& entry,
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
    /// GUI-thread collision resolution before the worker starts (Files shows
    /// its conflict dialog up front): subfolder guards, same-folder duplicate
    /// naming, and the replace/skip/rename prompt.
    QList<FileExplorerTransferItem> resolveTransferItems(const PasteBatch& batch,
                                                         const QList<PasteItem>& items,
                                                         PasteCollisionPolicy* policy,
                                                         QStringList* blockers);
    /// Completion actions bound to one transfer worker run.
    struct TransferCompletion {
        FileExplorerHistoryOperation history_op{FileExplorerHistoryOperation::Copy};
        /// Status-center card family (Files FileOperationType).
        FileExplorerOperationType card_operation{FileExplorerOperationType::Copy};
        FileManagementTarget source_target;
        FileManagementTarget destination_target;
        QString destination_dir;
        /// Pre-translated "%1 of %2" status template for the final message.
        QString status_template;
        /// Title for the blockers dialog (defaults to Paste).
        QString failure_title;
        int requested_count{0};
        bool move{false};
        bool consume_clipboard{false};
        bool record_history{true};
    };
    /// Posts the in-progress status-center card, wires progress/cancel, and
    /// starts the transfer worker (Files FilesystemHelpers + StatusCenter).
    void startTransferWorker(const FileExplorerTransferRequest& request,
                             const TransferCompletion& completion);
    void finishTransferWorker(FileExplorerTransferWorker* worker,
                              FileExplorerStatusCenterItem* card,
                              const TransferCompletion& completion);
    [[nodiscard]] FileExplorerStatusCardRequest terminalTransferCard(
        const FileExplorerTransferWorker* worker,
        const TransferCompletion& completion,
        bool canceled) const;
    void applyTransferSideEffects(FileExplorerTransferWorker* worker,
                                  const TransferCompletion& completion,
                                  int written);
    /// Synchronous engine adapter for the short in-place legs (history,
    /// cross-pane copy); the paste path runs the same engine on the worker.
    bool transferEntrySync(const FileManagementTarget& source_target,
                           const PasteItem& item,
                           const FileManagementTarget& target,
                           const QString& destination,
                           QStringList* blockers);
    /// Copy Out / Export Folder: one item to a picked host destination on the
    /// worker, with explicit capped-read semantics for oversized raw files.
    void startExportWorker(const FileManagementTarget& source_target,
                           const FileExplorerTransferItem& item,
                           const QString& destination_dir);
    /// Undo/redo journal (Files StorageHistoryWrapper + StorageHistoryOperations).
    void executePasteTo(const FileManagementTarget& target,
                        const PasteSources& sources,
                        const QString& destination_dir);
    /// Sidebar drag targets (Files SidebarViewModel): drag-to-tag, drop onto
    /// a target row, and favorites drag-reorder; plus the drive Eject action.
    bool handleSidebarViewportEvent(QEvent* event);
    bool handleSidebarDragOver(QDropEvent* drop);
    bool handleSidebarDrop(QDropEvent* drop);
    bool handleSidebarTargetDrop(int target_index, QDropEvent* drop);
    bool filterPaneViewportEvent(QObject* watched, QEvent* event);
    [[nodiscard]] Qt::DropAction sidebarPasteAction(const FileManagementTarget& target,
                                                    const QDropEvent* drop) const;
    void applyDroppedTag(const QString& tag, const QMimeData* mime);
    void reorderFavorite(int from_position, int to_position);
    bool maybeStartFavoriteDrag(QEvent* event);
    [[nodiscard]] bool canEjectTarget(const FileManagementTarget& target) const;
    void ejectLocalTargetAtIndex(int target_index);
    void showReorderFavoritesDialog();
    void addSidebarGlobalMenuActions(QMenu* menu);
    void setSidebarCompact(bool compact);
    /// Files ShowCheckboxesWhenSelectingItems: selection checkboxes.
    [[nodiscard]] bool showCheckboxesEnabled() const;
    void setShowCheckboxes(bool enabled);
    void toggleSelectionForPath(FileExplorerPane* pane, const QString& path, bool checked);
    /// Files ShowFlattenOptions gate and the recursive flatten walk.
    [[nodiscard]] bool showFlattenOptionsEnabled() const;
    /// Files Settings page subset (Ctrl+, and the sidebar gear).
    void showExplorerSettings();
    void applyExplorerSettings(bool checkboxes,
                               bool double_click_up,
                               bool filter_header,
                               bool flatten,
                               int status_center_visibility);
    /// Files StatusCenterVisibility == DuringOngoingFileOperations.
    [[nodiscard]] bool statusCenterDuringOperationsOnly() const;
    int flattenFolderTree(const FileManagementTarget& target,
                          const QString& root,
                          const QString& current,
                          int depth,
                          QStringList* blockers);
    void removeEmptiedSubfolder(const FileManagementTarget& target,
                                const QString& path,
                                QStringList* blockers);
    void installSelectionCheckboxes(FileExplorerPane* pane);
    void appendViewToggleActions(QMenu* menu, const FileExplorerCommandContext& context);
    void recordHistory(FileExplorerHistoryOperation operation,
                       const FileManagementTarget& source_target,
                       const FileManagementTarget& destination_target,
                       QVector<FileExplorerHistoryItem> items);
    void undoLastOperation();
    void redoLastOperation();
    bool executeHistory(const FileExplorerStorageHistory& history, bool undo);
    bool executeHistoryTransfer(const FileExplorerStorageHistory& history, bool undo);
    bool resolveHistoryEndpoints(const FileExplorerStorageHistory& history,
                                 bool undo,
                                 FileManagementTarget* from_target,
                                 FileManagementTarget* to_target,
                                 bool* same_target);
    /// One reverse/replay leg: same-target moves rename, everything else runs
    /// the transfer kernel (with the moved source deleted afterwards).
    struct HistoryTransferLeg {
        const FileManagementTarget& from_target;
        const FileManagementTarget& to_target;
        bool move{false};
        bool same_target{false};
    };
    void applyHistoryTransferItem(const HistoryTransferLeg& leg,
                                  bool directory,
                                  const QString& from_path,
                                  const QString& to_path,
                                  QStringList* blockers);
    /// Files: a replayed history operation posts its own terminal card.
    void postHistoryCard(FileExplorerOperationType operation,
                         const QStringList& sources,
                         const QString& destination_dir,
                         bool ok);
    [[nodiscard]] static QString historyParentPath(const QString& path);
    bool undoByDeletingCreatedEntries(const FileExplorerStorageHistory& history,
                                      bool undo_of_create);
    bool redoCreateEntries(const FileExplorerStorageHistory& history);
    bool executeHistoryDelete(const FileExplorerStorageHistory& history,
                              bool created_entries,
                              QStringList* blockers);
    bool confirmHistoryDelete(int item_count, bool undo_of_create);
    void showMutationResult(const QString& title, const FileManagementMutationResult& result);
    [[nodiscard]] FileExplorerSelection currentSelection() const;
    [[nodiscard]] FileExplorerCommandContext commandContext() const;
    [[nodiscard]] bool selectionHasTags(const FileExplorerSelection& selection) const;
    void buildItemContextMenu(QMenu* menu, const FileExplorerCommandContext& context);
    void buildBackgroundContextMenu(QMenu* menu, const FileExplorerCommandContext& context);
    void addTagsSubmenu(QMenu* menu, const FileExplorerCommandContext& context);
    void toggleTagOnSelection(const QString& tag, bool add);
    bool propertiesRenameAllowed(const FileManagementTarget& target,
                                 const QString& captured_target_id,
                                 const QString& captured_directory,
                                 const QString& edited);
    void commitPropertiesRename(const QString& captured_target_id,
                                const QString& captured_directory,
                                const QString& original,
                                const QString& edited);
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
    void extractSelection(ExtractMode mode);
    [[nodiscard]] QList<FileExplorerArchiveExtractItem> extractArchiveItems(
        ExtractMode mode, const FileManagementTarget& target);
    /// Card skeleton shared by the in-progress and terminal archive cards
    /// (the zip codec reports no progress, so both are indeterminate).
    [[nodiscard]] FileExplorerStatusCardRequest archiveCardRequest(
        const FileExplorerArchiveRequest& request, FileExplorerReturnResult result) const;
    void startArchiveWorker(const FileExplorerArchiveRequest& request,
                            const QString& failure_title);
    void finishArchiveWorker(FileExplorerArchiveWorker* worker,
                             FileExplorerStatusCenterItem* card,
                             const QString& failure_title);
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
    bool dispatchArchiveCommand(FileExplorerCommandId command);
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
    /// Files FilterHeader row (Ctrl+Shift+F): filters the loaded listing.
    void buildFilterHeader(QVBoxLayout* layout);
    [[nodiscard]] bool showFilterHeaderEnabled() const;
    void toggleFilterHeader();
    void applyFilterHeaderText(const QString& text);
    void clearFolderFilterOnNavigation();
    /// Inline omnibar search mode (Files Omnibar search): recents, debounced
    /// live suggestions, and full submits into the listing.
    void startSearchWorker(const QString& query,
                           int max_results,
                           std::function<void(const QVector<SearchMatch>&)> on_matches,
                           std::function<void()> on_finished);
    void populateOmnibarSearch(const QString& text);
    void runSearchSuggestions(const QString& query);
    void submitExplorerSearch(const QString& query);
    void submitSearchSuggestion(QListWidgetItem* item, const QString& typed);
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
    FileExplorerStatusCenterModel* m_status_center{nullptr};
    FileExplorerStatusCenterFlyout* m_status_flyout{nullptr};
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
    // Files IsInfoPaneEnabled: the user's info-pane preference survives the
    // adaptive narrow-width hide so widening can bring the pane back.
    bool m_details_pane_enabled{true};
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
    // Favorites drag-reorder candidate: press position and pin index armed on
    // mouse-press over a favorites row (-1 = none).
    QPoint m_sidebar_press_pos;
    int m_sidebar_press_favorite{-1};
    // Undo/redo journal (Files StorageHistoryWrapper): per-panel, in-memory.
    FileExplorerStorageHistoryWrapper m_storage_history;
    // Per-batch capture of the paths the transfer kernel actually wrote
    // (collision-resolved), committed into one history entry per batch.
    QVector<FileExplorerHistoryItem> m_transfer_journal;
    // Single-flight guard (Files StorageHistoryHelpers semaphore): suppresses
    // recording while an undo/redo replays operations through the kernel.
    bool m_history_busy{false};
    // Most recent on-demand hash of a selected file, surfaced in the Evidence tab.
    QString m_last_hash_name;
    QString m_last_hash_sha256;
    bool m_last_hash_capped{false};
    // Live omnibar search worker (one per search; stopped on re-search/mode exit).
    AdvancedSearchWorker* m_search_worker{nullptr};
    // Entry name to select once the next listing arrives (open-search-result flow).
    QString m_pending_select_name;
    // Inline search-mode plumbing: 200 ms suggestion debounce (Files), the
    // pending query it fires with, and the accumulating full-search results.
    QTimer* m_search_suggest_timer{nullptr};
    QString m_pending_search_suggest;
    QVector<FileManagementEntry> m_search_result_entries;
    QSet<QString> m_search_result_paths;
    // Files FilterHeader row (Ctrl+Shift+F): filter box + 250 ms debounce.
    QWidget* m_filter_header{nullptr};
    QLineEdit* m_filter_box{nullptr};
    QTimer* m_filter_debounce{nullptr};
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
