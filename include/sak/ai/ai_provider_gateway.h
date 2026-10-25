// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_provider_registry.h"

#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>

#include <functional>
#include <utility>

namespace sak::ai {

inline constexpr int kDefaultProviderGatewayTimeoutMs = 20'000;

class AiMcpSessionPool;

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
        // Set for any tool that needs a HARD human confirmation in every non-chat access mode:
        // browser/desktop input, clipboard writes, extension changes, and unclassified tools.
        // Unattended does not bypass this explicit plan flag.
        bool requires_confirmation = false;
        QString preview;
    };

    explicit AiProviderGateway(AiProviderRegistry registry = AiProviderRegistry{});

    /// Inject the required persistent MCP session pool. Missing wiring makes Win32 MCP calls fail
    /// closed. Not owned; must outlive this gateway.
    void setMcpSessionPool(AiMcpSessionPool* pool) { m_mcp_pool = pool; }

    /// Cancellation for the Win32 MCP calls this gateway makes: polled while a call is in flight,
    /// and when it turns true the MCP server is terminated and the call returns promptly.
    void setCancelCheck(std::function<bool()> should_cancel) {
        m_should_cancel = std::move(should_cancel);
    }

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
    /// The win32_mcp_call leg of checkAvailability. Split out so the dispatcher stays a flat
    /// operation switch rather than carrying one operation's planning inline.
    [[nodiscard]] QJsonObject win32McpAvailability(const QJsonObject& args,
                                                   const QString& operation) const;

    [[nodiscard]] Win32McpCallPlan planWin32McpCall(const QJsonObject& args,
                                                    QString* error_message = nullptr) const;
    /// Executes a plan produced by planWin32McpCall. The plan is a plain struct, so this
    /// re-validates the execution-shaping fields it carries (security profile, timeout) and
    /// fails closed on anything outside the planner's range. It does NOT re-derive the
    /// authorization flags -- gating on read_only/high_risk/requires_confirmation is the
    /// caller's job and must happen on the plan this call is given.
    [[nodiscard]] QJsonObject callWin32Mcp(const Win32McpCallPlan& plan,
                                           QString* error_message = nullptr) const;

    [[nodiscard]] static bool isWin32ReadOnlyTool(const QString& tool_name);
    [[nodiscard]] static bool isWin32HighRiskTool(const QString& tool_name);
    /// Tools that act AS the user and therefore require an explicit human confirmation in every
    /// non-chat access mode: browser input injection into the active tab, physical desktop
    /// mouse/keyboard/UIA input, focus changes, system clipboard writes, and browser-extension
    /// install/uninstall. Any tool on none of the classifier lists is confirmed as well (see
    /// planWin32McpCall), so this list is the named subset, not the whole gate.
    [[nodiscard]] static bool isWin32InputTool(const QString& tool_name);
    /// The subset of input tools that drive the PHYSICAL desktop (mouse/keyboard/UIA/focus).
    /// Only these may auto-run inside a win32_gui recipe on the single recipe-level
    /// authorization; browser/clipboard/extension input tools still demand a per-call confirm.
    /// A newly added input tool defaults to REJECTED in recipes until whitelisted here.
    [[nodiscard]] static bool isWin32DesktopInputTool(const QString& tool_name);
    /// Builds the MCP child environment for @p security_profile. Fails CLOSED (empty
    /// environment, error_message set) on an unrecognized profile, on a provider manifest whose
    /// "environment" is not an object of strings, and on any manifest entry that would redirect
    /// the child's code or data resolution (PATH, QT_*, TEMP, LOCALAPPDATA, ...).
    [[nodiscard]] static QProcessEnvironment win32McpEnvironment(const QString& security_profile,
                                                                 const QJsonObject& provider,
                                                                 QString* error_message = nullptr);
    [[nodiscard]] static QJsonObject win32McpResult(const QJsonObject& provider,
                                                    const QString& tool_name,
                                                    const QJsonObject& tool_arguments,
                                                    const QString& security_profile,
                                                    const QJsonObject& mcp_message);

private:
    AiProviderRegistry m_registry;
    AiMcpSessionPool* m_mcp_pool{nullptr};  // not owned; see setMcpSessionPool
    std::function<bool()> m_should_cancel;  // see setCancelCheck
};

}  // namespace sak::ai
