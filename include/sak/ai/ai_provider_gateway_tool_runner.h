// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_provider_gateway.h"

#include <QJsonObject>
#include <QString>

#include <functional>

namespace sak::ai {

enum class AiProviderGatewayToolAccess {
    ChatAndResearch,
    AssistedFullAccess,
    UnattendedFullAccess,
};

struct AiProviderGatewayToolOptions {
    int default_output_bytes = 512 * 1024;
    int min_output_bytes = 1024;
    int max_output_bytes = 4 * 1024 * 1024;
};

struct AiProviderGatewayToolCallbacks {
    std::function<bool(const QString& title, const QString& preview, bool risky)> confirm;
    std::function<bool(const QString& preview, bool risky)> offer_restore_point;
    std::function<QString()> allocate_command_id;
    std::function<AiCommandResult(const AiCommandRequest& request, const QString& command_id)>
        execute_powershell;
    std::function<void(const QString& message)> append_local_event;
    std::function<void(const QString& line)> log_output;
    std::function<void(const QString& preview, const QJsonObject& result)> record_command;
    std::function<void(const QString& role, const QString& title, const QString& body)>
        append_session_memory;
};

class AiProviderGatewayToolRunner {
public:
    [[nodiscard]] static QJsonObject run(const QJsonObject& args,
                                         AiProviderGateway* gateway,
                                         AiProviderGatewayToolAccess access,
                                         const AiProviderGatewayToolCallbacks& callbacks,
                                         AiProviderGatewayToolOptions options = {});
};

}  // namespace sak::ai
