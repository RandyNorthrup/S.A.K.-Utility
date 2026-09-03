// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

namespace sak::ai {

enum class AiAccessMode {
    Invalid,
    ChatAndResearch,
    AssistedFullAccess,
    UnattendedFullAccess,
};

[[nodiscard]] bool aiAccessModeIsValid(AiAccessMode mode) noexcept;
[[nodiscard]] bool aiAccessModeAllowsLocalExecution(AiAccessMode mode) noexcept;

struct AiPromptAssemblyInput {
    AiAccessMode access_mode{AiAccessMode::Invalid};
    QString workflow_catalog;
    QString skill_catalog;
    QString context_notes;
    QString session_memory;
    QStringList pending_steering_messages;
};

/// Trusted policy text and untrusted reference text are separate values so a caller cannot
/// accidentally promote stored memory, catalogs, context, or steering into API instructions.
struct AiPromptAssembly {
    QString trusted_instructions;
    QString untrusted_user_context;
    QString error_message;

    [[nodiscard]] bool ok() const noexcept { return error_message.isEmpty(); }
};

class AiPromptAssembler {
public:
    [[nodiscard]] static AiPromptAssembly assemble(const AiPromptAssemblyInput& input);
    [[nodiscard]] static QStringList baseGuardrails();
};

}  // namespace sak::ai
