// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_async_tool_runner.cpp
/// @brief Unit tests for AiAsyncToolRunner: the worker-thread wrapper that keeps
/// the GUI thread live while a blocking AI tool handler runs (P10-04).

#include "sak/ai/ai_async_tool_runner.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <stdexcept>

class TestAiAsyncToolRunner : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // A blocking job runs off the owning thread and its result is delivered back
    // via finished() on the owning thread.
    void resultIsDeliveredOnOwningThread() {
        sak::ai::AiAsyncToolRunner runner;
        const Qt::HANDLE owning = QThread::currentThreadId();
        std::atomic<Qt::HANDLE> work_thread{owning};

        QSignalSpy spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        const bool started = runner.start([&work_thread]() {
            work_thread = QThread::currentThreadId();
            QThread::msleep(60);
            return QJsonObject{{QStringLiteral("success"), true}, {QStringLiteral("value"), 42}};
        });
        QVERIFY(started);
        QVERIFY(runner.isRunning());

        // QTRY_COMPARE, not spy.wait(). QSignalSpy::wait() records origCount on entry
        // and only reports emissions that land after it, so a job that completed before
        // the main thread got here would leave wait() blocking for a second finished()
        // that never comes -- a false failure on working code. QTRY_COMPARE returns at
        // once when the signal already arrived and still polls the same 5s when it has not.
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!runner.isRunning());
        QVERIFY(work_thread.load() != owning);  // ran off the owning thread

        const QJsonObject result = spy.at(0).at(0).toJsonObject();
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("value")).toInt(), 42);
    }

    // start() does not block the caller: it returns promptly even though the work
    // sleeps. This is the property that keeps the GUI thread responsive.
    void startDoesNotBlockCaller() {
        sak::ai::AiAsyncToolRunner runner;
        QElapsedTimer timer;
        timer.start();
        QSignalSpy spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        QVERIFY(runner.start([]() {
            QThread::msleep(300);
            return QJsonObject{};
        }));
        QVERIFY(timer.elapsed() < 200);  // returned well before the 300ms work
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    }

    // Only one job at a time.
    void secondStartWhileRunningIsRejected() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        QVERIFY(runner.start([]() {
            QThread::msleep(80);
            return QJsonObject{{QStringLiteral("first"), true}};
        }));
        QVERIFY(!runner.start([]() { return QJsonObject{{QStringLiteral("second"), true}}; }));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toJsonObject().value(QStringLiteral("first")).toBool());
    }

    // detach() suppresses finished() for the in-flight job (cancellation), and a
    // new job may start once the detached one has drained.
    void detachSuppressesFinishedAndAllowsRestart() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        QVERIFY(runner.start([]() {
            QThread::msleep(120);
            return QJsonObject{{QStringLiteral("stale"), true}};
        }));
        runner.detach();

        // Give the detached job time to finish; finished() must NOT fire. The count is
        // asserted on BOTH sides of the window: !wait() alone is also false when the
        // signal arrived before the wait was entered, so it cannot carry this claim.
        QCOMPARE(spy.count(), 0);
        QVERIFY(!spy.wait(1000));
        QCOMPARE(spy.count(), 0);
        QVERIFY(!runner.isRunning());

        // A fresh job now runs and delivers normally. It is trivial work, so it can
        // complete before the main thread reaches the wait.
        QVERIFY(runner.start([]() { return QJsonObject{{QStringLiteral("fresh"), true}}; }));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toJsonObject().value(QStringLiteral("fresh")).toBool());
    }

    // R5 p11_gui-5: a detached job keeps executing, so the runner keeps reporting
    // isRunning() and announces its exit with drained() -- the only notification a
    // detached job produces. Callers that gate on isRunning() need it to learn when the
    // abandoned work actually stopped; without it they would wait forever.
    void detachedJobKeepsRunningAndAnnouncesDrain() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy finished_spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        QSignalSpy drained_spy(&runner, &sak::ai::AiAsyncToolRunner::drained);
        QSemaphore gate;
        QVERIFY(runner.start([&gate]() {
            gate.acquire();
            return QJsonObject{{QStringLiteral("stale"), true}};
        }));

        runner.detach();
        // Detaching drops the RESULT; it does not stop the work.
        QVERIFY(runner.isRunning());
        QCOMPARE(drained_spy.count(), 0);

        gate.release();
        QTRY_COMPARE_WITH_TIMEOUT(drained_spy.count(), 1, 5000);
        QCOMPARE(drained_spy.count(), 1);
        QCOMPARE(finished_spy.count(), 0);
        QVERIFY(!runner.isRunning());
    }

    // An attached job announces both: finished() with its result, then drained().
    void attachedJobEmitsFinishedThenDrained() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy drained_spy(&runner, &sak::ai::AiAsyncToolRunner::drained);
        QSignalSpy finished_spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        QVERIFY(runner.start([]() { return QJsonObject{{QStringLiteral("done"), true}}; }));

        // Nothing gates this job, so both signals can already be recorded by the time
        // the main thread gets here.
        QTRY_COMPARE_WITH_TIMEOUT(drained_spy.count(), 1, 5000);
        QCOMPARE(drained_spy.count(), 1);
        QCOMPARE(finished_spy.count(), 1);
        QVERIFY(!runner.isRunning());
    }

    // A throwing job must not take the process down. QFutureWatcher::result() rethrows
    // whatever the callable threw, and onWatcherFinished is a slot invoked during Qt event
    // delivery, so before the guard an escaping exception terminated the application. The
    // runner has no way to invent a result, so finished() is correctly NOT emitted -- but
    // drained() must still fire or anything gating on isRunning() waits forever, and the
    // runner has to be reusable afterwards.
    void throwingJobDoesNotTerminateAndStillDrains() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy drained_spy(&runner, &sak::ai::AiAsyncToolRunner::drained);
        QSignalSpy finished_spy(&runner, &sak::ai::AiAsyncToolRunner::finished);

        QVERIFY(runner.start(
            []() -> QJsonObject { throw std::runtime_error("deliberate tool handler failure"); }));

        QTRY_COMPARE_WITH_TIMEOUT(drained_spy.count(), 1, 5000);
        QCOMPARE(finished_spy.count(), 0);
        QVERIFY(!runner.isRunning());

        // Still usable: the failed job must not wedge the one-at-a-time slot.
        QVERIFY(runner.start([]() { return QJsonObject{{QStringLiteral("after"), true}}; }));
        QTRY_COMPARE_WITH_TIMEOUT(finished_spy.count(), 1, 5000);
        QCOMPARE(drained_spy.count(), 2);
        QVERIFY(finished_spy.at(0).at(0).toJsonObject().value(QStringLiteral("after")).toBool());
    }

    // A non-std exception must be caught by the catch-all arm, not escape as terminate().
    void throwingNonStdExceptionAlsoDrains() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy drained_spy(&runner, &sak::ai::AiAsyncToolRunner::drained);
        QSignalSpy finished_spy(&runner, &sak::ai::AiAsyncToolRunner::finished);

        QVERIFY(runner.start([]() -> QJsonObject { throw 42; }));

        QTRY_COMPARE_WITH_TIMEOUT(drained_spy.count(), 1, 5000);
        QCOMPARE(finished_spy.count(), 0);
        QVERIFY(!runner.isRunning());
    }
};

QTEST_MAIN(TestAiAsyncToolRunner)
#include "test_ai_async_tool_runner.moc"
