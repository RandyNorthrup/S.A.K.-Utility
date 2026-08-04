// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_template.h"

#include "sak/ai/ai_workflow_placeholders.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

namespace sak::ai {

namespace {

QString cleanString(const QJsonObject& object, const QString& key) {
    return object.value(key).toString().trimmed();
}

QStringList stringListFromJson(const QJsonValue& value) {
    QStringList result;
    const QJsonArray array = value.toArray();
    for (const auto& item : array) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) {
            result.append(text);
        }
    }
    return result;
}

QVector<WorkflowRequiredInput> requiredInputsFromJson(const QJsonValue& value) {
    QVector<WorkflowRequiredInput> result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const auto& item : array) {
        if (!item.isObject()) {
            continue;
        }
        const QJsonObject object = item.toObject();
        WorkflowRequiredInput input;
        input.id = cleanString(object, QStringLiteral("id"));
        input.label = cleanString(object, QStringLiteral("label"));
        input.type = cleanString(object, QStringLiteral("type"));
        input.required = object.value(QStringLiteral("required")).toBool(false);
        result.append(input);
    }
    return result;
}

QVector<WorkflowRequirement> requirementsFromJson(const QJsonValue& value) {
    QVector<WorkflowRequirement> result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const auto& item : array) {
        if (!item.isObject()) {
            continue;
        }
        const QJsonObject object = item.toObject();
        WorkflowRequirement requirement;
        requirement.id = cleanString(object, QStringLiteral("id"));
        requirement.kind = cleanString(object, QStringLiteral("kind"));
        requirement.required = object.value(QStringLiteral("required")).toBool(false);
        requirement.install_policy = cleanString(object, QStringLiteral("install_policy"));
        result.append(requirement);
    }
    return result;
}

QVector<WorkflowAgent> agentsFromJson(const QJsonValue& value) {
    QVector<WorkflowAgent> result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const auto& item : array) {
        if (!item.isObject()) {
            continue;
        }
        const QJsonObject object = item.toObject();
        WorkflowAgent agent;
        agent.id = cleanString(object, QStringLiteral("id"));
        agent.model_policy = cleanString(object, QStringLiteral("model_policy"));
        agent.tool_policy = cleanString(object, QStringLiteral("tool_policy"));
        agent.token_budget = object.value(QStringLiteral("token_budget")).toInt(0);
        result.append(agent);
    }
    return result;
}

QVector<WorkflowPhase> phasesFromJson(const QJsonValue& value) {
    QVector<WorkflowPhase> result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const auto& item : array) {
        if (!item.isObject()) {
            continue;
        }
        const QJsonObject object = item.toObject();
        WorkflowPhase phase;
        phase.id = cleanString(object, QStringLiteral("id"));
        phase.type = cleanString(object, QStringLiteral("type"));
        phase.agent = cleanString(object, QStringLiteral("agent"));
        phase.tool = cleanString(object, QStringLiteral("tool"));
        phase.operation = cleanString(object, QStringLiteral("operation"));
        phase.arguments = object.value(QStringLiteral("arguments")).toObject();
        phase.condition = cleanString(object, QStringLiteral("condition"));
        phase.prompt = cleanString(object, QStringLiteral("prompt"));
        phase.expected_output = cleanString(object, QStringLiteral("expected_output"));
        phase.risk = cleanString(object, QStringLiteral("risk"));
        phase.completion = cleanString(object, QStringLiteral("completion"));
        phase.always_run = object.value(QStringLiteral("always_run")).toBool(false);
        if (!phase.always_run) {
            const QString type_lower = phase.type.toLower();
            if (type_lower == QLatin1String("cleanup")) {
                phase.always_run = true;
            }
        }
        result.append(phase);
    }
    return result;
}

WorkflowCancelPolicy cancelPolicyFromJson(const QJsonValue& value) {
    WorkflowCancelPolicy policy;
    if (!value.isObject()) {
        return policy;
    }
    const QJsonObject object = value.toObject();
    policy.cancel_children = object.value(QStringLiteral("cancel_children")).toBool(true);
    policy.cancel_tools = object.value(QStringLiteral("cancel_tools")).toBool(true);
    policy.preserve_partial_artifacts =
        object.value(QStringLiteral("preserve_partial_artifacts")).toBool(true);
    policy.report_partial_state = object.value(QStringLiteral("report_partial_state")).toBool(true);
    return policy;
}

void addMissingError(QStringList* errors, const QString& field) {
    if (errors != nullptr) {
        errors->append(QStringLiteral("Missing required field: %1").arg(field));
    }
}

