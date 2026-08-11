// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_attachment_saver.cpp
/// @brief Shared attachment-saving utilities

#include "sak/email_attachment_saver.h"

#include "sak/logger.h"

#include <QByteArray>
#include <QFile>
#include <QHashFunctions>
#include <QRegularExpression>
#include <QSaveFile>

#include <utility>

namespace sak {

// ============================================================================
// Attachment identity
// ============================================================================

size_t qHash(const AttachmentRef& ref, size_t seed) noexcept {
    return qHashMulti(seed, ref.message_id, ref.index);
}

// ============================================================================
// Filename sanitization
// ============================================================================

QString sanitizeAttachmentFilename(const QString& filename) {
    if (filename.isEmpty()) {
        return QStringLiteral("attachment");
    }
    QString result = filename;
    static const QRegularExpression kInvalidChars(QStringLiteral(R"([<>:"/\\|?*])"));
    result.replace(kInvalidChars, QStringLiteral("_"));
    result = result.trimmed();
    while (result.endsWith(QLatin1Char('.'))) {
        result.chop(1);
    }
    if (result.isEmpty()) {
        return QStringLiteral("attachment");
    }
    return result;
}

// ============================================================================
// Save to exact path
// ============================================================================

AttachmentSaveResult saveAttachmentToPath(const QString& path, const QByteArray& data) {
    // QSaveFile writes to a temporary and only replaces the target on commit(),
    // so a short/failed write never leaves a truncated file reported as saved.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        const QString error = QStringLiteral("Failed to save attachment: %1").arg(path);
        sak::logError("Failed to save attachment: {}", path.toStdString());
        return {false, path, error};
    }
    const qint64 written = file.write(data);
    if (written != static_cast<qint64>(data.size())) {
        file.cancelWriting();
        const QString error = QStringLiteral("Short write saving attachment: %1").arg(path);
        sak::logError("Short write saving attachment: {}", path.toStdString());
        return {false, path, error};
    }
    if (!file.commit()) {
        const QString error = QStringLiteral("Failed to finalize attachment: %1").arg(path);
        sak::logError("Failed to finalize attachment: {}", path.toStdString());
        return {false, path, error};
    }
    return {true, path, {}};
}

// ============================================================================
// Save to directory (sanitize + dedupe)
// ============================================================================

AttachmentSaveResult saveAttachmentToDirectory(const QString& dir,
                                               const QString& filename,
                                               const QByteArray& data) {
    if (dir.isEmpty()) {
        // An empty dir would produce a root-relative "/<name>" target. Refuse rather than
        // write to a guessed location.
        return {false, {}, QStringLiteral("No attachment directory specified")};
    }
    if (data.isNull()) {
        // A null (absent) payload means the content was never delivered. Saving a zero-byte
        // file and reporting success would hide that failure. A genuinely empty attachment
        // arrives as a non-null zero-length QByteArray and is still saved.
        return {false, {}, QStringLiteral("No attachment content to save")};
    }
    const QString safe_name = sanitizeAttachmentFilename(filename);
    QString file_path = dir + QStringLiteral("/") + safe_name;

    if (QFile::exists(file_path)) {
        const int dot = safe_name.lastIndexOf(QLatin1Char('.'));
        const QString base = (dot > 0) ? safe_name.left(dot) : safe_name;
        const QString ext = (dot > 0) ? safe_name.mid(dot) : QString();
        int counter = 1;
        constexpr int kMaxDedupeAttempts = 999;
        do {
            file_path = QStringLiteral("%1/%2_%3%4").arg(dir, base, QString::number(counter), ext);
            ++counter;
        } while (QFile::exists(file_path) && counter <= kMaxDedupeAttempts);
    }

    // Never write onto an existing file: when every dedupe slot is taken the loop
    // above exits with a path that still exists, and saveAttachmentToPath would
    // otherwise truncate it.
    if (QFile::exists(file_path)) {
        const QString error = QStringLiteral("No unique attachment name available in: %1").arg(dir);
        sak::logError("No unique attachment name available in: {}", dir.toStdString());
        return {false, file_path, error};
    }

    return saveAttachmentToPath(file_path, data);
}

// ============================================================================
// AttachmentBatchSave
// ============================================================================

bool AttachmentBatchSave::begin(const QString& dir, const QVector<AttachmentRef>& expected) {
    // Refuse to overlap batches: two live expectation sets would interleave their
    // arrivals, and each batch would record payloads the other asked for.
    if (isActive() || dir.isEmpty() || expected.isEmpty()) {
        return false;
    }
    QSet<AttachmentRef> outstanding;
    outstanding.reserve(expected.size());
    for (const auto& ref : expected) {
        outstanding.insert(ref);
    }
    m_dir = dir;
    m_outstanding = std::move(outstanding);
    m_expected_count = static_cast<int>(m_outstanding.size());
    m_succeeded = 0;
    m_failed = 0;
    return true;
}

bool AttachmentBatchSave::expects(const AttachmentRef& ref) const {
    return isActive() && m_outstanding.contains(ref);
}

AttachmentSaveResult AttachmentBatchSave::recordOne(const AttachmentRef& ref,
                                                    const QString& filename,
                                                    const QByteArray& data) {
    // An arrival this batch did not ask for, or already recorded, is refused
    // outright. Writing it would put the wrong payload in one of the batch's
    // slots and let the batch report a full count it never actually filled.
    if (!expects(ref)) {
        return {false,
                {},
                QStringLiteral("Attachment %1 of message %2 is not outstanding in this batch")
                    .arg(ref.index)
                    .arg(ref.message_id)};
    }
    m_outstanding.remove(ref);
    auto result = saveAttachmentToDirectory(m_dir, filename, data);
    if (result.success) {
        ++m_succeeded;
    } else {
        ++m_failed;
    }
    return result;
}

void AttachmentBatchSave::recordError() {
    // Every call site establishes the active batch first: the panel and the
    // attachments browser test isActive(), and runPstAttachmentSaves() only
    // reaches this after begin() returned true.
    Q_ASSERT(isActive());
    if (!isActive()) {
        // Release-build fail-closed: never count a failure against a batch that is not running.
        return;
    }
    ++m_failed;
}

bool AttachmentBatchSave::isComplete() const {
    return m_expected_count > 0 && (m_succeeded + m_failed >= m_expected_count);
}

bool AttachmentBatchSave::isActive() const {
    return !m_dir.isEmpty();
}

QString AttachmentBatchSave::summaryText() const {
    if (m_failed == 0) {
        return QStringLiteral("Saved %1 attachment(s) to %2").arg(m_succeeded).arg(m_dir);
    }
    return QStringLiteral("Saved %1 of %2 attachment(s) (%3 failed)")
        .arg(m_succeeded)
        .arg(m_expected_count)
        .arg(m_failed);
}

void AttachmentBatchSave::reset() {
    m_dir.clear();
    m_outstanding.clear();
    m_expected_count = 0;
    m_succeeded = 0;
    m_failed = 0;
}

}  // namespace sak
