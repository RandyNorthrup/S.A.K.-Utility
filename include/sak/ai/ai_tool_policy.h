// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>

namespace sak::ai {

enum class AiToolPolicy {
    NoLocalExecution,
    ReadOnlyPc,
    PackageToolsOnly,
    DownloadOnly,
    MutatingRequiresLease,
    ExclusiveMutatingExecutor,
};

struct AiToolCallRequest {
    QString tool_name;
    QString operation;
    QString command_preview;
    QString user_message;
    bool requires_admin{false};
};

struct AiToolPolicyDecision {
    bool allowed{false};
    bool risky_change{false};
    bool requires_lease{false};
    bool requires_exclusive_lease{false};
    bool restore_point_recommended{false};
    QString reason;
};

[[nodiscard]] AiToolPolicy toolPolicyFromString(const QString& value);
[[nodiscard]] QString toolPolicyToString(AiToolPolicy policy);
[[nodiscard]] bool isKnownLocalTool(const QString& tool_name);
[[nodiscard]] bool isMutatingPackageOperation(const QString& operation);
[[nodiscard]] bool commandLooksRiskyChange(const QString& preview);
[[nodiscard]] AiToolPolicyDecision evaluateToolPolicy(AiToolPolicy policy,
                                                      const AiToolCallRequest& request);

}  // namespace sak::ai
