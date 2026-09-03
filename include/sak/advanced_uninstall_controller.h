// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_controller.h
/// @brief Orchestrates uninstall pipeline, manages queue, and persists settings

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QObject>
#include <QSet>
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
/// State machine: Idle -> Enumerating -> Uninstalling -> LeftoverScanning ->
/// Cleaning -> Idle. Owns all worker threads and coordinates the full
/// uninstall -> scan -> cleanup pipeline.
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
                          ScanLevel scanLevel,
                          bool createRestorePoint,
                          bool autoCleanSafe);

    /// @brief Force uninstall a program (skip native uninstaller)
    void forceUninstall(const ProgramInfo& program, ScanLevel scanLevel, bool createRestorePoint);

    /// @brief Remove a UWP package
    void removeUwpPackage(const ProgramInfo& program);

    /// @brief Remove only the registry entry (orphaned program)
    void removeRegistryEntry(const ProgramInfo& program);

    /// @brief Clean selected leftover items
    void cleanLeftovers(const QVector<LeftoverItem>& selectedItems);

    /// @brief True if @p item's type can go to the Recycle Bin (File/Folder or a file-shortcut
    ///        StartupEntry). Registry keys/values, services, tasks, firewall rules, shell
    ///        extensions have NO Recycle Bin and are IRREVERSIBLE -- auto-clean must never
    ///        auto-delete them.
    [[nodiscard]] static bool isRecoverableLeftoverType(const LeftoverItem& item);

    /// @brief Items eligible for AUTOMATIC (no human review) cleanup: Safe AND recoverable-type,
    ///        each marked selected. Safe-but-irreversible items are deliberately excluded so an
    ///        automatic delete can never be unrecoverable.
    static QVector<LeftoverItem> autoCleanableLeftovers(const QVector<LeftoverItem>& items);

    /// @brief Cancel the current operation
    void cancelOperation();

    // -- Batch Uninstall --

    /// @brief Add a program to the batch queue
    void addToQueue(const ProgramInfo& program, ScanLevel scanLevel, bool autoCleanSafe);

    /// @brief Remove an item from the batch queue.
    /// @return true when @p index named and removed an item; false with statusMessage otherwise.
    [[nodiscard]] bool removeFromQueue(int index);

    /// @brief Get the current queue
    [[nodiscard]] QVector<UninstallQueueItem> queue() const;

    /// @brief Clear the batch queue
    void clearQueue();

    /// @brief Start processing the batch queue
    void startBatchUninstall(bool createRestorePoint);

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

    // -- Restore Point Manager --

    /// @brief Access the restore point manager
    [[nodiscard]] RestorePointManager* restorePointManager() const;

Q_SIGNALS:
    /// @brief State changed
    void stateChanged(sak::AdvancedUninstallController::State newState);

    /// @brief Programs enumeration started
    void enumerationStarted();

    /// @brief Programs enumeration progress
    void enumerationProgress(int current, int total);

    /// @brief Programs enumeration completed
    void enumerationFinished(QVector<sak::ProgramInfo> programs);

    /// @brief Programs enumeration failed
    void enumerationFailed(const QString& error);

    /// @brief Programs enumeration was cancelled and reached its public terminal state.
    void enumerationCancelled();

    /// @brief Uninstall pipeline started
    void uninstallStarted(const QString& programName);

    /// @brief Uninstall pipeline progress
    void uninstallProgress(int percent, const QString& phase);

    /// @brief Native uninstaller launched
    void nativeUninstallerStarted(const QString& programName);

    /// @brief Native uninstaller finished
    void nativeUninstallerFinished(int exitCode);

    /// @brief Restore point created
    void restorePointCreated(const QString& name);

    /// @brief Leftover scan started
    void leftoverScanStarted(sak::ScanLevel level);

    /// @brief Leftover scan progress
    void leftoverScanProgress(const QString& currentPath, int found);

    /// @brief Leftover scan finished with results
    void leftoverScanFinished(QVector<sak::LeftoverItem> leftovers);

    /// @brief Uninstall pipeline finished
    void uninstallFinished(sak::UninstallReport report);

    /// @brief Uninstall operation failed
    void uninstallFailed(const QString& error);

    /// @brief Uninstall operation cancelled
    void uninstallCancelled();

    /// @brief Cleanup started
    void cleanupStarted(int totalItems);

    /// @brief Emitted when auto-clean-safe kicks off an automatic cleanup of the SAFE-classified
    ///        leftovers after an uninstall (Review/Risky items are never auto-cleaned). @p
    ///        itemCount is the number of safe items being auto-removed.
    void autoCleanSafeStarted(int itemCount);

    /// @brief Individual item cleaned
    void itemCleaned(const QString& path, bool success);

    /// @brief Actionable reason an individual cleanup item was left in place.
    void itemCleanupError(const QString& path, const QString& reason);

    /// @brief Cleanup completed
    void cleanupFinished(int succeeded, int failed, qint64 bytesRecovered);

    /// @brief Cleanup was cancelled partway (distinct from cleanupFinished, which
    ///        must NOT be emitted on cancel or a cancelled cleanup reads as success)
    void cleanupCancelled();

    /// @brief Cleanup worker failed before it could produce cleanupFinished.
    void cleanupFailed(const QString& error);

    /// @brief Advanced Uninstall preferences could not be loaded or durably saved.
    void settingsError(const QString& error);

    /// @brief Items that could NOT be sent to the Recycle Bin and were permanently deleted despite
    ///        recycle mode (so the UI can warn the user the recoverable-default was not honored).
    void recycleFallbackItems(QStringList paths);

    /// @brief Batch queue item status changed
    void queueItemStatusChanged(int index, sak::UninstallQueueItem::Status status);

    /// @brief Batch processing complete
    void batchFinished(int succeeded, int failed);

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
    void onUninstallWorkerFailed(int errorCode, const QString& message);
    void onUninstallWorkerCancelled();

    // Cleanup worker slots
    void onCleanupComplete(int succeeded, int failed, qint64 bytesRecovered);
    void onCleanupWorkerFailed(int errorCode, const QString& message);
    void onCleanupWorkerCancelled();

