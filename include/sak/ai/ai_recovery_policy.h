// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

namespace sak::ai {

enum class AiRecoveryAction {
    Retry,
    Reassign,
    ContinueDegraded,
    AskHuman,
    Abort,
};

struct AiFailureContext {
    QString workflow_id;
    QString phase_id;
    QString phase_type;
    QString agent_id;
    QString tool_name;
    QString risk;
    QString error_message;
    bool user_cancelled{false};
};

struct AiRecoveryDecision {
    AiRecoveryAction action{AiRecoveryAction::Abort};
    QString reason;
    QString suggested_agent;
    bool requires_human{false};
    bool retry_allowed{false};
    bool safe_to_continue{false};
    bool preserve_artifacts{true};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiRecoveryDecision fromJson(const QJsonObject& object);
};

[[nodiscard]] QString recoveryActionToString(AiRecoveryAction action);
[[nodiscard]] AiRecoveryAction recoveryActionFromString(const QString& value);

class AiRecoveryPolicy {
public:
    [[nodiscard]] static AiRecoveryDecision classifyFailure(const AiFailureContext& context);
};

}  // namespace sak::ai
