// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace sak::ai {

// Result of executing one win32_gui recipe step through the desktop-control engine. `planned` is
// false when the step's tool could not be planned at all (unknown/unadvertised tool -> a recipe
// bug, always fatal). `high_risk` marks a step whose tool is on the high-risk exec allowlist
// (close_window / kill_process / start_process) -- disallowed inside a GUI recipe. `tool_error`
// is a runtime failure of the tool itself (e.g. a label was not found); it is fatal unless the
// step is optional.
struct Win32StepOutcome {
    bool planned{false};
    bool high_risk{false};
    bool tool_error{false};
    QString result_text;
    QString error;
};

// Executes one recipe step (a {tool, arguments, optional?} object) and reports the outcome.
using Win32StepExecutor = std::function<Win32StepOutcome(const QJsonObject& step)>;

// Run a win32_gui recipe: execute each step in order via `exec`, aggregating per-step results.
// Stops at the first fatal step -- a planning failure, a high-risk tool, or a non-optional tool
// error -- and reports success=false with the reason. Optional steps that fail are recorded and
// skipped. Returns {steps:[{index,tool,optional,ok,result_text,error?}], step_count, success,
// error?}. Pure orchestration (no gateway/process dependency) so it is unit-testable with a fake
// executor; the live wiring supplies an executor backed by planWin32McpCall + callWin32Mcp.
[[nodiscard]] QJsonObject executeWin32GuiSteps(const QJsonArray& steps,
                                               const Win32StepExecutor& exec);

}  // namespace sak::ai
