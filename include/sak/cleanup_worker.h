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

/// @brief Deletes selected leftover items (files, folders, registry, services, tasks)
///
/// Processes each item sequentially, reporting progress and continuing
/// on individual failures. Tracks succeeded, failed, and bytes recovered.
class CleanupWorker : public WorkerBase {
    Q_OBJECT

public:
    explicit CleanupWorker(const QVector<LeftoverItem>& selectedItems,
                           QObject* parent = nullptr);
    ~CleanupWorker() override = default;

    CleanupWorker(const CleanupWorker&) = delete;
    CleanupWorker& operator=(const CleanupWorker&) = delete;
    CleanupWorker(CleanupWorker&&) = delete;
    CleanupWorker& operator=(CleanupWorker&&) = delete;

Q_SIGNALS:
    void itemCleaned(const QString& path, bool success);
    void cleanupComplete(int succeeded, int failed, qint64 bytesRecovered);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    QVector<LeftoverItem> m_items;

    [[nodiscard]] bool deleteFile(const QString& path);
    [[nodiscard]] bool deleteFolder(const QString& path);
    [[nodiscard]] bool deleteRegistryKey(const QString& fullKeyPath);
    [[nodiscard]] bool deleteRegistryValue(const QString& keyPath,
                                           const QString& valueName);
    [[nodiscard]] bool removeService(const QString& serviceName);
    [[nodiscard]] bool removeScheduledTask(const QString& taskName);
    [[nodiscard]] bool removeFirewallRule(const QString& ruleName);
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_base_of_v<WorkerBase, CleanupWorker>,
    "CleanupWorker must inherit WorkerBase.");
static_assert(!std::is_copy_constructible_v<CleanupWorker>,
    "CleanupWorker must not be copy-constructible.");

} // namespace sak
