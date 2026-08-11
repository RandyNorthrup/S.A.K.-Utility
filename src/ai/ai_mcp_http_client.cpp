// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_http_client.h"

#include "sak/layout_constants.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
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
// AI-controlled tool name + argument tree is serialized into one contiguous body; cap it so a
// hostile/oversized argument object cannot drive an unbounded upload / memory blow-up.
constexpr int kMaxRequestBytes = 1024 * 1024;
// Grace added to the semaphore wait beyond the request timeout so a worker thread that failed to
// START (finish() never runs) can never deadlock the caller on an unconditional acquire.
constexpr int kSemaphoreWaitGraceMs = 30'000;
constexpr qsizetype kSseDataPrefixLength = 5;
constexpr int kMinimumRequestTimeoutMs = sak::kMillisecondsPerSecond;
// Upper bound on the caller-supplied timeout: rejects negative/zero waits and keeps
// timeout_ms + kSemaphoreWaitGraceMs from signed-overflowing int.
constexpr int kMaxRequestTimeoutMs = 3'600'000;
// Inclusive-lower / exclusive-upper bounds of the HTTP 2xx success class.
constexpr int kHttpStatusSuccessMin = 200;
constexpr int kHttpStatusSuccessEnd = 300;

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

// A JSON-RPC response carries jsonrpc "2.0", an id, and either a result or an error; a
// notification (e.g. notifications/progress) has a method and no id. Only the former is our
// answer. Requiring the version string rejects arbitrary id+result objects that are not
// JSON-RPC at all.
[[nodiscard]] bool isJsonRpcResponse(const QJsonObject& object) {
    return object.value(QStringLiteral("jsonrpc")).toString() == QStringLiteral("2.0") &&
           object.contains(QStringLiteral("id")) &&
           (object.contains(QStringLiteral("result")) || object.contains(QStringLiteral("error")));
}

// Parses one complete SSE event's accumulated data and decides what it is. Returns the object
// (and clears error_message, since a found answer has no error) when it is a JSON-RPC response;
// a valid non-response object is a streamed notification (progress/logging) and is skipped; an
// unparsable event is remembered in last_unparsed for the final error but never glued onto the
// next event, so fragments from separate events can no longer be concatenated into a fake object.
// Consumes event_data on every non-empty call.
[[nodiscard]] QJsonObject flushSseEvent(QByteArray& event_data,
                                        QByteArray& last_unparsed,
                                        QString* error_message) {
    if (event_data.isEmpty()) {
        return {};
    }
    QString ignored_error;
    const QJsonObject object = parseJsonObject(event_data, &ignored_error);
    if (object.isEmpty()) {
        last_unparsed = event_data;
    } else if (isJsonRpcResponse(object)) {
        event_data.clear();
        if (error_message) {
            error_message->clear();
        }
        return object;
    }
    event_data.clear();
    return {};
}

// After every SSE event is consumed without yielding a JSON-RPC response, resolve the failure:
// surface the real parse error from the last malformed event, else report that no JSON-RPC data
// arrived. Always fails closed (returns {}).
[[nodiscard]] QJsonObject settleSseFailure(const QByteArray& last_unparsed,
                                           QString* error_message) {
    if (!last_unparsed.isEmpty()) {
        return parseJsonObject(last_unparsed, error_message);
    }
    if (error_message) {
        *error_message = QStringLiteral("MCP response did not contain JSON-RPC data");
    }
    return {};
}

