// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_provider_gateway.h"
#include "sak/layout_constants.h"

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
    static constexpr int kDefaultOutputKilobytes = 512;
    static constexpr int kMaximumOutputMegabytes = 4;

    int default_output_bytes = kDefaultOutputKilobytes * static_cast<int>(sak::kBytesPerKB);
    int min_output_bytes = static_cast<int>(sak::kBytesPerKB);
    /// Deliberately STRICTER than kAiCommandOutputBytesCeiling: a stricter local budget is a
    /// policy choice and works, because run() clamps to it and the broker accepts the result.
    /// Wider would not be a choice at all -- the broker REFUSES rather than clamps above its
    /// own ceiling, so a wider budget here would only decide which commands die at the door.
    int max_output_bytes = kMaximumOutputMegabytes * static_cast<int>(sak::kBytesPerMB);
};

static_assert(AiProviderGatewayToolOptions{}.max_output_bytes <= kAiCommandOutputBytesCeiling,
              "gateway tool output ceiling must not exceed what ExecutionBroker accepts");
static_assert(AiProviderGatewayToolOptions{}.min_output_bytes <=
                  AiProviderGatewayToolOptions{}.default_output_bytes,
              "gateway tool output floor must not exceed its default");
static_assert(AiProviderGatewayToolOptions{}.default_output_bytes <=
                  AiProviderGatewayToolOptions{}.max_output_bytes,
              "gateway tool output default must not exceed its ceiling");

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
                                         const AiProviderGateway* gateway,
                                         AiProviderGatewayToolAccess access,
                                         const AiProviderGatewayToolCallbacks& callbacks,
                                         AiProviderGatewayToolOptions options = {});
};

}  // namespace sak::ai
