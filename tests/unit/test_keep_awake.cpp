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
    void guard_construction();
    void guard_nonCopyable();
    void guard_scopeActivation();
    void powerRequest_values();
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
    [[maybe_unused]] const auto cleanup2 = KeepAwake::stop();
}

void TestKeepAwake::startStop_doubleStop() {
    [[maybe_unused]] const auto first_stop = KeepAwake::stop();
    const auto result = KeepAwake::stop();
    QVERIFY(!KeepAwake::isActive());
}

void TestKeepAwake::guard_construction() {
    [[maybe_unused]] const auto cleanup = KeepAwake::stop();
    {
        KeepAwakeGuard guard;
        QVERIFY(guard.isActive());
        QVERIFY(KeepAwake::isActive());
    }
    // Guard destroyed — should be inactive
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

QTEST_MAIN(TestKeepAwake)
#include "test_keep_awake.moc"
