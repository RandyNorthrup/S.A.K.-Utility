// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file cleanup_worker.h
/// @brief WorkerBase subclass for deleting selected leftover items safely

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/worker_base.h"

#include <QVector>

#include <type_traits>

namespace sak {

/// @brief Deletes selected leftover items
///        (files, folders, registry, services, tasks)
///
/// Processes each item sequentially, reporting progress and continuing
/// on individual failures. Tracks succeeded, failed, and bytes recovered.
/// Supports recycle bin deletion and scheduling locked files for removal on reboot.
class CleanupWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @param selectedItems  Items to clean up
    /// @param useRecycleBin  If true, files are sent to the Recycle Bin instead
    ///                       of permanent deletion
    /// @param parent         Parent QObject
    explicit CleanupWorker(const QVector<LeftoverItem>& selectedItems,
                           bool useRecycleBin = false,
                           QObject* parent = nullptr);
    ~CleanupWorker() override = default;

    CleanupWorker(const CleanupWorker&) = delete;
    CleanupWorker& operator=(const CleanupWorker&) = delete;
    CleanupWorker(CleanupWorker&&) = delete;
    CleanupWorker& operator=(CleanupWorker&&) = delete;

Q_SIGNALS:
    void itemCleaned(const QString& path, bool success);
    void cleanupComplete(int succeeded, int failed, qint64 bytesRecovered);

    /// @brief Emitted when locked files are scheduled for removal on next reboot
    void rebootPendingItems(QStringList paths);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    QVector<LeftoverItem> m_items;
    bool m_useRecycleBin = false;
    QStringList m_rebootPendingPaths;  ///< Paths scheduled for removal on reboot

    /// @brief Delete a file, falling back to recycle bin or reboot scheduling
    [[nodiscard]] bool deleteFile(const QString& path);

    /// @brief Delete a folder, falling back to reboot scheduling for locked contents
    [[nodiscard]] bool deleteFolder(const QString& path);

    /// @brief Send a file to the Windows Recycle Bin
    [[nodiscard]] bool sendToRecycleBin(const QString& path);

    /// @brief Schedule a locked file for removal on next Windows reboot
    [[nodiscard]] bool scheduleRebootRemoval(const QString& path);

    [[nodiscard]] bool deleteRegistryKey(const QString& fullKeyPath);
    [[nodiscard]] bool deleteRegistryValue(const QString& keyPath, const QString& valueName);
    [[nodiscard]] bool removeService(const QString& serviceName);
    [[nodiscard]] bool removeScheduledTask(const QString& taskName);
    [[nodiscard]] bool removeFirewallRule(const QString& ruleName);
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_base_of_v<WorkerBase, CleanupWorker>,
              "CleanupWorker must inherit WorkerBase.");
static_assert(!std::is_copy_constructible_v<CleanupWorker>,
              "CleanupWorker must not be copy-constructible.");

}  // namespace sak