[[nodiscard]] QJsonObject scanSseForJsonRpcResponse(const QByteArray& response_body,
                                                    QString* error_message) {
    const QList<QByteArray> lines = response_body.split('\n');
    QByteArray event_data;     // the data: payload accumulated for the CURRENT SSE event
    QByteArray last_unparsed;  // most recent event whose joined data was not valid JSON

    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            // A blank line terminates the current SSE event.
            const QJsonObject response = flushSseEvent(event_data, last_unparsed, error_message);
            if (!response.isEmpty()) {
                return response;
            }
            continue;
        }
        if (!line.startsWith("data:")) {
            continue;
        }
        const QByteArray data = line.mid(kSseDataPrefixLength).trimmed();
        if (data == "[DONE]") {
            continue;
        }
        // Multiple data: lines within ONE event are joined with a newline (SSE framing).
        if (!event_data.isEmpty()) {
            event_data.append('\n');
        }
        event_data.append(data);
    }

    // Flush a trailing event that had no terminating blank line.
    const QJsonObject response = flushSseEvent(event_data, last_unparsed, error_message);
    if (!response.isEmpty()) {
        return response;
    }
    return settleSseFailure(last_unparsed, error_message);
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
        if (m_endpoint.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0) {
            // http is only permitted on loopback; a proxy would route that cleartext body off
            // the machine, so pin NoProxy to keep the loopback invariant real.
            manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        }
        QNetworkRequest request(m_endpoint);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Accept", "application/json, text/event-stream");
        // Only follow SAME-ORIGIN redirects: a cross-origin 307/308 would re-POST the tool name
        // and arguments to an unvalidated host (or downgrade loopback->remote) without
        // revalidation.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::SameOriginRedirectPolicy);

        m_reply = manager->post(request, m_body);
        // Bound the socket read buffer so an over-large body cannot balloon memory before the cap
        // is applied at read time, and abort incrementally the instant the download (or a declared
        // Content-Length) crosses the cap, instead of buffering the whole reply first.
        m_reply->setReadBufferSize(static_cast<qint64>(kMaxResponseBytes) + 1);
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        QObject::connect(m_reply, &QNetworkReply::finished, this, [this]() { finish(false); });
        QObject::connect(m_reply, &QNetworkReply::readyRead, this, [this]() { onReadyRead(); });
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
    // Drains decoded bytes as they arrive (bounded by the cap) and finishes early the moment a
    // complete JSON-RPC response is in hand. Without this an SSE server that delivers the answer
    // then keeps the stream open would force the caller to wait out the whole timeout and report
    // failure, risking a retry of an operation the server already performed. All reply signal
    // handlers run on this one worker thread, so the non-atomic m_haveResponse is safe here.
    void onReadyRead() {
        if (m_completed.load()) {
            return;
        }
        const qint64 room = static_cast<qint64>(kMaxResponseBytes) + 1 - m_accumulated.size();
        if (room > 0 && m_reply->bytesAvailable() > 0) {
            m_accumulated.append(m_reply->read(qMin(m_reply->bytesAvailable(), room)));
        }
        if (m_accumulated.size() > kMaxResponseBytes) {
            m_tooLarge = true;
            m_reply->abort();
            finish(false);
            return;
        }
        QString ignored_error;
        if (!extractJsonRpcMessage(m_accumulated, &ignored_error).isEmpty()) {
            m_haveResponse = true;
            finish(false);
        }
    }

    void finish(bool timed_out) {
        bool expected = false;
        if (!m_completed.compare_exchange_strong(expected, true)) {
            return;
        }
        if (m_timer->isActive()) {
            m_timer->stop();
        }
        if (!timed_out) {
            m_sinks.state->status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            if (!m_haveResponse) {
                const qint64 room = static_cast<qint64>(kMaxResponseBytes) + 1 -
                                    m_accumulated.size();
                if (room > 0 && m_reply->bytesAvailable() > 0) {
                    m_accumulated.append(m_reply->read(qMin(m_reply->bytesAvailable(), room)));
                }
                // A decoded body over the cap (e.g. a compressed payload that expands past it,
                // undercounted by the wire-byte downloadProgress) must be rejected, not
                // truncated-and-accepted as if it were a complete message.
                if (m_accumulated.size() > kMaxResponseBytes || m_reply->bytesAvailable() > 0) {
                    m_tooLarge = true;
                }
            }
            m_sinks.state->response_body = m_accumulated;
            m_sinks.state->network_error = m_reply->error();
            m_sinks.state->network_error_text = m_reply->errorString();
        }
        m_sinks.state->timed_out = timed_out;
        m_sinks.state->too_large = m_tooLarge.load();
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
    QByteArray m_accumulated;
    bool m_haveResponse{false};
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
    const bool acquired = done.tryAcquire(1, timeout_ms + kSemaphoreWaitGraceMs);
    network_thread.quit();
    // SAK-ALLOW-BLOCKING: `network_thread` is a stack local destroyed on return, and
    // ~QThread on a live thread aborts the process. quit() precedes it, and QThread::exit
    // is remembered even if exec() has not started yet, so this cannot hang on a thread
    // that raced past its own event loop.
    network_thread.wait();
    // Write state.timed_out ONLY after wait() has joined the worker, so it can never race the
    // worker's own finish() write to the shared HttpCallState (that would be C++ data-race UB).
    if (!acquired) {
        state.timed_out = true;
    }
    return state;
}

// Validates the status line of a completed reply that had no transport error. Returns an empty
// string for a usable 2xx envelope; otherwise the reason it must not be parsed as a successful MCP
// result -- a 3xx we did not follow (cross-origin) or any other non-2xx, or a missing status code.
[[nodiscard]] QString statusEnvelopeFailure(const HttpCallState& state) {
    const int status_code = state.status.isValid() ? state.status.toInt() : 0;
    if (status_code >= kHttpStatusSuccessMin && status_code < kHttpStatusSuccessEnd) {
        return {};
    }
    return state.status.isValid()
               ? QStringLiteral("MCP HTTP request returned an unexpected status (HTTP %1)")
                     .arg(status_code)
               : QStringLiteral("MCP HTTP response had no HTTP status code");
}

// Returns why the HTTP call cannot yield an MCP result, or an empty string when the reply is a
// usable 2xx envelope. Checked in the order the failure is enforced: the local caps (too-large,
// timeout) before any status the server returned, then the transport error itself.
[[nodiscard]] QString httpFailureReason(const HttpCallState& state) {
    if (state.too_large) {
        return QStringLiteral("MCP HTTP response exceeded the %1-byte cap").arg(kMaxResponseBytes);
    }
    if (state.timed_out) {
        return QStringLiteral("MCP HTTP request timed out");
    }
    if (state.network_error == QNetworkReply::NoError) {
        return statusEnvelopeFailure(state);
    }
    return QStringLiteral("MCP HTTP request failed%1: %2")
        .arg(state.status.isValid() ? QStringLiteral(" (HTTP %1)").arg(state.status.toInt())
                                    : QString(),
             state.network_error_text);
}

bool explainHttpFailure(const HttpCallState& state, QString* error_message) {
    const QString reason = httpFailureReason(state);
    if (reason.isEmpty()) {
        return false;
    }
    if (error_message) {
        *error_message = reason;
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
    if (body.size() > kMaxRequestBytes) {
        if (error_message) {
            *error_message =
                QStringLiteral("MCP request body exceeds the %1-byte cap").arg(kMaxRequestBytes);
        }
        return {};
    }
    // Clamp the caller-supplied timeout: a negative/zero wait would block the semaphore forever
    // and an overflowing one would wrap negative when the grace is added.
    const int effective_timeout_ms =
        qBound(kMinimumRequestTimeoutMs, timeout_ms, kMaxRequestTimeoutMs);
    const HttpCallState state = performHttpToolCall(endpoint, body, effective_timeout_ms);
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
