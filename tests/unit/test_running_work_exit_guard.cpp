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
#include <QElapsedTimer>
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
            this,
            name,
            [this]() { return running; },
            [this]() { ++cancel_requests; },
            [this]() { return cannot_stop_note; });
    }
    /// The operation stops, as the registered probe will next observe.
    void finish() { running = false; }
    bool running{true};
    int cancel_requests{0};
    /// Non-empty while part of the work is something a cancel cannot stop.
    QString cannot_stop_note;
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
    QString informative;
    QStringList buttons;
    bool answered{false};

private:
    void answerIfOpen() {
        if (m_deadline.elapsed() > kDialogTimeoutMs) {
            // A box without the expected button is closed rather than left to hold the test in
            // its event loop; the test then fails on what it expected to answer.
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->reject();
            }
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
                informative = box->informativeText();
                for (const QAbstractButton* shown : box->buttons()) {
                    buttons.append(shown->text().remove(QLatin1Char('&')));
                }
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
    void workWindowsFinishesIsNotPromisedAStop();
    void cannotStopTextForEveryIntent();
    void everyRegisteredNameReadsRight();
    void promptFollowsTheWorkItAsksAbout();
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
    QCOMPARE(answer.question, QStringLiteral("Still running: USB write. Cancel and exit?"));
    QCOMPARE(answer.informative,
             QStringLiteral("Each operation is asked to stop at its next safe point, and S.A.K. "
                            "Utility waits for it before it closes."));
    QVERIFY(answer.buttons.contains(QStringLiteral("Exit after cancelling")));
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
                 "Still running: App installation. Cancel and restart as administrator?"));
    QCOMPARE(install.cancel_requests, 0);
}

void RunningWorkExitGuardTests::questionNamesEveryRunningOperation() {
    QCOMPARE(RunningWorkExitGuard::question(RunningWorkExitGuard::Intent::Exit,
                                            {QStringLiteral("A"), QStringLiteral("B")}),
             QStringLiteral("Still running: A and B. Cancel and exit?"));
    QCOMPARE(RunningWorkExitGuard::question(
                 RunningWorkExitGuard::Intent::Exit,
                 {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}),
             QStringLiteral("Still running: A, B and C. Cancel and exit?"));
}

void RunningWorkExitGuardTests::registrationEndsWithItsOwner() {
    auto owner = std::make_unique<FakeOperation>(QStringLiteral("Profile backup"));
    QVERIFY(RunningOperations::runningNames().contains(QStringLiteral("Profile backup")));
    owner.reset();
    QVERIFY(!RunningOperations::runningNames().contains(QStringLiteral("Profile backup")));
}

// A full format Windows has taken over goes on after the app closes. The prompt says so, and does
// not offer "Exit after cancelling" as if leaving would stop it; exiting is still allowed, and
// still stops what can be stopped.
void RunningWorkExitGuardTests::workWindowsFinishesIsNotPromisedAStop() {
    QWidget window;
    RunningWorkExitGuard guard(&window);
    FakeOperation apply(QStringLiteral("Partition changes"));
    apply.cannot_stop_note = QStringLiteral(
        "Wipe Disk - Disk 4: Windows is carrying out a full "
        "format. Closing S.A.K. Utility will not stop it.");
    MessageBoxAnswer answer(
        RunningWorkExitGuard::proceedAnywayButtonText(RunningWorkExitGuard::Intent::Exit));
    int proceeded = 0;
    QVERIFY(!guard.proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::Exit,
                                               [&proceeded]() { ++proceeded; }));
    QVERIFY2(answer.answered, "the prompt offers to exit anyway");
    QCOMPARE(answer.question, QStringLiteral("Still running: Partition changes. Exit anyway?"));
    QCOMPARE(answer.informative,
             apply.cannot_stop_note +
                 QStringLiteral("\n\nEverything else S.A.K. Utility can stop is asked to stop "
                                "at its next safe point, and S.A.K. Utility waits for that before "
                                "it closes."));
    QVERIFY2(!answer.buttons.contains(QStringLiteral("Exit after cancelling")),
             qPrintable(answer.buttons.join(QStringLiteral(", "))));
    QVERIFY(answer.buttons.contains(QStringLiteral("Exit anyway")));
    QCOMPARE(apply.cancel_requests, 1);
    apply.finish();
    QTRY_COMPARE_WITH_TIMEOUT(proceeded, 1, kDialogTimeoutMs);
}

void RunningWorkExitGuardTests::cannotStopTextForEveryIntent() {
    QCOMPARE(RunningWorkExitGuard::questionWhenSomeCannotStop(RunningWorkExitGuard::Intent::Exit,
                                                              {QStringLiteral("A")}),
             QStringLiteral("Still running: A. Exit anyway?"));
    QCOMPARE(RunningWorkExitGuard::questionWhenSomeCannotStop(
                 RunningWorkExitGuard::Intent::RestartElevated,
                 {QStringLiteral("A"), QStringLiteral("B")}),
             QStringLiteral("Still running: A and B. Restart as administrator anyway?"));
    QCOMPARE(RunningWorkExitGuard::proceedAnywayButtonText(RunningWorkExitGuard::Intent::Exit),
             QStringLiteral("Exit anyway"));
    QCOMPARE(RunningWorkExitGuard::proceedAnywayButtonText(
                 RunningWorkExitGuard::Intent::RestartElevated),
             QStringLiteral("Restart anyway"));
    QCOMPARE(RunningWorkExitGuard::informativeText({}),
             QStringLiteral("Each operation is asked to stop at its next safe point, and S.A.K. "
                            "Utility waits for it before it closes."));
    QVERIFY(RunningWorkExitGuard::informativeText({QStringLiteral("one"), QStringLiteral("two")})
                .startsWith(QStringLiteral("one\n\ntwo\n\n")));
}

