// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_controller.h
/// @brief Orchestrates uninstall pipeline, manages queue, and persists settings

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

#include <memory>
#include <type_traits>

class AdvancedUninstallControllerTests;

namespace sak {

class ProgramEnumerator;
class UninstallWorker;
class CleanupWorker;
class RestorePointManager;

/// @brief Orchestrates the uninstall pipeline and manages application state
///
/// Activity state, not one linear pipeline: enumeration runs Idle -> Enumerating -> Idle and stays
/// Enumerating while a cancelled worker completes its cooperative stop;
/// uninstall runs Idle -> Uninstalling, may enter LeftoverScanning, and returns to Idle or enters
/// optional Cleaning before Idle. Manual cleanup runs Idle -> Cleaning -> Idle. Batch uninstall
/// alternates Uninstalling/LeftoverScanning per item and may finish through Cleaning.
class AdvancedUninstallController : public QObject {
    Q_OBJECT

    friend class ::AdvancedUninstallControllerTests;

public:
    /// @brief Controller state machine states
    enum class State {
        Idle,
        Enumerating,
        Uninstalling,
        LeftoverScanning,
        Cleaning
    };

    explicit AdvancedUninstallController(QObject* parent = nullptr);
    ~AdvancedUninstallController() override;

    AdvancedUninstallController(const AdvancedUninstallController&) = delete;
    AdvancedUninstallController& operator=(const AdvancedUninstallController&) = delete;
    AdvancedUninstallController(AdvancedUninstallController&&) = delete;
    AdvancedUninstallController& operator=(AdvancedUninstallController&&) = delete;

    /// @brief Get current controller state
    [[nodiscard]] State currentState() const;

    // -- Program Enumeration --

    /// @brief Start enumerating all installed programs
    void refreshPrograms();

    /// @brief Get the last enumeration result
    [[nodiscard]] QVector<ProgramInfo> programs() const;

    // -- Uninstall Operations --

    /// @brief Uninstall a single program
    void uninstallProgram(const ProgramInfo& program,
                          ScanLevel scan_level,
                          bool create_restore_point,
                          bool auto_clean_safe);

    /// @brief Force uninstall a program (skip native uninstaller)
    void forceUninstall(const ProgramInfo& program,
                        ScanLevel scan_level,
                        bool create_restore_point);

    /// @brief Remove only the registry entry (orphaned program)
    void removeRegistryEntry(const ProgramInfo& program);

    /// @brief Clean selected leftover items
    void cleanLeftovers(const QVector<LeftoverItem>& selected_items);

    /// @brief True if @p item's type can go to the Recycle Bin (File/Folder or a file-shortcut
    ///        StartupEntry). Registry keys/values, services, tasks, firewall rules, shell
    ///        extensions have NO Recycle Bin and are IRREVERSIBLE -- auto-clean must never
    ///        auto-delete them.
    [[nodiscard]] static bool isRecoverableLeftoverType(const LeftoverItem& item);

    /// @brief Items eligible for AUTOMATIC (no human review) cleanup: Safe AND recoverable-type,
    ///        each marked selected. Safe-but-irreversible items are deliberately excluded so an
    ///        automatic delete can never be unrecoverable.
    [[nodiscard]] static QVector<LeftoverItem> autoCleanableLeftovers(
        const QVector<LeftoverItem>& items);

    /// @brief Cancel the current operation
    void cancelOperation();

    // -- Batch Uninstall --

    /// @brief Add one unique program to the bounded batch queue.
    /// @return true when added; false with statusMessage when refused.
    [[nodiscard]] bool addToQueue(const ProgramInfo& program,
                                  ScanLevel scan_level,
                                  bool auto_clean_safe);

    /// @brief Remove an item from the batch queue.
    /// @return true when @p index named and removed an item; false with statusMessage otherwise.
    [[nodiscard]] bool removeFromQueue(int index);

    /// @brief Get the current queue
    [[nodiscard]] QVector<UninstallQueueItem> queue() const;

    /// @brief Clear the batch queue.
    /// @return true when cleared; false with statusMessage when a batch owns the queue.
    [[nodiscard]] bool clearQueue();

    /// @brief Start processing the batch queue.
    /// @return true once the first queued worker is started; false when validation or required
    ///         restore-point creation refuses the batch. Restore failure also emits batchFailed.
    [[nodiscard]] bool startBatchUninstall(bool create_restore_point);

    // -- Settings --

    /// @brief Load settings from ConfigManager.
    /// @return true only when the store and every persisted value were healthy.
    [[nodiscard]] bool loadSettings();

