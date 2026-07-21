// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_panel.h
/// @brief Modern disk and partition manager panel.

#pragma once

#include "sak/partition_manager_controller.h"
#include "sak/widget_helpers.h"

#include <QColor>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

#include <memory>
#include <optional>

class QLabel;
class QListWidget;
class QMenu;
class QAbstractButton;
class QHBoxLayout;
class QToolButton;
class QVBoxLayout;
class QTableWidget;
class QTextEdit;
class QWidget;
class QPoint;

namespace sak {

class LogToggleSwitch;

class PartitionManagerPanel : public QWidget {
    Q_OBJECT

public:
    explicit PartitionManagerPanel(QWidget* parent = nullptr);
    ~PartitionManagerPanel() override = default;

    [[nodiscard]] LogToggleSwitch* logToggle() const { return m_logToggle; }

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
    void setTestInventoryForReview(const PartitionInventory& inventory);
    void queueTestOperationForReview(PartitionOperationType type, const PartitionTarget& target);
    [[nodiscard]] bool showApplyReviewDialogForTest();
#endif

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);
    void openBenchmarkRequested();
    void openImageFlasherRequested();

private Q_SLOTS:
    void refreshInventory();
    void applyQueue();
    void cancelApply();
    void dryRunQueue();
    void discardQueue();
    void undoQueue();
    void redoQueue();
    void onTableSelectionChanged();
    void onCreatePartition();
    void onDeletePartition();
    void onFormatPartition();
    void onSetDriveLetter();
    void onSetPartitionLabel();
    void onChangeApfsVolumeLabel();
    void onCheckFileSystem();
    void onInspectNonNativeFileSystem();
    void onBrowseNonNativeFileSystem();
    void onCheckNonNativeFileSystem();
    void onApfsRootFileMutation();
    void onSurfaceTest();
    void onSpaceAnalyzer();
    void onSetPartitionHidden();
    void onSetPartitionActive();
    void onSetPartitionTypeId();
    void onManageBitLocker();
    void onOpenOptimizeDrives();
    void onSsdSecureErase();
    void onOpenDiskBenchmark();
    void onOpenBootableMedia();
    void onInitializeDisk();
    void onDeleteAllPartitions();
    void onExploreSelected();
    void onShowProperties();
    void onResizePartition();
    void onAllocateFreeSpace();
    void onConvertPrimaryLogical();
    void onChangeVolumeSerialNumber();
    void onConvertDynamicDiskToBasic();
    void onExtendPartitionWizard();
    void onQuickPartition();
    void onMergePartitions();
    void onSplitPartition();
    void onConvertStyle();
    void onConvertFileSystem();
    void onChangeClusterSize();
    void onCloneDisk();
    void onCopyPartitionWizard();
    void onCreateImage();
    void onRestoreImage();
    void onDataRecovery();
    void onPartitionRecoveryWizard();
    void onMigrateOs();
    void onRepairBoot();
    void onOptimizeSsd();
    void onWipeSelected();
    void showPartitionContextMenu(const QPoint& position);
    void onExecutionFinished(const sak::PartitionExecutionResult& result);

private:
    enum class TableRowKind {
        Disk,
        Partition,
        Unallocated,
    };

    // Per-action enablement gates for a Partition Manager action link. Each bool tags the
    // button with a property that updateSpecialTargetButtonState() (partition_manager_panel.cpp)
    // routes to the matching enablement check; at most one of the routed flags should be set.
    struct ActionLinkOptions {
        // Target kinds the action applies to (disk / partition / volume / unallocated).
        QStringList target_kinds;
        // Action requires the selected volume to have a mounted drive letter.
        bool requires_drive_letter{false};
        // Action applies only to Windows-native file systems (NTFS/exFAT/FAT32).
        bool windows_native_filesystem{false};
        // Action is the filesystem resize/move path (extra ext-resize support gating).
        bool resize_filesystem{false};
        // Action inspects read-only non-native metadata (no write).
        bool inspect_non_native_filesystem{false};
        // Action browses a read-only non-native (HFS+/APFS/ext) file system.
        bool browse_non_native_filesystem{false};
        // Action runs the non-native check/repair path (fsck-class tools).
        bool check_non_native_filesystem{false};
        // Action queues an APFS container op (snapshot/resize) on a generated container.
        bool apfs_root_file_mutation{false};
        // Action is the unified "Change Label" verb (Windows-native volume label or a
        // generated APFS container's COW volume-label commit); enablement spans both paths.
        bool change_label{false};
    };

