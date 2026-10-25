// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/error_codes.h"

#include <QThread>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>

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
    void requestStop() noexcept;

    /**
     * @brief Check if stop has been requested
     * @return True if cancellation requested
     */
    [[nodiscard]] bool stopRequested() const noexcept;

    /**
     * @brief Check if worker is currently running
     * @return True if worker is executing
     */
    [[nodiscard]] bool isExecuting() const noexcept;

    /**
     * @brief Stop and destroy a worker its owner no longer needs, without ever terminating it.
     *
     * A worker that is not running is destroyed at once. A running one is asked to stop and given
     * @p stop_wait_ms. If it is still running after that, the overrun is reported and the
     * worker -- which owns everything its execute() uses -- goes to sak::ThreadCustody, which
     * destroys it on the application thread once it finishes. Its signals are disconnected first,
     * so a retained worker can no longer reach the owner that gave it up. Call it on the thread the
     * worker object belongs to. Owners hold workers in sak::WorkerPtr, which routes every
     * destruction through here.
     *
     * @return true when the worker was destroyed, false when it was retained in custody.
     */
    static bool release(std::unique_ptr<WorkerBase> worker, int stop_wait_ms) noexcept;

    /// release() with the standard stop deadline, sak::kTimeoutThreadShutdownMs.
    static bool release(std::unique_ptr<WorkerBase> worker) noexcept;

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
     * This method runs in the worker thread. Use checkStop() to
     * check for cancellation requests and emit progress() for updates.
     *
     * @return Expected containing success or error code
     */
    virtual auto execute() -> std::expected<void, sak::error_code> = 0;

    /**
     * @brief Stop the worker and join its thread. Idempotent (a no-op once the
     *        thread has finished).
     *
     * MUST be called from the destructor of every derived worker, BEFORE that
     * class's own members are destroyed. ~WorkerBase runs as the base subobject,
     * i.e. AFTER the derived members are gone, so relying on it alone leaves a
     * still-running execute() touching freed derived state (use-after-free). This
     * helper lets each derived dtor join the thread while its state is still alive.
     *
     * The join is cooperative and bounded by kTimeoutThreadShutdownMs, and never
     * terminates the thread. A destructor cannot keep its own object alive, so an
     * owner whose worker may outlive that bound releases it through release()
     * (sak::WorkerPtr does so on its own); a running worker that is destroyed
     * directly past the bound is a programming error and stops the process.
     */
    void stopAndJoin() noexcept;

    /**
     * @brief Check if stop requested and emit cancelled if true
     * @return True if stop requested
     */
    [[nodiscard]] bool checkStop() const;

    /**
     * @brief Report progress to UI thread
     * @param current Current progress value
     * @param total Total progress value
     * @param message Status message
     */
    void reportProgress(int current, int total, const QString& message = {});

private:
    /**
     * @brief QThread run implementation
     *
     * final: a derived worker overrides execute(), never run(). Sealing run()
     * stops a subclass from silently bypassing the cancellation, state-tracking,
     * error-handling, and terminal-signal machinery this base guarantees.
     */
    void run() final;

    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_is_running{false};
};

namespace sak {

/// unique_ptr deleter that destroys a worker through WorkerBase::release(): a worker still
/// running past its stop deadline goes to custody instead of being destroyed underneath itself.
struct WorkerReleaser {
    void operator()(WorkerBase* worker) const noexcept {
        WorkerBase::release(std::unique_ptr<WorkerBase>(worker));
    }
};

/// Owning pointer for a worker thread. Resetting it, replacing it or destroying its owner never
/// terminates the thread and never waits past the worker's stop deadline.
template <typename Worker>
using WorkerPtr = std::unique_ptr<Worker, WorkerReleaser>;

/// make_unique for WorkerPtr.
template <typename Worker, typename... Args>
[[nodiscard]] WorkerPtr<Worker> makeWorker(Args&&... args) {
    static_assert(std::is_base_of_v<WorkerBase, Worker>, "makeWorker owns WorkerBase workers.");
    return WorkerPtr<Worker>(new Worker(std::forward<Args>(args)...));
}

/// Releases @p worker now through WorkerBase::release() with an owner-chosen stop deadline,
/// leaving @p worker empty. Returns false when the worker was retained in custody.
template <typename Worker>
bool releaseWorker(WorkerPtr<Worker>& worker, int stop_wait_ms) noexcept {
    return WorkerBase::release(std::unique_ptr<WorkerBase>(worker.release()), stop_wait_ms);
}

}  // namespace sak

// -- Compile-Time Invariants --------------------------------------------------

/// WorkerBase must inherit QThread (which inherits QObject).
static_assert(std::is_base_of_v<QThread, WorkerBase>, "WorkerBase must inherit QThread.");

/// WorkerBase must be abstract (pure virtual execute()).
static_assert(std::is_abstract_v<WorkerBase>, "WorkerBase must be abstract.");

/// WorkerBase must not be copyable or movable.
static_assert(!std::is_copy_constructible_v<WorkerBase>,
              "WorkerBase must not be copy-constructible.");
static_assert(!std::is_move_constructible_v<WorkerBase>,
              "WorkerBase must not be move-constructible.");
