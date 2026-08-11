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
// Grace added to the semaphore wait beyond the invoke timeout. The worker's own timeout timer
// releases the semaphore within kModelInvokeTimeoutMs on every normal path; the grace only
// matters if the worker thread failed to START (QThread::start can fail under resource
// exhaustion), in which case an unconditional acquire would deadlock forever.
constexpr int kSemaphoreWaitGraceMs = 30'000;

struct ModelInvokeState {
    OpenAIResponseResult m_result;
    QString m_error;
    bool m_got_response{false};
    bool m_got_failure{false};
    bool m_timed_out{false};
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
    req.safety_identifier = request.safety_identifier;
    req.enable_web_search = enable_web_search;
    // Local tools are only offered when the subagent opted in (an executor is
    // wired and its policy permits execution); the runner sets this per task.
    req.enable_local_tools = request.enable_local_tools;
    return req;
}

IAiModelClient::Response modelResponseFromState(const ModelInvokeState& state,
                                                const CancellationToken& token) {
    IAiModelClient::Response response;
    if (tokenCancelled(token)) {
        response.error_message = token.cancelReason();
        return response;
    }
    if (state.m_got_response) {
        response.success = true;
        response.text = state.m_result.output_text;
        response.usage = state.m_result.usage;
        response.response_id = state.m_result.id;
        response.tool_calls.reserve(state.m_result.function_calls.size());
        for (const auto& call : state.m_result.function_calls) {
            response.tool_calls.append({.call_id = call.call_id,
                                        .name = call.name,
                                        .arguments_json = call.arguments_json});
        }
        return response;
    }
    if (state.m_got_failure) {
        response.error_message = state.m_error;
        return response;
    }
    if (state.m_timed_out) {
        response.error_message =
            QStringLiteral("Model invocation timed out after %1 ms").arg(kModelInvokeTimeoutMs);
        return response;
    }
    response.error_message = QStringLiteral("Model invocation returned no result");
    return response;
}

void joinWorkerAndRecordTimeout(QThread& network_thread,
                                QSemaphore& done,
                                ModelInvokeState& state) {
    // Timed acquire so a failed thread start (finish() never runs) cannot deadlock forever.
    const bool signalled = done.tryAcquire(1, kModelInvokeTimeoutMs + kSemaphoreWaitGraceMs);
    network_thread.quit();
    network_thread.wait();  // SAK-ALLOW-BLOCKING: stack QThread, quit() sent; join mandatory

    // Record the acquire-timeout ONLY after the join. Writing `state` from this thread while
    // a worker handler might still be writing the same fields would be a data race; once
    // wait() returns no worker code can run, so this write is ordered after every worker
    // write. Guard on the terminal flags so a result that landed at the boundary still wins.
    if (!signalled && !state.m_got_response && !state.m_got_failure && !state.m_timed_out) {
        state.m_timed_out = true;
    }
}

}  // namespace

OpenAIResponsesModelClient::OpenAIResponsesModelClient(QObject* parent) : QObject(parent) {}

OpenAIResponsesModelClient::~OpenAIResponsesModelClient() = default;

void OpenAIResponsesModelClient::setEnableWebSearch(bool enabled) {
    m_enable_web_search = enabled;
}

IAiModelClient::Response OpenAIResponsesModelClient::runResponseRequest(
    const OpenAIResponseRequest& request, const CancellationToken& token) const {
    if (tokenCancelled(token)) {
        Response response;
        response.error_message = token.cancelReason();
        return response;
    }

    ModelInvokeState state;
    QSemaphore done;
    QThread network_thread;
    QObject* worker = new QObject;
    worker->moveToThread(&network_thread);
    QObject::connect(&network_thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&network_thread, &QThread::started, worker, [&, worker, request]() {
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
                             state.m_result = result;
                             state.m_got_response = true;
                             finish();
                         });
        QObject::connect(client,
                         &OpenAIResponsesClient::requestFailed,
                         worker,
                         [&, finish](const QString& error_message) {
                             state.m_error = error_message;
                             state.m_got_failure = true;
                             finish();
                         });
        QObject::connect(cancel_timer, &QTimer::timeout, worker, [&, finish]() {
            if (tokenCancelled(token)) {
                state.m_error = token.cancelReason();
                finish();
            }
        });
        QObject::connect(timeout_timer, &QTimer::timeout, worker, [&, finish]() {
            state.m_timed_out = true;
            finish();
        });
        cancel_timer->start();
        timeout_timer->start(kModelInvokeTimeoutMs);
        client->createResponse(request);
    });
    network_thread.start();
    joinWorkerAndRecordTimeout(network_thread, done, state);

    return modelResponseFromState(state, token);
}

IAiModelClient::Response OpenAIResponsesModelClient::invoke(const Request& request,
                                                            const CancellationToken& token) {
    return runResponseRequest(openAiRequestFromModelRequest(request, m_enable_web_search), token);
}

IAiModelClient::Response OpenAIResponsesModelClient::continueWithToolOutputs(
    const Request& request,
    const QString& response_id,
    const QVector<AiSubagentToolOutput>& outputs,
    const CancellationToken& token) {
    // Resume the same server-side turn: reference the prior response and submit
    // the executed tool outputs, without re-sending the original input.
    OpenAIResponseRequest req = openAiRequestFromModelRequest(request, m_enable_web_search);
    req.input.clear();
    req.previous_response_id = response_id;
    req.function_outputs.reserve(outputs.size());
    for (const auto& output : outputs) {
        req.function_outputs.append({.call_id = output.call_id, .output = output.output_json});
    }
    return runResponseRequest(req, token);
}

}  // namespace sak::ai
