// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_queue.cpp
/// @brief Pending operation queue implementation.

#include "sak/partition_operation_queue.h"

namespace sak {

void PartitionOperationQueue::setBaseLayoutHash(const QString& hash) {
    if (m_base_layout_hash == hash) {
        return;
    }
    m_base_layout_hash = hash;
    discard();
}

void PartitionOperationQueue::addPreview(const OperationPreview& preview) {
    if (m_operations.isEmpty()) {
        m_base_layout_hash = preview.before_layout_hash;
    }
    for (const auto& operation : preview.operations) {
        m_operations.append(operation);
    }
    m_redo.clear();
}

bool PartitionOperationQueue::undo() {
    if (m_operations.isEmpty()) {
        return false;
    }
    m_redo.append(m_operations.takeLast());
    return true;
}

bool PartitionOperationQueue::redo() {
    if (m_redo.isEmpty()) {
        return false;
    }
    m_operations.append(m_redo.takeLast());
    return true;
}

void PartitionOperationQueue::discard() {
    m_operations.clear();
    m_redo.clear();
}

const QVector<PartitionOperation>& PartitionOperationQueue::operations() const noexcept {
    return m_operations;
}

bool PartitionOperationQueue::isEmpty() const noexcept {
    return m_operations.isEmpty();
}

bool PartitionOperationQueue::canRedo() const noexcept {
    return !m_redo.isEmpty();
}

bool PartitionOperationQueue::canApply(const QString& current_layout_hash) const {
    return !m_operations.isEmpty() && m_base_layout_hash == current_layout_hash &&
           blockers().isEmpty();
}

QString PartitionOperationQueue::baseLayoutHash() const {
    return m_base_layout_hash;
}

QStringList PartitionOperationQueue::blockers() const {
    QStringList out;
    for (const auto& operation : m_operations) {
        out.append(operation.blockers);
    }
    return out;
}

QStringList PartitionOperationQueue::warnings() const {
    QStringList out;
    for (const auto& operation : m_operations) {
        out.append(operation.warnings);
    }
    return out;
}

}  // namespace sak
