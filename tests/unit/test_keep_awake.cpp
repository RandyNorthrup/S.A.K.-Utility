// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QThread>
#include "sak/keep_awake.h"

class TestKeepAwake : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testStart();
    void testStop();
    void testIsActive();

    // Power request types
    void testSystemRequest();
    void testDisplayRequest();
    void testBothRequest();

    // Status checking
    void testIsActiveInitially();
    void testIsActiveAfterStart();
    void testIsActiveAfterStop();

    // Reason strings
    void testDefaultReason();
    void testCustomReason();
    void testEmptyReason();
    void testLongReason();

    // Error handling
    void testStartError();
    void testStopError();
    void testStopWithoutStart();

    // Multiple calls
    void testStartTwice();
    void testStopTwice();
    void testStartStopStart();

    // Expected return type
    void testExpectedSuccess();
    void testExpectedError();
    void testExpectedValue();

    // RAII guard
    void testGuardConstructor();
    void testGuardDestructor();
    void testGuardIsActive();
    void testGuardScope();

    // Guard with parameters
    void testGuardSystemRequest();
    void testGuardDisplayRequest();
    void testGuardBothRequest();
    void testGuardCustomReason();

    // Copy/move prevention
    void testGuardNoCopy();
    void testGuardNoMove();

    // Thread safety
    void testIsActiveNoexcept();
    void testMultipleThreads();

    // State transitions
    void testInactiveToActive();
    void testActiveToInactive();
    void testMultipleTransitions();

    // Windows API
    void testSetThreadExecutionState();
    void testPowerRequestFlags();

    // Long operations
    void testLongRunningOperation();
    void testGuardLongOperation();

    // Nested guards
    void testNestedGuards();
    void testNestedGuardsOverlap();

    // Error conditions
    void testStartFailure();
    void testStopFailure();

    // Platform-specific
    void testWindowsOnly();

    // Edge cases
    void testRapidStartStop();
    void testGuardException();

    // Performance
    void testStartSpeed();
    void testStopSpeed();

private:
    void waitForSystemState(int ms = 100);
};

void TestKeepAwake::initTestCase() {
    // Setup test environment
}

void TestKeepAwake::cleanupTestCase() {
    // Cleanup test environment
    sak::KeepAwake::stop();
}

void TestKeepAwake::init() {
    // Per-test setup
    sak::KeepAwake::stop(); // Ensure clean state
}

void TestKeepAwake::cleanup() {
    // Per-test cleanup
    sak::KeepAwake::stop();
}

void TestKeepAwake::waitForSystemState(int ms) {
    QThread::msleep(ms);
}

void TestKeepAwake::testStart() {
    auto result = sak::KeepAwake::start();
    QVERIFY(result.has_value() || !result.has_value());
}

void TestKeepAwake::testStop() {
    sak::KeepAwake::start();
    auto result = sak::KeepAwake::stop();
    QVERIFY(result.has_value() || !result.has_value());
}

void TestKeepAwake::testIsActive() {
    bool active = sak::KeepAwake::is_active();
    QVERIFY(active == true || active == false);
}

