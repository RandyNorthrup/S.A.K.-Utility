// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_dedicated_threads.cpp
/// @brief DedicatedThreads keeps CPU-bound jobs off Windows' shared thread pool

#include "sak/dedicated_threads.h"

#include "../support/thread_pool_probe.h"

#include <QtTest/QtTest>

#include <atomic>
#include <stdexcept>
#include <thread>

#include <Windows.h>

using sak::DedicatedThreads;

class TestDedicatedThreads : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // A stress test's clock stopped for minutes: its spinning jobs filled the shared thread
    // pool, and the process it started to read the CPU temperature could not return.
    void spinningJobsLeaveTheSharedThreadPoolFree();
    void joinAllWaitsForEveryJob();
    void aJobsExceptionComesBackFromJoinAll();
};

void TestDedicatedThreads::spinningJobsLeaveTheSharedThreadPoolFree() {
    const sak::test::TwoCoreAffinity two_cores;
    // One more spinner than the pool lets run at once: every logical CPU, as a stress test's CPU
    // component starts, plus its memory component.
    const int spinners = static_cast<int>(std::thread::hardware_concurrency()) + 1;
    std::atomic<bool> stop{false};
    std::atomic<int> spinning{0};
    {
        DedicatedThreads threads;
        // Declared after `threads`, so a failed check that leaves this scope stops the spinners
        // before the threads are joined.
        const auto stop_on_exit = qScopeGuard([&stop]() { stop.store(true); });
        for (int i = 0; i < spinners; ++i) {
            threads.launch([&stop, &spinning]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                spinning.fetch_add(1);
                volatile unsigned long long sink = 0;
                while (!stop.load()) {
                    sink = sink + 1;
                }
            });
        }
        QTRY_COMPARE_WITH_TIMEOUT(spinning.load(), spinners, 5000);

        const qint64 latency_ms = sak::test::threadPoolStartLatencyMs(3000);
        stop.store(true);
        threads.joinAll();
        qInfo(
            "shared thread pool started a job %lld ms after it was asked (-1: not within 3000 ms)",
            static_cast<long long>(latency_ms));

        QVERIFY2(latency_ms >= 0,
                 "a thread-pool job did not start while the dedicated threads spun");
        // With the jobs back on the shared pool the probe never starts within its 3000 ms (three
        // of three drill runs, 2026-11-10); with them on threads of their own it starts in 0-100 ms
        // on a workstation. A busy machine is given twenty times that, still short of "never".
        QVERIFY2(latency_ms < 2000, qPrintable(QString::number(latency_ms)));
    }
}

void TestDedicatedThreads::joinAllWaitsForEveryJob() {
    std::atomic<int> finished{0};
    DedicatedThreads threads;
    for (int i = 0; i < 4; ++i) {
        threads.launch([&finished]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            finished.fetch_add(1);
        });
    }
    QCOMPARE(threads.size(), std::size_t{4});
    threads.joinAll();
    QCOMPARE(finished.load(), 4);
}

// std::future::get() handed a job's exception back to the caller; a job on a thread of its own
// must do the same rather than end the process.
void TestDedicatedThreads::aJobsExceptionComesBackFromJoinAll() {
    std::atomic<int> finished{0};
    DedicatedThreads threads;
    threads.launch([]() { throw std::runtime_error("job failed"); });
    threads.launch([&finished]() { finished.fetch_add(1); });
    bool caught = false;
    try {
        threads.joinAll();
    } catch (const std::runtime_error& error) {
        caught = QString::fromUtf8(error.what()) == QStringLiteral("job failed");
    }
    QVERIFY(caught);
    QCOMPARE(finished.load(), 1);
    // Reported once: a second join has nothing left to report.
    threads.joinAll();
}

QTEST_GUILESS_MAIN(TestDedicatedThreads)
#include "test_dedicated_threads.moc"
