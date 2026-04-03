// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_controller.cpp
/// @brief OST/PST conversion queue orchestration

#include "sak/ost_converter_controller.h"

#include "sak/conversion_report_generator.h"
#include "sak/logger.h"
#include "sak/ost_conversion_worker.h"
#include "sak/ost_converter_constants.h"

#include <QFileInfo>
#include <QThread>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

OstConverterController::OstConverterController(QObject* parent) : QObject(parent) {}

OstConverterController::~OstConverterController() {
    cancelAll();
}

// ============================================================================
// Queue Management
// ============================================================================

void OstConverterController::addFile(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        logWarning("OST Converter: file does not exist: {}", path.toStdString());
        return;
    }

    // Check for duplicate
    for (const auto& job : m_queue) {
        if (job.source_path == path) {
            Q_EMIT statusMessage(tr("File already in queue: %1").arg(fi.fileName()),
                                 ost::kTimerStatusMessageMs);
            return;
        }
    }

    OstConversionJob job;
    job.source_path = path;
    job.display_name = fi.fileName();
    job.file_size_bytes = fi.size();

    QString suffix = fi.suffix().toLower();
    job.is_ost = (suffix == QStringLiteral("ost"));

    int index = m_queue.size();
    m_queue.append(job);

    logInfo("OST Converter: added file to queue: {}", fi.fileName().toStdString());

    Q_EMIT fileAdded(index, job);
    Q_EMIT statusMessage(tr("Added: %1").arg(fi.fileName()), ost::kTimerStatusMessageMs);
}

void OstConverterController::removeFile(int index) {
    if (index < 0 || index >= m_queue.size()) {
        return;
    }
    if (m_running) {
        return;
    }

    m_queue.removeAt(index);
    Q_EMIT fileRemoved(index);
}

void OstConverterController::clearQueue() {
    if (m_running) {
        return;
    }

    m_queue.clear();
    Q_EMIT queueCleared();
}

// ============================================================================
// Conversion Control
// ============================================================================

void OstConverterController::startConversion(const OstConversionConfig& config) {
    if (m_running || m_queue.isEmpty()) {
        return;
    }

    m_config = config;
    m_cancelled.store(false);
    m_running = true;
    m_current_file_index = -1;
    m_next_queued_index = 0;
    m_active_workers.clear();
    m_report_path.clear();

    // Initialize batch result
    m_batch_result = OstConversionBatchResult();
    m_batch_result.files_total = m_queue.size();
    m_batch_result.batch_started = QDateTime::currentDateTime();

    logInfo("OST Converter: starting batch conversion — {} files, {} threads",
            std::to_string(m_queue.size()),
            std::to_string(config.max_threads));

    Q_EMIT conversionStarted(m_queue.size());

    // Launch up to max_threads concurrent workers
    int threads_to_launch = qMin(config.max_threads, m_queue.size());
    for (int i = 0; i < threads_to_launch; ++i) {
        startNextFile();
    }
}

void OstConverterController::cancelAll() {
    m_cancelled.store(true);

    // Cancel all active workers
    for (auto& aw : m_active_workers) {
        if (aw.worker) {
            aw.worker->cancel();
        }
    }

    // Shut down all worker threads
    for (auto& aw : m_active_workers) {
        if (aw.thread) {
            aw.thread->quit();
            if (!aw.thread->wait(ost::kTimeoutThreadShutdownMs)) {
                logWarning("OST Converter: worker thread did not stop gracefully");
                aw.thread->terminate();
                aw.thread->wait(ost::kTimeoutThreadTerminateMs);
            }
            delete aw.thread;
            aw.thread = nullptr;
            aw.worker = nullptr;
        }
    }
    m_active_workers.clear();

    if (m_running) {
        m_running = false;
        // Mark remaining queued files as cancelled
        for (auto& job : m_queue) {
            if (job.status == OstConversionJob::Status::Queued ||
                job.status == OstConversionJob::Status::Converting) {
                job.status = OstConversionJob::Status::Cancelled;
            }
        }
        finalizeBatch();
    }
}

bool OstConverterController::isRunning() const {
    return m_running;
}

const QVector<OstConversionJob>& OstConverterController::queue() const {
    return m_queue;
}

const QString& OstConverterController::reportPath() const {
    return m_report_path;
}

// ============================================================================
// Worker Lifecycle
// ============================================================================

