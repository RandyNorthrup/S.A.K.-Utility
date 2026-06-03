// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_queue.h
/// @brief Pending operation queue for Partition Manager.

#pragma once

#include "sak/partition_manager_types.h"

namespace sak {

class PartitionOperationQueue {
public:
    void setBaseLayoutHash(const QString& hash);
    void addPreview(const OperationPreview& preview);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    void discard();

    [[nodiscard]] const QVector<PartitionOperation>& operations() const noexcept;
    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool canApply(const QString& current_layout_hash) const;
    [[nodiscard]] QString baseLayoutHash() const;
    [[nodiscard]] QStringList blockers() const;
    [[nodiscard]] QStringList warnings() const;

private:
    QVector<PartitionOperation> m_operations;
    QVector<PartitionOperation> m_redo;
    QString m_base_layout_hash;
};

}  // namespace sak
