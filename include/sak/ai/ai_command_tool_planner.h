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
    /// The command as the guard, the risk classifier and the persisted record see it: RAW, so
    /// classification matches the text that will actually run. Never put this in front of a
    /// human -- use @c display_preview.
    QString preview;
    /// The same command rendered safe to DISPLAY: control characters, bidi overrides and
    /// zero-width marks are made visible, so what the approver reads cannot differ from what
    /// runs. Kept separate from @c preview because sanitising the classifier's input would
    /// change which commands are judged risky.
    QString display_preview;
    QString guard_block_error;
    QString guard_approval_reason;
    bool risky_change{false};
};

class AiCommandToolPlanner {
public:
    struct Options {
        int max_output_bytes{kDefaultCommandToolMaxOutputBytes};
    };
    static_assert(kDefaultCommandToolMaxOutputBytes <= kAiCommandOutputBytesCeiling,
                  "command-tool output budget must not exceed what ExecutionBroker accepts");

    /// @brief Plan one command tool call from AI-supplied arguments.
    ///
    /// @p tool_name must be EXACTLY one of the closed set "run_powershell", "run_cmd",
    /// or "run_process" -- any other name (including a case/whitespace variant the
    /// router would still route as a command tool) is refused. A refused call, and any
    /// call whose arguments fail the broker's typed validation, comes back with a
    /// non-empty @c guard_block_error, a @c request.validation_error carrying the same
    /// message, @c risky_change set, and a default-denied @c policy_decision, so a
    /// caller that only checks the block error still fails closed.
    ///
    /// The same refusal covers a request that PARSES but that the broker would decline at
    /// launch -- an empty shell command, an elevated cmd.exe or direct-process launch, an
    /// out-of-domain output budget. Those are decided here, through the broker's own
    /// @c aiCommandPreconditionError, so a plan that cannot run is never previewed,
    /// risk-classified, approved by a human and leased before anything notices.
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
