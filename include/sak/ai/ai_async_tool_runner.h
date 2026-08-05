// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QFutureWatcher>
#include <QJsonObject>
#include <QObject>

#include <functional>

namespace sak::ai {

/// @brief Runs one blocking tool-dispatch call on a worker thread and delivers
/// its JSON result back on the owning (GUI) thread.
///
/// The AI panel dispatches built-in tools (download, package, offline, provider
/// gateway) whose handlers block for minutes. Running them inline froze the GUI
/// thread, so repaints and the Stop action could not run (P10-04). This runner
/// executes the work on the global thread pool and emits finished() on the
/// thread that owns the runner, so only the result marshals back while the GUI
/// event loop stays live during the blocking call.
///
/// One job at a time. Cancellation is cooperative: the caller signals the
/// underlying operation to stop (e.g. worker->cancel()) and calls detach() so
/// the late result is discarded; the pool task still runs to completion in the
/// background before the slot frees.
class AiAsyncToolRunner : public QObject {
    Q_OBJECT

public:
    using Work = std::function<QJsonObject()>;

    explicit AiAsyncToolRunner(QObject* parent = nullptr);
    ~AiAsyncToolRunner() override;

    /// Starts @p work on a pool thread. Returns false if a job is already
    /// running (one at a time). finished() is emitted on the owning thread when
    /// the work returns, unless detach() was called in the meantime.
    bool start(Work work);

    [[nodiscard]] bool isRunning() const { return m_running; }

    /// Abandons the in-flight job: its result will be dropped and finished()
    /// will not be emitted for it. The pool task keeps running to completion in
    /// the background; a new start() is allowed once it finishes. isRunning()
    /// stays true until it does, because the abandoned work is still mutating
    /// the machine -- callers must keep treating the runner as busy and wait for
    /// drained() rather than assuming the job stopped.
    void detach();

Q_SIGNALS:
    /// Result of an ATTACHED job, delivered on the owning thread.
    void finished(const QJsonObject& result);

    /// The pool task left the pool, whether it was attached or detached. This is
    /// the only notification a detached job produces, so it is what lets a caller
    /// that gated on isRunning() (a cancelled-but-still-running mutation) learn
    /// that the runner finally went idle. Emitted after finished() when attached.
    void drained();

private:
    void onWatcherFinished();

    QFutureWatcher<QJsonObject> m_watcher;
    bool m_running{false};
    bool m_attached{true};
};

}  // namespace sak::ai
