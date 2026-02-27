// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_deployment_manager.cpp
/// @brief Unit tests for deployment queue management

#include <QtTest/QtTest>

#include "sak/deployment_manager.h"
#include "sak/orchestration_types.h"

#include <QSignalSpy>

class DeploymentManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Constructor
    void constructor_emptyQueue();

    // Enqueue / Dequeue
    void enqueue_incrementsCount();
    void dequeue_decrementsCount();
    void dequeue_fifoOrder();
    void peek_doesNotRemove();

    // hasPending
    void hasPending_emptyFalse();
    void hasPending_withItemTrue();

    // Signals
    void enqueue_emitsSignal();
    void dequeue_emitsSignal();

    // enqueueForDestination
    void enqueueForDestination_withReadinessCheck();
    void enqueueForDestination_rejected();

    // Readiness check
    void setReadinessCheck_usedOnEnqueue();
};

// Helper to create a test assignment
static sak::DeploymentAssignment makeAssignment(const QString& id)
{
    sak::DeploymentAssignment assignment;
    assignment.deployment_id = id;
    assignment.source_user = "test_user";
    return assignment;
}

// ============================================================================
// Constructor
// ============================================================================

void DeploymentManagerTests::constructor_emptyQueue()
{
    sak::DeploymentManager mgr;
    QVERIFY(!mgr.hasPending());
    QCOMPARE(mgr.pendingCount(), 0);
}

// ============================================================================
// Enqueue / Dequeue
// ============================================================================

void DeploymentManagerTests::enqueue_incrementsCount()
{
    sak::DeploymentManager mgr;
    mgr.enqueue(makeAssignment("dest1"));
    QCOMPARE(mgr.pendingCount(), 1);

    mgr.enqueue(makeAssignment("dest2"));
    QCOMPARE(mgr.pendingCount(), 2);
}

void DeploymentManagerTests::dequeue_decrementsCount()
{
    sak::DeploymentManager mgr;
    mgr.enqueue(makeAssignment("dest1"));
    mgr.enqueue(makeAssignment("dest2"));

    mgr.dequeue();
    QCOMPARE(mgr.pendingCount(), 1);

    mgr.dequeue();
    QCOMPARE(mgr.pendingCount(), 0);
}

void DeploymentManagerTests::dequeue_fifoOrder()
{
    sak::DeploymentManager mgr;
    mgr.enqueue(makeAssignment("first"));
    mgr.enqueue(makeAssignment("second"));
    mgr.enqueue(makeAssignment("third"));

    auto a1 = mgr.dequeue();
    QCOMPARE(a1.deployment_id, QString("first"));

    auto a2 = mgr.dequeue();
    QCOMPARE(a2.deployment_id, QString("second"));

    auto a3 = mgr.dequeue();
    QCOMPARE(a3.deployment_id, QString("third"));
}

void DeploymentManagerTests::peek_doesNotRemove()
{
    sak::DeploymentManager mgr;
    mgr.enqueue(makeAssignment("peek_test"));

    sak::DeploymentAssignment peeked;
    QVERIFY(mgr.peek(&peeked));
    QCOMPARE(peeked.deployment_id, QString("peek_test"));
    QCOMPARE(mgr.pendingCount(), 1); // Still in queue
}

// ============================================================================
// hasPending
// ============================================================================

void DeploymentManagerTests::hasPending_emptyFalse()
{
    sak::DeploymentManager mgr;
    QVERIFY(!mgr.hasPending());
}

void DeploymentManagerTests::hasPending_withItemTrue()
{
    sak::DeploymentManager mgr;
    mgr.enqueue(makeAssignment("item"));
    QVERIFY(mgr.hasPending());
}

// ============================================================================
// Signals
// ============================================================================

void DeploymentManagerTests::enqueue_emitsSignal()
{
    sak::DeploymentManager mgr;
    QSignalSpy spy(&mgr, &sak::DeploymentManager::deploymentQueued);
    QVERIFY(spy.isValid());

    mgr.enqueue(makeAssignment("signal_test"));
    QCOMPARE(spy.count(), 1);
}

void DeploymentManagerTests::dequeue_emitsSignal()
{
    sak::DeploymentManager mgr;
    mgr.enqueue(makeAssignment("deq_signal"));

    QSignalSpy spy(&mgr, &sak::DeploymentManager::deploymentDequeued);
    QVERIFY(spy.isValid());

    mgr.dequeue();
    QCOMPARE(spy.count(), 1);
}

// ============================================================================
// enqueueForDestination with Readiness Check
// ============================================================================

void DeploymentManagerTests::enqueueForDestination_withReadinessCheck()
{
    sak::DeploymentManager mgr;
    mgr.setReadinessCheck([](const QString&, qint64, QString*) -> bool {
        return true; // Always ready
    });

    mgr.enqueueForDestination(makeAssignment("ready_dest"), "ready_dest", 1024);
    QCOMPARE(mgr.pendingCount(), 1);
}

void DeploymentManagerTests::enqueueForDestination_rejected()
{
    sak::DeploymentManager mgr;
    mgr.setReadinessCheck([](const QString&, qint64, QString* reason) -> bool {
        if (reason) *reason = "Not enough space";
        return false;
    });

    QSignalSpy spy(&mgr, &sak::DeploymentManager::deploymentRejected);
    QVERIFY(spy.isValid());

    mgr.enqueueForDestination(makeAssignment("rejected_dest"), "rejected_dest", 999999);
    QCOMPARE(mgr.pendingCount(), 0);
    QCOMPARE(spy.count(), 1);
}

// ============================================================================
// Readiness Check
// ============================================================================

void DeploymentManagerTests::setReadinessCheck_usedOnEnqueue()
{
    sak::DeploymentManager mgr;
    bool checkCalled = false;
    mgr.setReadinessCheck([&checkCalled](const QString&, qint64, QString*) -> bool {
        checkCalled = true;
        return true;
    });

    mgr.enqueueForDestination(makeAssignment("check_dest"), "check_dest", 100);
    QVERIFY(checkCalled);
}

QTEST_GUILESS_MAIN(DeploymentManagerTests)
#include "test_deployment_manager.moc"
