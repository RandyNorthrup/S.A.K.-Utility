// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

namespace sak::ai {

struct AiPromptAssemblyInput {
    QString access_mode_label;
    QString agent_profile;
    QString workflow_catalog;
    QString context_notes;
    QString session_memory;
    QStringList pending_steering_messages;
    bool local_execution_enabled{false};
    bool assisted_full_access{false};
    bool unattended_full_access{false};
};

class AiPromptAssembler {
public:
    [[nodiscard]] static QString assemble(const AiPromptAssemblyInput& input);
    [[nodiscard]] static QStringList baseGuardrails();
};

}  // namespace sak::ai
