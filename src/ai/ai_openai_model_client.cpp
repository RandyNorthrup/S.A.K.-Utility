// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_openai_model_client.h"

#include <QEventLoop>
#include <QTimer>

namespace sak::ai {

namespace {

constexpr int kModelInvokeTimeoutMs = 300'000;

struct ModelInvokeState {
    OpenAIResponseResult result;
    QString error;
    bool got_response{false};
    bool got_failure{false};
    bool timed_out{false};
};

bool tokenCancelled(const CancellationToken& token) {
    return token.isValid() && token.isCancellationRequested();
}

QString combinedInput(const IAiModelClient::Request& request) {
    QString input = request.objective.trimmed();
    if (!request.context.isEmpty()) {
        if (!input.isEmpty()) {
            input += QStringLiteral("\n\n");
        }
        input += request.context;
    }
    return input;
}

OpenAIResponseRequest openAiRequestFromModelRequest(const IAiModelClient::Request& request,
                                                    bool enable_web_search) {
    OpenAIResponseRequest req;
    req.api_key = request.api_key;
    req.model = request.model;
    req.instructions = request.system_instructions;
    req.input = combinedInput(request);
    req.reasoning_effort = request.reasoning_effort;
    req.enable_web_search = enable_web_search;
    req.enable_local_tools = false;
    return req;
}

IAiModelClient::Response modelResponseFromState(const ModelInvokeState& state,
                                                const CancellationToken& token) {
    IAiModelClient::Response response;
    if (tokenCancelled(token)) {
        response.error_message = token.cancelReason();
        return response;
    }
    if (state.got_response) {
        response.success = true;
        response.text = state.result.output_text;
        response.usage = state.result.usage;
        return response;
    }
    if (state.got_failure) {
        response.error_message = state.error;
        return response;
    }
    if (state.timed_out) {
        response.error_message =
            QStringLiteral("Model invocation timed out after %1 ms").arg(kModelInvokeTimeoutMs);
        return response;
    }
    response.error_message = QStringLiteral("Model invocation returned no result");
    return response;
}

}  // namespace

OpenAIResponsesModelClient::OpenAIResponsesModelClient(QObject* parent)
    : QObject(parent), m_client(this) {}

OpenAIResponsesModelClient::~OpenAIResponsesModelClient() = default;

void OpenAIResponsesModelClient::setEnableWebSearch(bool enabled) {
    m_enable_web_search = enabled;
}

IAiModelClient::Response OpenAIResponsesModelClient::invoke(const Request& request,
                                                            const CancellationToken& token) {
    if (tokenCancelled(token)) {
        Response response;
        response.error_message = token.cancelReason();
        return response;
    }

    const OpenAIResponseRequest req = openAiRequestFromModelRequest(request, m_enable_web_search);
    QEventLoop loop;
    ModelInvokeState state;

    const auto on_ready = QObject::connect(&m_client,
                                           &OpenAIResponsesClient::responseReady,
                                           &loop,
                                           [&](const OpenAIResponseResult& result) {
                                               state.result = result;
                                               state.got_response = true;
                                               loop.quit();
                                           });
    const auto on_failed = QObject::connect(
        &m_client, &OpenAIResponsesClient::requestFailed, &loop, [&](const QString& error_message) {
            state.error = error_message;
            state.got_failure = true;
            loop.quit();
        });

    QTimer cancel_timer;
    cancel_timer.setInterval(100);
    QObject::connect(&cancel_timer, &QTimer::timeout, &loop, [&]() {
        if (tokenCancelled(token)) {
            m_client.cancel();
            loop.quit();
        }
    });
    cancel_timer.start();

    QTimer timeout_timer;
    timeout_timer.setSingleShot(true);
    QObject::connect(&timeout_timer, &QTimer::timeout, &loop, [&]() {
        state.timed_out = true;
        m_client.cancel();
        loop.quit();
    });
    timeout_timer.start(kModelInvokeTimeoutMs);

    m_client.createResponse(req);
    if (!state.got_response && !state.got_failure && !state.timed_out && !tokenCancelled(token)) {
        loop.exec();
    }
    cancel_timer.stop();
    timeout_timer.stop();
    QObject::disconnect(on_ready);
    QObject::disconnect(on_failed);

    return modelResponseFromState(state, token);
}

}  // namespace sak::ai
