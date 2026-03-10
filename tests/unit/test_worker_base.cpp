// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_worker_base.cpp
/// @brief Unit tests for WorkerBase thread lifecycle, exception safety, and cancellation

#include "sak/error_codes.h"
#include "sak/worker_base.h"

#include <QtTest/QtTest>

#include <stdexcept>

// ============================================================================
// Test Worker Implementations
// ============================================================================

/// @brief Worker that completes successfully
class SuccessWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override { return {}; }
};

/// @brief Worker that returns an error
class FailWorker : public WorkerBase {
    Q_OBJECT
public:
    using WorkerBase::WorkerBase;

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        return std::unexpected(sak::error_code::internal_error);
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

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        while (!checkStop()) {
            QThread::msleep(10);
        }
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
    void isExecutingFlag();
    void destructorStopsThread();
};

void WorkerBaseTests::successfulExecution() {
    SuccessWorker worker;
    QSignalSpy started_spy(&worker, &WorkerBase::started);
    QSignalSpy finished_spy(&worker, &WorkerBase::finished);
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(started_spy.count(), 1);
    QCOMPARE(finished_spy.count(), 1);
    QCOMPARE(failed_spy.count(), 0);
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
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::internal_error));
}

void WorkerBaseTests::exceptionSafety_stdException() {
    ThrowWorker worker;
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    // Must NOT crash â€” exception caught, failed() emitted
    QCOMPARE(failed_spy.count(), 1);

    const auto args = failed_spy.first();
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::internal_error));
    QVERIFY(args[1].toString().contains("deliberate test exception"));
}

void WorkerBaseTests::exceptionSafety_badAlloc() {
    OOMWorker worker;
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(failed_spy.count(), 1);
    const auto args = failed_spy.first();
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::internal_error));
    QVERIFY(args[1].toString().contains("exception"));
}

void WorkerBaseTests::exceptionSafety_unknownException() {
    UnknownThrowWorker worker;
    QSignalSpy failed_spy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(failed_spy.count(), 1);
    const auto args = failed_spy.first();
    QCOMPARE(args[0].toInt(), static_cast<int>(sak::error_code::internal_error));
    QVERIFY(args[1].toString().contains("unknown"));
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
}

void WorkerBaseTests::progressReporting() {
    ProgressWorker worker;
    QSignalSpy progress_spy(&worker, &WorkerBase::progress);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(progress_spy.count(), 5);  // 0, 25, 50, 75, 100
    QCOMPARE(progress_spy[0][0].toInt(), 0);
    QCOMPARE(progress_spy[4][0].toInt(), 100);
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
    // Test that destructor properly cleans up
    {
        CancellableWorker worker;
        worker.start();
        QThread::msleep(50);
        QVERIFY(worker.isRunning());
    }
    // If we get here without hanging, the destructor worked
    QVERIFY(true);
}

QTEST_MAIN(WorkerBaseTests)
#include "test_worker_base.moc"
