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

bool containsAny(const QString& haystack, const QStringList& needles) {
    for (const QString& needle : needles) {
        if (haystack.contains(needle)) {
            return true;
        }
    }
    return false;
}

bool isAppLikeInput(const WorkflowRequiredInput& input) {
    const QString id = input.id.trimmed().toLower();
    const QString label = input.label.trimmed().toLower();
    // The id carries two extra tokens the label does not: a "query"/"name" field names
    // a destructive target here even when its label never says "app".
    static const QStringList kIdTerms{
        QStringLiteral("app"),
        QStringLiteral("package"),
        QStringLiteral("product"),
        QStringLiteral("query"),
        QStringLiteral("name"),
        QStringLiteral("software"),
        QStringLiteral("target"),
    };
    static const QStringList kLabelTerms{
        QStringLiteral("app"),
        QStringLiteral("package"),
        QStringLiteral("product"),
        QStringLiteral("software"),
        QStringLiteral("target"),
    };
    return containsAny(id, kIdTerms) || containsAny(label, kLabelTerms);
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

const QStringList& genericAppTerms() {
    static const QStringList kTerms{
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
    return kTerms;
}

QStringList significantWords(const QStringList& raw_words) {
    // Vague qualifiers carry no target identity of their own ("the app", "some
    // software", "any package", "whatever"). Strip them and any surrounding
    // punctuation, then the value is ambiguous when nothing specific remains or
    // every remaining word is itself a generic placeholder. An exact-token check
    // alone lets multi-word and trailing-punctuation forms ("app.", "the app")
    // slip through as if they were specific destructive targets.
    static const QStringList kQualifierTerms{
        QStringLiteral("the"),
        QStringLiteral("a"),
        QStringLiteral("an"),
        QStringLiteral("some"),
        QStringLiteral("any"),
        QStringLiteral("my"),
        QStringLiteral("your"),
        QStringLiteral("our"),
        QStringLiteral("that"),
        QStringLiteral("this"),
        QStringLiteral("these"),
        QStringLiteral("those"),
        QStringLiteral("other"),
        QStringLiteral("another"),
        QStringLiteral("default"),
        QStringLiteral("whatever"),
        QStringLiteral("just"),
    };
    QStringList significant;
    for (const QString& raw : raw_words) {
        QString word = raw;
        while (!word.isEmpty() && !word.front().isLetterOrNumber()) {
            word.remove(0, 1);
        }
        while (!word.isEmpty() && !word.back().isLetterOrNumber()) {
            word.chop(1);
        }
        if (word.isEmpty() || kQualifierTerms.contains(word)) {
            continue;
        }
        significant.append(word);
    }
    return significant;
}

bool allTermsGeneric(const QStringList& words) {
    for (const QString& word : words) {
        if (!genericAppTerms().contains(word)) {
            return false;
        }
    }
    return true;
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
    const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
    const QStringList raw_words = text.split(whitespace, Qt::SkipEmptyParts);
    const QStringList significant = significantWords(raw_words);
    if (significant.isEmpty()) {
        return true;
    }
    if (allTermsGeneric(significant)) {
        return true;
    }
    if (raw_words.size() == 1 &&
        user_message.trimmed().split(whitespace, Qt::SkipEmptyParts).size() <=
            kGenericUserMessageWordLimit &&
        genericAppTerms().contains(user_message.trimmed().toLower())) {
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
