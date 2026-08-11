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
    const QFileInfo fi(path);
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

    const QString suffix = fi.suffix().toLower();
    job.is_ost = (suffix == QStringLiteral("ost"));

    const int index = m_queue.size();
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
    m_next_queued_index = 0;
    m_active_workers.clear();
    m_report_path.clear();

    // Re-queue every job. The per-job statuses survive a finished run, so without this a
    // second startConversion() found nothing still Queued: startNextFile() walked the whole
    // queue, saw no active workers, and called finalizeBatch() immediately - emitting
    // conversionStarted(N) and then "0/N files succeeded" for a queue that had converted
    // perfectly the first time. Progress counters are reset with it so the new run does not
    // inherit the previous one's totals.
    for (auto& job : m_queue) {
        job.status = OstConversionJob::Status::Queued;
        job.items_processed = 0;
        job.items_recovered = 0;
        job.items_failed = 0;
    }

    // Initialize batch result
    m_batch_result = OstConversionBatchResult();
    m_batch_result.files_total = m_queue.size();
    m_batch_result.batch_started = QDateTime::currentDateTime();

    logInfo("OST Converter: starting batch conversion - {} files, {} threads",
            std::to_string(m_queue.size()),
            std::to_string(config.max_threads));

    Q_EMIT conversionStarted(m_queue.size());

    // Launch up to max_threads concurrent workers. Clamp to at least one: a
    // zero/negative max_threads would launch no worker, so finalizeBatch() would
    // never run and the batch would wedge (m_running stuck true, no completion
    // signal). m_queue is non-empty per the guard above, so this launches >= 1.
    // Bound it from ABOVE too: an untrusted/oversized max_threads must not spawn
    // one QThread per queued file and exhaust handles, memory, and scheduler
    // resources -- cap the peak worker count regardless of the requested value.
    constexpr int kMaxConcurrentWorkers = 64;
    const int threads_to_launch = qMin(qBound(1, config.max_threads, kMaxConcurrentWorkers),
                                       m_queue.size());
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

    // Shut down all worker threads (join before delete; defer on refuse-to-stop).
    for (auto& aw : m_active_workers) {
        disposeWorker(aw);
    }
    m_active_workers.clear();

    if (m_running) {
        m_running = false;
        // Mark remaining queued files as cancelled, and COUNT them. They used to be flipped
        // to Cancelled and then never accounted for anywhere, so files_succeeded +
        // files_failed came out short of files_total and the completion line reported
        // "X/N files succeeded" with the difference unexplained - indistinguishable from
        // files that had failed silently.
        for (auto& job : m_queue) {
            if (job.status == OstConversionJob::Status::Queued ||
                job.status == OstConversionJob::Status::Converting) {
                job.status = OstConversionJob::Status::Cancelled;
                ++m_batch_result.files_cancelled;
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

    const int file_index = m_next_queued_index;
    ++m_next_queued_index;

    auto& job = m_queue[file_index];
    job.status = OstConversionJob::Status::Converting;

    Q_EMIT fileConversionStarted(file_index);

    // Create worker thread. The thread is intentionally UNPARENTED: if a worker ever refuses to
    // stop, disposeWorker() defers its deletion to the thread's own finished signal. Were the
    // thread parented to this controller, ~QObject would force-delete it while still running
    // (abort), defeating that deferral. Ownership is instead handled explicitly in disposeWorker.
    ActiveWorker aw;
    aw.file_index = file_index;
    aw.thread = new QThread();
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
    const QString source = job.source_path;
    const OstConversionConfig config = m_config;
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

    logInfo("OST Converter: batch complete - {} succeeded, {} failed, {} cancelled of {}",
            std::to_string(m_batch_result.files_succeeded),
            std::to_string(m_batch_result.files_failed),
            std::to_string(m_batch_result.files_cancelled),
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

OstConversionJob::Status OstConverterController::classifyOutcome(
    const OstConversionResult& result) {
    // Any failed item or recorded error (e.g. a source-open failure, or a message
    // whose attachment could not be read) means the conversion was not clean.
    // A negative counter cannot occur on a real run and signals a corrupt result, so it
    // fails closed too -- a negative items_failed would otherwise slip past the old > 0
    // test. Everything else -- including a validly empty mailbox -- is Complete.
    if (result.items_failed != 0 || !result.errors.isEmpty() || result.items_converted < 0 ||
        result.items_recovered < 0 || result.bytes_written < 0) {
        return OstConversionJob::Status::Failed;
    }
    return OstConversionJob::Status::Complete;
}

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

    // Fail closed: a run with any failed item OR a recorded error is not a clean
    // conversion, even when some items were written -- a partial export must not be
    // reported as Complete. classifyOutcome centralizes that rule.
    job.status = classifyOutcome(result);
    if (job.status == OstConversionJob::Status::Failed) {
        job.error_message = result.errors.isEmpty()
                                ? tr("%1 item(s) failed").arg(result.items_failed)
                                : result.errors.first();
        ++m_batch_result.files_failed;
    } else {
        ++m_batch_result.files_succeeded;
    }

    job.items_processed = result.items_converted;
    job.items_failed = result.items_failed;
    job.items_recovered = result.items_recovered;
    job.bytes_written = result.bytes_written;

    m_batch_result.file_results.append(result);

    Q_EMIT fileConversionComplete(file_index, result);

    destroyActiveWorker(worker_index);

    // Start next file if available
    startNextFile();
}

void OstConverterController::destroyActiveWorker(int worker_index) {
    if (worker_index < 0 || worker_index >= m_active_workers.size()) {
        return;
    }
    // The worker was created without a parent and never deleted, leaking on every
    // completed conversion; delete it once its thread has stopped, then the thread.
    auto& aw = m_active_workers[worker_index];
    disposeWorker(aw);
    m_active_workers.removeAt(worker_index);
}

void OstConverterController::disposeWorker(ActiveWorker& aw) {
    if (!aw.thread) {
        return;
    }
    aw.thread->quit();
    // The worker checks the cancel flag at every folder/item loop, so a cancelled conversion
    // returns promptly and this graceful wait succeeds. We deliberately do NOT call terminate():
    // forcibly killing a thread mid-PST-write can corrupt the output file and leave internal locks
    // held. If the thread still has not stopped after the full budget (e.g. blocked in a stuck
    // syscall), defer deletion to its own finished signal rather than deleting -- or force-killing
    // -- a live QThread. Worst case the thread runs to natural completion and self-deletes; that is
    // a bounded leak, strictly safer than a terminate() that risks data corruption or a crash.
    const int stop_budget_ms = ost::kTimeoutThreadShutdownMs + ost::kTimeoutThreadTerminateMs;
    if (aw.thread->wait(stop_budget_ms)) {
        delete aw.worker;
        delete aw.thread;
    } else {
        logWarning(
            "OST Converter: worker thread did not stop; deferring cleanup to its finished "
            "signal (no forced terminate)");
        connect(aw.thread, &QThread::finished, aw.worker, &QObject::deleteLater);
        connect(aw.thread, &QThread::finished, aw.thread, &QObject::deleteLater);
    }
    aw.worker = nullptr;
    aw.thread = nullptr;
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

    logError("OST Converter: worker error - {}", message.toStdString());
    Q_EMIT errorOccurred(file_index, message);
}

}  // namespace sak
