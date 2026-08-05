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

#include <utility>

namespace sak {

namespace {

constexpr int kCancellationPollIntervalMs = sak::kTimerPollingFastMs;

// A response with no caller-supplied cap (max_response_bytes <= 0) would let QNetworkReply
// buffer an arbitrarily large body into memory. Clamp any such request to this bound so no
// caller can trigger unbounded growth; callers that legitimately need more must opt in.
constexpr qint64 kDefaultMaxResponseBytes = 512LL * 1024 * 1024;  // 512 MiB

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
        return !m_sinks.result->timed_out && !m_sinks.result->cancelled &&
               reply->error() == QNetworkReply::NoError;
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
    thread.wait();
    return result;
}

}  // namespace sak
