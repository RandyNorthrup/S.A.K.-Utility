// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action.h"

#include "sak/format_utils.h"
#include "sak/layout_constants.h"

#include <QDateTime>
#include <QtGlobal>

namespace sak {

namespace {
constexpr int kLogBoxContentWidth = 65;
constexpr int kDurationPrecision = 2;
constexpr int kCompletedInPrefixWidth = 15;
}  // namespace

QuickAction::QuickAction(QObject* parent) : QObject(parent) {}

void QuickAction::setStatus(ActionStatus status) {
    if (m_status != status) {
        m_status = status;
        Q_EMIT statusChanged(status);
    }
}

void QuickAction::setScanResult(const ScanResult& result) {
    m_scan_result = result;
}

void QuickAction::setExecutionResult(const ExecutionResult& result) {
    m_execution_result = result;
}

void QuickAction::cancel() {
    m_cancelled = true;

    if (m_status == ActionStatus::Scanning || m_status == ActionStatus::Running) {
        setStatus(ActionStatus::Cancelled);
    }
}

// === Shared helpers ===

void QuickAction::emitCancelledResult(const QString& message) {
    ExecutionResult result;
    result.success = false;
    result.message = message;
    setExecutionResult(result);
    setStatus(ActionStatus::Cancelled);
    Q_EMIT executionComplete(result);
}

void QuickAction::emitCancelledResult(const QString& message, const QDateTime& start_time) {
    ExecutionResult result;
    result.success = false;
    result.message = message;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    setExecutionResult(result);
    setStatus(ActionStatus::Cancelled);
    Q_EMIT executionComplete(result);
}

void QuickAction::emitFailedResult(const QString& message,
                                   const QString& log,
                                   const QDateTime& start_time) {
    ExecutionResult result;
    result.success = false;
    result.message = message;
    result.log = log;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    setExecutionResult(result);
    setStatus(ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

void QuickAction::finishWithResult(const ExecutionResult& result, ActionStatus status) {
    setExecutionResult(result);
    setStatus(status);
    Q_EMIT executionComplete(result);
}

QString QuickAction::formatFileSize(qint64 bytes) {
    return sak::formatBytes(bytes);
}

QString QuickAction::formatLogBox(const QString& title,
                                  const QStringList& content_lines,
                                  qint64 duration_ms) {
    const QString top =
        QStringLiteral("+================================================================+\n");
    const QString sep =
        QStringLiteral("+================================================================+\n");
    const QString bottom =
        QStringLiteral("+================================================================+\n");

    QString box = top;
    box += QString("| %1|\n").arg(title.leftJustified(kLogBoxContentWidth));
    box += sep;

    for (const QString& line : content_lines) {
        if (line == "---") {
            box += sep;
        } else {
            box += QString("| %1|\n").arg(line.leftJustified(kLogBoxContentWidth));
        }
    }

    if (duration_ms >= 0) {
        const double duration_seconds = duration_ms / kMillisecondsPerSecondF;
        const QString duration_text = QString::number(duration_seconds, 'f', kDurationPrecision);
        const int padding_width = kLogBoxContentWidth - kCompletedInPrefixWidth -
                                  duration_text.length();
        box += sep;
        box += QString("| Completed in: %1 seconds%2|\n")
                   .arg(duration_seconds, 0, 'f', kDurationPrecision)
                   .arg(QString(qMax(0, padding_width), ' '));
    }

    box += bottom;
    return box;
}

QString QuickAction::sanitizePathForBackup(const QString& path) {
    QString safe = path;
    safe.replace(':', '_');
    safe.replace('\\', '_');
    safe.replace('/', '_');
    return safe;
}

}  // namespace sak
