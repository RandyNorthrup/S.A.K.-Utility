// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_controller.h
/// @brief Orchestrates OST/PST conversion queue and worker threads

#pragma once

#include "sak/ost_converter_types.h"

#include <QObject>
#include <QVector>

#include <atomic>
#include <memory>
#include <type_traits>

class QThread;

namespace sak {

class OstConversionWorker;

/// @brief Manages the conversion queue and dispatches workers
class OstConverterController : public QObject {
    Q_OBJECT

public:
    explicit OstConverterController(QObject* parent = nullptr);
    ~OstConverterController() override;

    // Non-copyable, non-movable
    OstConverterController(const OstConverterController&) = delete;
    OstConverterController& operator=(const OstConverterController&) = delete;
    OstConverterController(OstConverterController&&) = delete;
    OstConverterController& operator=(OstConverterController&&) = delete;

    /// Add a file to the conversion queue
    void addFile(const QString& path);

    /// Remove a file from the queue (before conversion starts)
    void removeFile(int index);

    /// Clear the entire queue
    void clearQueue();

    /// Start the conversion batch
    void startConversion(const OstConversionConfig& config);

    /// Cancel all in-progress conversions
    void cancelAll();

    /// Whether a conversion is currently running
    [[nodiscard]] bool isRunning() const;

    /// Get the current queue
    [[nodiscard]] const QVector<OstConversionJob>& queue() const;

    /// Get the path to the last generated report
    [[nodiscard]] const QString& reportPath() const;

    /// Classify a finished conversion into a queue status. Fail closed: any failed
    /// item or recorded error demotes the job to Failed even when some items
    /// converted, so a partial/errored run is never reported as a clean Complete.
    [[nodiscard]] static OstConversionJob::Status classifyOutcome(
        const OstConversionResult& result);

Q_SIGNALS:
    void fileAdded(int index, sak::OstConversionJob job);
    void fileRemoved(int index);
    void queueCleared();

    // Conversion progress
    void conversionStarted(int total_files);
    void fileConversionStarted(int file_index);
    void fileProgressUpdated(int file_index,
                             int items_done,
                             int items_total,
                             QString current_folder);
    void fileConversionComplete(int file_index, sak::OstConversionResult result);
    void allConversionsComplete(sak::OstConversionBatchResult result);

    // Errors
    void errorOccurred(int file_index, QString message);
    void warningOccurred(int file_index, QString message);

    // Status
    void statusMessage(QString message, int timeout_ms);

private Q_SLOTS:
    void onWorkerFinished(sak::OstConversionResult result);
    void onWorkerProgress(int items_done, int items_total, QString folder);
    void onWorkerError(QString message);

private:
    void startNextFile();

    /// Stop and destroy the worker + thread at @p worker_index, then drop it from
    /// m_active_workers.
    void destroyActiveWorker(int worker_index);
    void finalizeBatch();

    QVector<OstConversionJob> m_queue;
    OstConversionConfig m_config;
    std::atomic<bool> m_cancelled{false};
    bool m_running = false;

    // Worker threads (supports N concurrent workers)
    struct ActiveWorker {
        QThread* thread = nullptr;
        OstConversionWorker* worker = nullptr;
        int file_index = -1;
    };

    /// Stop and dispose one active worker/thread. Deletes both only once the
    /// thread has actually stopped; if it refuses to stop, defers deletion to the
    /// thread's finished signal instead of deleting a live thread (abort/UAF).
    void disposeWorker(ActiveWorker& aw);

    QVector<ActiveWorker> m_active_workers;
    int m_next_queued_index = 0;

    // Batch tracking
    OstConversionBatchResult m_batch_result;
    QString m_report_path;
};

// Compile-Time Invariants
static_assert(!std::is_copy_constructible_v<OstConverterController>,
              "OstConverterController must not be copy-constructible.");

}  // namespace sak