private:
    /// @brief Apply the fail-closed clean-leftovers screens to the selected items,
    /// returning only the allowed ones and reporting/counting the refused. Keeps
    /// OS-critical / unrecoverable / injection targets from reaching CleanupWorker.
    QVector<LeftoverItem> screenCleanupItems(const QVector<LeftoverItem>& selectedItems,
                                             int* refusedCount);

    /// @brief If @p autoClean and @p report has SAFE leftovers, start an automatic RECOVERABLE
    ///        (Recycle Bin, always) cleanup of just those, screened. Returns true iff a cleanup was
    ///        started; when it does, the caller must NOT emit uninstallFinished -- it is deferred
    ///        to onCleanupComplete so the panel does not flip the controller to Enumerating and
    ///        refuse the cleanup.
    bool maybeAutoCleanSafeLeftovers(const UninstallReport& report, bool autoClean);

    /// @brief Start a CleanupWorker on already-screened items. Shared by the manual clean path and
    ///        auto-clean. @p useRecycleBin forces the Recycle Bin; @p requireRecoverable
    ///        (auto-clean) makes a recycle failure leave the item in place rather than permanently
    ///        delete it.
    void startCleanupWorker(const QVector<LeftoverItem>& screenedItems,
                            bool useRecycleBin,
                            bool requireRecoverable);

    /// @brief Complete a deferred single-uninstall auto-clean: clear the in-progress flag, re-show
    ///        the leftovers STILL PRESENT (not just risk-based -- anything not confirmed removed),
    ///        and emit the deferred uninstallFinished. Safe to call from the cleanup success,
    ///        failure, AND cancel paths (a no-op unless an auto-clean was in progress).
    void finalizeDeferredAutoClean();

    /// @brief The subset of @p all whose item was NOT confirmed removed by the auto-clean (path not
    ///        in m_autoCleanedPaths) -- so a screened-out or failed Safe item, and every
    ///        Review/Risky item, stays visible for review instead of silently vanishing.
    [[nodiscard]] QVector<LeftoverItem> leftoversStillPresent(
        const QVector<LeftoverItem>& all) const;

    /// @brief Wire a freshly created CleanupWorker's signals to this controller (incl. the
    ///        recycle-fallback re-emit so the UI can warn about permanent deletions).
    void connectCleanupWorkerSignals(CleanupWorker* worker);

    /// @brief Transition to a new state
    void setState(State newState);

    /// @brief Clean up all workers
    void cleanupWorkers();

    /// Stop and join the enumeration thread. If it refuses the bounded cooperative join, detach the
    /// thread and enumerator together so controller teardown cannot destroy either live object.
    void stopEnumThread();

    void connectUninstallWorkerSignals(bool includeNativeSignals);
    void connectBatchUninstallWorkerSignals();
    void finishBatchUninstall();

    /// @brief Process next item in batch queue
    void processNextQueueItem();

    State m_state = State::Idle;

    // Monotonic enumeration generation. Bumped on every refreshPrograms() so a stale completion
    // from a superseded (cancelled-then-restarted) run can be told apart from the current run's.
    int m_enumGeneration = 0;
    bool m_enumerationStopping = false;

    // Workers
    std::unique_ptr<ProgramEnumerator> m_enumerator;
    std::unique_ptr<UninstallWorker> m_uninstallWorker;
    std::unique_ptr<CleanupWorker> m_cleanupWorker;
    std::unique_ptr<RestorePointManager> m_restoreManager;
    QThread* m_enumThread = nullptr;

    // Data
    QVector<ProgramInfo> m_programs;
    QVector<UninstallQueueItem> m_queue;
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
    // Lower-cased paths/names the in-progress auto-clean CONFIRMED removed (from itemCleaned
    // success), so finalizeDeferredAutoClean can re-show exactly the items still on disk.
    QSet<QString> m_autoCleanedPaths;

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
