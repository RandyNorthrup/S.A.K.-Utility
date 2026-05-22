// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/openai_response_types.h"

#include <QJsonObject>
#include <QString>

namespace sak::ai {

enum class AiToolCallKind {
    Unknown,
    Shell,
    Process,
    Screenshot,
    Download,
    PackageManager,
    OfflineDownloader,
    ProviderGateway,
    SessionSearch,
};

class AiToolCallRouter {
public:
    struct PreparedCall {
        AiToolCallKind kind{AiToolCallKind::Unknown};
        QJsonObject metadata;
        OpenAIFunctionOutput output;
        bool recognized{false};
    };

    struct ParsedArguments {
        QJsonObject arguments;
        OpenAIFunctionOutput output;
        QString error_message;
        bool ok{false};
    };

    [[nodiscard]] static AiToolCallKind kindForName(const QString& name);
    [[nodiscard]] static bool isCommandTool(AiToolCallKind kind);
    [[nodiscard]] static bool isBuiltInTool(AiToolCallKind kind);
    [[nodiscard]] static QJsonObject metadataFor(const OpenAIFunctionCall& call, int index);
    [[nodiscard]] static PreparedCall prepare(const OpenAIFunctionCall& call, int index);
    [[nodiscard]] static ParsedArguments parseArguments(const OpenAIFunctionCall& call);
    [[nodiscard]] static OpenAIFunctionOutput errorOutput(const OpenAIFunctionCall& call,
                                                          const QString& message,
                                                          const QJsonObject& extra = {});
    [[nodiscard]] static OpenAIFunctionOutput cancelledOutput(const OpenAIFunctionCall& call);
};

}  // namespace sak::ai
