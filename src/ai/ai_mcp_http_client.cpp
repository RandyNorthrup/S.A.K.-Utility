// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_http_client.h"

#include "sak/layout_constants.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <utility>

namespace sak::ai {

namespace {

constexpr int kMaxResponseBytes = 1024 * 1024;
// Grace added to the semaphore wait beyond the request timeout so a worker thread that failed to
// START (finish() never runs) can never deadlock the caller on an unconditional acquire.
constexpr int kSemaphoreWaitGraceMs = 30'000;
constexpr qsizetype kSseDataPrefixLength = 5;
constexpr int kMinimumRequestTimeoutMs = sak::kMillisecondsPerSecond;

struct HttpCallState {
    QVariant status;
    QByteArray response_body;
    QNetworkReply::NetworkError network_error{QNetworkReply::NoError};
    QString network_error_text;
    bool timed_out{false};
    bool too_large{false};
};

struct HttpWorkerSinks {
    HttpCallState* state{nullptr};
    QSemaphore* done{nullptr};
    QThread* owner_thread{nullptr};
};

[[nodiscard]] QJsonObject toolCallPayload(const QString& tool_name, const QJsonObject& arguments) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("name"), tool_name},
                     {QStringLiteral("arguments"), arguments}}},
    };
}

[[nodiscard]] QJsonObject parseJsonObject(const QByteArray& bytes, QString* error_message) {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes.trimmed(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message =
                QStringLiteral("Invalid MCP JSON response: %1").arg(parse_error.errorString());
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return doc.object();
}

// A JSON-RPC response carries an id and either a result or an error; a notification
// (e.g. notifications/progress) has a method and no id. Only the former is our answer.
[[nodiscard]] bool isJsonRpcResponse(const QJsonObject& object) {
    return object.contains(QStringLiteral("id")) &&
           (object.contains(QStringLiteral("result")) || object.contains(QStringLiteral("error")));
}

[[nodiscard]] QJsonObject scanSseForJsonRpcResponse(const QByteArray& response_body,
                                                    QString* error_message) {
    QByteArray combined_data;
    const QList<QByteArray> lines = response_body.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (!line.startsWith("data:")) {
            continue;
        }
        QByteArray data = line.mid(kSseDataPrefixLength).trimmed();
        if (data == "[DONE]") {
            continue;
        }

        QString ignored_error;
        const QJsonObject object = parseJsonObject(data, &ignored_error);
        if (object.isEmpty()) {
            // A JSON object may span multiple data: lines; accumulate partial fragments.
            if (!combined_data.isEmpty()) {
                combined_data.append('\n');
            }
            combined_data.append(data);
            continue;
        }
        // Skip streamed notifications (progress/logging) that precede the response, so we
        // return the id-bearing result rather than the first data event.
        if (isJsonRpcResponse(object)) {
            if (error_message) {
                error_message->clear();
            }
            return object;
        }
    }

    if (!combined_data.isEmpty()) {
        return parseJsonObject(combined_data, error_message);
    }
    if (error_message) {
        *error_message = QStringLiteral("MCP response did not contain JSON-RPC data");
    }
    return {};
}

[[nodiscard]] QJsonObject extractJsonRpcMessage(const QByteArray& response_body,
                                                QString* error_message) {
    const QByteArray trimmed = response_body.trimmed();
    if (trimmed.startsWith('{')) {
        const QJsonObject object = parseJsonObject(trimmed, error_message);
        if (object.isEmpty()) {
            return {};
        }
        // A bare JSON body must still be a real JSON-RPC response (id + result/error); do not
        // accept an arbitrary object as a successful tool result. Fail closed on the SSE path's
        // same requirement rather than trusting any `{`-leading payload.
        if (!isJsonRpcResponse(object)) {
            if (error_message) {
                *error_message = QStringLiteral("MCP response is not a JSON-RPC response");
            }
            return {};
        }
        return object;
    }
    return scanSseForJsonRpcResponse(response_body, error_message);
}

class HttpToolCallWorker final : public QObject {
public:
    HttpToolCallWorker(QUrl endpoint, QByteArray body, int timeout_ms, const HttpWorkerSinks& sinks)
        : m_endpoint(std::move(endpoint))
        , m_body(std::move(body))
        , m_timeoutMs(timeout_ms)
        , m_sinks(sinks) {}

    void start() {
        auto* manager = new QNetworkAccessManager(this);
        QNetworkRequest request(m_endpoint);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);

        m_reply = manager->post(request, m_body);
        // Bound the socket read buffer so an over-large body cannot balloon memory before the cap
        // is applied at read time, and abort incrementally the instant the download (or a declared
        // Content-Length) crosses the cap, instead of buffering the whole reply first.
        m_reply->setReadBufferSize(static_cast<qint64>(kMaxResponseBytes) + 1);
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        QObject::connect(m_reply, &QNetworkReply::finished, this, [this]() { finish(false); });
        QObject::connect(
            m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
                if (received > kMaxResponseBytes || total > kMaxResponseBytes) {
                    m_tooLarge = true;
                    m_reply->abort();
                    finish(false);
                }
            });
        QObject::connect(m_timer, &QTimer::timeout, this, [this]() {
            m_reply->abort();
            finish(true);
        });
        m_timer->start(qMax(m_timeoutMs, kMinimumRequestTimeoutMs));
    }

