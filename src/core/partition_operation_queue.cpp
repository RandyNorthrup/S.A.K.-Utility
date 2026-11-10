// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_queue.cpp
/// @brief Pending operation queue implementation.

#include "sak/partition_operation_queue.h"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace sak {

namespace {

void addFingerprintField(QCryptographicHash* hash, const QByteArray& value) {
    hash->addData(QByteArray::number(value.size()));
    hash->addData(QByteArrayLiteral(":"));
    hash->addData(value);
    hash->addData(QByteArrayLiteral(";"));
}

void addTargetFingerprint(QCryptographicHash* hash, const PartitionTarget& target) {
    addFingerprintField(hash, QByteArray::number(static_cast<int>(target.kind)));
    addFingerprintField(hash, QByteArray::number(target.disk_number.value()));
    addFingerprintField(hash, QByteArray::number(target.partition_number.value()));
    addFingerprintField(hash, target.partition_guid.toUtf8());
    addFingerprintField(hash, target.volume_guid.toUtf8());
    addFingerprintField(hash, target.drive_letter.toUtf8());
    addFingerprintField(hash, QByteArray::number(target.offset_bytes));
    addFingerprintField(hash, QByteArray::number(target.size_bytes));
    addFingerprintField(hash, target.disk_unique_id.toUtf8());
    addFingerprintField(hash, target.disk_serial_number.toUtf8());
    addFingerprintField(hash, QByteArray::number(target.disk_size_bytes));
}

}  // namespace

void PartitionOperationQueue::setBaseLayoutHash(const QString& hash) {
    if (m_base_layout_hash == hash) {
        return;
    }
    m_base_layout_hash = hash;
    m_operations.clear();
    m_redo.clear();
    ++m_revision;
}

bool PartitionOperationQueue::addPreview(const OperationPreview& preview) {
    if (preview.operations.isEmpty() ||
        preview.operations.size() > kMaxOperationCount - m_operations.size()) {
        return false;
    }
    if (m_operations.isEmpty()) {
        m_base_layout_hash = preview.before_layout_hash;
    }
    m_operations.append(preview.operations);
    m_redo.clear();
    ++m_revision;
    return true;
}

bool PartitionOperationQueue::replaceOperations(const QVector<PartitionOperation>& operations) {
    if (operations.isEmpty() || operations.size() > kMaxOperationCount) {
        return false;
    }
    m_operations = operations;
    return true;
}

bool PartitionOperationQueue::undo() {
    if (m_operations.isEmpty()) {
        return false;
    }
    m_redo.append(m_operations.takeLast());
    ++m_revision;
    return true;
}

bool PartitionOperationQueue::redo() {
    if (m_redo.isEmpty()) {
        return false;
    }
    m_operations.append(m_redo.takeLast());
    ++m_revision;
    return true;
}

void PartitionOperationQueue::discard() {
    if (m_operations.isEmpty() && m_redo.isEmpty()) {
        return;
    }
    m_operations.clear();
    m_redo.clear();
    ++m_revision;
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

qsizetype PartitionOperationQueue::remainingCapacity() const noexcept {
    return kMaxOperationCount - m_operations.size();
}

quint64 PartitionOperationQueue::revision() const noexcept {
    return m_revision;
}

QString PartitionOperationQueue::fingerprint() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFingerprintField(&hash, m_base_layout_hash.toUtf8());
    for (const auto& operation : m_operations) {
        addFingerprintField(&hash, operation.id.toUtf8());
        addFingerprintField(&hash, QByteArray::number(static_cast<int>(operation.type)));
        addTargetFingerprint(&hash, operation.target);
        addFingerprintField(&hash, QJsonDocument(operation.payload).toJson(QJsonDocument::Compact));
    }
    return QString::fromLatin1(hash.result().toHex());
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
