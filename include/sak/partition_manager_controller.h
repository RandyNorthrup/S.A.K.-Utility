// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_controller.h
/// @brief Controller for Partition Manager UI.

#pragma once

#include "sak/partition_executor.h"
#include "sak/partition_file_system_tool_runner.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_operation_queue.h"
#include "sak/storage_inventory_worker.h"

#include <QFutureWatcher>
#include <QObject>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace sak {

class PartitionManagerController : public QObject {
    Q_OBJECT

public:
    explicit PartitionManagerController(QObject* parent = nullptr);
    ~PartitionManagerController() override;

    [[nodiscard]] const PartitionInventory& inventory() const noexcept;
    [[nodiscard]] const PartitionOperationQueue& queue() const noexcept;
    [[nodiscard]] PartitionManagerState state() const noexcept;
    [[nodiscard]] bool interactionBlocked() const noexcept;
    /// Whether a cancel can stop the running work now: false while a step is in a phase Windows
    /// finishes on its own (see windowsPhase()), which no cancel can stop.
    [[nodiscard]] bool canCancel() const noexcept;
    /// The step Windows is carrying out on its own right now, if any.
    [[nodiscard]] const std::optional<WindowsContinuedPhase>& windowsPhase() const noexcept;
    [[nodiscard]] PartitionQueueReviewToken queueReviewToken() const;

    // --- Pure reporting seams (no instance state; unit-tested without a live scan) ---
    /// @brief Stable state after a scan: Failed for no disks, ReadyPartial for any incomplete
    ///        metadata, and Ready only for an authoritative warning-free inventory.
    [[nodiscard]] static PartitionManagerState inventoryReadyState(
        const PartitionInventory& inventory);
    /// @brief Status line for a completed scan, including empty and partial scan reasons.
    [[nodiscard]] static QString inventoryStatusMessage(const PartitionInventory& inventory);

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
    using InventoryScanner = std::function<StorageInventoryScanResult(const CancelCheck&)>;
    using InventoryScannerWithProgress = std::function<StorageInventoryScanResult(
        const CancelCheck&, const InventoryScanProgressCallback&)>;
    using ApplyExecutor = std::function<PartitionExecutionResult(
        const QVector<PartitionOperation>&, bool, bool, const CancelCheck&)>;
    using FileSystemCheckRunner = std::function<PartitionFileSystemToolRunResult(
        const PartitionFileSystemReadOnlyCheckRequest&, const CancelCheck&)>;
#endif

public Q_SLOTS:
    void refreshInventory();
    /// The same refresh, read with administrator rights: in the helper (one UAC prompt for the
    /// session) when this process has none. For the state a standard scan cannot read.
    /// @return false when an operation is running and nothing was started.
    bool refreshInventoryWithAdministratorRights();
    bool queueOperation(PartitionOperationType type,
                        const PartitionTarget& target,
                        const QJsonObject& payload = {});
    bool queueOperations(const QVector<PartitionOperationRequest>& requests);
    void undo();
    void redo();
    void discardQueue();
    bool applyQueue(const PartitionQueueReviewToken& reviewed,
                    bool dry_run = false,
                    bool use_elevation = true);
    /// Cancel the running work. Refused, with a status message, while a step is in a phase
    /// Windows finishes on its own: cancelling then would stop nothing that is under way.
    void cancel();
    /// Stop everything that can be stopped because the application is closing: the running
    /// step's script and everything after it. A phase Windows finishes on its own goes on, and the
    /// batch's result names that step as possibly still running in Windows.
    void cancelForExit();
    void runReadOnlyFileSystemCheck(const QString& file_system, const QString& target_path);
    /// @brief Run one read-only operation now instead of queueing it for Apply.
    /// A scan changes nothing on the disk, so making the user queue it and then press Apply is
    /// ceremony. Only operations the planner classifies as read-only are accepted, after the
    /// same safety validation a queued operation gets; anything else is refused and never runs.
    /// The queue is left untouched and no preflight/verification inventory scan runs. The result
    /// arrives through executionFinished like an applied batch.
    /// @return true when the run was started.
    bool runReadOnlyOperation(PartitionOperationType type,
                              const PartitionTarget& target,
                              const QJsonObject& payload = {});

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
    void setTestInventory(const PartitionInventory& inventory);
    // Drives the state machine directly so a test can assert the panel's running-operation
    // guard without dispatching a real (destructive) apply.
    void setTestState(PartitionManagerState state);
    void setInventoryScannerForTesting(InventoryScanner scanner);
    void setInventoryScannerWithProgressForTesting(InventoryScannerWithProgress scanner);
    void setApplyInventoryScannerForTesting(InventoryScanner scanner);
    void setApplyExecutorForTesting(ApplyExecutor executor);
    void setFileSystemCheckRunnerForTesting(FileSystemCheckRunner runner);
    /// Reports a Windows-continued phase as the executor's signals do, for an apply run by a
    /// test executor; nothing means the phase ended.
    void reportWindowsPhaseForTesting(const std::optional<WindowsContinuedPhase>& phase);
#endif

Q_SIGNALS:
    void inventoryChanged(const sak::PartitionInventory& inventory);
    void queueChanged(const QVector<sak::PartitionOperation>& operations);
    void stateChanged(sak::PartitionManagerState state);
    void statusMessage(const QString& message, int timeout_ms);
    /// A request was checked and not added to the queue, for these reasons.
    void queueRefused(const QStringList& reasons);
    /// Apply read the disks again and refused to start, for these reasons. Nothing was written.
    void applyRefused(const QStringList& reasons);
    void progressUpdate(int current, int maximum);
    /// A scan reached this disk, of this many.
    void inventoryScanProgress(int disk, int disks);
    void logOutput(const QString& message);
    void executionFinished(const sak::PartitionExecutionResult& result);
    /// A step began or left a phase Windows finishes on its own; read windowsPhase().
    void windowsPhaseChanged();
    void fileSystemCheckFinished(const sak::PartitionFileSystemToolRunResult& result);

private:
    enum class InventoryScanPurpose {
        None,
        Refresh,
        ApplyPreflight,
        ApplyVerification
    };

