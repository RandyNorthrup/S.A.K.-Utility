// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_queue.h
/// @brief Pending operation queue for Partition Manager.

#pragma once

#include "sak/partition_manager_types.h"

namespace sak {

class PartitionOperationQueue {
public:
    /// Hard upper bound for one pending batch. addPreview() refuses an entire preview when
    /// appending it would cross this limit; it never leaves a partially appended batch.
    static constexpr qsizetype kMaxOperationCount = 256;

    void setBaseLayoutHash(const QString& hash);
    bool addPreview(const OperationPreview& preview);
    bool replaceOperations(const QVector<PartitionOperation>& operations);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    void discard();

    [[nodiscard]] const QVector<PartitionOperation>& operations() const noexcept;
    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] qsizetype remainingCapacity() const noexcept;
    [[nodiscard]] quint64 revision() const noexcept;
    [[nodiscard]] QString fingerprint() const;
    [[nodiscard]] bool canApply(const QString& current_layout_hash) const;
    [[nodiscard]] QString baseLayoutHash() const;
    [[nodiscard]] QStringList blockers() const;
    [[nodiscard]] QStringList warnings() const;

private:
    QVector<PartitionOperation> m_operations;
    QVector<PartitionOperation> m_redo;
    QString m_base_layout_hash;
    quint64 m_revision{0};
};

}  // namespace sak
