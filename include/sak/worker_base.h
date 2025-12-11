#pragma once

#include "sak/error_codes.h"
#include <QThread>
#include <atomic>
#include <expected>
#include <stop_token>
#include <functional>

/**
 * @brief Base class for worker threads
 * 
 * Provides a Qt-integrated worker thread with C++23 features including
 * std::stop_token for cancellation and std::expected for error handling.
 * 
 * Thread-Safety: This class is thread-safe. Signals are emitted from
 * worker thread and should be connected with Qt::QueuedConnection.
 */
class WorkerBase : public QThread {
    Q_OBJECT

public:
    /**
     * @brief Construct worker thread
     * @param parent Parent QObject
     */
    explicit WorkerBase(QObject* parent = nullptr);
    
    /**
     * @brief Destructor - ensures thread is stopped
     */
    ~WorkerBase() override;

    // Disable copy and move
    WorkerBase(const WorkerBase&) = delete;
    WorkerBase& operator=(const WorkerBase&) = delete;
    WorkerBase(WorkerBase&&) = delete;
    WorkerBase& operator=(WorkerBase&&) = delete;

    /**
     * @brief Request cancellation of worker
     */
    void request_stop() noexcept;

    /**
     * @brief Check if stop has been requested
     * @return True if cancellation requested
     */
    [[nodiscard]] bool stop_requested() const noexcept;

    /**
     * @brief Check if worker is currently running
     * @return True if worker is executing
     */
    [[nodiscard]] bool is_running() const noexcept;

Q_SIGNALS:
    /**
     * @brief Emitted when worker starts
     */
    void started();

    /**
     * @brief Emitted when worker completes successfully
     */
    void finished();

    /**
     * @brief Emitted when worker fails
     * @param error_code Error code indicating failure reason
     * @param message Human-readable error message
     */
    void failed(int error_code, const QString& message);

    /**
     * @brief Emitted when worker is cancelled
     */
    void cancelled();

    /**
     * @brief Emitted to report progress
     * @param current Current progress value
     * @param total Total progress value
     * @param message Optional status message
     */
    void progress(int current, int total, const QString& message);

protected:
    /**
     * @brief Main worker execution - override in derived classes
     * 
     * This method runs in the worker thread. Use check_stop() to
     * check for cancellation requests and emit progress() for updates.
     * 
     * @return Expected containing success or error code
     */
    virtual auto execute() -> std::expected<void, sak::error_code> = 0;

    /**
     * @brief Check if stop requested and emit cancelled if true
     * @return True if stop requested
     */
    [[nodiscard]] bool check_stop();

    /**
     * @brief Report progress to UI thread
     * @param current Current progress value
     * @param total Total progress value
     * @param message Status message
     */
    void report_progress(int current, int total, const QString& message = {});

private:
    /**
     * @brief QThread run implementation
     */
    void run() override;

    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_is_running{false};
};
