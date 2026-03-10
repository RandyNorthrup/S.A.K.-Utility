// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"

#include <QSignalSpy>
#include <QTest>

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

// ============================================================================
// Test class
// ============================================================================

class TestQuickActionController : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();

    // ── Registration & lookup ───────────────────────────────
    void testRegisterAndGet();
    void testGetUnknownReturnsNull();
    void testGetAllActions();
    void testNoDuplicateRegistration();

    // ── Category filtering ──────────────────────────────────
    void testGetByCategory();
    void testGetByCategoryEmpty();

    // ── Configuration setters ───────────────────────────────
    void testSetBackupLocation();
    void testSetLoggingEnabled();

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

    // Registering a second action with same name
    // should still allow it (controller uses name as key)
    auto action2 = std::make_unique<ControllerStubAction>("Dup");
    const QString name2 = m_ctrl->registerAction(std::move(action2));

    // Behavior: the latest registration should be accessible
    QVERIFY(m_ctrl->getAction("Dup") != nullptr);
}

// ============================================================================
// Category filtering
// ============================================================================

void TestQuickActionController::testGetByCategory() {
    m_ctrl->registerAction(
        std::make_unique<ControllerStubAction>("Maint1", QuickAction::ActionCategory::Maintenance));
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>(
        "Backup1", QuickAction::ActionCategory::QuickBackup));
    m_ctrl->registerAction(
        std::make_unique<ControllerStubAction>("Maint2", QuickAction::ActionCategory::Maintenance));

    auto maint = m_ctrl->getActionsByCategory(QuickAction::ActionCategory::Maintenance);
    QCOMPARE(static_cast<int>(maint.size()), 2);

    auto backup = m_ctrl->getActionsByCategory(QuickAction::ActionCategory::QuickBackup);
    QCOMPARE(static_cast<int>(backup.size()), 1);
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

QTEST_MAIN(TestQuickActionController)
#include "test_quick_action_controller.moc"
