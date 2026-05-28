// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_openai_model_client.h"

#include "sak/layout_constants.h"

#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

namespace sak::ai {

namespace {

constexpr int kModelInvokeTimeoutMs = 300'000;
constexpr int kCancellationPollIntervalMs = sak::kTimerPollingFastMs;

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

OpenAIResponsesModelClient::OpenAIResponsesModelClient(QObject* parent) : QObject(parent) {}

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
    ModelInvokeState state;
    QSemaphore done;
    QThread network_thread;
    QObject* worker = new QObject;
    worker->moveToThread(&network_thread);
    QObject::connect(&network_thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&network_thread, &QThread::started, worker, [&, worker, req]() {
        auto* client = new OpenAIResponsesClient(worker);
        auto* cancel_timer = new QTimer(worker);
        auto* timeout_timer = new QTimer(worker);
        cancel_timer->setInterval(kCancellationPollIntervalMs);
        timeout_timer->setSingleShot(true);
        const auto completed = std::make_shared<std::atomic_bool>(false);
        const auto finish = [&, client, completed]() {
            bool expected = false;
            if (!completed->compare_exchange_strong(expected, true)) {
                return;
            }
            client->cancel();
            done.release();
            network_thread.quit();
        };
        QObject::connect(client,
                         &OpenAIResponsesClient::responseReady,
                         worker,
                         [&, finish](const OpenAIResponseResult& result) {
                             state.result = result;
                             state.got_response = true;
                             finish();
                         });
        QObject::connect(client,
                         &OpenAIResponsesClient::requestFailed,
                         worker,
                         [&, finish](const QString& error_message) {
                             state.error = error_message;
                             state.got_failure = true;
                             finish();
                         });
        QObject::connect(cancel_timer, &QTimer::timeout, worker, [&, finish]() {
            if (tokenCancelled(token)) {
                state.error = token.cancelReason();
                finish();
            }
        });
        QObject::connect(timeout_timer, &QTimer::timeout, worker, [&, finish]() {
            state.timed_out = true;
            finish();
        });
        cancel_timer->start();
        timeout_timer->start(kModelInvokeTimeoutMs);
        client->createResponse(req);
    });
    network_thread.start();
    done.acquire();
    network_thread.quit();
    network_thread.wait();

    return modelResponseFromState(state, token);
}

}  // namespace sak::ai
