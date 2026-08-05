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

/// @brief Every source of in-flight AI work the assistant panel owns, sampled at one
/// instant. Collecting them in one struct is what makes "is the panel busy" a single
/// authority instead of several flags that can disagree: a source omitted here is a
/// source the panel would report as idle while it is still executing.
struct AiPanelActivity {
    /// Responses API request in flight.
    bool client_busy{false};
    /// A tool turn is open (tool results still owed to the model).
    bool tool_turn_active{false};
    /// A workflow run is executing its phases.
    bool workflow_run_active{false};
    /// The local command broker is running a shell command.
    bool execution_broker_running{false};
    /// The offline deployment worker is running.
    bool offline_worker_running{false};
    /// The async built-in tool runner still has a pool task executing. This stays true
    /// after the panel DETACHES the job on Stop: detaching only drops the result, the
    /// blocking work (a package install, an app-action recipe) keeps mutating the
    /// machine until it returns.
    bool async_tool_runner_running{false};
};

/// @brief True when any tracked source of AI work is still in flight.
[[nodiscard]] bool aiPanelIsBusy(const AiPanelActivity& activity) noexcept;

/// @brief Run status a Stop request resolves to: Cancelling while any work is still
/// draining, Cancelled only once nothing is in flight. Reporting Cancelled while a
/// detached mutation still runs would let the panel accept new work on top of it.
[[nodiscard]] AiRunStatus aiStopRunStatus(const AiPanelActivity& activity) noexcept;

}  // namespace sak::ai
