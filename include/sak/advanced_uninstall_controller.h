// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_controller.h
/// @brief Orchestrates uninstall pipeline, manages queue, and persists settings

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QObject>
#include <QThread>
#include <QVector>

#include <memory>
#include <type_traits>

namespace sak {

class ProgramEnumerator;
class UninstallWorker;
class CleanupWorker;
class RestorePointManager;

/// @brief Orchestrates the uninstall pipeline and manages application state
///
/// State machine: Idle → Enumerating → Uninstalling → LeftoverScanning →
/// Cleaning → Idle. Owns all worker threads and coordinates the full
/// uninstall → scan → cleanup pipeline.
class AdvancedUninstallController : public QObject {
    Q_OBJECT

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

    // ── Program Enumeration ──

    /// @brief Start enumerating all installed programs
    void refreshPrograms();

    /// @brief Get the last enumeration result
    [[nodiscard]] QVector<ProgramInfo> programs() const;

    // ── Uninstall Operations ──

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

    /// @brief Cancel the current operation
    void cancelOperation();

    // ── Batch Uninstall ──

    /// @brief Add a program to the batch queue
    void addToQueue(const ProgramInfo& program, ScanLevel scanLevel, bool autoCleanSafe);

    /// @brief Remove an item from the batch queue
    void removeFromQueue(int index);

    /// @brief Get the current queue
    [[nodiscard]] QVector<UninstallQueueItem> queue() const;

    /// @brief Clear the batch queue
    void clearQueue();

    /// @brief Start processing the batch queue
    void startBatchUninstall(bool createRestorePoint);

    // ── Settings ──

    /// @brief Load settings from ConfigManager
    void loadSettings();

    /// @brief Save settings to ConfigManager
    void saveSettings();

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

    // ── Restore Point Manager ──

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

    /// @brief Individual item cleaned
    void itemCleaned(const QString& path, bool success);

    /// @brief Cleanup completed
    void cleanupFinished(int succeeded, int failed, qint64 bytesRecovered);

    /// @brief Locked files scheduled for removal on next Windows reboot
    void rebootPendingItems(QStringList paths);

    /// @brief Batch queue item status changed
    void queueItemStatusChanged(int index, sak::UninstallQueueItem::Status status);

    /// @brief Batch processing complete
    void batchFinished(int succeeded, int failed);

    /// @brief Status message for status bar
    void statusMessage(const QString& message, int timeout);

    /// @brief Progress update
    void progressUpdate(int current, int maximum);

private Q_SLOTS:
    // Enumerator slots
    void onEnumerationFinished(QVector<sak::ProgramInfo> programs);
    void onEnumerationFailed(const QString& error);

    // Uninstall worker slots
    void onUninstallComplete(sak::UninstallReport report);
    void onUninstallWorkerFailed(int errorCode, const QString& message);
    void onUninstallWorkerCancelled();

    // Cleanup worker slots
    void onCleanupComplete(int succeeded, int failed, qint64 bytesRecovered);
    void onCleanupWorkerFailed(int errorCode, const QString& message);

private:
    /// @brief Transition to a new state
    void setState(State newState);

    /// @brief Clean up all workers
    void cleanupWorkers();

    /// @brief Process next item in batch queue
    void processNextQueueItem();

    State m_state = State::Idle;

    // Workers
    std::unique_ptr<ProgramEnumerator> m_enumerator;
    std::unique_ptr<UninstallWorker> m_uninstall_worker;
    std::unique_ptr<CleanupWorker> m_cleanup_worker;
    std::unique_ptr<RestorePointManager> m_restore_manager;
    QThread* m_enumThread = nullptr;

    // Data
    QVector<ProgramInfo> m_programs;
    QVector<UninstallQueueItem> m_queue;
    int m_batchIndex = -1;
    bool m_batchRestorePointCreated = false;

    // Last uninstall report for auto-clean-safe
    UninstallReport m_lastReport;
    bool m_autoCleanSafe = true;

    // Preferences
    ScanLevel m_defaultScanLevel = ScanLevel::Moderate;
    bool m_autoRestorePoint = true;
    bool m_showSystemComponents = false;
    bool m_useRecycleBin = false;
    bool m_selectAllByDefault = false;
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_base_of_v<QObject, AdvancedUninstallController>,
              "AdvancedUninstallController must inherit QObject.");
static_assert(!std::is_copy_constructible_v<AdvancedUninstallController>,
              "AdvancedUninstallController must not be copy-constructible.");

}  // namespace sak
