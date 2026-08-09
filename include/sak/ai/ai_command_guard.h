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
struct AiCommandGuardResult {
    QString block_error;
    QString approval_reason;
    /// False on a default-constructed result: the guard was never run. Only
    /// evaluateCommandGuard() sets it, so a result that never reached the guard can
    /// never be read as an evaluated "allow" decision.
    bool evaluated{false};

    /// Allowed only when the guard actually ran and raised no block.
    [[nodiscard]] bool allowed() const { return evaluated && block_error.isEmpty(); }
    /// A block and an approval requirement are mutually exclusive by construction.
    [[nodiscard]] bool requiresApproval() const {
        return evaluated && block_error.isEmpty() && !approval_reason.isEmpty();
    }
};

[[nodiscard]] QString commandGuardBlockError(const AiCommandRequest& request,
                                             const QString& preview);
[[nodiscard]] QString commandGuardApprovalReason(const AiCommandRequest& request,
                                                 const QString& preview);
[[nodiscard]] AiCommandGuardResult evaluateCommandGuard(const AiCommandRequest& request,
                                                        const QString& preview);

}  // namespace sak::ai
