// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_transfer_runner.h"

#include "sak/layout_constants.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

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
        , m_shouldCancel(std::move(should_cancel))
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
        QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
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
            if (m_shouldCancel && m_shouldCancel()) {
                m_sinks.result->cancelled = true;
                m_sinks.result->error_message = QStringLiteral("Network transfer cancelled");
                reply->abort();
            }
        });
        if (m_shouldCancel) {
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
    NetworkCancelCheck m_shouldCancel;
    NetworkProgressCallback m_progress;
    NetworkTransferSinks m_sinks;
    QElapsedTimer m_elapsed;
};

}  // namespace

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
