// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/openai_responses_client.h"

#include <QObject>
#include <QString>

namespace sak::ai {

/// @brief IAiModelClient adapter over OpenAIResponsesClient.
///
/// Wraps the signal-based responses client in a synchronous invoke() that
/// blocks the calling thread on a local QEventLoop until either the
/// responseReady/requestFailed signal fires or the supplied cancellation
/// token is tripped. Suitable for subagent runners scheduled on worker
/// threads from the orchestrator.
class OpenAIResponsesModelClient
    : public QObject
    , public IAiModelClient {
    Q_OBJECT

public:
    explicit OpenAIResponsesModelClient(QObject* parent = nullptr);
    ~OpenAIResponsesModelClient() override;

    void setEnableWebSearch(bool enabled);
    [[nodiscard]] bool enableWebSearch() const { return m_enable_web_search; }

    [[nodiscard]] Response invoke(const Request& request, const CancellationToken& token) override;

private:
    OpenAIResponsesClient m_client;
    bool m_enable_web_search{false};
};

}  // namespace sak::ai