// "Partition changes is running" was wrong grammar, and a template that agrees a verb with the
// name is wrong for any plural name. These are the names every panel registers with
// RunningOperations::add today; each reads right alone, with another, and with several.
void RunningWorkExitGuardTests::everyRegisteredNameReadsRight() {
    const QStringList registered{QStringLiteral("Program uninstall"),
                                 QStringLiteral("AI tool call"),
                                 QStringLiteral("App installation"),
                                 QStringLiteral("Offline package operation"),
                                 QStringLiteral("Quick action"),
                                 QStringLiteral("File copy or move"),
                                 QStringLiteral("USB write"),
                                 QStringLiteral("Network reset"),
                                 QStringLiteral("File organization"),
                                 QStringLiteral("Partition changes"),
                                 QStringLiteral("Profile backup"),
                                 QStringLiteral("Profile restore")};
    using Intent = RunningWorkExitGuard::Intent;
    for (const QString& name : registered) {
        QCOMPARE(RunningWorkExitGuard::question(Intent::Exit, {name}),
                 QStringLiteral("Still running: %1. Cancel and exit?").arg(name));
        QCOMPARE(
            RunningWorkExitGuard::question(Intent::RestartElevated, {name}),
            QStringLiteral("Still running: %1. Cancel and restart as administrator?").arg(name));
        QCOMPARE(RunningWorkExitGuard::questionWhenSomeCannotStop(Intent::Exit, {name}),
                 QStringLiteral("Still running: %1. Exit anyway?").arg(name));
        QCOMPARE(RunningWorkExitGuard::questionWhenSomeCannotStop(Intent::RestartElevated, {name}),
                 QStringLiteral("Still running: %1. Restart as administrator anyway?").arg(name));
    }
    QCOMPARE(RunningWorkExitGuard::question(
                 Intent::Exit, {QStringLiteral("Partition changes"), QStringLiteral("USB write")}),
             QStringLiteral("Still running: Partition changes and USB write. Cancel and exit?"));
    QCOMPARE(RunningWorkExitGuard::questionWhenSomeCannotStop(Intent::Exit, registered),
             QStringLiteral("Still running: %1 and Profile restore. Exit anyway?")
                 .arg(registered.mid(0, registered.size() - 1).join(QStringLiteral(", "))));
    for (const QString& text :
         {RunningWorkExitGuard::question(Intent::Exit, registered),
          RunningWorkExitGuard::question(Intent::Exit, {registered.first()})}) {
        QVERIFY2(!text.contains(QStringLiteral(" is running")) &&
                     !text.contains(QStringLiteral(" are running")) &&
                     !text.contains(QStringLiteral(" it ")) &&
                     !text.contains(QStringLiteral(" them ")),
                 qPrintable(text));
    }
}

// The live check: the format ended while the close prompt was open, and the batch's "complete"
// result then opened over a prompt still saying a full format was running. The prompt follows
// the work it asks about: a note that goes is taken out, and work that ends closes it.
void RunningWorkExitGuardTests::promptFollowsTheWorkItAsksAbout() {
    QWidget window;
    RunningWorkExitGuard guard(&window);
    FakeOperation apply(QStringLiteral("Partition changes"));
    apply.cannot_stop_note = QStringLiteral("Windows is carrying out a full format.");
    QString question_after_phase;
    QString informative_after_phase;
    QStringList buttons_after_phase;
    bool closed_when_finished = false;
    int step = 0;
    QTimer driver;
    driver.setInterval(kDialogPollMs);
    QElapsedTimer deadline;
    deadline.start();
    QObject::connect(&driver, &QTimer::timeout, [&]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (deadline.elapsed() > kDialogTimeoutMs) {
            driver.stop();
            if (box != nullptr) {
                box->reject();  // fail on what was expected, never hang in the box
            }
            return;
        }
        if (box == nullptr) {
            return;
        }
        if (step == 0) {
            // The full format ends; the batch goes on.
            apply.cannot_stop_note.clear();
            RunningOperations::notifyChanged();
            question_after_phase = box->text();
            informative_after_phase = box->informativeText();
            for (const QAbstractButton* shown : box->buttons()) {
                buttons_after_phase.append(shown->text().remove(QLatin1Char('&')));
            }
            step = 1;
            return;
        }
        // The batch ends and is about to show its result; its probe still says running.
        RunningOperations::notifyChanged(&apply);
        closed_when_finished = !box->isVisible();
        driver.stop();
        if (!closed_when_finished) {
            box->reject();
        }
    });
    driver.start();
    bool proceeded = false;
    QVERIFY(!guard.proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::Exit,
                                               [&proceeded]() { proceeded = true; }));
    QCOMPARE(question_after_phase,
             QStringLiteral("Still running: Partition changes. Cancel and exit?"));
    QCOMPARE(informative_after_phase, RunningWorkExitGuard::informativeText({}));
    QVERIFY2(buttons_after_phase.contains(QStringLiteral("Exit after cancelling")) &&
                 !buttons_after_phase.contains(QStringLiteral("Exit anyway")),
             qPrintable(buttons_after_phase.join(QStringLiteral(", "))));
    QVERIFY2(closed_when_finished, "the prompt goes before the finished work's result appears");
    QCOMPARE(apply.cancel_requests, 0);
    QVERIFY(!guard.isWaitingForCancellation());
    QTest::qWait(kDialogPollMs * 3);
    QVERIFY2(!proceeded, "work that ended on its own is not a confirmed exit");
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