    struct ActionLinkSpec {
        QString text;
        QString icon_path;
        QString tooltip;
        void (PartitionManagerPanel::*slot)(){nullptr};
        ActionLinkOptions options;
    };

    void setupUi();
    void createRibbon(QVBoxLayout* root);
    QWidget* createActionsPane();
    void addActionsPaneTitle(QVBoxLayout* layout, QWidget* pane);
    void addActionSpecSection(QVBoxLayout* layout,
                              QWidget* pane,
                              const QString& title,
                              const QVector<ActionLinkSpec>& specs,
                              bool scroll_buttons = false);
    void addPendingOperationsSection(QVBoxLayout* layout, QWidget* pane);
    [[nodiscard]] ActionLinkSpec makeActionSpec(const QString& text,
                                                const QString& icon_path,
                                                const QString& tooltip,
                                                void (PartitionManagerPanel::*slot)(),
                                                const ActionLinkOptions& options) const;
    [[nodiscard]] QVector<ActionLinkSpec> wizardActionSpecs() const;
    [[nodiscard]] QVector<ActionLinkSpec> partitionOperationActionSpecs() const;
    [[nodiscard]] QVector<ActionLinkSpec> layoutOperationActionSpecs() const;
    [[nodiscard]] QVector<ActionLinkSpec> filesystemOperationActionSpecs() const;
    [[nodiscard]] QVector<ActionLinkSpec> nativeFilesystemActionSpecs() const;
    [[nodiscard]] QVector<ActionLinkSpec> nonNativeFilesystemActionSpecs() const;
    [[nodiscard]] QVector<ActionLinkSpec> maintenanceOperationActionSpecs() const;
    [[nodiscard]] QVector<ActionLinkSpec> advancedOperationActionSpecs() const;
    [[nodiscard]] QToolButton* createConfiguredActionLink(QWidget* parent,
                                                          const ActionLinkSpec& spec);
    QWidget* createWorkspace(QWidget* parent);
    QWidget* createDiskMapPane(QWidget* parent);
    QWidget* createLegend(QWidget* parent);
    QToolButton* createRibbonButton(QWidget* parent,
                                    const QString& text,
                                    const QString& icon_path,
                                    const QString& tooltip);
    QToolButton* createActionLink(QWidget* parent,
                                  const QString& text,
                                  const QString& icon_path,
                                  const QString& tooltip);
    void addSidebarSection(QVBoxLayout* layout,
                           const QString& title,
                           const QVector<QToolButton*>& buttons,
                           bool scroll_buttons = false);
    void addLegendItem(QHBoxLayout* layout, const QString& text, const QColor& color);
    void connectController();
    void rebuildTable(const PartitionInventory& inventory);
    void rebuildDiskMap(const PartitionInventory& inventory);
    void addDiskMapRow(QVBoxLayout* layout, const PartitionDiskInfo& disk);
    void addOrderedDiskMapSegments(QHBoxLayout* layout,
                                   const PartitionDiskInfo& disk,
                                   const std::optional<PartitionTarget>& selected);
    QWidget* createPartitionSegment(const PartitionDiskInfo& disk,
                                    const PartitionInfoEx& partition,
                                    const std::optional<PartitionTarget>& selected);
    QWidget* createUnallocatedSegment(const UnallocatedRegion& region,
                                      const std::optional<PartitionTarget>& selected);
    QWidget* createDiskTile(const PartitionDiskInfo& disk);
    void selectTargetInTable(const PartitionTarget& target);
    void addDiskRow(const PartitionDiskInfo& disk);
    void addPartitionRow(const PartitionDiskInfo& disk, const PartitionInfoEx& partition);
    void addUnallocatedRow(const UnallocatedRegion& region);
    void attachDiskMapContextMenu(QWidget* widget, const PartitionTarget& target);
    void rebuildQueue(const QVector<PartitionOperation>& operations);
    void updateActionState();
    void updateRefreshButtonState();
    void updateDetails();
    void queueOperation(PartitionOperationType type, const QJsonObject& payload = {});
    bool queueUnallocatedFreeSpace(const PartitionTarget& target, const PartitionDiskInfo& disk);
    bool queueAdjacentDonorFreeSpace(const PartitionTarget& target,
                                     const PartitionDiskInfo& disk,
                                     const PartitionInfoEx& partition);
    void queueQuickPartitionOperations(const PartitionDiskInfo& disk, const QJsonObject& options);
    void queueBitLockerUnlock(QJsonObject payload);
    [[nodiscard]] bool showApplyReviewDialog();
    void showSelectedTargetContextMenuAt(const QPoint& global_position);
    void addDiskContextMenuActions(QMenu& menu);
    void addPartitionContextMenuActions(QMenu& menu, bool has_drive_letter);
    void addPartitionLayoutContextMenuActions(QMenu& menu, const PartitionInfoEx* partition);
    void addPartitionFilesystemContextMenuActions(QMenu& menu, const PartitionInfoEx* partition);
    void addPartitionMaintenanceContextMenuActions(QMenu& menu, const PartitionInfoEx* partition);
    void addPartitionAdvancedContextMenuActions(QMenu& menu);
    void addUnallocatedContextMenuActions(QMenu& menu);