    /// @brief Save and sync settings through ConfigManager.
    /// @return true only when the store was healthy and the disk sync succeeded.
    [[nodiscard]] bool saveSettings();

    /// @brief Get the auto-restore-point preference
    [[nodiscard]] bool autoRestorePoint() const;
    void setAutoRestorePoint(bool enabled);

    /// @brief Get the auto-clean-safe preference
    [[nodiscard]] bool autoCleanSafe() const;
    void setAutoCleanSafe(bool enabled);

    /// @brief Get the default scan level
    [[nodiscard]] ScanLevel defaultScanLevel() const;
    void setDefaultScanLevel(ScanLevel level);

    /// @brief Get the show-system-components preference
    [[nodiscard]] bool showSystemComponents() const;
    void setShowSystemComponents(bool show);

    /// @brief Get the use-recycle-bin preference
    [[nodiscard]] bool useRecycleBin() const;
    void setUseRecycleBin(bool enabled);

    /// @brief Get the select-all-by-default preference
    [[nodiscard]] bool selectAllByDefault() const;
    void setSelectAllByDefault(bool enabled);

Q_SIGNALS:
    /// @brief State changed
    void stateChanged(sak::AdvancedUninstallController::State new_state);

    /// @brief Programs enumeration started
    void enumerationStarted();

    /// @brief Programs enumeration progress
    void enumerationProgress(int current, int total);

    /// @brief Programs enumeration completed
    void enumerationFinished(QVector<sak::ProgramInfo> programs);

    /// @brief Programs enumeration failed
    void enumerationFailed(const QString& error);

    /// @brief Enumeration cancellation reached terminal Idle after cooperative worker stop.
    void enumerationCancelled();

    /// @brief Uninstall pipeline started
    void uninstallStarted(const QString& program_name);

    /// @brief Uninstall pipeline progress
    void uninstallProgress(int percent, const QString& phase);

    /// @brief Native uninstaller launched
    void nativeUninstallerStarted(const QString& program_name);

    /// @brief Native uninstaller finished
    void nativeUninstallerFinished(int exit_code);

    /// @brief Restore point created
    void restorePointCreated(const QString& name);

    /// @brief Leftover scan started
    void leftoverScanStarted(sak::ScanLevel level);

    /// @brief Leftover scan progress
    void leftoverScanProgress(const QString& current_path, int found);

    /// @brief Leftover scan finished with results
    void leftoverScanFinished(QVector<sak::LeftoverItem> leftovers);

    /// @brief Remaining leftovers changed after cleanup; this is not another scan terminal.
    void leftoversUpdated(QVector<sak::LeftoverItem> leftovers);

    /// @brief Uninstall pipeline finished
    void uninstallFinished(sak::UninstallReport report);

    /// @brief Uninstall operation failed
    void uninstallFailed(int error_code, const QString& error);

    /// @brief Uninstall operation cancelled
    void uninstallCancelled();

    /// @brief Cleanup started
    void cleanupStarted(int total_items);

    /// @brief Emitted when auto-clean-safe kicks off an automatic cleanup of the SAFE-classified
    ///        leftovers after an uninstall (Review/Risky items are never auto-cleaned). @p
    ///        item_count is the number of safe items being auto-removed.
    void autoCleanSafeStarted(int item_count);

    /// @brief Individual item cleaned
    void itemCleaned(sak::LeftoverItem item, bool success);

    /// @brief Actionable reason an individual cleanup item was left in place.
    void itemCleanupError(sak::LeftoverItem item, const QString& reason);

    /// @brief Cleanup completed, including successful automatic cleanup.
    void cleanupFinished(int succeeded, int failed, qint64 bytes_recovered);

    /// @brief Cleanup was cancelled partway (distinct from cleanupFinished, which
    ///        must NOT be emitted on cancel or a cancelled cleanup reads as success)
    void cleanupCancelled();

    /// @brief Cleanup worker failed before it could produce cleanupFinished.
    void cleanupFailed(int error_code, const QString& error);

    /// @brief Advanced Uninstall preferences could not be loaded or durably saved.
    void settingsError(const QString& error);

    /// @brief Batch queue item status changed
    void queueItemStatusChanged(int index, sak::UninstallQueueItem::Status status);

    /// @brief One batch item failed; exact worker code/message are also retained in queue().
    void batchItemFailed(int index, int error_code, const QString& message);

    /// @brief Entire batch pipeline completed, including optional automatic cleanup.
    void batchFinished(int succeeded,
                       int failed,
                       int cleanup_succeeded,
                       int cleanup_failed,
                       qint64 cleanup_bytes_recovered);

