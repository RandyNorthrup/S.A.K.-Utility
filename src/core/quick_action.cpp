// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/quick_action.h"

namespace sak {

QuickAction::QuickAction(QObject* parent)
    : QObject(parent)
{
}

void QuickAction::setStatus(ActionStatus status)
{
    if (m_status != status) {
        m_status = status;
        Q_EMIT statusChanged(status);
    }
}

void QuickAction::setScanResult(const ScanResult& result)
{
    m_scan_result = result;
}

void QuickAction::setExecutionResult(const ExecutionResult& result)
{
    m_execution_result = result;
}

void QuickAction::cancel()
{
    m_cancelled = true;
    
    if (m_status == ActionStatus::Scanning || m_status == ActionStatus::Running) {
        setStatus(ActionStatus::Cancelled);
    }
}

} // namespace sak
