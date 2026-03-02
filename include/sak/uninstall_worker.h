// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file uninstall_worker.h
/// @brief WorkerBase subclass for executing the uninstall pipeline

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/worker_base.h"

#include <QSet>
#include <QVector>

#include <type_traits>

namespace sak {

/// @brief Executes the uninstall pipeline on a background thread
///
/// Pipeline: restore point → registry snapshot → native uninstall →
/// leftover scan → report. Supports standard, forced, UWP, and
/// registry-only uninstall modes.
class UninstallWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Uninstall mode
    enum class Mode {
        Standard,        ///< Run native uninstaller + leftover scan
        ForcedUninstall, ///< Skip native uninstaller, scan + remove all traces
        UwpRemove,       ///< Remove UWP package via PowerShell
        RegistryOnly     ///< Only remove the registry uninstall entry (orphaned)
    };

    explicit UninstallWorker(const ProgramInfo& program, Mode mode,
                             ScanLevel scanLevel,
                             bool createRestorePoint = true,
                             QObject* parent = nullptr);
    ~UninstallWorker() override = default;

    UninstallWorker(const UninstallWorker&) = delete;
    UninstallWorker& operator=(const UninstallWorker&) = delete;
    UninstallWorker(UninstallWorker&&) = delete;
    UninstallWorker& operator=(UninstallWorker&&) = delete;

Q_SIGNALS:
    /// @brief Native uninstaller has been launched
    void nativeUninstallerStarted(const QString& programName);

    /// @brief Native uninstaller completed
    void nativeUninstallerFinished(int exitCode);

    /// @brief Registry snapshot captured (before state)
    void registrySnapshotCaptured();

    /// @brief Restore point created
    void restorePointCreated(const QString& name);

    /// @brief Leftover scan started
    void leftoverScanStarted(sak::ScanLevel level);

    /// @brief Leftover scan progress
    void leftoverScanProgress(const QString& currentPath, int found);

    /// @brief Leftover scan complete
    void leftoverScanFinished(QVector<sak::LeftoverItem> leftovers);

    /// @brief Full uninstall pipeline complete
    void uninstallComplete(sak::UninstallReport report);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    ProgramInfo m_program;
    Mode m_mode;
    ScanLevel m_scanLevel;
    bool m_createRestorePoint{true};

    // Pipeline stages
    [[nodiscard]] bool createRestorePoint();
    [[nodiscard]] bool captureRegistrySnapshot();
    [[nodiscard]] bool runNativeUninstaller();
    [[nodiscard]] QVector<LeftoverItem> scanLeftovers();
    [[nodiscard]] bool removeUwpPackage();
    [[nodiscard]] bool removeRegistryEntry();

    // Registry snapshot data
    QSet<QString> m_registrySnapshotBefore;

    /// @brief Stop flag passed to LeftoverScanner (bridges WorkerBase::stopRequested)
    std::atomic<bool> m_scanStopFlag{false};

    // Helpers
    [[nodiscard]] bool isMsiInstaller() const;
    [[nodiscard]] QString buildMsiUninstallCommand() const;
    [[nodiscard]] QString extractGuidFromUninstallString() const;
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_base_of_v<WorkerBase, UninstallWorker>,
    "UninstallWorker must inherit WorkerBase.");
static_assert(!std::is_copy_constructible_v<UninstallWorker>,
    "UninstallWorker must not be copy-constructible.");

} // namespace sak

Q_DECLARE_METATYPE(sak::UninstallWorker::Mode)
