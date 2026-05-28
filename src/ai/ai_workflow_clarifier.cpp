// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_clarifier.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

namespace sak::ai {

namespace {

constexpr qsizetype kMinimumSpecificAppValueChars = 2;
constexpr qsizetype kGenericUserMessageWordLimit = 3;

bool hasValue(const QJsonValue& value) {
    if (value.isString()) {
        return !value.toString().trimmed().isEmpty();
    }
    if (value.isArray()) {
        return !value.toArray().isEmpty();
    }
    return !value.isNull() && !value.isUndefined();
}

bool isAppLikeInput(const WorkflowRequiredInput& input) {
    const QString id = input.id.trimmed().toLower();
    const QString label = input.label.trimmed().toLower();
    return id.contains(QStringLiteral("app")) || id.contains(QStringLiteral("package")) ||
           id.contains(QStringLiteral("product")) || id.contains(QStringLiteral("query")) ||
           id.contains(QStringLiteral("name")) || label.contains(QStringLiteral("app")) ||
           label.contains(QStringLiteral("package")) || label.contains(QStringLiteral("product"));
}

QString firstTextValue(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        for (const auto& item : array) {
            const QString text = item.toString().trimmed();
            if (!text.isEmpty()) {
                return text;
            }
        }
    }
    return {};
}

}  // namespace

bool AiWorkflowClarifier::looksAmbiguousAppValue(const QString& value,
                                                 const QString& user_message) {
    const QString text = value.trimmed().toLower();
    if (text.isEmpty()) {
        return true;
    }
    if (text.size() < kMinimumSpecificAppValueChars) {
        return true;
    }
    static const QStringList generic_terms{
        QStringLiteral("app"),
        QStringLiteral("application"),
        QStringLiteral("program"),
        QStringLiteral("software"),
        QStringLiteral("installer"),
        QStringLiteral("package"),
        QStringLiteral("tool"),
        QStringLiteral("thing"),
        QStringLiteral("it"),
        QStringLiteral("that"),
        QStringLiteral("this"),
    };
    if (generic_terms.contains(text)) {
        return true;
    }
    if (text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts).size() == 1 &&
        user_message.trimmed()
                .split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts)
                .size() <= kGenericUserMessageWordLimit &&
        generic_terms.contains(user_message.trimmed().toLower())) {
        return true;
    }
    return false;
}

AiWorkflowClarificationResult AiWorkflowClarifier::analyze(const WorkflowTemplate& workflow,
                                                           const QString& user_message,
                                                           const QJsonObject& input_values) {
    AiWorkflowClarificationResult result;
    for (const auto& input : workflow.required_inputs) {
        const QString id = input.id.trimmed();
        if (id.isEmpty() || !input.required) {
            continue;
        }
        const QString label = input.label.trimmed().isEmpty() ? id : input.label.trimmed();
        const QJsonValue value = input_values.value(id);
        if (!hasValue(value)) {
            AiWorkflowClarificationQuestion question;
            question.input_id = id;
            question.label = label;
            question.type = input.type;
            question.reason = QStringLiteral("required input is missing");
            question.question = QStringLiteral("What should I use for %1?").arg(label);
            result.questions.append(question);
            continue;
        }
        if (isAppLikeInput(input) && looksAmbiguousAppValue(firstTextValue(value), user_message)) {
            AiWorkflowClarificationQuestion question;
            question.input_id = id;
            question.label = label;
            question.type = input.type;
            question.reason = QStringLiteral("app/package value is ambiguous");
            question.question =
                QStringLiteral("Which exact app, package, or product should I use for %1?")
                    .arg(label);
            result.questions.append(question);
        }
    }
    return result;
}

}  // namespace sak::ai
