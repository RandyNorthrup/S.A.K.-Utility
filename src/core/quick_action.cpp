// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action.h"

#include "sak/format_utils.h"

#include <QDateTime>
#include <QtGlobal>

namespace sak {

QuickAction::QuickAction(QObject* parent) : QObject(parent) {}

void QuickAction::setStatus(ActionStatus status) {
    // Single atomic exchange: the compare-and-store is one op, closing the check-then-set
    // window that could double-emit when the GUI-thread cancel and worker-thread setStatus race.
    if (m_status.exchange(status) != status) {
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

    // One atomic load so the guard sees a single consistent snapshot of the status word.
    const ActionStatus current = m_status.load();
    if (current == ActionStatus::Scanning || current == ActionStatus::Running) {
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

QString QuickAction::sanitizePathForBackup(const QString& path) {
    QString safe = path;
    safe.replace(':', '_');
    safe.replace('\\', '_');
    safe.replace('/', '_');
    return safe;
}

}  // namespace sak