void OstConverterController::startNextFile() {
    if (m_cancelled.load()) {
        if (m_active_workers.isEmpty()) {
            finalizeBatch();
        }
        return;
    }

    // Find next queued file
    while (m_next_queued_index < m_queue.size()) {
        if (m_queue[m_next_queued_index].status == OstConversionJob::Status::Queued) {
            break;
        }
        ++m_next_queued_index;
    }

    if (m_next_queued_index >= m_queue.size()) {
        // No more files to start
        if (m_active_workers.isEmpty()) {
            finalizeBatch();
        }
        return;
    }

    int file_index = m_next_queued_index;
    ++m_next_queued_index;

    auto& job = m_queue[file_index];
    job.status = OstConversionJob::Status::Converting;

    Q_EMIT fileConversionStarted(file_index);

    // Create worker thread
    ActiveWorker aw;
    aw.file_index = file_index;
    aw.thread = new QThread(this);
    aw.worker = new OstConversionWorker();
    aw.worker->moveToThread(aw.thread);

    connect(aw.worker,
            &OstConversionWorker::conversionFinished,
            this,
            &OstConverterController::onWorkerFinished,
            Qt::QueuedConnection);
    connect(aw.worker,
            &OstConversionWorker::progressUpdated,
            this,
            &OstConverterController::onWorkerProgress,
            Qt::QueuedConnection);
    connect(aw.worker,
            &OstConversionWorker::errorOccurred,
            this,
            &OstConverterController::onWorkerError,
            Qt::QueuedConnection);

    // Start conversion when thread starts
    QString source = job.source_path;
    OstConversionConfig config = m_config;
    connect(aw.thread, &QThread::started, aw.worker, [worker = aw.worker, source, config]() {
        worker->convert(source, config);
    });

    m_active_workers.append(aw);
    aw.thread->start();
}

void OstConverterController::finalizeBatch() {
    m_running = false;
    m_batch_result.batch_finished = QDateTime::currentDateTime();

    // Aggregate results
    for (const auto& file_result : m_batch_result.file_results) {
        m_batch_result.total_items_converted += file_result.items_converted;
        m_batch_result.total_items_recovered += file_result.items_recovered;
        m_batch_result.total_bytes_written += file_result.bytes_written;
    }

    // Generate report if configured
    if (m_config.generate_html_report && !m_config.output_directory.isEmpty()) {
        m_report_path = ConversionReportGenerator::generateHtmlReport(m_batch_result,
                                                                      m_config.output_directory);
    }

    logInfo("OST Converter: batch complete — {}/{} files succeeded",
            std::to_string(m_batch_result.files_succeeded),
            std::to_string(m_batch_result.files_total));

    Q_EMIT allConversionsComplete(m_batch_result);
    Q_EMIT statusMessage(tr("Conversion complete: %1/%2 files succeeded")
                             .arg(m_batch_result.files_succeeded)
                             .arg(m_batch_result.files_total),
                         ost::kTimerStatusLongMs);
}

// ============================================================================
// Worker Slots
// ============================================================================

void OstConverterController::onWorkerFinished(OstConversionResult result) {
    auto* sender_worker = qobject_cast<OstConversionWorker*>(sender());

    // Find which ActiveWorker completed
    int worker_index = -1;
    int file_index = -1;
    for (int i = 0; i < m_active_workers.size(); ++i) {
        if (m_active_workers[i].worker == sender_worker) {
            worker_index = i;
            file_index = m_active_workers[i].file_index;
            break;
        }
    }

    if (file_index < 0 || file_index >= m_queue.size()) {
        return;
    }

    auto& job = m_queue[file_index];

    if (result.items_failed > 0 && result.items_converted == 0) {
        job.status = OstConversionJob::Status::Failed;
        job.error_message = result.errors.isEmpty() ? tr("All items failed")
                                                    : result.errors.first();
        ++m_batch_result.files_failed;
    } else {
        job.status = OstConversionJob::Status::Complete;
        ++m_batch_result.files_succeeded;
    }

    job.items_processed = result.items_converted;
    job.items_failed = result.items_failed;
    job.items_recovered = result.items_recovered;
    job.bytes_written = result.bytes_written;

    m_batch_result.file_results.append(result);

    Q_EMIT fileConversionComplete(file_index, result);

    // Clean up worker thread
    if (worker_index >= 0) {
        auto& aw = m_active_workers[worker_index];
        if (aw.thread) {
            aw.thread->quit();
            aw.thread->wait(ost::kTimeoutThreadShutdownMs);
            aw.thread->deleteLater();
        }
        m_active_workers.removeAt(worker_index);
    }

    // Start next file if available
    startNextFile();
}

void OstConverterController::onWorkerProgress(int items_done, int items_total, QString folder) {
    auto* sender_worker = qobject_cast<OstConversionWorker*>(sender());

    int file_index = -1;
    for (const auto& aw : m_active_workers) {
        if (aw.worker == sender_worker) {
            file_index = aw.file_index;
            break;
        }
    }

    if (file_index >= 0 && file_index < m_queue.size()) {
        auto& job = m_queue[file_index];
        job.items_processed = items_done;
        job.items_total = items_total;
        job.current_folder = folder;
    }

    Q_EMIT fileProgressUpdated(file_index, items_done, items_total, folder);
}

void OstConverterController::onWorkerError(QString message) {
    auto* sender_worker = qobject_cast<OstConversionWorker*>(sender());

    int file_index = -1;
    for (const auto& aw : m_active_workers) {
        if (aw.worker == sender_worker) {
            file_index = aw.file_index;
            break;
        }
    }

    logError("OST Converter: worker error — {}", message.toStdString());
    Q_EMIT errorOccurred(file_index, message);
}

}  // namespace sak
