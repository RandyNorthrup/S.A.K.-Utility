// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_workflow_template.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace sak::ai {

struct AiWorkflowClarificationQuestion {
    QString input_id;
    QString label;
    QString type;
    QString question;
    QString reason;
};

struct AiWorkflowClarificationResult {
    QVector<AiWorkflowClarificationQuestion> questions;

    [[nodiscard]] bool needsClarification() const noexcept { return !questions.isEmpty(); }
};

class AiWorkflowClarifier {
public:
    [[nodiscard]] static AiWorkflowClarificationResult analyze(const WorkflowTemplate& workflow,
                                                               const QString& user_message,
                                                               const QJsonObject& input_values);

    [[nodiscard]] static bool looksAmbiguousAppValue(const QString& value,
                                                     const QString& user_message);
};

}  // namespace sak::ai
