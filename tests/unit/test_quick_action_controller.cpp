// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/action_constants.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"
#include "sak/thread_custody.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

using namespace sak;

// ============================================================================
// Stub action for controller tests
// ============================================================================

class ControllerStubAction : public QuickAction {
    Q_OBJECT
public:
    explicit ControllerStubAction(const QString& name,
                                  ActionCategory category = ActionCategory::Maintenance,
                                  bool admin = false,
                                  QObject* parent = nullptr)
        : QuickAction(parent), m_name(name), m_category(category), m_admin(admin) {}

    QString name() const override { return m_name; }
    QString description() const override { return m_name + " description"; }
    QIcon icon() const override { return {}; }
    ActionCategory category() const override { return m_category; }
    bool requiresAdmin() const override { return m_admin; }
    void scan() override {
        ScanResult r;
        r.applicable = true;
        r.summary = "Scan complete";
        setScanResult(r);
        setStatus(ActionStatus::Ready);
    }
    void execute() override {
        setStatus(ActionStatus::Running);
        ExecutionResult r;
        r.success = true;
        r.message = "Done";
        finishWithResult(r, ActionStatus::Success);
    }

private:
    QString m_name;
    ActionCategory m_category;
    bool m_admin;
};

/// Facts a stubborn action reports about itself, shared so they outlive the action.
struct StubbornActionFacts {
    std::atomic<bool> may_finish{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> destroyed{false};
};

/// Scan that cannot reach a cancellation point until the test lets it -- a long native call.
class StubbornScanAction : public ControllerStubAction {
    Q_OBJECT
public:
    explicit StubbornScanAction(std::shared_ptr<StubbornActionFacts> facts)
        : ControllerStubAction(QStringLiteral("Stubborn")), m_facts(std::move(facts)) {}
    ~StubbornScanAction() override { m_facts->destroyed.store(true, std::memory_order_release); }

    void scan() override {
        m_facts->entered.store(true, std::memory_order_release);
        while (!m_facts->may_finish.load(std::memory_order_acquire)) {
            QThread::msleep(kStubbornPollMs);
        }
        // Reached only by running to completion: a terminated thread never gets here.
        m_facts->completed.store(true, std::memory_order_release);
        ControllerStubAction::scan();
    }

    static constexpr unsigned long kStubbornPollMs = 10;

private:
    std::shared_ptr<StubbornActionFacts> m_facts;
};

// ============================================================================
// Test class
// ============================================================================

class TestQuickActionController : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();

    // -- Registration & lookup -------------------------------
    void testRegisterAndGet();
    void testGetUnknownReturnsNull();
    void testGetAllActions();
    void testNoDuplicateRegistration();

    // -- Category filtering ----------------------------------
    void testGetByCategory();
    void testGetByCategoryEmpty();

    // -- Configuration setters -------------------------------
    void testSetBackupLocation();
    void testSetLoggingEnabled();
    void testElevatedTaskIdsAndPayloadUseHelperContract();
    void testBitLockerPayloadHasNoDefaultDestination();

    // -- Teardown --------------------------------------------
    void testDestroyDuringStuckScanRetainsActionInsteadOfBlocking();

private:
    std::unique_ptr<QuickActionController> m_ctrl;
};

void TestQuickActionController::init() {
    m_ctrl = std::make_unique<QuickActionController>();
}

// ============================================================================
// Registration & lookup
// ============================================================================

void TestQuickActionController::testRegisterAndGet() {
    auto action = std::make_unique<ControllerStubAction>("TestAction1");
    const QString name = m_ctrl->registerAction(std::move(action));

    QCOMPARE(name, "TestAction1");
    QVERIFY(m_ctrl->getAction("TestAction1") != nullptr);
    QCOMPARE(m_ctrl->getAction("TestAction1")->name(), "TestAction1");
}

void TestQuickActionController::testGetUnknownReturnsNull() {
    QVERIFY(m_ctrl->getAction("NoSuchAction") == nullptr);
}

void TestQuickActionController::testGetAllActions() {
    Q_ASSERT(m_ctrl);
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("A1"));
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("A2"));
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("A3"));

    auto all = m_ctrl->getAllActions();
    QCOMPARE(static_cast<int>(all.size()), 3);
}

void TestQuickActionController::testNoDuplicateRegistration() {
    Q_ASSERT(m_ctrl);
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("Dup"));

    // A second action with a duplicate name is REJECTED (fail closed): registerAction stores
    // nothing and returns an empty id, so the first "Dup" survives and stays routable. This avoids
    // the listing/routing split where getAction resolves the newest but getAllActions lists both.
    auto action2 = std::make_unique<ControllerStubAction>("Dup");
    const QString name2 = m_ctrl->registerAction(std::move(action2));

    QCOMPARE(name2, QString());
    QCOMPARE(static_cast<int>(m_ctrl->getAllActions().size()), 1);
    QVERIFY(m_ctrl->getAction("Dup") != nullptr);
    QCOMPARE(m_ctrl->getAction("Dup")->name(), QStringLiteral("Dup"));
}

