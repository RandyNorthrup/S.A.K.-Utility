// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_chat_backend.h"
#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/openai_responses_client.h"

#include <QObject>
#include <QString>

#include <functional>

namespace sak::ai {

/// @brief IAiModelClient adapter over a signal-based chat backend.
///
/// Defaults to the OpenAI Responses API; setBackendFactory() selects any other
/// provider backend. Wraps the signal-based backend in a synchronous invoke()
/// backed by a short-lived worker thread. The caller waits for completion while Qt
/// network work stays on the worker event loop instead of a nested caller loop.
/// Suitable for subagent runners scheduled on worker threads from the
/// orchestrator.
class OpenAIResponsesModelClient
    : public QObject
    , public IAiModelClient {
    Q_OBJECT

public:
    /// Creates the backend on the invoke() worker thread, parented to @p parent.
    using BackendFactory = std::function<AiChatBackend*(QObject* parent)>;

    explicit OpenAIResponsesModelClient(QObject* parent = nullptr);
    ~OpenAIResponsesModelClient() override;

    void setEnableWebSearch(bool enabled);
    [[nodiscard]] bool enableWebSearch() const { return m_enable_web_search; }
    void setBackendFactory(BackendFactory factory);

    [[nodiscard]] Response invoke(const Request& request, const CancellationToken& token) override;

private:
    bool m_enable_web_search{false};
    BackendFactory m_backend_factory;
};

}  // namespace sak::ai
