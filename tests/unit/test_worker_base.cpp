// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_worker_base.cpp
/// @brief Unit tests for WorkerBase thread lifecycle, exception safety, and cancellation

#include "sak/error_codes.h"
#include "sak/worker_base.h"

#include <QtTest/QtTest>

#include <atomic>
#include <stdexcept>

// Comfortably above the ~10ms cooperative shutdown and far below the 15s wait that precedes
// terminate(), so the two paths are unambiguously separated.
constexpr qint64 kCooperativeShutdownBudgetMs = 5000;

// ============================================================================
// Test Worker Implementations
// ============================================================================

/// @brief Worker that completes successfully, recording lifecycle facts only observable from
///        INSIDE execute(). isExecuting() reads m_is_running, and every call site outside the
///        worker thread samples it at a moment where m_is_running and QThread::isRunning()
///        necessarily agree -- so nothing could tell the two apart.
class SuccessWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

    // Written on the worker thread; read only after wait() has joined.
    bool started_seen_before_execute = false;
    bool executing_inside_execute = false;
    std::atomic<bool> started_seen{false};

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        started_seen_before_execute = started_seen.load(std::memory_order_acquire);
        executing_inside_execute = isExecuting();
        return {};
    }
};

/// @brief Worker that returns an error. The code is deliberately NOT internal_error: both catch
///        blocks in run() hardcode internal_error, so a worker returning it cannot show that the
///        error branch consults result.error() at all.
class FailWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        return std::unexpected(sak::error_code::registry_access_denied);
    }
};

/// @brief Worker that throws std::runtime_error
class ThrowWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        throw std::runtime_error("deliberate test exception");
    }
};

/// @brief Worker that throws std::bad_alloc (OOM)
class OOMWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override { throw std::bad_alloc(); }
};

/// @brief Worker that throws a non-std exception (int)
class UnknownThrowWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override { throw 42; }
};

/// @brief Worker that sleeps until cancelled
class CancellableWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

    /// Qt's own interruption flag, sampled ON THE WORKER THREAD at the moment the stop is
    /// observed. It has to be read there: QThread::isInterruptionRequested() answers false once
    /// the thread has finished, so a post-wait() check cannot see it.
    std::atomic<bool> interruption_seen_on_stop{false};

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        while (!checkStop()) {
            QThread::msleep(10);
        }
        interruption_seen_on_stop.store(isInterruptionRequested(), std::memory_order_release);
        return {};
    }
};

/// @brief Worker that drives reportProgress's refusal guard from BOTH of its arms, plus the two
///        well-formed boundary calls that must still be emitted.
class GuardProbeWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        reportProgress(0, 0, QStringLiteral("zero total"));       // total <= 0 arm
        reportProgress(0, -1, QStringLiteral("negative total"));  // total <= 0 arm
        reportProgress(-1, 100, QStringLiteral("negative cur"));  // current < 0 arm
        reportProgress(-1, 0, QStringLiteral("both bad"));        // both arms
        reportProgress(0, 1, QStringLiteral("ok low"));           // accepted boundary
        reportProgress(1, 1, QStringLiteral("ok high"));          // accepted boundary
        return {};
    }
};

/// @brief Worker that reports progress
class ProgressWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        for (int i = 0; i <= 100; i += 25) {
            reportProgress(i, 100, QString("Step %1").arg(i));
        }
        return {};
    }
};

// ============================================================================
// Test Class
// ============================================================================

class WorkerBaseTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void successfulExecution();
    void failedExecution();
    void exceptionSafety_stdException();
    void exceptionSafety_badAlloc();
    void exceptionSafety_unknownException();
    void cancellation();
    void progressReporting();
    void progressGuardRefusesNonsenseValues();
    void isExecutingFlag();
    void destructorStopsThread();
};

void WorkerBaseTests::successfulExecution() {
    SuccessWorker worker;
    QSignalSpy started_spy(&worker, &WorkerBase::started);
    QSignalSpy finished_spy(&worker, &WorkerBase::finished);
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);
    // Direct connection: the flag must be set on the WORKER thread, before execute() reads it.
    QObject::connect(
        &worker,
        &WorkerBase::started,
        &worker,
        [&worker]() { worker.started_seen.store(true, std::memory_order_release); },
        Qt::DirectConnection);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(started_spy.count(), 1);
    QCOMPARE(finished_spy.count(), 1);
    QCOMPARE(failed_spy.count(), 0);
    // Counts carry no ORDER, so started() need not have preceded the work or the terminal signal.
    // The worker records what it saw from inside execute(), which is the only vantage point where
    // the ordering is observable at all.
    QVERIFY2(worker.started_seen_before_execute, "started() must be emitted before execute() runs");
    // ... and isExecuting() must be TRUE across execute(). It reads m_is_running, which every
    // out-of-thread call site samples only where it necessarily agrees with QThread::isRunning();
    // this is the sampling point that separates the two.
    QVERIFY2(worker.executing_inside_execute, "isExecuting() must be true while execute() runs");
    // After the join, both are false again -- and here m_is_running is cleared BEFORE the terminal
    // signal, so this also pins that the epilogue ran.
    QVERIFY(!worker.isExecuting());
    QVERIFY(!worker.isRunning());
}

