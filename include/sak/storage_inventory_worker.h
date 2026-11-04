// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file storage_inventory_worker.h
/// @brief Storage inventory collection and parsing for Partition Manager.

#pragma once

#include "sak/partition_manager_types.h"
#include "sak/process_runner.h"

#include <QObject>

#include <atomic>
#include <expected>
#include <functional>

namespace sak {

struct StorageInventoryScanResult {
    PartitionInventory inventory;
    bool cancelled{false};
};

/// How the inventory script runs for a scan that needs administrator rights.
struct PrivilegedInventoryRunners {
    std::function<ProcessResult()> in_this_process;
    /// The result, or why the helper could not run it.
    std::function<std::expected<ProcessResult, QString>()> in_helper;
};

/// Runs the script where it can read everything: here if this process is elevated, otherwise in the
/// helper. If the helper could not run it, the script runs here, so the panel keeps its list, and
/// @p notes says what stopped it. A note makes the inventory fail Apply's preflight.
[[nodiscard]] ProcessResult runPrivilegedInventoryScript(bool process_is_elevated,
                                                         const PrivilegedInventoryRunners& runners,
                                                         QStringList* notes);

class StorageInventoryWorker : public QObject {
    Q_OBJECT

public:
    explicit StorageInventoryWorker(QObject* parent = nullptr);

    [[nodiscard]] PartitionInventory scan();
    void cancel() noexcept;

    /// Scan disks/partitions/volumes on this machine.
    /// @param allow_elevation when false, raw file-system probing of foreign
    /// partitions never escalates via the elevation broker (no UAC prompt, no
    /// elevated helper): such partitions simply keep an empty file_system and a
    /// warning. Headless/read-only callers (e.g. the AI assistant) pass false so
    /// a "read-only" inventory can never silently trigger a privilege escalation.
    [[nodiscard]] static PartitionInventory scanCurrentSystem(
        bool allow_elevation = true, const CancelCheck& should_cancel = {});
    [[nodiscard]] static StorageInventoryScanResult scanCurrentSystemResult(
        bool allow_elevation = true, const CancelCheck& should_cancel = {});
    /// What Partition Manager shows after Scan Disks. It never asks for elevation: a person who
    /// only wants to look at disks gets no prompt. Without administrator rights it cannot read
    /// BitLocker, a volume's dirty bit or a raw partition, and marks them pending, not failed.
    [[nodiscard]] static StorageInventoryScanResult scanForBrowsing(
        const CancelCheck& should_cancel = {});
    /// The scan a press of Apply runs before it writes anything, and again afterwards. It runs in
    /// the session's elevated helper, whose start is the job's one prompt, so it reads what the
    /// browsing scan could not, right before the write. If the helper cannot run it, the prompt
    /// having been declined for one, the inventory says so and Apply stops.
    [[nodiscard]] static StorageInventoryScanResult scanForApply(
        const CancelCheck& should_cancel = {});
    [[nodiscard]] static PartitionInventory parseInventoryJson(const QByteArray& json_data);
    /// The same parse for a scan made under a standard-user token, as scanForBrowsing's is.
    [[nodiscard]] static PartitionInventory parseStandardUserInventoryJson(
        const QByteArray& json_data);
    /// What a queue is checked against: the same whoever scanned, elevated or not.
    [[nodiscard]] static QString layoutHash(const PartitionInventory& inventory);
    [[nodiscard]] static QString inventoryPowerShellScript();

Q_SIGNALS:
    void inventoryReady(const sak::PartitionInventory& inventory);
    void inventoryError(const QString& message);

private:
    enum class ReadLevel {
        Everything,
        WhatAStandardUserCanRead
    };
    [[nodiscard]] static StorageInventoryScanResult finishScan(const ProcessResult& result,
                                                               ReadLevel level,
                                                               bool allow_elevation,
                                                               const CancelCheck& should_cancel);
    [[nodiscard]] static PartitionInventory parseInventory(const QByteArray& json_data,
                                                           ReadLevel level);

    std::atomic_bool m_cancelled{false};
};

}  // namespace sak
