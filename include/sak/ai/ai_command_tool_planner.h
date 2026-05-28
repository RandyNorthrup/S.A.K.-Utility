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

    [[nodiscard]] static AiCommandToolPlan buildPlan(const QString& tool_name,
                                                     const QJsonObject& args,
                                                     AiToolPolicy policy,
                                                     Options options = {});
    [[nodiscard]] static bool isPotentiallyDestructiveCommand(const AiCommandRequest& request,
                                                              const QString& preview);
};

}  // namespace sak::ai
