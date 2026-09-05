// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/openai_responses_client.h"

#include <QObject>
#include <QString>

#include <functional>

class QThread;

namespace sak::ai {

/// @brief Convert the generic model request while preserving untrusted text as user-role messages.
[[nodiscard]] OpenAIResponseRequest openAiRequestFromModelRequest(
    const IAiModelClient::Request& request, bool enable_web_search);

/// @brief IAiModelClient adapter over OpenAIResponsesClient.
///
/// Wraps the signal-based responses client in a synchronous invoke() backed by
/// a short-lived worker thread. The caller waits for completion while Qt
/// network work stays on the worker event loop instead of a nested caller loop.
/// Suitable for subagent runners scheduled on worker threads from the
/// orchestrator.
class OpenAIResponsesModelClient
    : public QObject
    , public IAiModelClient {
    Q_OBJECT

public:
    explicit OpenAIResponsesModelClient(QObject* parent = nullptr);
    ~OpenAIResponsesModelClient() override;

    void setEnableWebSearch(bool enabled);
    /// @brief Replaces QThread::start for deterministic launch-failure tests.
    /// @note Set before invoke(); an empty function restores normal thread startup.
    void setThreadStarterForTesting(std::function<void(QThread&)> starter);

    [[nodiscard]] Response invoke(const Request& request, const CancellationToken& token) override;
    [[nodiscard]] Response continueWithToolOutputs(const Request& request,
                                                   const QString& response_id,
                                                   const QVector<AiSubagentToolOutput>& outputs,
                                                   const CancellationToken& token) override;

private:
    [[nodiscard]] Response runResponseRequest(const OpenAIResponseRequest& request,
                                              const CancellationToken& token) const;

    bool m_enable_web_search{false};
    std::function<void(QThread&)> m_thread_starter;
};

}  // namespace sak::ai
