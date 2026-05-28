// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_provider_registry.h"

#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>

namespace sak::ai {

inline constexpr int kDefaultProviderGatewayTimeoutMs = 20'000;

class AiProviderGateway {
public:
    struct Win32McpCallPlan {
        QJsonObject provider;
        QString tool_name;
        QJsonObject tool_arguments;
        QString security_profile;
        int timeout_ms = kDefaultProviderGatewayTimeoutMs;
        bool read_only = false;
        bool high_risk = false;
        QString preview;
    };

    explicit AiProviderGateway(AiProviderRegistry registry = AiProviderRegistry{});

    [[nodiscard]] QJsonObject providers(QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerStatuses(QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerStatus(const QString& provider_id,
                                             QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject docsQuery(const QJsonObject& args,
                                        QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject appManifest(const QString& app_id,
                                          QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject appCapabilities(const QString& app_id,
                                              const QString& action,
                                              QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject checkAvailability(const QJsonObject& args,
                                                QString* error_message = nullptr) const;

    [[nodiscard]] Win32McpCallPlan planWin32McpCall(const QJsonObject& args,
                                                    QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject callWin32Mcp(const Win32McpCallPlan& plan,
                                           QString* error_message = nullptr) const;

    [[nodiscard]] static bool isWin32ReadOnlyTool(const QString& tool_name);
    [[nodiscard]] static bool isWin32HighRiskTool(const QString& tool_name);
    [[nodiscard]] static QProcessEnvironment win32McpEnvironment(const QString& security_profile,
                                                                 const QJsonObject& provider);
    [[nodiscard]] static QJsonObject win32McpResult(const QJsonObject& provider,
                                                    const QString& tool_name,
                                                    const QJsonObject& tool_arguments,
                                                    const QString& security_profile,
                                                    const QJsonObject& mcp_message);

private:
    AiProviderRegistry m_registry;
};

}  // namespace sak::ai
