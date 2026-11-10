// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_execution_broker.h"
#include "sak/layout_constants.h"

#include <QJsonObject>
#include <QString>

#include <functional>

namespace sak::ai {

struct AiWorkflowPowerShellToolOptions {
    static constexpr int kDefaultOutputKilobytes = 512;

    int default_output_bytes = kDefaultOutputKilobytes * static_cast<int>(sak::kBytesPerKB);
    int min_output_bytes = static_cast<int>(sak::kBytesPerKB);
    /// THE BROKER'S CEILING, not a private one. This carried its own "64 MiB hard ceiling",
    /// four times what ExecutionBroker accepts, and run() CLAMPS to it -- so a workflow phase
    /// asking for more than 16 MiB was quietly reduced to a value the broker then refused
    /// outright ("max_output_bytes 67108864 is outside the range 1-16777216") and the command
    /// never ran. The clamp read as a safety measure while being the thing that broke the run.
    int max_output_bytes = kAiCommandOutputBytesCeiling;
};

// A local output budget may be STRICTER than the broker's; it may never be wider, because the
// broker refuses -- rather than clamps -- anything above its ceiling. Asserted so the next
// widening of this ceiling fails to compile instead of shipping commands that cannot launch.
static_assert(AiWorkflowPowerShellToolOptions{}.max_output_bytes <= kAiCommandOutputBytesCeiling,
              "workflow PowerShell output ceiling must not exceed what ExecutionBroker accepts");
static_assert(AiWorkflowPowerShellToolOptions{}.min_output_bytes <=
                  AiWorkflowPowerShellToolOptions{}.default_output_bytes,
              "workflow PowerShell output floor must not exceed its default");
static_assert(AiWorkflowPowerShellToolOptions{}.default_output_bytes <=
                  AiWorkflowPowerShellToolOptions{}.max_output_bytes,
              "workflow PowerShell output default must not exceed its ceiling");

struct AiWorkflowPowerShellToolCallbacks {
    std::function<bool(const QString& title, const QString& preview, bool risky)> confirm;
    std::function<QString()> allocate_command_id;
    std::function<AiCommandResult(const AiCommandRequest& request, const QString& command_id)>
        execute_powershell;
    std::function<void(const QString& message)> append_local_event;
    std::function<void(const QString& line)> log_output;
    std::function<void(const QString& preview, const QJsonObject& result)> record_command;
    std::function<void(const QString& role, const QString& title, const QString& body)>
        append_session_memory;
};

class AiWorkflowPowerShellToolRunner {
public:
    [[nodiscard]] static QJsonObject run(const QJsonObject& args,
                                         const QString& command_preview,
                                         const AiWorkflowPowerShellToolCallbacks& callbacks,
                                         AiWorkflowPowerShellToolOptions options = {});
};

}  // namespace sak::ai
