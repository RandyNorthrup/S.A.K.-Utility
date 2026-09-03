// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_execution_broker.h"

#include <QString>

namespace sak::ai {

/// @brief Pure guard checks for AI-issued command tools.
///
/// These checks fail fast for command shapes that caused bad agent behavior in
/// manual QA, and request explicit approval for package-manager trust bypasses.
/// Decisions use only executable request fields. Display previews and manifest
/// labels are untrusted presentation metadata and cannot weaken or trigger a guard.
class AiCommandGuardResult final {
public:
    /// Allowed means execution may proceed without another authorization step.
    [[nodiscard]] bool allowed() const {
        return m_block_error.isEmpty() && m_approval_reason.isEmpty();
    }
    [[nodiscard]] bool blocked() const { return !m_block_error.isEmpty(); }
    [[nodiscard]] bool requiresApproval() const {
        return m_block_error.isEmpty() && !m_approval_reason.isEmpty();
    }
    [[nodiscard]] const QString& blockError() const { return m_block_error; }
    [[nodiscard]] const QString& approvalReason() const { return m_approval_reason; }

private:
    friend AiCommandGuardResult evaluateCommandGuard(const AiCommandRequest& request);

    AiCommandGuardResult(QString block_error, QString approval_reason);

    QString m_block_error;
    QString m_approval_reason;
};

[[nodiscard]] AiCommandGuardResult evaluateCommandGuard(const AiCommandRequest& request);

}  // namespace sak::ai
