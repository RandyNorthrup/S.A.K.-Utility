// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak::ai {

struct WorkflowRequiredInput {
    QString id;
    QString label;
    QString type;
    bool required{false};
};

struct WorkflowRequirement {
    QString id;
    QString kind;
    bool required{false};
    QString install_policy;
};

struct WorkflowAgent {
    QString id;
    QString model_policy;
    QString tool_policy;
    int token_budget{0};
};

struct WorkflowPhase {
    QString id;
    QString type;
    QString agent;
    QString tool;
    QString operation;
    QJsonObject arguments;
    QString condition;
    QString prompt;
    QString expected_output;
    QString risk;
    QString completion;
    bool always_run{false};
};

struct WorkflowCancelPolicy {
    bool cancel_children{true};
    bool cancel_tools{true};
    bool preserve_partial_artifacts{true};
    bool report_partial_state{true};
};

struct WorkflowTemplate {
    int schema_version{0};
    QString id;
    QString title;
    QString role;
    QString category;
    QString description;
    QString starter_prompt;
    QString source_path;
    QVector<WorkflowRequiredInput> required_inputs;
    QVector<WorkflowRequirement> required_software;
    QStringList instructions;
    QStringList skills;
    QVector<WorkflowAgent> agents;
    QVector<WorkflowPhase> phases;
    QStringList acceptance_criteria;
    WorkflowCancelPolicy cancel_policy;

    [[nodiscard]] bool isValid(QStringList* errors = nullptr) const;
    [[nodiscard]] QString promptSummary() const;

    [[nodiscard]] static WorkflowTemplate fromJson(const QJsonObject& object,
                                                   const QString& source_path,
                                                   QStringList* errors = nullptr);
    [[nodiscard]] static WorkflowTemplate fromJsonBytes(const QByteArray& bytes,
                                                        const QString& source_path,
                                                        QStringList* errors = nullptr);
};

}  // namespace sak::ai
