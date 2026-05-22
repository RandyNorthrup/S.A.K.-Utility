// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace sak::ai {

class AiMcpHttpClient {
public:
    [[nodiscard]] static QJsonObject callTool(const QUrl& endpoint,
                                              const QString& tool_name,
                                              const QJsonObject& arguments,
                                              int timeout_ms,
                                              QString* error_message = nullptr);

    [[nodiscard]] static QJsonObject extractJsonRpcMessageForTesting(
        const QByteArray& response_body, QString* error_message = nullptr);
    [[nodiscard]] static QJsonObject toolCallPayloadForTesting(const QString& tool_name,
                                                               const QJsonObject& arguments);
};

}  // namespace sak::ai