private:
    void finish(bool timed_out) {
        bool expected = false;
        if (!m_completed.compare_exchange_strong(expected, true)) {
            return;
        }
        if (m_timer->isActive()) {
            m_timer->stop();
        }
        m_sinks.state->timed_out = timed_out;
        m_sinks.state->too_large = m_tooLarge.load();
        if (!timed_out) {
            m_sinks.state->status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            m_sinks.state->response_body = m_reply->read(
                qMin(m_reply->bytesAvailable(), static_cast<qint64>(kMaxResponseBytes)));
            m_sinks.state->network_error = m_reply->error();
            m_sinks.state->network_error_text = m_reply->errorString();
        }
        m_reply->deleteLater();
        m_sinks.done->release();
        m_sinks.owner_thread->quit();
    }

    QUrl m_endpoint;
    QByteArray m_body;
    int m_timeoutMs{0};
    HttpWorkerSinks m_sinks;
    QNetworkReply* m_reply{nullptr};
    QTimer* m_timer{nullptr};
    std::atomic_bool m_completed{false};
    std::atomic_bool m_tooLarge{false};
};

// A loopback host may use plain http (a local MCP server is not a MITM/eavesdrop surface); every
// other host MUST use https so a manifest typo or disk override cannot downgrade to cleartext.
[[nodiscard]] bool isLoopbackHost(const QString& host) {
    return host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ||
           host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1");
}

[[nodiscard]] bool endpointSchemeIsSecure(const QUrl& endpoint) {
    if (endpoint.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return endpoint.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 &&
           isLoopbackHost(endpoint.host());
}

bool validateHttpToolCall(const QUrl& endpoint, const QString& tool_name, QString* error_message) {
    if (!endpoint.isValid() || endpoint.scheme().isEmpty() || endpoint.host().isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Invalid MCP endpoint");
        }
        return false;
    }
    if (!endpointSchemeIsSecure(endpoint)) {
        if (error_message) {
            *error_message = QStringLiteral("MCP endpoint must use https (or http on loopback): %1")
                                 .arg(endpoint.toString());
        }
        return false;
    }
    if (tool_name.trimmed().isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("MCP tool name is empty");
        }
        return false;
    }
    return true;
}

HttpCallState performHttpToolCall(const QUrl& endpoint, const QByteArray& body, int timeout_ms) {
    HttpCallState state;
    QSemaphore done;
    QThread network_thread;
    auto* worker =
        new HttpToolCallWorker(endpoint,
                               body,
                               timeout_ms,
                               {.state = &state, .done = &done, .owner_thread = &network_thread});
    worker->moveToThread(&network_thread);
    QObject::connect(&network_thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&network_thread, &QThread::started, worker, [worker]() { worker->start(); });
    network_thread.start();
    // Timed acquire so a worker thread that failed to start cannot hang the caller forever; the
    // worker releases within timeout_ms on every normal path, so a grace past it only fires on a
    // genuine start failure.
    if (!done.tryAcquire(1, timeout_ms + kSemaphoreWaitGraceMs)) {
        state.timed_out = true;
    }
    network_thread.quit();
    // SAK-ALLOW-BLOCKING: `network_thread` is a stack local destroyed on return, and
    // ~QThread on a live thread aborts the process. quit() precedes it, and QThread::exit
    // is remembered even if exec() has not started yet, so this cannot hang on a thread
    // that raced past its own event loop.
    network_thread.wait();
    return state;
}

bool explainHttpFailure(const HttpCallState& state, QString* error_message) {
    if (state.too_large) {
        if (error_message) {
            *error_message =
                QStringLiteral("MCP HTTP response exceeded the %1-byte cap").arg(kMaxResponseBytes);
        }
        return true;
    }
    if (state.timed_out) {
        if (error_message) {
            *error_message = QStringLiteral("MCP HTTP request timed out");
        }
        return true;
    }
    if (state.network_error == QNetworkReply::NoError) {
        return false;
    }
    if (error_message) {
        *error_message = QStringLiteral("MCP HTTP request failed%1: %2")
                             .arg(state.status.isValid()
                                      ? QStringLiteral(" (HTTP %1)").arg(state.status.toInt())
                                      : QString(),
                                  state.network_error_text);
    }
    return true;
}

bool explainJsonRpcError(const QJsonObject& message, QString* error_message) {
    if (!message.contains(QStringLiteral("error"))) {
        return false;
    }
    const QJsonObject error = message.value(QStringLiteral("error")).toObject();
    const QString error_text = error.value(QStringLiteral("message")).toString();
    if (error_message) {
        *error_message = error_text.isEmpty() ? QStringLiteral("MCP JSON-RPC error") : error_text;
    }
    return true;
}

}  // namespace

QJsonObject AiMcpHttpClient::callTool(const QUrl& endpoint,
                                      const QString& tool_name,
                                      const QJsonObject& arguments,
                                      int timeout_ms,
                                      QString* error_message) {
    if (!validateHttpToolCall(endpoint, tool_name, error_message)) {
        return {};
    }

    const QByteArray body =
        QJsonDocument(toolCallPayload(tool_name, arguments)).toJson(QJsonDocument::Compact);
    const HttpCallState state = performHttpToolCall(endpoint, body, timeout_ms);
    if (explainHttpFailure(state, error_message)) {
        return {};
    }

    QJsonObject message = extractJsonRpcMessage(state.response_body, error_message);
    if (message.isEmpty()) {
        return {};
    }
    if (explainJsonRpcError(message, error_message)) {
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return message;
}

QJsonObject AiMcpHttpClient::extractJsonRpcMessageForTesting(const QByteArray& response_body,
                                                             QString* error_message) {
    return extractJsonRpcMessage(response_body, error_message);
}

QJsonObject AiMcpHttpClient::toolCallPayloadForTesting(const QString& tool_name,
                                                       const QJsonObject& arguments) {
    return toolCallPayload(tool_name, arguments);
}

}  // namespace sak::ai
