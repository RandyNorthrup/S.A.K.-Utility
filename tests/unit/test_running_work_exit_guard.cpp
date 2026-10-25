// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_running_work_exit_guard.cpp
/// @brief Asking before exit/relaunch while work runs, then cancelling it cooperatively.

#include "sak/main_window.h"
#include "sak/running_operations.h"
#include "sak/running_work_exit_guard.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QtTest/QtTest>

#include <functional>
#include <memory>

using sak::RunningOperations;
using sak::RunningWorkExitGuard;

namespace {

constexpr int kDialogPollMs = 20;
constexpr int kDialogTimeoutMs = 5000;

/// One fake long operation: registered like a panel registers its own, running until stopped.
class FakeOperation : public QObject {
public:
    explicit FakeOperation(const QString& name) {
        RunningOperations::add(
            this, name, [this]() { return running; }, [this]() { ++cancel_requests; });
    }
    /// The operation stops, as the registered probe will next observe.
    void finish() { running = false; }
    bool running{true};
    int cancel_requests{0};
};

/// Clicks the button whose text is @p button_text on the next message box that opens, and
/// records the question it asked. Polls because the box runs its own nested event loop.
class MessageBoxAnswer : public QObject {
public:
    explicit MessageBoxAnswer(QString button_text) : m_button_text(std::move(button_text)) {
        m_timer.setInterval(kDialogPollMs);
        connect(&m_timer, &QTimer::timeout, this, [this]() { answerIfOpen(); });
        m_timer.start();
        m_deadline.start();
    }
    QString question;
    bool answered{false};

private:
    void answerIfOpen() {
        if (m_deadline.elapsed() > kDialogTimeoutMs) {
            m_timer.stop();
            return;
        }
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        for (QAbstractButton* button : box->buttons()) {
            if (button->text().remove(QLatin1Char('&')) == m_button_text) {
                question = box->text();
                answered = true;
                m_timer.stop();
                button->click();
                return;
            }
        }
    }
    QString m_button_text;
    QTimer m_timer;
    QElapsedTimer m_deadline;
};

QString cancelButtonText() {
    return QMessageBox(QMessageBox::Warning, {}, {}, QMessageBox::Cancel)
        .button(QMessageBox::Cancel)
        ->text()
        .remove(QLatin1Char('&'));
}

}  // namespace

class RunningWorkExitGuardTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void nothingRunningProceedsAtOnce();
    void declinedExitLeavesWorkRunning();
    void confirmedExitCancelsThenProceedsOnceStopped();
    void restartIntentAsksAboutRestarting();
    void questionNamesEveryRunningOperation();
    void registrationEndsWithItsOwner();
    void mainWindowCloseAsksWhileWorkRuns();
};

void RunningWorkExitGuardTests::nothingRunningProceedsAtOnce() {
    QWidget window;
    RunningWorkExitGuard guard(&window);
    FakeOperation idle(QStringLiteral("Idle job"));
    idle.finish();
    bool proceeded = false;
    QVERIFY(guard.proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::Exit,
                                              [&proceeded]() { proceeded = true; }));
    QVERIFY2(!proceeded, "the caller carries out the action itself when nothing runs");
    QCOMPARE(idle.cancel_requests, 0);
}

void RunningWorkExitGuardTests::declinedExitLeavesWorkRunning() {
    QWidget window;
    RunningWorkExitGuard guard(&window);
    FakeOperation write(QStringLiteral("USB write"));
    MessageBoxAnswer answer(cancelButtonText());
    bool proceeded = false;
    QVERIFY(!guard.proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::Exit,
                                               [&proceeded]() { proceeded = true; }));
    QVERIFY2(answer.answered, "closing with work running must ask first");
    QCOMPARE(answer.question, QStringLiteral("USB write is running. Cancel it and exit?"));
    QCOMPARE(write.cancel_requests, 0);
    QVERIFY(!guard.isWaitingForCancellation());
    QTest::qWait(kDialogPollMs * 5);
    QVERIFY(!proceeded);
}

void RunningWorkExitGuardTests::confirmedExitCancelsThenProceedsOnceStopped() {
    QWidget window;
    RunningWorkExitGuard guard(&window);
    FakeOperation write(QStringLiteral("USB write"));
    MessageBoxAnswer answer(
        RunningWorkExitGuard::proceedButtonText(RunningWorkExitGuard::Intent::Exit));
    int proceeded = 0;
    QVERIFY(!guard.proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::Exit,
                                               [&proceeded]() { ++proceeded; }));
    QVERIFY(answer.answered);
    QCOMPARE(write.cancel_requests, 1);
    QVERIFY(guard.isWaitingForCancellation());
    QVERIFY2(window.findChild<QProgressDialog*>() != nullptr,
             "the wait for cooperative cancellation must show progress");

    // Still stopping: the exit must not happen yet, and a second close is not a second prompt.
    QTest::qWait(kDialogPollMs * 10);
    QCOMPARE(proceeded, 0);
    QVERIFY(!guard.proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::Exit, {}));

    write.finish();
    QTRY_COMPARE_WITH_TIMEOUT(proceeded, 1, kDialogTimeoutMs);
    QVERIFY(!guard.isWaitingForCancellation());
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QProgressDialog*>() == nullptr, kDialogTimeoutMs);
}

void RunningWorkExitGuardTests::restartIntentAsksAboutRestarting() {
    QWidget window;
    RunningWorkExitGuard guard(&window);
    FakeOperation install(QStringLiteral("App installation"));
    MessageBoxAnswer answer(cancelButtonText());
    QVERIFY(!guard.proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::RestartElevated, {}));
    QCOMPARE(answer.question,
             QStringLiteral(
                 "App installation is running. Cancel it and restart as administrator?"));
    QCOMPARE(install.cancel_requests, 0);
}

void RunningWorkExitGuardTests::questionNamesEveryRunningOperation() {
    QCOMPARE(RunningWorkExitGuard::question(RunningWorkExitGuard::Intent::Exit,
                                            {QStringLiteral("A"), QStringLiteral("B")}),
             QStringLiteral("A and B are running. Cancel them and exit?"));
    QCOMPARE(RunningWorkExitGuard::question(
                 RunningWorkExitGuard::Intent::Exit,
                 {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}),
             QStringLiteral("A, B and C are running. Cancel them and exit?"));
}

void RunningWorkExitGuardTests::registrationEndsWithItsOwner() {
    auto owner = std::make_unique<FakeOperation>(QStringLiteral("Profile backup"));
    QVERIFY(RunningOperations::runningNames().contains(QStringLiteral("Profile backup")));
    owner.reset();
    QVERIFY(!RunningOperations::runningNames().contains(QStringLiteral("Profile backup")));
}

// The main window is the one caller: its close must consult the guard, and declining keeps it open.
void RunningWorkExitGuardTests::mainWindowCloseAsksWhileWorkRuns() {
    sak::MainWindow window;
    FakeOperation apply(QStringLiteral("Partition changes"));
    MessageBoxAnswer answer(cancelButtonText());
    QCloseEvent declined;
    QApplication::sendEvent(&window, &declined);
    QVERIFY2(answer.answered, "closing the main window with work running must ask first");
    QVERIFY2(!declined.isAccepted(), "a declined exit must keep the window open");
    QCOMPARE(apply.cancel_requests, 0);

    apply.finish();
    QCloseEvent idle;
    QApplication::sendEvent(&window, &idle);
    QVERIFY(idle.isAccepted());
}

QTEST_MAIN(RunningWorkExitGuardTests)
#include "test_running_work_exit_guard.moc"
