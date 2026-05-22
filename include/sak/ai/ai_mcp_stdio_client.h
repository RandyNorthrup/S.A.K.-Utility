// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>

namespace sak::ai {

struct AiMcpStdioCallRequest {
    QString command;
    QString tool_name;
    QJsonObject arguments;
    QProcessEnvironment environment;
    int timeout_ms{0};
};

class AiMcpStdioClient {
public:
    [[nodiscard]] static QJsonObject callTool(const AiMcpStdioCallRequest& request,
                                              QString* error_message = nullptr);

    [[nodiscard]] static QJsonObject initializePayloadForTesting();
    [[nodiscard]] static QJsonObject toolCallPayloadForTesting(int id,
                                                               const QString& tool_name,
                                                               const QJsonObject& arguments);
};

}  // namespace sak::ai
