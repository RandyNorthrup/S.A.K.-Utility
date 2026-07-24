// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file storage_inventory_worker.h
/// @brief Storage inventory collection and parsing for Partition Manager.

#pragma once

#include "sak/partition_manager_types.h"

#include <QObject>

namespace sak {

class StorageInventoryWorker : public QObject {
    Q_OBJECT

public:
    explicit StorageInventoryWorker(QObject* parent = nullptr);

    [[nodiscard]] PartitionInventory scan();

    /// Scan disks/partitions/volumes on this machine.
    /// @param allow_elevation when false, raw file-system probing of foreign
    /// partitions never escalates via the elevation broker (no UAC prompt, no
    /// elevated helper): such partitions simply keep an empty file_system and a
    /// warning. Headless/read-only callers (e.g. the AI assistant) pass false so
    /// a "read-only" inventory can never silently trigger a privilege escalation.
    [[nodiscard]] static PartitionInventory scanCurrentSystem(bool allow_elevation = true);
    [[nodiscard]] static PartitionInventory parseInventoryJson(const QByteArray& json_data);
    [[nodiscard]] static QString inventoryPowerShellScript();

Q_SIGNALS:
    void inventoryReady(const sak::PartitionInventory& inventory);
    void inventoryError(const QString& message);
};

}  // namespace sak
