// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_human_gate.h"

#include <QJsonObject>
#include <QString>

namespace sak::ai {

enum class AiRunStatus {
    Idle,
    Planning,
    Running,
    WaitingForHuman,
    Cancelling,
    Cancelled,
    Completed,
    Failed,
};

struct AiRunState {
    QString run_id;
    QString workflow_id;
    AiRunStatus status{AiRunStatus::Idle};
    QString phase_id;
    int active_subagents{0};
    int completed_subagents{0};
    int active_tools{0};
    int completed_tools{0};
    QString message;
    bool has_pending_human_gate{false};
    AiHumanGate pending_human_gate;

    [[nodiscard]] bool isTerminal() const noexcept;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiRunState fromJson(const QJsonObject& object);
};

[[nodiscard]] QString runStatusToString(AiRunStatus status);
[[nodiscard]] AiRunStatus runStatusFromString(const QString& value);
[[nodiscard]] bool isTerminalRunStatus(AiRunStatus status) noexcept;

}  // namespace sak::ai
