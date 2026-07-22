// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_async_tool_runner.cpp
/// @brief Unit tests for AiAsyncToolRunner: the worker-thread wrapper that keeps
/// the GUI thread live while a blocking AI tool handler runs (P10-04).

#include "sak/ai/ai_async_tool_runner.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>

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

        QVERIFY(spy.wait(5000));
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
        runner.start([]() {
            QThread::msleep(300);
            return QJsonObject{};
        });
        QVERIFY(timer.elapsed() < 200);  // returned well before the 300ms work
        QVERIFY(spy.wait(5000));
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
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toJsonObject().value(QStringLiteral("first")).toBool());
    }

    // detach() suppresses finished() for the in-flight job (cancellation), and a
    // new job may start once the detached one has drained.
    void detachSuppressesFinishedAndAllowsRestart() {
        sak::ai::AiAsyncToolRunner runner;
        QSignalSpy spy(&runner, &sak::ai::AiAsyncToolRunner::finished);
        runner.start([]() {
            QThread::msleep(120);
            return QJsonObject{{QStringLiteral("stale"), true}};
        });
        runner.detach();

        // Give the detached job time to finish; finished() must NOT fire.
        QVERIFY(!spy.wait(1000));
        QCOMPARE(spy.count(), 0);
        QVERIFY(!runner.isRunning());

        // A fresh job now runs and delivers normally.
        QVERIFY(runner.start([]() { return QJsonObject{{QStringLiteral("fresh"), true}}; }));
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toJsonObject().value(QStringLiteral("fresh")).toBool());
    }
};

QTEST_MAIN(TestAiAsyncToolRunner)
#include "test_ai_async_tool_runner.moc"
