// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/ai_tool_policy.h"

#include <QJsonObject>
#include <QSet>
#include <QString>

#include <functional>

namespace sak::ai {

/// @brief Concrete IAiSubagentToolExecutor that lets an acting subagent run tools
/// through the panel's existing gate chain.
///
/// Each requested tool call is gated by an allowlist (which tools a subagent may
/// invoke at all), its arguments are parsed, and the call is handed to an
/// injected dispatch callback -- the panel wires that to
/// AiToolDispatcher::dispatch (policy + lease + health), the same worker-thread
/// path that workflow tool_action phases already use. Fail closed: a
/// disallowed/empty tool name, unparseable arguments, a missing dispatcher, or a
/// cancelled token all return a structured error output WITHOUT dispatching.
class AiSubagentToolExecutor : public IAiSubagentToolExecutor {
public:
    /// Runs one gated tool call and returns the raw result object. Wired by the
    /// panel to AiToolDispatcher::dispatch on the workflow worker thread.
    using RawDispatch = std::function<QJsonObject(AiToolPolicy policy,
                                                  const AiToolCallRequest& request,
                                                  const QJsonObject& arguments,
                                                  const QString& agent_id,
                                                  const CancellationToken& token)>;

    explicit AiSubagentToolExecutor(RawDispatch dispatch);

    /// Only these (lowercased) tool names may be dispatched; every other call
    /// fails closed. Defaults to empty (deny all) until the panel populates it.
    void setAllowedTools(QSet<QString> tool_names);
    [[nodiscard]] const QSet<QString>& allowedTools() const { return m_allowed_tools; }

    [[nodiscard]] AiSubagentToolOutput executeToolCall(const AiSubagentTask& task,
                                                       const AiSubagentToolCall& call,
                                                       const CancellationToken& token) override;

private:
    RawDispatch m_dispatch;
    QSet<QString> m_allowed_tools;
};

}  // namespace sak::ai
