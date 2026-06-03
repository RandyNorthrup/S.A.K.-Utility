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
    [[nodiscard]] static PartitionInventory scanCurrentSystem();
    [[nodiscard]] static PartitionInventory parseInventoryJson(const QByteArray& json_data);
    [[nodiscard]] static QString inventoryPowerShellScript();

Q_SIGNALS:
    void inventoryReady(const sak::PartitionInventory& inventory);
    void inventoryError(const QString& message);
};

}  // namespace sak
