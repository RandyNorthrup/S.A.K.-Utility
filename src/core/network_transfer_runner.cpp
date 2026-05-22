// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_transfer_runner.h"

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
                          NetworkTransferSinks sinks)
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
        if (m_request.timeout_ms > 0) {
            timeout_timer->start(m_request.timeout_ms);
        }
        return timeout_timer;
    }

    QTimer* createCancelTimer(QNetworkReply* reply) {
        auto* cancel_timer = new QTimer(this);
        cancel_timer->setInterval(100);
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
        QObject::connect(
            reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
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
            m_sinks.result->body = reply->readAll();
            m_sinks.result->success = true;
            m_sinks.result->bytes_received = m_sinks.result->body.size();
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

    QThread thread;
    QSemaphore finished;
    auto* worker = new NetworkTransferWorker(
        request,
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
