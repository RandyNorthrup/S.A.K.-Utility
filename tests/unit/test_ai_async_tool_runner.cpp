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
        const Qt::HANDLE owning = QThread::currentThreadId();
        std::atomic<Qt::HANDLE> work_thread{owning};
        // Declared before the runner so it outlives every emission it records.
        std::atomic<Qt::HANDLE> delivery_thread{nullptr};
        sak::ai::AiAsyncToolRunner runner;

        QSignalSpy spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        // A context-free connect is a DIRECT connection, so this lambda runs on whatever thread
        // emitted -- the only way to observe the DELIVERY thread this test is named for. A
        // receiver-bound connection cannot carry the claim, because Qt would re-queue a
        // pool-thread emission onto the main thread and hide it.
        QObject::connect(&runner, &sak::ai::AiAsyncToolRunner::finished, [&delivery_thread]() {
            delivery_thread = QThread::currentThreadId();
        });
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
        // The WORK ran off-thread; the RESULT came back ON the owning thread. Only the second
        // half is the contract this test is named for -- the panel's finished() slot touches
        // GUI state, so an emission straight from the pool task would be a live data race --
        // and nothing here observed it.
        QVERIFY(delivery_thread.load() == owning);

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

        // start()'s single false return carries TWO guards (already-running, empty callable).
        // The rejection above exercises only the first; isolate the second on an IDLE runner,
        // where already-running cannot be what refused it. Without this an empty std::function
        // reaches the pool and the "started" job yields only a swallowed bad_function_call:
        // drained() with no result and no reported failure.
        QVERIFY(!runner.isRunning());
        QVERIFY(!runner.start(sak::ai::AiAsyncToolRunner::Work{}));
        QVERIFY(!runner.isRunning());
        QCOMPARE(spy.count(), 1);
    }

    // detach() suppresses finished() for the in-flight job (cancellation), and a
    // new job may start once the detached one has drained.
    void detachSuppressesFinishedAndAllowsRestart() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        const sak::ai::AiAsyncToolRunner::CancelToken token = runner.cancelToken();
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
        // Still raised from the detach: draining does not lower the token, only the next
        // start() does. Asserting it here is what keeps the check after start() honest.
        QVERIFY(token->load());

        // A fresh job now runs and delivers normally. It is trivial work, so it can
        // complete before the main thread reaches the wait.
        QVERIFY(runner.start([]() { return QJsonObject{{QStringLiteral("fresh"), true}}; }));
        // The token is per-job: start() must lower it, or the fresh job inherits the cancelled
        // predecessor's raise and cooperative Work returns instantly.
        QVERIFY(!token->load());
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
        const sak::ai::AiAsyncToolRunner::CancelToken token = runner.cancelToken();
        QVERIFY(token != nullptr);
        QVERIFY(runner.start([&gate]() {
            gate.acquire();
            return QJsonObject{{QStringLiteral("stale"), true}};
        }));
        QVERIFY(!token->load());  // start() lowered the per-job token

        runner.detach();
        // Detaching drops the RESULT; it does not stop the work -- but it MUST also raise the
        // cooperative cancellation token. QtConcurrent::run has no built-in cancellation, so
        // that token is the only lever over the abandoned task; a detach() that merely hid the
        // result would leave cooperative Work spinning to completion.
        QVERIFY(token->load());
        QVERIFY(runner.isRunning());
        // A start() refused by the one-at-a-time guard must leave the in-flight job alone.
        // start() re-arms m_attached and lowers the per-job token only AFTER its guard
        // (ai_async_tool_runner.cpp:38-46). An implementation that did either before the
        // guard would silently un-cancel this detached job -- cooperative Work would resume
        // to completion -- or re-attach it, so the stale result it was detached from would
        // be delivered as live. The panel refuses that way on every busy tool call
        // (ai_assistant_panel.cpp:5997-6005). Nothing else in this file calls start() while a
        // job is in flight with the token raised, so both mutants stay green without this.
        QVERIFY(!runner.start([]() { return QJsonObject{{QStringLiteral("intruder"), true}}; }));
        QVERIFY(token->load());
        QVERIFY(runner.isRunning());
        QCOMPARE(drained_spy.count(), 0);
        QCOMPARE(drained_spy.count(), 0);

        gate.release();
        QTRY_COMPARE_WITH_TIMEOUT(drained_spy.count(), 1, 5000);
        QCOMPARE(drained_spy.count(), 1);
        QCOMPARE(finished_spy.count(), 0);
        QVERIFY(!runner.isRunning());
    }

    // An attached job announces both: finished() with its result, then drained().
    void attachedJobEmitsFinishedThenDrained() {
        // Declared before the runner so it outlives every emission it records.
        QString order;
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy drained_spy(&runner, &sak::ai::AiAsyncToolRunner::drained);
        QSignalSpy finished_spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        // Two counts of 1 cannot carry the ORDER this test is named for: both signals are
        // emitted inside one onWatcherFinished() call, so the main thread only ever samples the
        // state after both have fired. Record the sequence from direct connections. drained() is
        // the went-idle notice, so a handler must not see it before the result it belongs to.
        QObject::connect(&runner, &sak::ai::AiAsyncToolRunner::finished, [&order, &runner]() {
            order += QStringLiteral("F");
            // Sampled from INSIDE the emission -- the only place this claim is visible.
            // onWatcherFinished clears m_running BEFORE it emits (ai_async_tool_runner.cpp:59
            // vs :81/:87), and the panel's finished() slot chains straight back into a new
            // start() for the next call of the same tool turn (finishAsyncBuiltInToolCall ->
            // appendToolOutputAndContinue -> dispatchNextToolCall -> dispatchBuiltInToolCall
            // -> startAsyncBuiltInToolCall). A runner that cleared m_running only after both
            // emissions would refuse that chained start, yet every isRunning() read in this
            // file is sampled after the emissions have already returned.
            QVERIFY(!runner.isRunning());
        });
        QObject::connect(&runner, &sak::ai::AiAsyncToolRunner::drained, [&order, &runner]() {
            order += QStringLiteral("D");
            // drained() is the went-idle notice: it must not arrive while still running.
            QVERIFY(!runner.isRunning());
        });
        QVERIFY(runner.start([]() { return QJsonObject{{QStringLiteral("done"), true}}; }));

        // Nothing gates this job, so both signals can already be recorded by the time
        // the main thread gets here.
        QTRY_COMPARE_WITH_TIMEOUT(drained_spy.count(), 1, 5000);
        QCOMPARE(drained_spy.count(), 1);
        QCOMPARE(finished_spy.count(), 1);
        QCOMPARE(order, QStringLiteral("FD"));
        // The attached job's OWN result is delivered, not an empty placeholder.
        QCOMPARE(finished_spy.at(0).at(0).toJsonObject(),
                 QJsonObject({{QStringLiteral("done"), true}}));
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

    // The destructor raises the cancellation token BEFORE it joins, so cooperative Work can
    // return promptly instead of the join blocking for the job's full duration -- the exact GUI
    // freeze this class exists to prevent. Nothing else in this file reads the token, so a
    // destructor that only joined would otherwise stay green.
    void destructorRaisesCancelTokenBeforeJoining() {
        sak::ai::AiAsyncToolRunner::CancelToken token;
        std::atomic<bool> work_saw_cancel{false};
        {
            sak::ai::AiAsyncToolRunner runner;
            token = runner.cancelToken();
            QVERIFY(token != nullptr);
            QVERIFY(!token->load());
            QVERIFY(runner.start([token, &work_saw_cancel]() {
                // Bounded so a runner that never raises the token FAILS instead of hanging.
                QElapsedTimer work_timer;
                work_timer.start();
                while (!token->load() && work_timer.elapsed() < 3000) {
                    QThread::msleep(5);
                }
                work_saw_cancel = token->load();
                return QJsonObject{};
            }));
        }  // ~AiAsyncToolRunner raises the token, then joins the pool task.
        QVERIFY(token->load());
        QVERIFY(work_saw_cancel.load());
    }
};

QTEST_MAIN(TestAiAsyncToolRunner)
#include "test_ai_async_tool_runner.moc"