void WorkerBaseTests::failedExecution() {
    FailWorker worker;
    QSignalSpy finished_spy(&worker, &WorkerBase::finished);
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(finished_spy.count(), 0);
    QCOMPARE(failed_spy.count(), 1);

    const auto args = failed_spy.first();
    // The worker's OWN error code must be propagated. While FailWorker returned internal_error --
    // the same code both catch blocks hardcode -- the error branch could have stopped consulting
    // result.error() entirely and every fixture in the file would still have reported 902.
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::registry_access_denied));
    // failed() carries (code, message) and the message arg was read by no assertion on this path,
    // though it is what the UI shows the user. It is rendered by sak::to_string, so it is exact.
    QCOMPARE(args[1].toString(),
             QString::fromStdString(
                 std::string(sak::to_string(sak::error_code::registry_access_denied))));
    QVERIFY2(!args[1].toString().isEmpty(), "the failure message must not be blank");
}

void WorkerBaseTests::exceptionSafety_stdException() {
    ThrowWorker worker;
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    // Must NOT crash -- exception caught, failed() emitted
    QCOMPARE(failed_spy.count(), 1);

    const auto args = failed_spy.first();
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::internal_error));
    // Fully determined: the catch block builds "Unhandled exception: %1" around what(), and
    // ThrowWorker's what() is a literal. contains() proved only that what() landed somewhere --
    // the prefix, its punctuation, and the absence of trailing noise were all unconstrained.
    QCOMPARE(args[1].toString(), QStringLiteral("Unhandled exception: deliberate test exception"));
    // The catch block also clears m_is_running BEFORE emitting. That store is sticky: drop it and
    // isExecuting() reports true forever after the thread has died, so any UI bound to it keeps a
    // spinner or a disabled Cancel button up for the life of the worker. No test sampled the flag
    // after an exception -- every isExecuting() call site in the file is on a worker that cannot
    // throw.
    QVERIFY2(!worker.isExecuting(), "isExecuting() must be false after a caught exception");
}

void WorkerBaseTests::exceptionSafety_badAlloc() {
    OOMWorker worker;
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(failed_spy.count(), 1);
    const auto args = failed_spy.first();
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::internal_error));
    // "exception" also appears in the catch-all message ("Unhandled unknown
    // exception"), so pin the std::exception path instead: bad_alloc must be
    // reported through what(), not fall through to the catch-all.
    QVERIFY(args[1].toString().startsWith(QStringLiteral("Unhandled exception:")));
}

void WorkerBaseTests::exceptionSafety_unknownException() {
    UnknownThrowWorker worker;
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(failed_spy.count(), 1);
    const auto args = failed_spy.first();
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::internal_error));
    QCOMPARE(args[1].toString(), QStringLiteral("Unhandled unknown exception"));
    // The catch-all's own m_is_running store, at a different production line from the
    // std::exception one and with the same consequence: a worker killed by a non-std throw would
    // report isExecuting() == true forever. The long comment on that block documents it as THE
    // fail-closed path -- "the run is marked not-running, the cause is logged, and the caller is
    // told the operation failed" -- and only the third of those three claims was asserted.
    QVERIFY2(!worker.isExecuting(), "isExecuting() must be false after an unknown exception");
}

void WorkerBaseTests::cancellation() {
    CancellableWorker worker;
    QSignalSpy cancelled_spy(&worker, &WorkerBase::cancelled);
    QSignalSpy finished_spy(&worker, &WorkerBase::finished);

    worker.start();
    QThread::msleep(50);  // Let it start running

    QVERIFY(worker.isExecuting());
    worker.requestStop();
    QVERIFY(worker.wait(5000));

    QCOMPARE(cancelled_spy.count(), 1);
    QCOMPARE(finished_spy.count(), 0);
    QVERIFY(!worker.isExecuting());
    // requestStop() does TWO things: it stores the atomic AND calls QThread::requestInterruption().
    // cancelled() fires off the atomic alone, so the second line was observable nowhere -- a
    // repo-wide grep for isInterruptionRequested returns zero hits, meaning nothing would notice
    // it disappearing, even though it is the Qt-standard cancellation channel a derived worker
    // running an event loop would rely on.
    QVERIFY2(worker.interruption_seen_on_stop.load(std::memory_order_acquire),
             "requestStop() must also raise Qt's own interruption flag");
    // The public accessor that carries the same fact was likewise called by no assertion here.
    QVERIFY(worker.stopRequested());
}

