// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_call_router.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace sak::ai {

namespace {

QString normalizedToolName(const QString& name) {
    return name.trimmed().toLower();
}

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

}  // namespace

AiToolCallKind AiToolCallRouter::kindForName(const QString& name) {
    const QString normalized = normalizedToolName(name);
    if (normalized == QLatin1String("run_powershell") || normalized == QLatin1String("run_cmd")) {
        return AiToolCallKind::Shell;
    }
    if (normalized == QLatin1String("run_process")) {
        return AiToolCallKind::Process;
    }
    if (normalized == QLatin1String("take_screenshot")) {
        return AiToolCallKind::Screenshot;
    }
    if (normalized == QLatin1String("download_file")) {
        return AiToolCallKind::Download;
    }
    if (normalized == QLatin1String("sak_package_manager")) {
        return AiToolCallKind::PackageManager;
    }
    if (normalized == QLatin1String("sak_offline_downloader")) {
        return AiToolCallKind::OfflineDownloader;
    }
    if (normalized == QLatin1String("sak_provider_gateway")) {
        return AiToolCallKind::ProviderGateway;
    }
    if (normalized == QLatin1String("sak_session_search")) {
        return AiToolCallKind::SessionSearch;
    }
    return AiToolCallKind::Unknown;
}

bool AiToolCallRouter::isCommandTool(AiToolCallKind kind) {
    return kind == AiToolCallKind::Shell || kind == AiToolCallKind::Process;
}

bool AiToolCallRouter::isBuiltInTool(AiToolCallKind kind) {
    return kind != AiToolCallKind::Unknown && !isCommandTool(kind);
}

QJsonObject AiToolCallRouter::metadataFor(const OpenAIFunctionCall& call, int index) {
    QJsonObject metadata;
    metadata[QStringLiteral("call_id")] = call.call_id;
    metadata[QStringLiteral("name")] = call.name;
    metadata[QStringLiteral("index")] = index;
    return metadata;
}

AiToolCallRouter::PreparedCall AiToolCallRouter::prepare(const OpenAIFunctionCall& call,
                                                         int index) {
    PreparedCall prepared;
    prepared.kind = kindForName(call.name);
    prepared.metadata = metadataFor(call, index);
    prepared.output.call_id = call.call_id;
    prepared.recognized = prepared.kind != AiToolCallKind::Unknown;
    if (!prepared.recognized) {
        prepared.output = errorOutput(call, QStringLiteral("Unknown function"));
    }
    return prepared;
}

AiToolCallRouter::ParsedArguments AiToolCallRouter::parseArguments(const OpenAIFunctionCall& call) {
    ParsedArguments parsed;
    parsed.output.call_id = call.call_id;

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(call.arguments_json.toUtf8(), &parse_error);
    if (parse_error.error == QJsonParseError::NoError && doc.isObject()) {
        parsed.arguments = doc.object();
        parsed.ok = true;
        return parsed;
    }

    parsed.error_message = QStringLiteral("Invalid %1 arguments").arg(call.name);
    parsed.output = errorOutput(call, parsed.error_message);
    return parsed;
}

OpenAIFunctionOutput AiToolCallRouter::errorOutput(const OpenAIFunctionCall& call,
                                                   const QString& message,
                                                   const QJsonObject& extra) {
    QJsonObject output = extra;
    output[QStringLiteral("error")] = message;

    OpenAIFunctionOutput result;
    result.call_id = call.call_id;
    result.output = compactJson(output);
    return result;
}

OpenAIFunctionOutput AiToolCallRouter::cancelledOutput(const OpenAIFunctionCall& call) {
    return errorOutput(call,
                       QStringLiteral("Cancelled before dispatch"),
                       QJsonObject{{QStringLiteral("cancelled"), true}});
}

}  // namespace sak::ai
