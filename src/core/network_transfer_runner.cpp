// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_transfer_runner.h"

#include "sak/layout_constants.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSemaphore>
#include <QSslConfiguration>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace sak {

namespace {

constexpr int kCancellationPollIntervalMs = sak::kTimerPollingFastMs;

// A response with no caller-supplied cap (max_response_bytes <= 0) would let QNetworkReply
// buffer an arbitrarily large body into memory. Clamp any such request to this bound so no
// caller can trigger unbounded growth; callers that legitimately need more must opt in.
constexpr qint64 kDefaultMaxResponseBytes = 512LL * 1024 * 1024;  // 512 MiB

// Only a 2xx status is an authoritative, complete body. The success range is [200, 300).
constexpr int kHttpSuccessStatusMin = 200;
constexpr int kHttpSuccessStatusEnd = 300;

constexpr char kMetadataTimedOutProperty[] = "sak_metadata_timed_out";
constexpr char kMetadataOversizeProperty[] = "sak_metadata_oversize";
constexpr char kMetadataMaxBytesProperty[] = "sak_metadata_max_bytes";
constexpr char kMetadataInjectedErrorProperty[] = "sak_metadata_injected_error";

thread_local NetworkMetadataFaultInjector g_network_metadata_fault_injector;

class InjectedMetadataReply final : public QNetworkReply {
public:
    InjectedMetadataReply(const QNetworkRequest& request,
                          const NetworkTransferResult& result,
                          QObject* parent)
        : QNetworkReply(parent), m_body(result.body) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, result.http_status);
        setProperty(kMetadataInjectedErrorProperty, result.error_message);
        if (result.timed_out) {
            setProperty(kMetadataTimedOutProperty, true);
            setError(QNetworkReply::TimeoutError,
                     result.error_message.isEmpty()
                         ? QStringLiteral("Network metadata request timed out")
                         : result.error_message);
        } else if (result.cancelled) {
            setError(QNetworkReply::OperationCanceledError,
                     result.error_message.isEmpty()
                         ? QStringLiteral("Network metadata request cancelled")
                         : result.error_message);
        } else if (!result.success) {
            setError(QNetworkReply::NetworkSessionFailedError,
                     result.error_message.isEmpty()
                         ? QStringLiteral("Network metadata request failed")
                         : result.error_message);
        }
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() { finish(); });
    }

    void abort() override {
        if (isFinished()) {
            return;
        }
        setError(QNetworkReply::OperationCanceledError,
                 QStringLiteral("Network metadata request cancelled"));
        // Constructor already queued finish() for the next event turn. Keep abort asynchronous too,
        // so callers always have time to connect QNetworkReply::finished after this seam returns.
    }

    [[nodiscard]] qint64 bytesAvailable() const override {
        return (m_body.size() - m_offset) + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 max_size) override {
        const qint64 remaining = m_body.size() - m_offset;
        const qint64 count = std::min(max_size, remaining);
        if (count <= 0) {
            return -1;
        }
        std::memcpy(data, m_body.constData() + m_offset, static_cast<std::size_t>(count));
        m_offset += count;
        return count;
    }

private:
    void finish() {
        if (isFinished()) {
            return;
        }
        setFinished(true);
        if (!m_body.isEmpty()) {
            Q_EMIT readyRead();
        }
        Q_EMIT finished();
    }

    QByteArray m_body;
    qint64 m_offset{0};
};

QString metadataRequestValidationError(const NetworkMetadataRequest& request) {
    if (!request.url.isValid() || request.url.isEmpty()) {
        return QStringLiteral("Invalid metadata URL");
    }
    if (request.url.scheme() != QStringLiteral("https")) {
        return QStringLiteral("Metadata URL must use HTTPS");
    }
    if (request.timeout_ms <= 0) {
        return QStringLiteral("Metadata timeout must be positive");
    }
    if (request.max_response_bytes <= 0) {
        return QStringLiteral("Metadata response-size limit must be positive");
    }
    return {};
}

QNetworkRequest metadataNetworkRequest(const NetworkMetadataRequest& request) {
    QNetworkRequest network_request(request.url);
    network_request.setTransferTimeout(request.timeout_ms);
    network_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
    QSslConfiguration ssl_config = QSslConfiguration::defaultConfiguration();
    ssl_config.setProtocol(QSsl::TlsV1_2OrLater);
    network_request.setSslConfiguration(ssl_config);
    for (const auto& [name, value] : request.raw_headers) {
        network_request.setRawHeader(name, value);
    }
    return network_request;
}