void validateWorkflowRequiredFields(const WorkflowTemplate& workflow, QStringList* errors) {
    if (workflow.schema_version != 1) {
        errors->append(
            QStringLiteral("Unsupported schema_version: %1").arg(workflow.schema_version));
    }
    if (workflow.id.isEmpty()) {
        addMissingError(errors, QStringLiteral("id"));
    }
    if (workflow.title.isEmpty()) {
        addMissingError(errors, QStringLiteral("title"));
    }
    if (workflow.role.isEmpty()) {
        addMissingError(errors, QStringLiteral("role"));
    }
    if (workflow.starter_prompt.isEmpty()) {
        addMissingError(errors, QStringLiteral("starter_prompt"));
    }
    if (workflow.phases.isEmpty()) {
        addMissingError(errors, QStringLiteral("phases"));
    }
}

[[nodiscard]] bool isKnownPhaseType(const QString& type) {
    return type.compare(QStringLiteral("delegate"), Qt::CaseInsensitive) == 0 ||
           type.compare(QStringLiteral("tool_action"), Qt::CaseInsensitive) == 0 ||
           type.compare(QStringLiteral("cleanup"), Qt::CaseInsensitive) == 0 ||
           type.compare(QStringLiteral("overseer"), Qt::CaseInsensitive) == 0;
}

void validateWorkflowPhases(const WorkflowTemplate& workflow, QStringList* errors) {
    QSet<QString> seen_ids;
    for (const auto& phase : workflow.phases) {
        if (phase.id.isEmpty()) {
            errors->append(QStringLiteral("Phase missing id in workflow %1").arg(workflow.id));
        } else if (seen_ids.contains(phase.id)) {
            // Duplicate phase ids alias each other in phase_results (later results silently
            // overwrite earlier ones and placeholders resolve to the wrong phase): reject.
            errors->append(QStringLiteral("Duplicate phase id '%1' in workflow %2")
                               .arg(phase.id, workflow.id));
        } else {
            seen_ids.insert(phase.id);
        }
        if (phase.type.isEmpty()) {
            errors->append(QStringLiteral("Phase %1 missing type").arg(phase.id));
        } else if (!isKnownPhaseType(phase.type)) {
            // Reject unknown types at load: at runtime they have no handler and only
            // surface as a per-phase error, hiding the author's typo (e.g. "tool-action").
            errors->append(
                QStringLiteral("Phase %1 has unsupported type '%2'").arg(phase.id, phase.type));
        }
        if (phase.prompt.isEmpty() && phase.completion.isEmpty()) {
            errors->append(QStringLiteral("Phase %1 needs prompt or completion").arg(phase.id));
        }
        // A run_powershell command is substituted in PowerShellSingleQuoted mode, which only
        // escapes embedded single quotes: reject at load any command whose ${...} placeholder is
        // NOT inside a single-quoted literal (raw injection risk), for user AND bundled templates.
        if (phase.tool == QLatin1String("run_powershell")) {
            QString ps_error;
            const QString command = phase.arguments.value(QStringLiteral("command")).toString();
            if (!powerShellCommandTemplateIsSingleQuoteSafe(command, &ps_error)) {
                errors->append(QStringLiteral("Phase %1: %2").arg(phase.id, ps_error));
            }
        }
    }
}

void validateWorkflowInputs(const WorkflowTemplate& workflow, QStringList* errors) {
    QSet<QString> seen_ids;
    for (const auto& input : workflow.required_inputs) {
        if (input.id.isEmpty()) {
            errors->append(
                QStringLiteral("Workflow %1 has a required input with no id").arg(workflow.id));
            continue;
        }
        if (seen_ids.contains(input.id)) {
            // Duplicate input ids collide in input_values; the second wins silently.
            errors->append(QStringLiteral("Duplicate required-input id '%1' in workflow %2")
                               .arg(input.id, workflow.id));
        } else {
            seen_ids.insert(input.id);
        }
    }
}

void appendRequiredInputSummary(QStringList* lines, const QVector<WorkflowRequiredInput>& inputs) {
    if (inputs.isEmpty()) {
        return;
    }
    *lines << QStringLiteral("Required inputs:");
    for (const auto& input : inputs) {
        *lines << QStringLiteral("- %1 (%2)%3")
                      .arg(input.label.isEmpty() ? input.id : input.label,
                           input.type.isEmpty() ? QStringLiteral("text") : input.type,
                           input.required ? QStringLiteral(" required") : QString());
    }
}

void appendRequirementSummary(QStringList* lines,
                              const QVector<WorkflowRequirement>& requirements) {
    if (requirements.isEmpty()) {
        return;
    }
    *lines << QStringLiteral("Required software/tools:");
    for (const auto& requirement : requirements) {
        *lines << QStringLiteral("- %1 [%2, %3]")
                      .arg(requirement.id, requirement.kind, requirement.install_policy);
    }
}

