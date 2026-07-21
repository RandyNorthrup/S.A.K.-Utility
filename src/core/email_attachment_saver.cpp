// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_attachment_saver.cpp
/// @brief Shared attachment-saving utilities

#include "sak/email_attachment_saver.h"

#include "sak/logger.h"

#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>

namespace sak {

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
        QString error = QStringLiteral("Failed to save attachment: %1").arg(path);
        sak::logError("Failed to save attachment: {}", path.toStdString());
        return {false, path, error};
    }
    const qint64 written = file.write(data);
    if (written != static_cast<qint64>(data.size())) {
        file.cancelWriting();
        QString error = QStringLiteral("Short write saving attachment: %1").arg(path);
        sak::logError("Short write saving attachment: {}", path.toStdString());
        return {false, path, error};
    }
    if (!file.commit()) {
        QString error = QStringLiteral("Failed to finalize attachment: %1").arg(path);
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
    QString safe_name = sanitizeAttachmentFilename(filename);
    QString file_path = dir + QStringLiteral("/") + safe_name;

    if (QFile::exists(file_path)) {
        int dot = safe_name.lastIndexOf(QLatin1Char('.'));
        QString base = (dot > 0) ? safe_name.left(dot) : safe_name;
        QString ext = (dot > 0) ? safe_name.mid(dot) : QString();
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
        QString error = QStringLiteral("No unique attachment name available in: %1").arg(dir);
        sak::logError("No unique attachment name available in: {}", dir.toStdString());
        return {false, file_path, error};
    }

    return saveAttachmentToPath(file_path, data);
}

// ============================================================================
// AttachmentBatchSave
// ============================================================================

void AttachmentBatchSave::begin(const QString& dir, int count) {
    Q_ASSERT(!dir.isEmpty());
    Q_ASSERT(count > 0);
    m_dir = dir;
    m_pending = count;
    m_succeeded = 0;
    m_failed = 0;
}

AttachmentSaveResult AttachmentBatchSave::recordOne(const QString& filename,
                                                    const QByteArray& data) {
    Q_ASSERT(isActive());
    auto result = saveAttachmentToDirectory(m_dir, filename, data);
    if (result.success) {
        ++m_succeeded;
    } else {
        ++m_failed;
    }
    return result;
}

void AttachmentBatchSave::recordError() {
    Q_ASSERT(isActive());
    ++m_failed;
}

bool AttachmentBatchSave::isComplete() const {
    return m_pending > 0 && (m_succeeded + m_failed >= m_pending);
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
        .arg(m_pending)
        .arg(m_failed);
}

void AttachmentBatchSave::reset() {
    m_dir.clear();
    m_pending = 0;
    m_succeeded = 0;
    m_failed = 0;
}

}  // namespace sak