    /// @brief Batch could not start, was cancelled during uninstall, or its automatic-cleanup
    ///        terminal failed/cancelled.
    void batchFailed(const QString& error);

    /// @brief Status message for status bar
    void statusMessage(const QString& message, int timeout);

    /// @brief Progress update
    void progressUpdate(int current, int maximum);

private Q_SLOTS:
    // Enumerator slots. @p generation identifies the run: a completion whose generation does not
    // match the current one is stale (a cancelled-then-restarted enumeration) and is dropped, so an
    // old run's result never overwrites the new one.
    void onEnumerationFinished(int generation, QVector<sak::ProgramInfo> programs);
    void onEnumerationFailed(int generation, const QString& error);
    void onLeftoverScanStarted(sak::ScanLevel level);

    // Uninstall worker slots
    void onUninstallComplete(sak::UninstallReport report);
    void onUninstallWorkerFailed(int error_code, const QString& message);
    void onUninstallWorkerCancelled();

    // Cleanup worker slots
    void onCleanupComplete(int succeeded, int failed, qint64 bytes_recovered);
    void onCleanupWorkerFailed(int error_code, const QString& message);
    void onCleanupWorkerCancelled();

private:
    struct CleanupRefusal {
        LeftoverItem item;
        QString reason;
    };

    struct CleanupScreenResult {
        QVector<LeftoverItem> allowed;
        QVector<CleanupRefusal> refused;
    };

    struct CleanupOutcomeState {
        QVector<LeftoverItem> items;
        qsizetype index = 0;
        bool identityFailed = false;
    };

    /// @brief Apply fail-closed target validation to selected items, returning allowed items and
    ///        reporting/counting protected, malformed, report-only, or injection-shaped refusals.
    ///        Irreversible registry/service/task/firewall targets remain allowed after explicit
    ///        manual authorization; recoverability is a separate automatic-cleanup policy.
    CleanupScreenResult screenCleanupItems(const QVector<LeftoverItem>& selected_items);

    /// @brief If @p auto_clean and @p report has SAFE leftovers, start an automatic RECOVERABLE
    ///        (Recycle Bin, always) cleanup of just those, screened. Returns true iff a cleanup was
    ///        started; when it does, the caller must NOT emit uninstallFinished -- it is deferred
    ///        to onCleanupComplete so the panel does not flip the controller to Enumerating and
    ///        refuse the cleanup.
    [[nodiscard]] bool maybeAutoCleanSafeLeftovers(UninstallReport& report, bool auto_clean);

    /// @brief Start a CleanupWorker on already-screened items. Shared by the manual clean path and
    ///        auto-clean. @p use_recycle_bin forces the Recycle Bin; @p require_recoverable
    ///        (auto-clean) makes a recycle failure leave the item in place rather than permanently
    ///        delete it.
    void startCleanupWorker(const QVector<LeftoverItem>& screened_items,
                            const QVector<CleanupRefusal>& refused_items,
                            bool use_recycle_bin,
                            bool require_recoverable);

    /// @brief Emit typed refusal outcomes for items rejected before worker construction.
    void reportCleanupRefusals(const QVector<CleanupRefusal>& refused_items);

    /// @brief Exact typed cleanup target identity. Preserves case and type-specific qualifiers.
    [[nodiscard]] static bool sameCleanupItemIdentity(const LeftoverItem& lhs,
                                                      const LeftoverItem& rhs);
    void appendUniqueBatchAutoCleanItems(const QVector<LeftoverItem>& items);

    /// @brief Refuse destructive-operation admission while cancelled enumeration is still active.
    [[nodiscard]] bool refuseWhileEnumerationStopping(const QString& operation);

    /// @brief Complete a deferred single-uninstall auto-clean: clear the in-progress flag, re-show
    ///        the leftovers STILL PRESENT (not just risk-based -- anything not confirmed removed),
    ///        and emit the deferred uninstallFinished. Safe to call from the cleanup success,
    ///        failure, AND cancel paths (a no-op unless an auto-clean was in progress).
    void finalizeDeferredAutoClean();

    /// @brief Merge automatic-cleanup counts into the deferred uninstall report before emission.
    void mergeDeferredAutoCleanResult(int succeeded, int failed, qint64 bytes_recovered);

    /// @brief Mark every pending automatic-cleanup item failed and retain @p error in the report.
    void failDeferredAutoClean(const QString& error);

    /// @brief Emit settingsError on the next event-loop turn so construction-time failures remain
    ///        observable after the owner connects signals.
    void deferSettingsError(const QString& error);

    /// @brief Complete or clear the pending batch summary after automatic cleanup.
    void finishDeferredBatchCleanup(int cleanup_succeeded,
                                    int cleanup_failed,
                                    qint64 cleanup_bytes_recovered);
    void failDeferredBatchCleanup(const QString& error);

