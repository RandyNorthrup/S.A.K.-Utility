// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_tool_policy.h"

#include <QJsonObject>
#include <QString>

namespace sak::ai {

inline constexpr int kDefaultCommandToolMaxOutputBytes = kAiCommandDefaultMaxOutputBytes;

struct AiCommandToolPlan {
    AiCommandRequest request;
    AiToolCallRequest policy_request;
    AiToolPolicyDecision policy_decision;
    QString shell_label;
    QString preview;
    QString guard_block_error;
    QString guard_approval_reason;
    bool risky_change{false};
};

class AiCommandToolPlanner {
public:
    struct Options {
        int max_output_bytes{kDefaultCommandToolMaxOutputBytes};
    };

    /// @brief Plan one command tool call from AI-supplied arguments.
    ///
    /// @p tool_name must be EXACTLY one of the closed set "run_powershell", "run_cmd",
    /// or "run_process" -- any other name (including a case/whitespace variant the
    /// router would still route as a command tool) is refused. A refused call, and any
    /// call whose arguments fail the broker's typed validation, comes back with a
    /// non-empty @c guard_block_error, a @c request.validation_error carrying the same
    /// message, @c risky_change set, and a default-denied @c policy_decision, so a
    /// caller that only checks the block error still fails closed.
    [[nodiscard]] static AiCommandToolPlan buildPlan(const QString& tool_name,
                                                     const QJsonObject& args,
                                                     AiToolPolicy policy,
                                                     Options options = {});
    /// @brief Text-based destructive classification for a shell command. A direct
    /// process launch is classified risky by buildPlan() regardless of this result:
    /// an arbitrary executable cannot be proven safe from its command line.
    [[nodiscard]] static bool isPotentiallyDestructiveCommand(const AiCommandRequest& request,
                                                              const QString& preview);
};

}  // namespace sak::ai