    [[nodiscard]] std::optional<PartitionTarget> selectedTarget() const;
    [[nodiscard]] const PartitionDiskInfo* selectedDisk() const;
    [[nodiscard]] const PartitionInfoEx* selectedPartition() const;
    [[nodiscard]] static QString flagsForPartition(const PartitionInfoEx& partition);
    [[nodiscard]] static QString rowKindName(TableRowKind kind);
    [[nodiscard]] static int usedPercent(const PartitionInfoEx& partition);
    [[nodiscard]] static uint64_t usedBytes(const PartitionInfoEx& partition);
    [[nodiscard]] static QString partitionLabel(const PartitionInfoEx& partition);
    [[nodiscard]] static QColor partitionColor(const PartitionDiskInfo& disk,
                                               const PartitionInfoEx& partition);
    [[nodiscard]] static QColor unallocatedColor();

    std::unique_ptr<PartitionManagerController> m_controller;
    PanelHeaderWidgets m_headerWidgets;
    QTableWidget* m_table{nullptr};
    QWidget* m_diskMapContainer{nullptr};
    QListWidget* m_queueList{nullptr};
    QTextEdit* m_details{nullptr};
    QLabel* m_pendingLabel{nullptr};
    QAbstractButton* m_refreshButton{nullptr};
    QAbstractButton* m_applyButton{nullptr};
    QAbstractButton* m_cancelButton{nullptr};
    QAbstractButton* m_dryRunButton{nullptr};
    QAbstractButton* m_discardButton{nullptr};
    QAbstractButton* m_undoButton{nullptr};
    QAbstractButton* m_redoButton{nullptr};
    QVector<QAbstractButton*> m_targetButtons;
    LogToggleSwitch* m_logToggle{nullptr};
    bool m_inventoryLoadStarted{false};
};

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
[[nodiscard]] QJsonObject partitionManagerAnalyzeSpaceForTest(const QString& rootPath);
#endif

}  // namespace sak
