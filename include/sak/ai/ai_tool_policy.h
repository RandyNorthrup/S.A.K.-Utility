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
    // Set when the command matches a catastrophic/irreversible operation (disk
    // format, partition wipe, boot-config edit, shadow-copy/backup deletion,
    // registry-hive delete, execution-policy bypass, recursive system-dir wipe).
    // Callers must require an explicit human confirmation for these even in
    // unattended mode -- they can destroy the system or data irreversibly.
    bool catastrophic_change{false};
    QString reason;
};

[[nodiscard]] AiToolPolicy toolPolicyFromString(const QString& value);
[[nodiscard]] QString toolPolicyToString(AiToolPolicy policy);
[[nodiscard]] bool isKnownLocalTool(const QString& tool_name);
[[nodiscard]] bool isMutatingPackageOperation(const QString& operation);
[[nodiscard]] bool commandLooksRiskyChange(const QString& preview);
// Obfuscated command shapes (encoded/base64/iex/remote-download-and-run) that
// hide their true action from the risk classifier. Treated as risky so they get
// the lease/restore-point/approval path instead of slipping through fail-open.
[[nodiscard]] bool commandLooksObfuscated(const QString& preview);
// Catastrophic/irreversible operations (see AiToolPolicyDecision::catastrophic).
[[nodiscard]] bool commandLooksCatastrophic(const QString& preview);
[[nodiscard]] AiToolPolicyDecision evaluateToolPolicy(AiToolPolicy policy,
                                                      const AiToolCallRequest& request);

// Restrictiveness rank (0 = most restrictive), for ordering/display. The modes are not a
// single capability line (ReadOnlyPc and PackageToolsOnly grant different things), so
// clamping uses capability containment rather than this rank.
[[nodiscard]] int toolPolicyRank(AiToolPolicy policy);

// Bounds a requested sub-agent policy to the session's ceiling: @p requested is honored
// only when the ceiling already grants everything it grants, otherwise the ceiling itself
// is returned. A sub-agent can therefore never gain a capability the session lacks.
[[nodiscard]] AiToolPolicy clampToolPolicy(AiToolPolicy requested, AiToolPolicy ceiling);

}  // namespace sak::ai