QNetworkReply* injectedMetadataReply(QNetworkAccessManager& manager,
                                     const QNetworkRequest& request,
                                     const NetworkTransferResult& result,
                                     qint64 max_response_bytes) {
    auto* reply = new InjectedMetadataReply(request, result, &manager);
    reply->setProperty(kMetadataMaxBytesProperty, max_response_bytes);
    return reply;
}

bool isSuccessfulHttpStatus(int status) {
    return status >= kHttpSuccessStatusMin && status < kHttpSuccessStatusEnd;
}

bool metadataReplyWasCancelled(QNetworkReply* reply,
                               const NetworkTransferResult& result,
                               bool oversized) {
    return reply->error() == QNetworkReply::OperationCanceledError && !result.timed_out &&
           !oversized;
}

bool recordMetadataReplyFailure(QNetworkReply* reply, NetworkTransferResult* result) {
    result->http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result->timed_out = reply->property(kMetadataTimedOutProperty).toBool() ||
                        reply->error() == QNetworkReply::TimeoutError;
    const bool oversized = reply->property(kMetadataOversizeProperty).toBool();
    result->cancelled = metadataReplyWasCancelled(reply, *result, oversized);
    if (result->timed_out) {
        result->error_message = QStringLiteral("Network metadata request timed out");
    } else if (oversized) {
        result->error_message = QStringLiteral("Network metadata response exceeded size limit");
    } else if (result->cancelled) {
        result->error_message = QStringLiteral("Network metadata request cancelled");
    } else if (result->http_status != 0 && !isSuccessfulHttpStatus(result->http_status)) {
        result->error_message =
            QStringLiteral("Network metadata request returned HTTP %1").arg(result->http_status);
    } else if (reply->error() != QNetworkReply::NoError) {
        const QString injected_error = reply->property(kMetadataInjectedErrorProperty).toString();
        result->error_message = injected_error.isEmpty() ? reply->errorString() : injected_error;
    } else if (result->http_status == 0) {
        result->error_message = QStringLiteral("Network metadata reply had no HTTP status");
    }
    return !result->error_message.isEmpty();
}

struct NetworkTransferSinks {
    NetworkTransferResult* result{nullptr};
    QSemaphore* finished{nullptr};
    QThread* owner_thread{nullptr};
};

class NetworkTransferWorker final : public QObject {
public:
    NetworkTransferWorker(NetworkTransferRequest request,
                          NetworkCancelCheck should_cancel,
                          NetworkProgressCallback progress,
                          const NetworkTransferSinks& sinks)
        : m_request(std::move(request))
        , m_should_cancel(std::move(should_cancel))
        , m_progress(std::move(progress))
        , m_sinks(sinks) {}

    void start() {
        auto* manager = new QNetworkAccessManager(this);
        manager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = createReply(manager, networkRequest());
        // Bound QNetworkReply's internal read buffer to the response cap so a hostile or
        // oversized body applies backpressure at the socket instead of buffering without limit.
        // max_response_bytes is always positive here (runNetworkTransfer clamps a missing cap).
        const qint64 buffer_cap = m_request.max_response_bytes;
        reply->setReadBufferSize(buffer_cap < std::numeric_limits<qint64>::max() ? buffer_cap + 1
                                                                                 : buffer_cap);
        auto* timeout_timer = createTimeoutTimer(reply);
        auto* cancel_timer = createCancelTimer(reply);
        connectProgress(reply);
        QObject::connect(reply, &QNetworkReply::finished, this, [=, this]() {
            finish(reply, timeout_timer, cancel_timer);
        });
    }

private:
    QNetworkRequest networkRequest() const {
        QNetworkRequest network_request(m_request.url);
        network_request.setTransferTimeout(m_request.timeout_ms);
        for (const auto& [name, value] : m_request.raw_headers) {
            network_request.setRawHeader(name, value);
        }
        return network_request;
    }

    QNetworkReply* createReply(QNetworkAccessManager* manager, const QNetworkRequest& request) {
        m_elapsed.start();
        switch (m_request.method) {
        case NetworkTransferMethod::Get:
            return manager->get(request);
        case NetworkTransferMethod::Post:
            return manager->post(request, m_request.body);
        case NetworkTransferMethod::Head:
            return manager->head(request);
        }
        return manager->get(request);
    }

    QTimer* createTimeoutTimer(QNetworkReply* reply) {
        auto* timeout_timer = new QTimer(this);
        timeout_timer->setSingleShot(true);
        QObject::connect(timeout_timer, &QTimer::timeout, this, [this, reply]() {
            m_sinks.result->timed_out = true;
            m_sinks.result->error_message = QStringLiteral("Network transfer timed out");
            reply->abort();
        });
        // runNetworkTransfer rejects a non-positive timeout, so the deadline is ALWAYS armed:
        // no transfer can sit forever on the synchronous finished.acquire() below.
        timeout_timer->start(m_request.timeout_ms);
        return timeout_timer;
    }

