// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_run_state.h"

namespace sak::ai {

QString runStatusToString(AiRunStatus status) {
    switch (status) {
    case AiRunStatus::Idle:
        return QStringLiteral("idle");
    case AiRunStatus::Planning:
        return QStringLiteral("planning");
    case AiRunStatus::Running:
        return QStringLiteral("running");
    case AiRunStatus::WaitingForHuman:
        return QStringLiteral("waiting_for_human");
    case AiRunStatus::Cancelling:
        return QStringLiteral("cancelling");
    case AiRunStatus::Cancelled:
        return QStringLiteral("cancelled");
    case AiRunStatus::Completed:
        return QStringLiteral("completed");
    case AiRunStatus::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

AiRunStatus runStatusFromString(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("planning")) {
        return AiRunStatus::Planning;
    }
    if (normalized == QLatin1String("running")) {
        return AiRunStatus::Running;
    }
    if (normalized == QLatin1String("waiting_for_human")) {
        return AiRunStatus::WaitingForHuman;
    }
    if (normalized == QLatin1String("cancelling")) {
        return AiRunStatus::Cancelling;
    }
    if (normalized == QLatin1String("cancelled")) {
        return AiRunStatus::Cancelled;
    }
    if (normalized == QLatin1String("completed")) {
        return AiRunStatus::Completed;
    }
    if (normalized == QLatin1String("failed")) {
        return AiRunStatus::Failed;
    }
    return AiRunStatus::Idle;
}

bool isTerminalRunStatus(AiRunStatus status) noexcept {
    return status == AiRunStatus::Cancelled || status == AiRunStatus::Completed ||
           status == AiRunStatus::Failed;
}

bool AiRunState::isTerminal() const noexcept {
    return isTerminalRunStatus(status);
}

QJsonObject AiRunState::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("run_id")] = run_id;
    obj[QStringLiteral("workflow_id")] = workflow_id;
    obj[QStringLiteral("status")] = runStatusToString(status);
    obj[QStringLiteral("phase_id")] = phase_id;
    obj[QStringLiteral("active_subagents")] = active_subagents;
    obj[QStringLiteral("completed_subagents")] = completed_subagents;
    obj[QStringLiteral("active_tools")] = active_tools;
    obj[QStringLiteral("completed_tools")] = completed_tools;
    obj[QStringLiteral("message")] = message;
    obj[QStringLiteral("has_pending_human_gate")] = has_pending_human_gate;
    if (has_pending_human_gate) {
        obj[QStringLiteral("pending_human_gate")] = pending_human_gate.toJson();
    }
    return obj;
}

AiRunState AiRunState::fromJson(const QJsonObject& object) {
    AiRunState state;
    state.run_id = object.value(QStringLiteral("run_id")).toString();
    state.workflow_id = object.value(QStringLiteral("workflow_id")).toString();
    state.status = runStatusFromString(object.value(QStringLiteral("status")).toString());
    state.phase_id = object.value(QStringLiteral("phase_id")).toString();
    state.active_subagents = object.value(QStringLiteral("active_subagents")).toInt(0);
    state.completed_subagents = object.value(QStringLiteral("completed_subagents")).toInt(0);
    state.active_tools = object.value(QStringLiteral("active_tools")).toInt(0);
    state.completed_tools = object.value(QStringLiteral("completed_tools")).toInt(0);
    state.message = object.value(QStringLiteral("message")).toString();
    state.has_pending_human_gate =
        object.value(QStringLiteral("has_pending_human_gate")).toBool(false);
    if (state.has_pending_human_gate) {
        state.pending_human_gate =
            AiHumanGate::fromJson(object.value(QStringLiteral("pending_human_gate")).toObject());
        if (state.pending_human_gate.gate_id.isEmpty()) {
            state.has_pending_human_gate = false;
        }
    }
    return state;
}

}  // namespace sak::ai
