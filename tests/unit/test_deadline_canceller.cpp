// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_deadline_canceller.cpp
/// @brief Unit tests for sak::DeadlineCanceller (wall-clock deadline / cancel monitor).

#include "sak/deadline_canceller.h"

#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <thread>

class TestDeadlineCanceller : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firesAndInvokesCallbackOnceOnTimeout();
    void finishBeforeTimeoutNeverFires();
    void destructorWithoutFinishDoesNotFire();
    void finishAfterTimeoutStaysFired();
};

// A short timeout with a longer wait: the monitor must fire, invoke the callback exactly once, and
// report fired()==true even though finish() is called afterward (the deadline won the race).
void TestDeadlineCanceller::firesAndInvokesCallbackOnceOnTimeout() {
    std::atomic<int> calls{0};
    {
        sak::DeadlineCanceller canceller([&calls]() { ++calls; }, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        canceller.finish();
        QVERIFY(canceller.fired());
    }
    QCOMPARE(calls.load(), 1);
}

// finish() called well before the deadline claims completion: fired() stays false and the callback
// never runs -- a run that genuinely completed is never reported as a timeout.
void TestDeadlineCanceller::finishBeforeTimeoutNeverFires() {
    std::atomic<int> calls{0};
    {
        sak::DeadlineCanceller canceller([&calls]() { ++calls; }, 100'000);
        canceller.finish();
        QVERIFY(!canceller.fired());
    }
    QCOMPARE(calls.load(), 0);
}

// Destroying the canceller (long timeout, no finish) stops+joins the monitor without firing.
void TestDeadlineCanceller::destructorWithoutFinishDoesNotFire() {
    std::atomic<int> calls{0};
    {
        sak::DeadlineCanceller canceller([&calls]() { ++calls; }, 100'000);
        // Immediately leaves scope -> dtor requests stop and joins.
    }
    QCOMPARE(calls.load(), 0);
}

// Once the deadline has fired, a later finish() cannot flip the outcome back to "completed".
void TestDeadlineCanceller::finishAfterTimeoutStaysFired() {
    std::atomic<int> calls{0};
    sak::DeadlineCanceller canceller([&calls]() { ++calls; }, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    QVERIFY(canceller.fired());
    canceller.finish();
    QVERIFY(canceller.fired());  // still a timeout
    QCOMPARE(calls.load(), 1);
}

QTEST_MAIN(TestDeadlineCanceller)
#include "test_deadline_canceller.moc"