    QTimer* createCancelTimer(QNetworkReply* reply) {
        auto* cancel_timer = new QTimer(this);
        cancel_timer->setInterval(kCancellationPollIntervalMs);
        QObject::connect(cancel_timer, &QTimer::timeout, this, [this, reply]() {
            if (m_should_cancel && m_should_cancel()) {
                m_sinks.result->cancelled = true;
                m_sinks.result->error_message = QStringLiteral("Network transfer cancelled");
                reply->abort();
            }
        });
        if (m_should_cancel) {
            cancel_timer->start();
        }
        return cancel_timer;
    }

    void connectProgress(QNetworkReply* reply) {
        QObject::connect(reply,
                         &QNetworkReply::downloadProgress,
                         this,
                         [this, reply](qint64 received, qint64 total) {
                             // Enforce the response-size cap early (on the reported total when
                             // known, else the running received count) so QNetworkReply's internal
                             // buffer is bounded to ~max_response_bytes instead of growing without
                             // limit.
                             const qint64 limit = m_request.max_response_bytes;
                             if (limit > 0 && (total > limit || received > limit)) {
                                 m_sinks.result->error_message =
                                     QStringLiteral("Response exceeded the maximum allowed size");
                                 reply->abort();
                                 return;
                             }
                             m_sinks.result->bytes_received = received;
                             m_sinks.result->bytes_total = total;
                             if (m_progress) {
                                 m_progress(received, total);
                             }
                         });
    }

    void finish(QNetworkReply* reply, QTimer* timeout_timer, QTimer* cancel_timer) {
        timeout_timer->stop();
        cancel_timer->stop();
        m_sinks.result->elapsed_ms = m_elapsed.elapsed();
        m_sinks.result->http_status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (transferSucceeded(reply)) {
            QByteArray body = reply->readAll();
            const qint64 limit = m_request.max_response_bytes;
            if (limit > 0 && body.size() > limit) {
                // A small-but-over-cap body delivered in one shot never tripped the
                // progress guard; reject it here rather than returning oversized data.
                m_sinks.result->error_message =
                    QStringLiteral("Response exceeded the maximum allowed size");
            } else {
                m_sinks.result->success = true;
                m_sinks.result->bytes_received = body.size();
                m_sinks.result->body = std::move(body);
            }
        } else if (m_sinks.result->error_message.isEmpty()) {
            m_sinks.result->error_message = reply->errorString();
        }
        reply->deleteLater();
        m_sinks.owner_thread->quit();
        m_sinks.finished->release();
    }

    bool transferSucceeded(QNetworkReply* reply) const {
        if (m_sinks.result->timed_out || m_sinks.result->cancelled ||
            reply->error() != QNetworkReply::NoError) {
            return false;
        }
        // A reply can complete with NoError yet carry a non-success HTTP status (a bare 304,
        // or a 3xx the redirect policy declined to follow). Only a 2xx is an authoritative,
        // complete body; anything else fails closed instead of surfacing a stale or empty
        // response as success. The scheme is restricted to http/https, so http_status is always
        // a real HTTP code once the reply has finished.
        const int status = m_sinks.result->http_status;
        return status >= kHttpSuccessStatusMin && status < kHttpSuccessStatusEnd;
    }

    NetworkTransferRequest m_request;
    NetworkCancelCheck m_should_cancel;
    NetworkProgressCallback m_progress;
    NetworkTransferSinks m_sinks;
    QElapsedTimer m_elapsed;
};

}  // namespace

ScopedNetworkMetadataFaultInjector::ScopedNetworkMetadataFaultInjector(
    NetworkMetadataFaultInjector injector)
    : m_previous(std::move(g_network_metadata_fault_injector)) {
    g_network_metadata_fault_injector = std::move(injector);
}

ScopedNetworkMetadataFaultInjector::~ScopedNetworkMetadataFaultInjector() {
    g_network_metadata_fault_injector = std::move(m_previous);
}