    /// @brief The subset of @p all whose exact typed item identity was NOT confirmed removed by the
    ///        auto-clean -- so a screened-out or failed Safe item, and every
    ///        Review/Risky item, stays visible for review instead of silently vanishing.
    [[nodiscard]] QVector<LeftoverItem> leftoversStillPresent(
        const QVector<LeftoverItem>& all) const;

    /// @brief Wire a freshly created CleanupWorker's terminal, item, and progress signals.
    void connectCleanupWorkerSignals(CleanupWorker* worker,
                                     const QVector<LeftoverItem>& items,
                                     int generation);
    void connectCleanupItemSignals(CleanupWorker* worker,
                                   const QVector<LeftoverItem>& items,
                                   int generation);
    void connectCleanupErrorSignal(CleanupWorker* worker,
                                   const std::shared_ptr<CleanupOutcomeState>& outcome_state,
                                   int generation);
    void connectCleanupOutcomeSignal(CleanupWorker* worker,
                                     const std::shared_ptr<CleanupOutcomeState>& outcome_state,
                                     int generation);
    void connectCleanupTerminalSignals(CleanupWorker* worker, int generation);

    /// @brief Transition to a new state
    void setState(State new_state);

    /// @brief Clean up all workers
    void cleanupWorkers();

    /// Stop and join the enumeration thread. If it refuses the bounded cooperative join, detach the
    /// thread and enumerator together so controller teardown cannot destroy either live object.
    void stopEnumThread();

    void connectUninstallWorkerSignals(bool include_native_signals, int generation);
    void connectUninstallNativeSignals(int generation);
    void connectUninstallScanSignals(int generation);
    void connectUninstallTerminalSignals(int generation);
    void finishBatchUninstall();

    /// @brief Process next item in batch queue
    void processNextQueueItem();

    State m_state = State::Idle;

    // Monotonic enumeration generation. Bumped on every refreshPrograms() so a stale completion
    // from a superseded (cancelled-then-restarted) run can be told apart from the current run's.
    int m_enumGeneration = 0;
    bool m_enumerationStopping = false;
    int m_uninstallGeneration = 0;
    int m_cleanupGeneration = 0;

    // Workers
    std::unique_ptr<ProgramEnumerator> m_enumerator;
    std::unique_ptr<UninstallWorker> m_uninstallWorker;
    std::unique_ptr<CleanupWorker> m_cleanupWorker;
    std::unique_ptr<RestorePointManager> m_restoreManager;
    QThread* m_enumThread = nullptr;

    // Data
    QVector<ProgramInfo> m_programs;
    QVector<UninstallQueueItem> m_queue;
    static constexpr int kMaxBatchQueueItems = 100;
    int m_batchIndex = -1;
    bool m_currentAutoCleanSafe = false;

    // SAFE leftovers accumulated across a batch (only from items whose autoCleanSafeLeftovers is
    // set); auto-cleaned in one pass at batch end so a cleanup never runs concurrently with the
    // next queued uninstall.
    QVector<LeftoverItem> m_batchAutoCleanItems;

    // Single-uninstall auto-clean in flight: uninstallFinished is deferred until the auto-clean
    // CleanupWorker completes (see maybeAutoCleanSafeLeftovers / onCleanupComplete), so the panel's
    // refreshPrograms() cannot flip the controller to Enumerating and refuse the cleanup.
    bool m_autoCleanInProgress = false;
    UninstallReport m_pendingAutoCleanReport;
    int m_pendingAutoCleanItemCount = 0;
    // Exact typed items the in-progress auto-clean confirmed removed.
    QVector<LeftoverItem> m_autoCleanedItems;
    int m_pendingCleanupRefused = 0;

    bool m_batchCompletionPending = false;
    int m_pendingBatchSucceeded = 0;
    int m_pendingBatchFailed = 0;

    // Preferences
    ScanLevel m_defaultScanLevel = ScanLevel::Moderate;
    bool m_autoRestorePoint = true;
    bool m_autoCleanSafePreference = true;
    bool m_showSystemComponents = false;
    bool m_useRecycleBin = false;
    bool m_selectAllByDefault = false;
};

// -- Compile-Time Invariants -------------------------------------------------

static_assert(std::is_base_of_v<QObject, AdvancedUninstallController>,
              "AdvancedUninstallController must inherit QObject.");
static_assert(!std::is_copy_constructible_v<AdvancedUninstallController>,
              "AdvancedUninstallController must not be copy-constructible.");

}  // namespace sak
