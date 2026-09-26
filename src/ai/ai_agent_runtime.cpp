// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_agent_runtime.h"

#include "sak/app_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <utility>

namespace sak::ai {

namespace {

QString firstExisting(const QStringList& candidates) {
    for (const auto& candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile()) {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
}

/// Muse Code installs `muse-bin-<version>.exe` next to a `.muse-version`
/// pointer file; launching the binary directly avoids the `.cmd` shim.
QString museBinaryIn(const QString& directory) {
    const QDir dir(directory);
    QFile pointer(dir.filePath(QStringLiteral(".muse-version")));
    if (pointer.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString version = QString::fromUtf8(pointer.readAll()).trimmed();
        const QString pinned = dir.filePath(
            AiAgentRuntime::executableName(QStringLiteral("muse-bin-%1").arg(version)));
        if (!version.isEmpty() && QFileInfo(pinned).isFile()) {
            return pinned;
        }
    }
    const QStringList binaries =
        dir.entryList({AiAgentRuntime::executableName(QStringLiteral("muse-bin-*"))},
                      QDir::Files,
                      QDir::Name | QDir::Reversed);
    return binaries.isEmpty() ? QString() : dir.filePath(binaries.first());
}

}  // namespace

AiAgentRuntime::AiAgentRuntime(QString app_dir,
                               QString data_root,
                               QProcessEnvironment base_environment)
    : m_app_dir(std::move(app_dir))
    , m_data_root(std::move(data_root))
    , m_base_environment(std::move(base_environment)) {}

AiAgentRuntime AiAgentRuntime::forApplication() {
    return AiAgentRuntime(QCoreApplication::applicationDirPath(),
                          sak::app_paths::dataRoot(),
                          QProcessEnvironment::systemEnvironment());
}

QString AiAgentRuntime::executableName(const QString& base_name) {
#ifdef Q_OS_WIN
    return base_name + QStringLiteral(".exe");
#else
    return base_name;
#endif
}

QStringList AiAgentRuntime::scrubbedVariables(ModelProviderId provider) {
    switch (provider) {
    case ModelProviderId::OpenAI:
        return {QStringLiteral("OPENAI_API_KEY"),
                QStringLiteral("CODEX_API_KEY"),
                QStringLiteral("OPENAI_BASE_URL")};
    case ModelProviderId::Anthropic:
        return {QStringLiteral("ANTHROPIC_API_KEY"),
                QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
                QStringLiteral("ANTHROPIC_BASE_URL"),
                QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN"),
                QStringLiteral("CLAUDECODE")};
    case ModelProviderId::Google:
        return {QStringLiteral("GEMINI_API_KEY"),
                QStringLiteral("GOOGLE_API_KEY"),
                QStringLiteral("GOOGLE_GENAI_USE_VERTEXAI"),
                QStringLiteral("NO_BROWSER")};
    case ModelProviderId::Muse:
        return {QStringLiteral("META_API_KEY"), QStringLiteral("MODEL_API_KEY")};
    }
    return {};
}

QString AiAgentRuntime::agentRoot(ModelProviderId provider) const {
    return QDir(m_app_dir).filePath(
        QStringLiteral("tools/ai_agents/%1").arg(modelProviderInfo(provider).agent_directory));
}

QString AiAgentRuntime::homeDirectory(ModelProviderId provider) const {
    if (provider == ModelProviderId::Muse) {
        return {};  // The technician's own Muse Code install keeps its profile.
    }
    return QDir(m_data_root)
        .filePath(QStringLiteral("ai_agents/%1").arg(modelProviderInfo(provider).agent_directory));
}

QString AiAgentRuntime::defaultWorkspace() const {
    return QDir(m_data_root).filePath(QStringLiteral("ai_agents/workspace"));
}

QString AiAgentRuntime::toolBridgeExecutable() const {
    return QDir(m_app_dir).filePath(executableName(QStringLiteral("sak_ai_tool_bridge")));
}

AiAgentRuntime::Program AiAgentRuntime::locateCodex() const {
    const QDir root(agentRoot(ModelProviderId::OpenAI));
    const QString server = firstExisting(
        {root.filePath(QStringLiteral("bin/") + executableName(QStringLiteral("codex-app-server"))),
         root.filePath(executableName(QStringLiteral("codex-app-server")))});
    if (!server.isEmpty()) {
        return {server, {}, true};
    }
    const QString cli = firstExisting(
        {root.filePath(QStringLiteral("bin/") + executableName(QStringLiteral("codex"))),
         root.filePath(executableName(QStringLiteral("codex")))});
    return {cli, {QStringLiteral("app-server")}, true};
}

AiAgentRuntime::Program AiAgentRuntime::locateClaude() const {
    const QString bundled =
        firstExisting({QDir(agentRoot(ModelProviderId::Anthropic))
                           .filePath(executableName(QStringLiteral("claude")))});
    if (!bundled.isEmpty()) {
        return {bundled, {}, true};
    }
    // The official native installer location, when the technician already has it.
    const QString home = m_base_environment.value(QStringLiteral("USERPROFILE"),
                                                  m_base_environment.value(QStringLiteral("HOME")));
    const QString installed = firstExisting({QDir(home).filePath(
        QStringLiteral(".local/bin/") + executableName(QStringLiteral("claude")))});
    return {installed, {}, false};
}

AiAgentRuntime::Program AiAgentRuntime::locateGemini() const {
    const QDir root(agentRoot(ModelProviderId::Google));
    const QString node = firstExisting(
        {root.filePath(QStringLiteral("node/") + executableName(QStringLiteral("node"))),
         root.filePath(executableName(QStringLiteral("node")))});
    const QString script = firstExisting({root.filePath(QStringLiteral("bundle/gemini.js"))});
    if (node.isEmpty() || script.isEmpty()) {
        return {};
    }
    return {node, {script}, true};
}

AiAgentRuntime::Program AiAgentRuntime::locateMuse() const {
    // Muse Code's binary has no published redistribution grant, so S.A.K. only
    // uses a copy the technician installed (or placed here themselves).
    const QString placed = firstExisting(
        {QDir(agentRoot(ModelProviderId::Muse)).filePath(executableName(QStringLiteral("muse")))});
    if (!placed.isEmpty()) {
        return {placed, {}, false};
    }
    const QString local_app_data = m_base_environment.value(QStringLiteral("LOCALAPPDATA"));
    if (local_app_data.isEmpty()) {
        return {};
    }
    return {museBinaryIn(QDir(local_app_data).filePath(QStringLiteral("Programs/muse"))),
            {},
            false};
}

AiAgentRuntime::Program AiAgentRuntime::locate(ModelProviderId provider) const {
    switch (provider) {
    case ModelProviderId::OpenAI:
        return locateCodex();
    case ModelProviderId::Anthropic:
        return locateClaude();
    case ModelProviderId::Google:
        return locateGemini();
    case ModelProviderId::Muse:
        return locateMuse();
    }
    return {};
}

QProcessEnvironment AiAgentRuntime::environment(ModelProviderId provider,
                                                const QString& home,
                                                bool bundled) const {
    QProcessEnvironment env = m_base_environment;
    for (const auto& name : scrubbedVariables(provider)) {
        env.remove(name);
    }
    switch (provider) {
    case ModelProviderId::OpenAI:
        env.insert(QStringLiteral("CODEX_HOME"), QDir::toNativeSeparators(home));
        break;
    case ModelProviderId::Anthropic:
        env.insert(QStringLiteral("CLAUDE_CONFIG_DIR"), QDir::toNativeSeparators(home));
        if (bundled) {
            // S.A.K. ships a pinned copy; updates arrive with S.A.K. releases.
            env.insert(QStringLiteral("DISABLE_UPDATES"), QStringLiteral("1"));
        }
        break;
    case ModelProviderId::Google:
        env.insert(QStringLiteral("GEMINI_CLI_HOME"), QDir::toNativeSeparators(home));
        env.insert(QStringLiteral("GEMINI_CLI_NO_RELAUNCH"), QStringLiteral("true"));
        break;
    case ModelProviderId::Muse:
        break;
    }
    return env;
}

AiAgentLaunch AiAgentRuntime::launch(ModelProviderId provider, QStringList arguments) const {
    const Program program = locate(provider);
    AiAgentLaunch result;
    result.provider = provider;
    result.bundled = program.bundled;
    result.available = !program.path.isEmpty();
    if (!result.available) {
        result.missing_reason =
            QStringLiteral("%1 runtime not found").arg(modelProviderInfo(provider).agent_name);
        return result;
    }
    result.home_directory = homeDirectory(provider);
    if (!result.home_directory.isEmpty()) {
        QDir().mkpath(result.home_directory);  // CODEX_HOME must already exist.
    }
    result.spec.program = program.path;
    result.spec.arguments = program.leading_arguments + arguments;
    result.spec.environment = environment(provider, result.home_directory, program.bundled);
    return result;
}

AiAgentLaunch AiAgentRuntime::serverLaunch(ModelProviderId provider) const {
    switch (provider) {
    case ModelProviderId::OpenAI:
    case ModelProviderId::Anthropic:
        return launch(provider, {});
    case ModelProviderId::Google:
        return launch(provider, {QStringLiteral("--acp")});
    case ModelProviderId::Muse:
        return launch(provider, {QStringLiteral("serve")});
    }
    return {};
}

AiAgentLaunch AiAgentRuntime::commandLaunch(ModelProviderId provider,
                                            const QStringList& arguments) const {
    return launch(provider, arguments);
}

}  // namespace sak::ai
