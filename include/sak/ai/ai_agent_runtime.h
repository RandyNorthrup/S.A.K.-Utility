// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_model_provider.h"
#include "sak/ai/ai_stdio_json_process.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace sak::ai {

/// @brief Resolved launch information for a vendor agent runtime.
struct AiAgentLaunch {
    ModelProviderId provider{ModelProviderId::OpenAI};
    AiStdioJsonProcess::LaunchSpec spec;
    QString home_directory;
    bool available{false};
    bool bundled{false};
    QString missing_reason;
};

/// @brief Locates the vendor agent runtimes and builds their environment.
///
/// Bundled runtimes live under `<app>/tools/ai_agents/<provider>/`. Each
/// runtime gets a private state directory under the S.A.K. data root so the
/// technician's sign-in stays with S.A.K. instead of the customer's profile.
/// API-key environment variables are removed so the subscription sign-in is
/// the credential actually used.
class AiAgentRuntime {
public:
    AiAgentRuntime(QString app_dir, QString data_root, QProcessEnvironment base_environment);
    /// @brief Uses the running application's directory, data root and env.
    [[nodiscard]] static AiAgentRuntime forApplication();

    /// @brief Long-running protocol server (app-server / stream-json / ACP / MSP).
    [[nodiscard]] AiAgentLaunch serverLaunch(ModelProviderId provider) const;
    /// @brief One-shot command such as `claude auth status`.
    [[nodiscard]] AiAgentLaunch commandLaunch(ModelProviderId provider,
                                              const QStringList& arguments) const;
    /// @brief Workspace used when the panel has not supplied a session folder.
    [[nodiscard]] QString defaultWorkspace() const;
    /// @brief Isolated state directory for a runtime (empty for Muse).
    [[nodiscard]] QString homeDirectory(ModelProviderId provider) const;
    /// @brief Path of the S.A.K. MCP tool bridge executable.
    [[nodiscard]] QString toolBridgeExecutable() const;

    [[nodiscard]] static QStringList scrubbedVariables(ModelProviderId provider);
    [[nodiscard]] static QString executableName(const QString& base_name);

private:
    struct Program {
        QString path;
        QStringList leading_arguments;
        bool bundled{false};
    };

    [[nodiscard]] Program locate(ModelProviderId provider) const;
    [[nodiscard]] Program locateCodex() const;
    [[nodiscard]] Program locateClaude() const;
    [[nodiscard]] Program locateGemini() const;
    [[nodiscard]] Program locateMuse() const;
    [[nodiscard]] QString agentRoot(ModelProviderId provider) const;
    [[nodiscard]] QProcessEnvironment environment(ModelProviderId provider,
                                                  const QString& home,
                                                  bool bundled) const;
    [[nodiscard]] AiAgentLaunch launch(ModelProviderId provider, QStringList arguments) const;

    QString m_app_dir;
    QString m_data_root;
    QProcessEnvironment m_base_environment;
};

}  // namespace sak::ai