    struct PendingApplyRequest {
        bool dry_run{false};
        bool use_elevation{true};
        PartitionQueueReviewToken reviewed;
        // A single read-only operation run outside the queue: nothing on disk changes, so its
        // result is published without a verification scan and the queue is not discarded.
        bool read_only_immediate{false};
    };

    PartitionOperationPlanner m_planner;
    PartitionOperationQueue m_queue;
    std::shared_ptr<PartitionExecutor> m_apply_executor;
    PartitionInventory m_inventory;
    PartitionInventory m_apply_before_inventory;
    QFutureWatcher<StorageInventoryScanResult>* m_inventory_watcher{nullptr};
    QFutureWatcher<PartitionExecutionResult>* m_apply_watcher{nullptr};
    QFutureWatcher<PartitionFileSystemToolRunResult>* m_file_system_check_watcher{nullptr};
    PartitionManagerState m_state{PartitionManagerState::Idle};
    InventoryScanPurpose m_inventory_scan_purpose{InventoryScanPurpose::None};
    std::shared_ptr<std::atomic_bool> m_inventory_cancel;
    std::shared_ptr<std::atomic_bool> m_apply_cancel;
    std::shared_ptr<std::atomic_bool> m_file_system_check_cancel;
    std::optional<PendingApplyRequest> m_pending_apply_request;
    std::optional<PendingApplyRequest> m_active_apply_request;
    std::optional<PartitionExecutionResult> m_pending_execution_result;
    std::optional<WindowsContinuedPhase> m_windows_phase;
    std::function<StorageInventoryScanResult(const CancelCheck&,
                                             const InventoryScanProgressCallback&)>
        m_inventory_scanner;
    std::function<StorageInventoryScanResult(const CancelCheck&,
                                             const InventoryScanProgressCallback&)>
        m_apply_inventory_scanner;
    std::function<PartitionExecutionResult(
        const QVector<PartitionOperation>&, bool, bool, const CancelCheck&)>
        m_apply_executor_for_testing;
    std::function<PartitionFileSystemToolRunResult(const PartitionFileSystemReadOnlyCheckRequest&,
                                                   const CancelCheck&)>
        m_file_system_check_runner;

    [[nodiscard]] bool rejectQueueMutationIfBusy();
    /// Running work a cancel request reaches, whatever phase it is in.
    [[nodiscard]] bool hasCancellableWork() const noexcept;
    void requestCancellation();
    void setWindowsPhase(const std::optional<WindowsContinuedPhase>& phase);
    [[nodiscard]] PartitionManagerState stableState() const;
    [[nodiscard]] static bool inventoryIsAuthoritative(const PartitionInventory& inventory);
    [[nodiscard]] static QString inventoryFailureMessage(const PartitionInventory& inventory);
    [[nodiscard]] bool revalidateQueuedOperations(const PartitionInventory& inventory,
                                                  QVector<PartitionOperation>* operations);
    [[nodiscard]] bool reviewTokenMatches(const PartitionQueueReviewToken& reviewed) const;
    static void bindDiskIdentity(const PartitionInventory& inventory,
                                 PartitionOperation* operation);
    static void bindRawPayloadTargetIdentity(const PartitionInventory& inventory,
                                             PartitionOperation* operation);
    static void bindBackupVolumeIdentity(PartitionOperation* operation);
    [[nodiscard]] static QString freshDiskIdentityBlocker(const PartitionInventory& inventory,
                                                          const PartitionOperation& operation);
    [[nodiscard]] static QString freshBackupIdentityBlocker(const PartitionOperation& operation);
    [[nodiscard]] static QString freshRawPayloadTargetIdentityBlocker(
        const PartitionInventory& inventory, const PartitionOperation& operation);
    [[nodiscard]] static bool projectQueuedOperation(PartitionInventory* inventory,
                                                     const PartitionOperation& operation);
    void startInventoryScan(InventoryScanPurpose purpose, bool with_elevation = false);
    void finishInventoryScan(QFutureWatcher<StorageInventoryScanResult>* watcher);
    [[nodiscard]] PartitionManagerState finishRefresh(PartitionInventory inventory, bool cancelled);
    [[nodiscard]] std::optional<PartitionManagerState> finishApplyPreflight(
        PartitionInventory inventory, bool cancelled);
    void startApplyExecution(const QVector<PartitionOperation>& operations,
                             const PendingApplyRequest& request);
    void finishApplyExecution(QFutureWatcher<PartitionExecutionResult>* watcher);
    [[nodiscard]] PartitionManagerState finishApplyVerification(PartitionInventory inventory,
                                                                bool cancelled);
    [[nodiscard]] PartitionManagerState publishExecutionResult(
        PartitionExecutionResult result, const PartitionInventory& after_inventory);
    void publishInventoryDiagnostics(const PartitionInventory& inventory);
    void setState(PartitionManagerState state);
    void emitQueueChanged();
};

}  // namespace sak