QNetworkReply* startNetworkMetadataGet(QNetworkAccessManager& manager,
                                       const NetworkMetadataRequest& request) {
    const QNetworkRequest network_request = metadataNetworkRequest(request);
    const QString validation_error = metadataRequestValidationError(request);
    if (!validation_error.isEmpty()) {
        NetworkTransferResult failure;
        failure.error_message = validation_error;
        return injectedMetadataReply(manager, network_request, failure, request.max_response_bytes);
    }

    QNetworkReply* reply = nullptr;
    if (g_network_metadata_fault_injector) {
        const auto injected = g_network_metadata_fault_injector(request);
        if (injected.has_value()) {
            reply = injectedMetadataReply(
                manager, network_request, *injected, request.max_response_bytes);
        }
    }

    if (reply == nullptr) {
        reply = manager.get(network_request);
    }
    reply->setProperty(kMetadataMaxBytesProperty, request.max_response_bytes);
    reply->setReadBufferSize(request.max_response_bytes < std::numeric_limits<qint64>::max()
                                 ? request.max_response_bytes + 1
                                 : request.max_response_bytes);
    auto* deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    QObject::connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty(kMetadataTimedOutProperty, true);
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, deadline, &QTimer::stop);
    QObject::connect(reply,
                     &QNetworkReply::downloadProgress,
                     reply,
                     [reply, limit = request.max_response_bytes](qint64 received, qint64 total) {
                         if (received > limit || total > limit) {
                             reply->setProperty(kMetadataOversizeProperty, true);
                             reply->abort();
                         }
                     });
    deadline->start(request.timeout_ms);
    return reply;
}

NetworkTransferResult takeNetworkMetadataResult(QNetworkReply* reply) {
    NetworkTransferResult result;
    if (reply == nullptr) {
        result.error_message = QStringLiteral("Network metadata reply is unavailable");
        return result;
    }
    if (recordMetadataReplyFailure(reply, &result)) {
        return result;
    }

    result.body = reply->readAll();
    // Qt can discover decompression/read errors while consuming an already-finished reply.
    // A pre-read error check alone would publish partial metadata as a successful response.
    if (recordMetadataReplyFailure(reply, &result)) {
        result.body.clear();
        return result;
    }
    result.bytes_received = result.body.size();
    const qint64 limit = reply->property(kMetadataMaxBytesProperty).toLongLong();
    if (limit <= 0 || result.body.size() > limit) {
        result.body.clear();
        result.bytes_received = 0;
        result.error_message = QStringLiteral("Network metadata response exceeded size limit");
        return result;
    }
    result.success = true;
    return result;
}

NetworkTransferResult runNetworkTransfer(const NetworkTransferRequest& request,
                                         const NetworkCancelCheck& should_cancel,
                                         const NetworkProgressCallback& progress) {
    NetworkTransferResult result;
    if (!request.url.isValid() || request.url.isEmpty()) {
        result.error_message = QStringLiteral("Invalid URL");
        return result;
    }

    // Restrict to http/https. QNetworkAccessManager also serves file:, qrc:, ftp: and data:
    // schemes, so an unrestricted URL fed from config or an AI-planned call could read a local
    // file (file:///...) or hit an unintended handler. Fail closed on anything else; every real
    // caller performs an HTTP(S) transfer. Host-level SSRF policy (loopback/private ranges) is
    // deliberately NOT enforced here: LAN callers rely on it and QUrl cannot resolve intent.
    const QString scheme = request.url.scheme();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        result.error_message = QStringLiteral(
            "Unsupported URL scheme: only http and https are "
            "allowed");
        return result;
    }

    // A non-positive timeout DISABLES Qt's transfer timeout and leaves the deadline timer
    // unarmed, so a hostile/slow-loris server would block this synchronous call forever when
    // no should_cancel callback is supplied. Reject it instead of substituting a default: the
    // caller's deadline is a real requirement, and silently inventing one hides the bad input.
    if (request.timeout_ms <= 0) {
        result.error_message = QStringLiteral("Invalid timeout_ms: must be positive");
        return result;
    }

    // Fail closed on unbounded responses: clamp a missing/unlimited cap to the default bound
    // before the worker ever starts reading, so an omitted max_response_bytes cannot let the
    // reply buffer grow without limit.
    NetworkTransferRequest bounded = request;
    if (bounded.max_response_bytes <= 0) {
        bounded.max_response_bytes = kDefaultMaxResponseBytes;
    }

    // Observe an already-set cancellation BEFORE the request is issued. The worker's cancel
    // timer only polls after the reply is created, so without this a request cancelled up front
    // (a POST with remote side effects, in particular) would still be transmitted once.
    if (should_cancel && should_cancel()) {
        result.cancelled = true;
        result.error_message = QStringLiteral("Network transfer cancelled");
        return result;
    }

    QThread thread;
    QSemaphore finished;
    auto* worker = new NetworkTransferWorker(
        bounded,
        should_cancel,
        progress,
        {.result = &result, .finished = &finished, .owner_thread = &thread});
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&thread, &QThread::started, worker, [worker]() { worker->start(); });

    thread.start();
    finished.acquire();
    // SAK-ALLOW-BLOCKING: `thread` is a stack local destroyed on return, and ~QThread on
    // a live thread aborts the process, so this join is not optional. The worker has
    // already released `finished`, so nothing is left to run.
    thread.wait();
    return result;
}

}  // namespace sak