void appendAgentSummary(QStringList* lines, const QVector<WorkflowAgent>& agents) {
    if (agents.isEmpty()) {
        return;
    }
    *lines << QStringLiteral("Specialists:");
    for (const auto& agent : agents) {
        *lines << QStringLiteral("- %1 (%2)").arg(agent.id, agent.tool_policy);
    }
}

void appendPhaseSummary(QStringList* lines, const QVector<WorkflowPhase>& phases) {
    if (phases.isEmpty()) {
        return;
    }
    *lines << QStringLiteral("Phases:");
    for (const auto& phase : phases) {
        const QString owner = phase.agent.isEmpty() ? phase.type : phase.agent;
        *lines << QStringLiteral("- %1: %2").arg(phase.id, owner);
        if (!phase.prompt.isEmpty()) {
            *lines << QStringLiteral("  %1").arg(phase.prompt);
        }
        if (!phase.completion.isEmpty()) {
            *lines << QStringLiteral("  Done when: %1").arg(phase.completion);
        }
    }
}

void appendStringListSummary(QStringList* lines,
                             const QString& heading,
                             const QStringList& values) {
    if (values.isEmpty()) {
        return;
    }
    *lines << heading;
    for (const auto& value : values) {
        *lines << QStringLiteral("- %1").arg(value);
    }
}

}  // namespace

bool WorkflowTemplate::isValid(QStringList* errors) const {
    QStringList local_errors;
    QStringList* target = errors != nullptr ? errors : &local_errors;
    // Judge validity by whether THIS workflow adds errors, not by whether the (possibly
    // shared, accumulating) list is empty: a caller that reuses one list across files
    // must not have an earlier file's error invalidate a later valid one.
    const qsizetype before = target->size();
    validateWorkflowRequiredFields(*this, target);
    validateWorkflowPhases(*this, target);
    validateWorkflowInputs(*this, target);
    return target->size() == before;
}

QString WorkflowTemplate::promptSummary() const {
    QStringList lines;
    lines << QStringLiteral("Workflow: %1").arg(title);
    if (!description.isEmpty()) {
        lines << QStringLiteral("Purpose: %1").arg(description);
    }
    lines << QStringLiteral("Task: %1").arg(starter_prompt);

    appendRequiredInputSummary(&lines, required_inputs);
    appendRequirementSummary(&lines, required_software);
    appendAgentSummary(&lines, agents);
    appendPhaseSummary(&lines, phases);
    appendStringListSummary(&lines, QStringLiteral("Acceptance criteria:"), acceptance_criteria);
    appendStringListSummary(&lines, QStringLiteral("Instruction files:"), instructions);
    appendStringListSummary(&lines, QStringLiteral("Skills:"), skills);

    return lines.join(QLatin1Char('\n'));
}

WorkflowTemplate WorkflowTemplate::fromJson(const QJsonObject& object,
                                            const QString& source_path,
                                            QStringList* errors) {
    WorkflowTemplate workflow;
    workflow.source_path = source_path;
    workflow.schema_version = object.value(QStringLiteral("schema_version")).toInt(0);
    workflow.id = cleanString(object, QStringLiteral("id"));
    workflow.title = cleanString(object, QStringLiteral("title"));
    workflow.role = cleanString(object, QStringLiteral("role"));
    workflow.category = cleanString(object, QStringLiteral("category"));
    workflow.description = cleanString(object, QStringLiteral("description"));
    workflow.starter_prompt = cleanString(object, QStringLiteral("starter_prompt"));
    workflow.required_inputs =
        requiredInputsFromJson(object.value(QStringLiteral("required_inputs")));
    workflow.required_software =
        requirementsFromJson(object.value(QStringLiteral("required_software")));
    workflow.instructions = stringListFromJson(object.value(QStringLiteral("instructions")));
    workflow.skills = stringListFromJson(object.value(QStringLiteral("skills")));
    workflow.agents = agentsFromJson(object.value(QStringLiteral("agents")));
    workflow.phases = phasesFromJson(object.value(QStringLiteral("phases")));
    workflow.acceptance_criteria =
        stringListFromJson(object.value(QStringLiteral("acceptance_criteria")));
    workflow.cancel_policy = cancelPolicyFromJson(object.value(QStringLiteral("cancel_policy")));
    (void)workflow.isValid(errors);
    return workflow;
}

WorkflowTemplate WorkflowTemplate::fromJsonBytes(const QByteArray& bytes,
                                                 const QString& source_path,
                                                 QStringList* errors) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("%1: invalid workflow JSON: %2")
                               .arg(source_path, parse_error.errorString()));
        }
        WorkflowTemplate invalid;
        invalid.source_path = source_path;
        return invalid;
    }
    return fromJson(document.object(), source_path, errors);
}

}  // namespace sak::ai
