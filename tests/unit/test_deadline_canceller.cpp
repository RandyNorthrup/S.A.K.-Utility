// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_deadline_canceller.cpp
/// @brief Unit tests for sak::DeadlineCanceller (wall-clock deadline / cancel monitor).

#include "sak/deadline_canceller.h"

#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

class TestDeadlineCanceller : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firesAndInvokesCallbackOnceOnTimeout();
    void finishBeforeTimeoutNeverFires();
    void destructorWithoutFinishDoesNotFire();
    void finishAfterTimeoutStaysFired();
    void missingCallbackIsObservable();
    void standardCallbackFailureIsObservable();
    void unknownCallbackFailureIsObservable();
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
        QCOMPARE(canceller.callbackFailure(), sak::DeadlineCanceller::CallbackFailure::None);
    }
    QCOMPARE(calls.load(), 1);
}

// A missing cancel target makes a fired deadline unable to interrupt its engine. Surface that
// configuration error through the same no-throw observation seam.
void TestDeadlineCanceller::missingCallbackIsObservable() {
    sak::DeadlineCanceller canceller({}, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    canceller.finish();

    QVERIFY(canceller.fired());
    QCOMPARE(canceller.callbackFailure(), sak::DeadlineCanceller::CallbackFailure::MissingCallback);
}

// A deadline callback is an external boundary. A standard exception must not terminate the
// monitor thread or disappear: finish() joins it, then callbackFailure() reports the exact class.
void TestDeadlineCanceller::standardCallbackFailureIsObservable() {
    sak::DeadlineCanceller canceller([]() { throw std::runtime_error("cancel failed"); }, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    canceller.finish();

    QVERIFY(canceller.fired());
    QCOMPARE(canceller.callbackFailure(),
             sak::DeadlineCanceller::CallbackFailure::StandardException);
}

// The catch-all is required, not a warning suppression: C++ callbacks can throw non-standard
// values. That failure class must also remain observable instead of escaping to std::terminate.
void TestDeadlineCanceller::unknownCallbackFailureIsObservable() {
    sak::DeadlineCanceller canceller([]() { throw 7; }, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    canceller.finish();

    QVERIFY(canceller.fired());
    QCOMPARE(canceller.callbackFailure(),
             sak::DeadlineCanceller::CallbackFailure::UnknownException);
}

// finish() called well before the deadline claims completion: fired() stays false and the callback
// never runs -- a run that genuinely completed is never reported as a timeout.
void TestDeadlineCanceller::finishBeforeTimeoutNeverFires() {
    std::atomic<int> calls{0};
    {
        // 60 ms deadline, finish() immediately: the deadline instant then elapses WHILE the
        // canceller is still alive, so a green result requires finish() to have CLAIMED the outcome
        // (compare_exchange Running -> Done) rather than merely being read before the deadline
        // could arrive. With a 100 s timeout the assertion was true by construction: fired() is
        // false for BOTH Running and Done, so a no-op finish() -- no CAS, no request_stop --
        // passed, and so did a monitor that stores Fired after its loop exits on a stop.
        sak::DeadlineCanceller canceller([&calls]() { ++calls; }, 60);
        canceller.finish();
        // Outlive the deadline: kPollMs is 50, so a monitor NOT claimed by finish() reaches its
        // deadline poll by ~110 ms and fires.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        QVERIFY(!canceller.fired());
        QCOMPARE(calls.load(), 0);
    }
    QCOMPARE(calls.load(), 0);
}

// Destroying the canceller (long timeout, no finish) stops+joins the monitor without firing.
void TestDeadlineCanceller::destructorWithoutFinishDoesNotFire() {
    std::atomic<int> calls{0};
    {
        // 300 ms deadline (not 100 s): the deadline instant elapses during the wait below, so the
        // monitor must actually have been stopped AND JOINED by destruction -- the jthread is
        // declared LAST, so it is stopped+joined FIRST, before m_state and before the caller's
        // engine/stop_source die. A 100 s timeout made this vacuous: read microseconds after the
        // scope closed and 100 s before anything could fire, it read calls == 0 even for a monitor
        // that was detached instead of joined, or one whose loop ignores st.stop_requested().
        sak::DeadlineCanceller canceller([&calls]() { ++calls; }, 300);
        // Immediately leaves scope -> dtor requests stop and joins.
    }
    // A monitor still alive here reaches its 300 ms deadline and increments `calls`, which is still
    // on this frame. This also turns the stop-token-ignoring mutant from a 100 s blocking
    // destructor (a ctest hang) into a deterministic 300 ms red.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
