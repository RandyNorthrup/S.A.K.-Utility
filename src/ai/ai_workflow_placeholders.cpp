// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_placeholders.h"

#include "sak/ai/ai_orchestrator.h"

#include <QJsonArray>
#include <QLatin1Char>
#include <QLatin1String>
#include <QRegularExpression>

namespace sak::ai {

namespace {

QString scalarJsonValueToString(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'f', 0);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return {};
}

QString workflowPhaseResultValue(const AiWorkflowPhaseContext& context, const QString& key) {
    if (!key.startsWith(QStringLiteral("result_"))) {
        return {};
    }
    const QString tail = key.mid(7);
    const QStringList known_fields{
        QStringLiteral("error_message"),
        QStringLiteral("source_url"),
        QStringLiteral("size_bytes"),
        QStringLiteral("stdout_path"),
        QStringLiteral("stderr_path"),
        QStringLiteral("exit_code"),
        QStringLiteral("sha256"),
        QStringLiteral("path"),
        QStringLiteral("success"),
    };
    for (const auto& field : known_fields) {
        const QString suffix = QStringLiteral("_%1").arg(field);
        if (!tail.endsWith(suffix)) {
            continue;
        }
        const QString phase_id = tail.left(tail.size() - suffix.size());
        const QJsonObject phase_result = context.phase_results.value(phase_id).toObject();
        if (phase_result.isEmpty()) {
            return {};
        }
        const QString direct = scalarJsonValueToString(phase_result.value(field));
        if (!direct.isEmpty()) {
            return direct;
        }
        return scalarJsonValueToString(
            phase_result.value(QStringLiteral("tool_result")).toObject().value(field));
    }
    return {};
}

QString workflowPlaceholderValue(const AiWorkflowPhaseContext& context,
                                 const QString& key,
                                 WorkflowPlaceholderMode mode) {
    QString value;
    if (key == QLatin1String("user_message")) {
        value = context.user_message.trimmed();
    } else if (key == QLatin1String("workflow_id")) {
        value = context.workflow_id.trimmed();
    } else if (key == QLatin1String("run_id")) {
        value = context.run_id.trimmed();
    } else if (key.startsWith(QStringLiteral("result_"))) {
        value = workflowPhaseResultValue(context, key);
    } else {
        value = workflowInputValue(context, key);
    }
    if (mode == WorkflowPlaceholderMode::PowerShellSingleQuoted) {
        value.replace(QLatin1Char('\''), QStringLiteral("''"));
    }
    return value;
}

}  // namespace

QString workflowInputValue(const AiWorkflowPhaseContext& context,
                           const QString& key,
                           const QString& fallback) {
    const auto value = context.input_values.value(key);
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        return text.isEmpty() ? fallback : text;
    }
    if (value.isArray()) {
        QStringList parts;
        const auto array = value.toArray();
        for (const auto& item : array) {
            if (item.isString() && !item.toString().trimmed().isEmpty()) {
                parts << item.toString().trimmed();
            }
        }
        return parts.isEmpty() ? fallback : parts.join(QStringLiteral(", "));
    }
    return fallback;
}

QString substituteWorkflowPlaceholders(const QString& text,
                                       const AiWorkflowPhaseContext& context,
                                       WorkflowPlaceholderMode mode) {
    static const QRegularExpression placeholder_pattern(QStringLiteral(R"(\$\{([A-Za-z0-9_]+)\})"));
    QString result;
    result.reserve(text.size());
    qsizetype cursor = 0;
    auto matches = placeholder_pattern.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(cursor, match.capturedStart() - cursor);
        result += workflowPlaceholderValue(context, match.captured(1), mode);
        cursor = match.capturedEnd();
    }
    result += text.mid(cursor);
    return result;
}

QJsonObject substituteWorkflowPlaceholdersInObject(const QJsonObject& object,
                                                   const AiWorkflowPhaseContext& context,
                                                   WorkflowPlaceholderMode mode) {
    QJsonObject substituted;
    for (auto it = object.begin(); it != object.end(); ++it) {
        substituted.insert(it.key(),
                           substituteWorkflowPlaceholdersInValue(it.value(), context, mode));
    }
    return substituted;
}

QJsonValue substituteWorkflowPlaceholdersInValue(const QJsonValue& value,
                                                 const AiWorkflowPhaseContext& context,
                                                 WorkflowPlaceholderMode mode) {
    if (value.isString()) {
        return substituteWorkflowPlaceholders(value.toString(), context, mode);
    }
    if (value.isArray()) {
        QJsonArray array;
        const auto values = value.toArray();
        for (const auto& item : values) {
            array.append(substituteWorkflowPlaceholdersInValue(item, context, mode));
        }
        return array;
    }
    if (value.isObject()) {
        return substituteWorkflowPlaceholdersInObject(value.toObject(), context, mode);
    }
    return value;
}

}  // namespace sak::ai
