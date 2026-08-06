// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_keep_awake.cpp
/// @brief Unit tests for KeepAwake and KeepAwakeGuard

#include "sak/keep_awake.h"

#include <QtTest/QtTest>

using namespace sak;

class TestKeepAwake : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initialState_inactive();
    void startStop_cycle();
    void startStop_doubleStart();
    void startStop_doubleStop();
    void overlapping_refcount();
    void guard_construction();
    void guard_nonCopyable();
    void guard_scopeActivation();
    void powerRequest_values();
    void executionStateForFlags_unionOfFlags();
};

void TestKeepAwake::initialState_inactive() {
    // Ensure clean state
    [[maybe_unused]] const auto stop_result = KeepAwake::stop();
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::startStop_cycle() {
    const auto start_result = KeepAwake::start();
    QVERIFY(start_result.has_value());
    QVERIFY(KeepAwake::isActive());

    const auto stop_result = KeepAwake::stop();
    QVERIFY(stop_result.has_value());
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::startStop_doubleStart() {
    [[maybe_unused]] const auto cleanup = KeepAwake::stop();
    const auto result1 = KeepAwake::start();
    const auto result2 = KeepAwake::start();
    QVERIFY(result1.has_value());
    QVERIFY(KeepAwake::isActive());
    // Two starts require two stops to fully release (reference counted).
    [[maybe_unused]] const auto cleanup2 = KeepAwake::stop();
    [[maybe_unused]] const auto cleanup3 = KeepAwake::stop();
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::startStop_doubleStop() {
    [[maybe_unused]] const auto first_stop = KeepAwake::stop();
    const auto result = KeepAwake::stop();
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::overlapping_refcount() {
    // Regression: overlapping keep-awake guards (e.g. a flash worker and a
    // stress-test worker running at once). The first stop() must NOT drop the
    // request while a second guard is still active.
    [[maybe_unused]] const auto cleanup = KeepAwake::stop();
    QVERIFY(!KeepAwake::isActive());

    const auto worker_a = KeepAwake::start();  // thread A installs
    const auto worker_b = KeepAwake::start();  // thread B overlaps
    QVERIFY(worker_a.has_value());
    QVERIFY(worker_b.has_value());
    QVERIFY(KeepAwake::isActive());

    // Worker A finishes first: still-running worker B must stay awake.
    [[maybe_unused]] const auto stop_a = KeepAwake::stop();
    QVERIFY(KeepAwake::isActive());

    // Worker B finishes: now the system may sleep again.
    [[maybe_unused]] const auto stop_b = KeepAwake::stop();
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::guard_construction() {
    [[maybe_unused]] const auto cleanup = KeepAwake::stop();
    {
        KeepAwakeGuard guard;
        QVERIFY(guard.isActive());
        QVERIFY(KeepAwake::isActive());
    }
    // Guard destroyed -- should be inactive
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::guard_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<KeepAwakeGuard>);
    QVERIFY(!std::is_move_constructible_v<KeepAwakeGuard>);
}

void TestKeepAwake::guard_scopeActivation() {
    [[maybe_unused]] const auto cleanup = KeepAwake::stop();
    QVERIFY(!KeepAwake::isActive());

    {
        KeepAwakeGuard guard(KeepAwake::PowerRequest::Both);
        QVERIFY(guard.isActive());
    }
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::powerRequest_values() {
    QCOMPARE(static_cast<int>(KeepAwake::PowerRequest::System), 0x01);
    QCOMPARE(static_cast<int>(KeepAwake::PowerRequest::Display), 0x02);
    QCOMPARE(static_cast<int>(KeepAwake::PowerRequest::Both), 0x03);
}

void TestKeepAwake::executionStateForFlags_unionOfFlags() {
    // Win32 documented constants (avoid pulling in <windows.h> in the test).
    constexpr unsigned kContinuous = 0x80'00'00'00u;  // ES_CONTINUOUS
    constexpr unsigned kSystem = 0x00'00'00'01u;      // ES_SYSTEM_REQUIRED
    constexpr unsigned kDisplay = 0x00'00'00'02u;     // ES_DISPLAY_REQUIRED

    const auto systemOnly =
        KeepAwake::executionStateForFlags(static_cast<int>(KeepAwake::PowerRequest::System));
    QCOMPARE(systemOnly, kContinuous | kSystem);

    // Regression: a later Display request must add its flag to the union rather
    // than being ignored (the old refcount installed only the first request's
    // flags and dropped every later one).
    const int unioned = static_cast<int>(KeepAwake::PowerRequest::System) |
                        static_cast<int>(KeepAwake::PowerRequest::Display);
    QCOMPARE(KeepAwake::executionStateForFlags(unioned), kContinuous | kSystem | kDisplay);
    QCOMPARE(KeepAwake::executionStateForFlags(static_cast<int>(KeepAwake::PowerRequest::Both)),
             kContinuous | kSystem | kDisplay);
}

QTEST_MAIN(TestKeepAwake)
#include "test_keep_awake.moc"