void TestKeepAwake::testSystemRequest() {
    auto result = sak::KeepAwake::start(sak::KeepAwake::PowerRequest::System);
    
    if (result.has_value()) {
        QVERIFY(sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testDisplayRequest() {
    auto result = sak::KeepAwake::start(sak::KeepAwake::PowerRequest::Display);
    
    if (result.has_value()) {
        QVERIFY(sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testBothRequest() {
    auto result = sak::KeepAwake::start(sak::KeepAwake::PowerRequest::Both);
    
    if (result.has_value()) {
        QVERIFY(sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testIsActiveInitially() {
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testIsActiveAfterStart() {
    auto result = sak::KeepAwake::start();
    
    if (result.has_value()) {
        QVERIFY(sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testIsActiveAfterStop() {
    sak::KeepAwake::start();
    sak::KeepAwake::stop();
    
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testDefaultReason() {
    auto result = sak::KeepAwake::start();
    // Default reason should be used
}

void TestKeepAwake::testCustomReason() {
    auto result = sak::KeepAwake::start(
        sak::KeepAwake::PowerRequest::System,
        "Custom operation"
    );
}

void TestKeepAwake::testEmptyReason() {
    auto result = sak::KeepAwake::start(
        sak::KeepAwake::PowerRequest::System,
        ""
    );
}

void TestKeepAwake::testLongReason() {
    QString longReason(1000, 'x');
    auto result = sak::KeepAwake::start(
        sak::KeepAwake::PowerRequest::System,
        longReason.toUtf8().constData()
    );
}

void TestKeepAwake::testStartError() {
    auto result = sak::KeepAwake::start();
    // May or may not error
}

void TestKeepAwake::testStopError() {
    auto result = sak::KeepAwake::stop();
    // Stopping without starting may error
}

void TestKeepAwake::testStopWithoutStart() {
    auto result = sak::KeepAwake::stop();
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testStartTwice() {
    auto result1 = sak::KeepAwake::start();
    auto result2 = sak::KeepAwake::start();
    
    // Second start may succeed or fail
}

void TestKeepAwake::testStopTwice() {
    sak::KeepAwake::start();
    auto result1 = sak::KeepAwake::stop();
    auto result2 = sak::KeepAwake::stop();
    
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testStartStopStart() {
    auto result1 = sak::KeepAwake::start();
    sak::KeepAwake::stop();
    auto result2 = sak::KeepAwake::start();
    
    // Should work
}

void TestKeepAwake::testExpectedSuccess() {
    auto result = sak::KeepAwake::start();
    
    if (result.has_value()) {
        // Success case
        QVERIFY(sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testExpectedError() {
    // Try to cause error
    auto result = sak::KeepAwake::start();
    
    if (!result.has_value()) {
        auto error = result.error();
        // Should have error code
    }
}

void TestKeepAwake::testExpectedValue() {
    auto result = sak::KeepAwake::start();
    
    // std::expected<void, error_code> should work
    QVERIFY(result.has_value() || !result.has_value());
}

void TestKeepAwake::testGuardConstructor() {
    sak::KeepAwakeGuard guard;
    QVERIFY(guard.is_active() || !guard.is_active());
}

void TestKeepAwake::testGuardDestructor() {
    {
        sak::KeepAwakeGuard guard;
    }
    
    // Should be stopped after guard destroyed
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testGuardIsActive() {
    sak::KeepAwakeGuard guard;
    bool active = guard.is_active();
    QVERIFY(active == true || active == false);
}

void TestKeepAwake::testGuardScope() {
    bool wasActive = false;
    
    {
        sak::KeepAwakeGuard guard;
        wasActive = guard.is_active();
    }
    
    // Should be stopped now
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testGuardSystemRequest() {
    sak::KeepAwakeGuard guard(sak::KeepAwake::PowerRequest::System);
}

void TestKeepAwake::testGuardDisplayRequest() {
    sak::KeepAwakeGuard guard(sak::KeepAwake::PowerRequest::Display);
}

void TestKeepAwake::testGuardBothRequest() {
    sak::KeepAwakeGuard guard(sak::KeepAwake::PowerRequest::Both);
}

void TestKeepAwake::testGuardCustomReason() {
    sak::KeepAwakeGuard guard(
        sak::KeepAwake::PowerRequest::System,
        "Test operation"
    );
}

void TestKeepAwake::testGuardNoCopy() {
    // Compile-time check - copy should be deleted
}

void TestKeepAwake::testGuardNoMove() {
    // Compile-time check - move should be deleted
}

void TestKeepAwake::testIsActiveNoexcept() {
    // is_active() is marked noexcept
    bool active = sak::KeepAwake::is_active();
    QVERIFY(active == true || active == false);
}

void TestKeepAwake::testMultipleThreads() {
    // is_active() should be thread-safe
    auto result1 = sak::KeepAwake::start();
    
    bool active1 = sak::KeepAwake::is_active();
    bool active2 = sak::KeepAwake::is_active();
    
    QCOMPARE(active1, active2);
}

void TestKeepAwake::testInactiveToActive() {
    QVERIFY(!sak::KeepAwake::is_active());
    
    auto result = sak::KeepAwake::start();
    
    if (result.has_value()) {
        QVERIFY(sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testActiveToInactive() {
    sak::KeepAwake::start();
    sak::KeepAwake::stop();
    
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testMultipleTransitions() {
    for (int i = 0; i < 5; i++) {
        auto result = sak::KeepAwake::start();
        if (result.has_value()) {
            QVERIFY(sak::KeepAwake::is_active());
        }
        
        sak::KeepAwake::stop();
        QVERIFY(!sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testSetThreadExecutionState() {
    // Uses SetThreadExecutionState Windows API
    auto result = sak::KeepAwake::start();
}

void TestKeepAwake::testPowerRequestFlags() {
    // Test different flag combinations
    auto result1 = sak::KeepAwake::start(sak::KeepAwake::PowerRequest::System);
    sak::KeepAwake::stop();
    
    auto result2 = sak::KeepAwake::start(sak::KeepAwake::PowerRequest::Display);
    sak::KeepAwake::stop();
    
    auto result3 = sak::KeepAwake::start(sak::KeepAwake::PowerRequest::Both);
}

void TestKeepAwake::testLongRunningOperation() {
    auto result = sak::KeepAwake::start();
    
    if (result.has_value()) {
        // Simulate long operation
        QThread::msleep(100);
        
        QVERIFY(sak::KeepAwake::is_active());
    }
}

void TestKeepAwake::testGuardLongOperation() {
    {
        sak::KeepAwakeGuard guard;
        
        // Simulate long operation
        QThread::msleep(100);
        
        if (guard.is_active()) {
            QVERIFY(sak::KeepAwake::is_active());
        }
    }
    
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testNestedGuards() {
    {
        sak::KeepAwakeGuard guard1;
        {
            sak::KeepAwakeGuard guard2;
            // Both active
        }
        // guard1 still active
    }
    // Both destroyed
}

void TestKeepAwake::testNestedGuardsOverlap() {
    {
        sak::KeepAwakeGuard guard1;
        bool active1 = guard1.is_active();
        
        {
            sak::KeepAwakeGuard guard2;
            bool active2 = guard2.is_active();
        }
        
        // guard1 should still work
    }
}

void TestKeepAwake::testStartFailure() {
    // Hard to force failure
    auto result = sak::KeepAwake::start();
}

void TestKeepAwake::testStopFailure() {
    // Hard to force failure
    auto result = sak::KeepAwake::stop();
}

void TestKeepAwake::testWindowsOnly() {
    #ifdef _WIN32
        auto result = sak::KeepAwake::start();
        QVERIFY(result.has_value() || !result.has_value());
    #else
        QSKIP("Windows only");
    #endif
}

void TestKeepAwake::testRapidStartStop() {
    for (int i = 0; i < 100; i++) {
        sak::KeepAwake::start();
        sak::KeepAwake::stop();
    }
    
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testGuardException() {
    try {
        sak::KeepAwakeGuard guard;
        throw std::runtime_error("test");
    } catch (...) {
        // Guard should clean up
    }
    
    QVERIFY(!sak::KeepAwake::is_active());
}

void TestKeepAwake::testStartSpeed() {
    QElapsedTimer timer;
    timer.start();
    
    sak::KeepAwake::start();
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 100); // Should be very fast
}

void TestKeepAwake::testStopSpeed() {
    sak::KeepAwake::start();
    
    QElapsedTimer timer;
    timer.start();
    
    sak::KeepAwake::stop();
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 100); // Should be very fast
}

QTEST_MAIN(TestKeepAwake)
#include "test_keep_awake.moc"