void WorkerBaseTests::progressReporting() {
    ProgressWorker worker;
    QSignalSpy progress_spy(&worker, &WorkerBase::progress);

    worker.start();
    QVERIFY(worker.wait(5000));

    // progress() carries (current, total, message) and only `current` was read, only at the two
    // endpoints -- so `total` and `message` were entirely unconstrained and emissions 1..3 were
    // held by the count alone. All three args of all five emissions are exact and knowable.
    QCOMPARE(progress_spy.count(), 5);
    for (int i = 0; i < progress_spy.count(); ++i) {
        const int expected_current = i * 25;
        QCOMPARE(progress_spy[i][0].toInt(), expected_current);
        QCOMPARE(progress_spy[i][1].toInt(), 100);
        QCOMPARE(progress_spy[i][2].toString(), QStringLiteral("Step %1").arg(expected_current));
    }
}

void WorkerBaseTests::progressGuardRefusesNonsenseValues() {
    // reportProgress refuses on TWO arms, `total <= 0 || current < 0`, and ProgressWorker only
    // ever feeds (0..100, 100) -- so neither arm was ever reached and the whole guard could be
    // deleted with the suite green. It is the only thing stopping a division by zero or a
    // negative percentage in every progress-bar consumer downstream.
    GuardProbeWorker worker;
    QSignalSpy progress_spy(&worker, &WorkerBase::progress);

    worker.start();
    QVERIFY(worker.wait(5000));

    // Of the six calls the worker makes, only the two well-formed ones may be emitted.
    QCOMPARE(progress_spy.count(), 2);
    QCOMPARE(progress_spy[0][0].toInt(), 0);
    QCOMPARE(progress_spy[0][1].toInt(), 1);
    QCOMPARE(progress_spy[1][0].toInt(), 1);
    QCOMPARE(progress_spy[1][1].toInt(), 1);
}

void WorkerBaseTests::isExecutingFlag() {
    CancellableWorker worker;
    QVERIFY(!worker.isExecuting());

    worker.start();
    QThread::msleep(50);
    QVERIFY(worker.isExecuting());

    worker.requestStop();
    QVERIFY(worker.wait(5000));
    QVERIFY(!worker.isExecuting());
}

void WorkerBaseTests::destructorStopsThread() {
    // The destructor must stop the worker COOPERATIVELY: ~WorkerBase -> stopAndJoin() ->
    // requestStop() -> checkStop() -> execute() returns -> run() emits cancelled() -> wait()
    // joins. That is what is pinned here, not merely "we got here".
    //
    // The old comment claimed a lost requestStop() would fall through to terminate() and then
    // std::abort(), so reaching the next line was the assertion. That was FACTUALLY WRONG, and
    // the wrongness was the whole hole: std::abort() is reached only if the post-terminate join
    // ALSO fails, and on Windows terminate() calls TerminateThread, which does kill a thread
    // sitting in msleep(10) -- so the 5s re-join succeeds and stopAndJoin() returns normally. The
    // mutation the comment named as covered actually PASSED; it merely took the full 15s shutdown
    // timeout and logged an error nobody reads, and this test has no ctest TIMEOUT property, so
    // the extra 15s was invisible to the gate. QVERIFY(true) cannot tell "stopped in 10ms" from
    // "force-terminated after 15s".
    //
    // This is also the only test in the file that reaches stopAndJoin()'s cooperative arm at all:
    // every other worker is explicitly requestStop()ed or wait()ed to completion first, so their
    // destructors take the not-running early-out.
    std::atomic<bool> cancelled_seen{false};
    QElapsedTimer timer;
    timer.start();
    {
        CancellableWorker worker;
        QObject::connect(
            &worker,
            &WorkerBase::cancelled,
            &worker,
            [&cancelled_seen]() { cancelled_seen.store(true, std::memory_order_release); },
            Qt::DirectConnection);
        worker.start();
        QThread::msleep(50);
        QVERIFY(worker.isRunning());
    }
    // run() reached its own cancellation epilogue: the thread was not force-terminated.
    QVERIFY2(cancelled_seen.load(std::memory_order_acquire),
             "the destructor must stop the worker cooperatively, not by terminate()");
    // ... and promptly, rather than via the 15s wait + terminate() arm.
    QVERIFY2(timer.elapsed() < kCooperativeShutdownBudgetMs,
             qPrintable(QStringLiteral("destructor took %1ms; the cooperative path is ~10ms and "
                                       "the terminate() fallback is 15000ms")
                            .arg(timer.elapsed())));
}

QTEST_MAIN(WorkerBaseTests)
#include "test_worker_base.moc"
