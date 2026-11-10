// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

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

[[nodiscard]] constexpr bool providerGatewayAccessIsChatOnly(AiProviderGatewayToolAccess access) {
    return access == AiProviderGatewayToolAccess::ChatAndResearch;
}

[[nodiscard]] constexpr bool providerGatewayAccessIsAssisted(AiProviderGatewayToolAccess access) {
    return access == AiProviderGatewayToolAccess::AssistedFullAccess;
}

[[nodiscard]] constexpr bool providerGatewayAccessIsUnattended(AiProviderGatewayToolAccess access) {
    return access == AiProviderGatewayToolAccess::UnattendedFullAccess;
}

/// The human gates a Win32 MCP call may need: a confirmation, and (Unattended, high-risk) a
/// restore-point offer. Both are required for the calls that use them; a missing one refuses.
struct AiProviderGatewayToolCallbacks {
    std::function<bool(const QString& title, const QString& preview, bool risky)> confirm;
    std::function<bool(const QString& preview, bool risky)> offer_restore_point;
};

class AiProviderGatewayToolRunner {
public:
    [[nodiscard]] static QJsonObject run(const QJsonObject& args,
                                         const AiProviderGateway* gateway,
                                         AiProviderGatewayToolAccess access,
                                         const AiProviderGatewayToolCallbacks& callbacks);
};

}  // namespace sak::ai