// ============================================================================
// Category filtering
// ============================================================================

void TestQuickActionController::testGetByCategory() {
    m_ctrl->registerAction(
        std::make_unique<ControllerStubAction>("Maint1", QuickAction::ActionCategory::Maintenance));
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>(
        "Recovery1", QuickAction::ActionCategory::EmergencyRecovery));
    m_ctrl->registerAction(
        std::make_unique<ControllerStubAction>("Maint2", QuickAction::ActionCategory::Maintenance));

    auto maint = m_ctrl->getActionsByCategory(QuickAction::ActionCategory::Maintenance);
    QCOMPARE(static_cast<int>(maint.size()), 2);

    auto recovery = m_ctrl->getActionsByCategory(QuickAction::ActionCategory::EmergencyRecovery);
    QCOMPARE(static_cast<int>(recovery.size()), 1);
}

void TestQuickActionController::testGetByCategoryEmpty() {
    m_ctrl->registerAction(
        std::make_unique<ControllerStubAction>("X", QuickAction::ActionCategory::Maintenance));

    auto troubleshoot = m_ctrl->getActionsByCategory(QuickAction::ActionCategory::Troubleshooting);
    QVERIFY(troubleshoot.empty());
}

// ============================================================================
// Configuration setters
// ============================================================================

void TestQuickActionController::testSetBackupLocation() {
    // Should not crash
    m_ctrl->setBackupLocation("C:/TestBackups");
}

void TestQuickActionController::testSetLoggingEnabled() {
    // Should not crash
    m_ctrl->setLoggingEnabled(true);
    m_ctrl->setLoggingEnabled(false);
}

void TestQuickActionController::testElevatedTaskIdsAndPayloadUseHelperContract() {
    using namespace quick_action_elevation;
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kResetNetworkName)),
             QString::fromLatin1(kResetNetworkTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kBackupBitLockerKeysName)),
             QString::fromLatin1(kBackupBitLockerKeysTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kVerifySystemFilesName)),
             QString::fromLatin1(kVerifySystemFilesTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kCheckDiskErrorsName)),
             QString::fromLatin1(kCheckDiskErrorsTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QStringLiteral("Unknown Admin Action")), QString());

    const QJsonObject payload = buildElevatedActionPayload(
        elevatedTaskIdForQuickAction(QString::fromLatin1(kBackupBitLockerKeysName)),
        QStringLiteral("C:/session/artifacts"));
    QCOMPARE(payload.value(QStringLiteral("backup_location")).toString(),
             QStringLiteral("C:/session/artifacts"));
}

void TestQuickActionController::testBitLockerPayloadHasNoDefaultDestination() {
    const QJsonObject payload = buildElevatedActionPayload(
        QString::fromLatin1(quick_action_elevation::kBackupBitLockerKeysTaskId), QString());
    QVERIFY(payload.isEmpty());
}

// ============================================================================
// Teardown
// ============================================================================

// Destroying the controller while an action's thread is stuck used to block the GUI thread with an
// unbounded wait. It must give the thread its bounded join, then report it and retain the thread
// AND the action it runs -- which the controller would otherwise free underneath it -- until the
// scan finishes on its own.
void TestQuickActionController::testDestroyDuringStuckScanRetainsActionInsteadOfBlocking() {
    constexpr int kStartTimeoutMs = 5000;
    constexpr qint64 kTeardownBoundMs = 20'000;  // controller's 10 s join plus slack
    constexpr int kReapTimeoutMs = 5000;
    auto facts = std::make_shared<StubbornActionFacts>();
    QVERIFY(!m_ctrl->registerAction(std::make_unique<StubbornScanAction>(facts)).isEmpty());
    m_ctrl->scanAction(QStringLiteral("Stubborn"));
    QTRY_VERIFY_WITH_TIMEOUT(facts->entered.load(std::memory_order_acquire), kStartTimeoutMs);
    const int custody_before = ThreadCustody::retainedCount();

    QElapsedTimer timer;
    timer.start();
    m_ctrl.reset();
    QVERIFY2(timer.elapsed() < kTeardownBoundMs, "controller teardown waited without bound");
    QVERIFY2(!facts->destroyed.load(std::memory_order_acquire),
             "the running action was freed underneath its thread");
    QCOMPARE(ThreadCustody::retainedCount(), custody_before + 1);
    QVERIFY(ThreadCustody::retainedReasons().constLast().startsWith(
        QStringLiteral("QuickAction thread for 'Stubborn' still running")));

    facts->may_finish.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(facts->destroyed.load(std::memory_order_acquire), kReapTimeoutMs);
    QVERIFY(facts->completed.load(std::memory_order_acquire));
    QCOMPARE(ThreadCustody::retainedCount(), custody_before);
}

QTEST_MAIN(TestQuickActionController)
#include "test_quick_action_controller.moc"
