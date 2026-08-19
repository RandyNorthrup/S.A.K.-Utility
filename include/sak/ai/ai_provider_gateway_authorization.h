// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_provider_gateway.h"
#include "sak/ai/ai_provider_gateway_tool_runner.h"

#include <QJsonObject>

namespace sak::ai {

/// Authorization gate for a win32 MCP tool call.
///
/// Given the planned call, the caller's access mode, and the UI callbacks, decide whether the call
/// may proceed. Returns an EMPTY object on approval, or a tool-error object
/// (success:false + error_message) describing the refusal. This is the SOLE gate on the plan's
/// authorization flags -- AiProviderGateway::callWin32Mcp deliberately does NOT re-derive
/// requires_confirmation / read_only / high_risk, so gating on them is this function's job.
///
/// The gate, by design:
///  - requires_confirmation (browser input injection: click/type/press-key/scroll): a HARD human
///    confirm in EVERY non-chat mode, including Unattended; a missing confirm callback fails
///    closed.
///  - Assisted + not read_only: a human confirm (carrying the plan's high_risk flag); missing
///    callback fails closed.
///  - Unattended + high_risk: an offered restore point; a missing restore-point callback fails
///    closed, and a cancelled restore point refuses the call.
///
/// Extracted from AiProviderGatewayToolRunner so the full authorization matrix is unit-testable
/// without linking the AI execution subsystem (the runner TU pulls in the command planner, GUI
/// runner, credential store, and execution broker); this decision depends only on the plan POD,
/// the access enum, and the callbacks struct.
[[nodiscard]] QJsonObject authorizeWin32McpCall(const AiProviderGateway::Win32McpCallPlan& plan,
                                                AiProviderGatewayToolAccess access,
                                                const AiProviderGatewayToolCallbacks& callbacks);

}  // namespace sak::ai
