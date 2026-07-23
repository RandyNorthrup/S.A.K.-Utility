// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_assistant_panel_tool_dispatch.cpp
/// @brief Certification test for P10-04: built-in AI tool handlers that block
/// (download/package/offline/provider-gateway) must run off the GUI thread so
/// the GUI stays responsive and Stop can cancel. Drives the real AiAssistantPanel
/// tool loop with an injected blocking handler and inspects the private async
/// state via a friend seam.

#include "sak/ai/ai_async_tool_runner.h"
#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_command_tool_planner.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/openai_response_types.h"
#include "sak/ai_assistant_panel.h"

#include <QComboBox>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <memory>

namespace sak {

// Friend of AiAssistantPanel: reaches the private tool dispatcher, run token, and
// async-in-flight state so the certification can drive and observe the real loop.
class AiAssistantPanelToolDispatchTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        // Keep the panel's stores out of the real user data directory.
        QStandardPaths::setTestModeEnabled(true);
    }

    // A download_file call whose handler blocks must run off the GUI thread: the
    // handler enters on a non-GUI thread, the GUI event loop keeps running, and
    // the async-in-flight flag is set. Releasing the handler drives the completion
    // back onto the GUI thread and advances the tool turn.
    void blockingBuiltInRunsOffGuiThreadAndCompletes() {
        AiAssistantPanel panel;
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};
        QSemaphore gate;

        installBlockingDownloadHandler(panel, &entered, &handler_thread, &gate);
        allowLocalToolsAndValidRun(panel);

        QSignalSpy finished_spy(panel.m_asyncToolRunner.get(), &ai::AiAsyncToolRunner::finished);
        panel.beginToolTurn(downloadResponse());

        // GUI thread was not blocked: the event loop still runs and the handler
        // enters on a different thread.
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);
        QVERIFY(handler_thread.load() != QThread::currentThreadId());
        QVERIFY(panel.m_asyncToolInFlight);
        QVERIFY(panel.m_asyncToolRunner->isRunning());
        QVERIFY(panel.m_toolTurn.active());

        // Let the handler finish: completion marshals back to the GUI thread,
        // clears the in-flight flag and records the completed tool.
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_asyncToolInFlight, 5000);
        QCOMPARE(finished_spy.count(), 1);
        QCOMPARE(panel.m_runState.completed_tools, 1);
    }

    // Stop while a blocking handler is mid-flight must cancel: the run token is
    // cancelled and the runner is detached, so the late result is dropped -- the
    // tool turn does not advance and no completion is delivered.
    void stopCancelsInFlightBlockingTool() {
        AiAssistantPanel panel;
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};
        QSemaphore gate;

        installBlockingDownloadHandler(panel, &entered, &handler_thread, &gate);
        allowLocalToolsAndValidRun(panel);

        QSignalSpy finished_spy(panel.m_asyncToolRunner.get(), &ai::AiAsyncToolRunner::finished);
        panel.beginToolTurn(downloadResponse());
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);

        panel.onStopClicked();
        QVERIFY(panel.m_activeToolRunToken.isValid());
        QVERIFY(panel.m_activeToolRunToken.isCancellationRequested());
        QVERIFY(!panel.m_asyncToolInFlight);
        QVERIFY(!panel.m_toolTurn.active());

        // Release the (now detached) handler and confirm the stale result is
        // dropped: finished() is never delivered and no tool is recorded.
        gate.release();
        QTest::qWait(300);
        QCOMPARE(finished_spy.count(), 0);
        QCOMPARE(panel.m_runState.completed_tools, 0);
    }

    // Destroying the panel while an async built-in tool is still running must not
    // dead-lock or use-after-free: the destructor drains the worker (which
    // captured `this`) before members are torn down.
    void destroyingPanelWhileToolRunsDrainsCleanly() {
        auto panel = std::make_unique<AiAssistantPanel>();
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};

        // Handler ignores any gate and just sleeps briefly, so the worker is
        // genuinely in-flight when the panel is destroyed.
        panel->m_toolDispatcher->registerHandler(
            QStringLiteral("download_file"),
            [&entered, &handler_thread](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                handler_thread = QThread::currentThreadId();
                entered = true;
                QThread::msleep(200);
                return QJsonObject{{QStringLiteral("success"), true}};
            });
        allowLocalToolsAndValidRun(*panel);
        panel->beginToolTurn(downloadResponse());
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);
        QVERIFY(panel->m_asyncToolInFlight);

        // Destroy mid-flight; must return without hanging or crashing.
        panel.reset();
        QVERIFY(true);
    }

    void cleanup() {
        // Never leave the approval seam installed for the next case or production.
        aiApprovalPromptTestHook() = nullptr;
    }

    // Wave 1b: a catastrophic/irreversible command must get an explicit human
    // confirmation gate even in Unattended mode -- it cannot silently auto-run.
    void catastrophicCommandForcesConfirmEvenInUnattended() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::OpenAIFunctionOutput output;
        const bool proceeded =
            panel.authorizeCommandForAccessMode(ctx, makePlan(true, true), &output);

        QVERIFY(proceeded);
        QVERIFY(countTitles(titles, QStringLiteral("Approve AI Command")) == 1);
        QVERIFY(countTitles(titles, QStringLiteral("Create Restore Point")) == 1);
    }

    // A merely-risky (non-catastrophic) command in Unattended mode keeps the
    // low-friction behavior: no mandatory confirm, and the restore point is
    // offered only once per session.
    void nonCatastrophicRiskyInUnattendedSkipsConfirmAndOffersOnce() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::OpenAIFunctionOutput output;
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, false), &output));
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, false), &output));

        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 0);
        QCOMPARE(countTitles(titles, QStringLiteral("Create Restore Point")), 1);
    }

    // The once-per-session restore-point dedup must NOT suppress the offer for
    // catastrophic commands: a single early restore point cannot be allowed to
    // leave later disk wipes unprotected.
    void catastrophicCommandReOffersRestorePointDespiteSessionFlag() {
        AiAssistantPanel panel;
        setUnattended(panel);
        panel.m_restorePointOfferedThisSession = true;  // an earlier op already offered
        QStringList titles;
        installApprovalHook(&titles);

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::OpenAIFunctionOutput output;
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, true), &output));
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, true), &output));

        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 2);
        QCOMPARE(countTitles(titles, QStringLiteral("Create Restore Point")), 2);
    }

private:
    // Unattended Full Access (combo index 2) -- the mode where risky commands run
    // without a per-command confirm unless the new catastrophic gate fires.
    static void setUnattended(AiAssistantPanel& panel) {
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(2);
    }

    static ai::AiCommandToolPlan makePlan(bool risky, bool catastrophic) {
        ai::AiCommandToolPlan plan;
        plan.shell_label = QStringLiteral("PowerShell");
        plan.preview = catastrophic ? QStringLiteral("format C: /y")
                                    : QStringLiteral("choco upgrade all -y");
        plan.risky_change = risky;
        plan.policy_decision.allowed = true;
        plan.policy_decision.risky_change = risky;
        plan.policy_decision.catastrophic_change = catastrophic;
        return plan;
    }

    // Scripts the modal approval seam: approve command confirmations, and choose
    // "Proceed Without" for restore-point offers (records the title, never creates
    // a real restore point). Records every prompt title it is shown.
    static void installApprovalHook(QStringList* titles) {
        aiApprovalPromptTestHook() = [titles](const QString& title, const QString&) -> int {
            titles->append(title);
            if (title.contains(QStringLiteral("Approve AI Command"))) {
                return 0;  // ApprovalPromptChoice::Accept
            }
            if (title.contains(QStringLiteral("Create Restore Point"))) {
                return 1;  // ApprovalPromptChoice::Secondary (Proceed Without)
            }
            return 3;      // ApprovalPromptChoice::Cancel
        };
    }

    static int countTitles(const QStringList& titles, const QString& needle) {
        int count = 0;
        for (const auto& title : titles) {
            if (title.contains(needle)) {
                ++count;
            }
        }
        return count;
    }

    static ai::OpenAIResponseResult downloadResponse() {
        ai::OpenAIFunctionCall call;
        call.call_id = QStringLiteral("call_1");
        call.name = QStringLiteral("download_file");
        call.arguments_json =
            QStringLiteral("{\"url\":\"https://example.com/f\",\"filename\":\"f\"}");
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_1");
        response.function_calls.append(call);
        return response;
    }

    // Assisted Full Access (combo index 1) allows local tools with a lease; a
    // valid root run token makes Stop/cancel meaningful.
    static void allowLocalToolsAndValidRun(AiAssistantPanel& panel) {
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(1);
        panel.m_runToken = ai::CancellationToken::createRoot(QStringLiteral("test_run"));
    }

    // Replace the real download handler with one that blocks on the test's gate,
    // recording the thread it ran on.
    static void installBlockingDownloadHandler(AiAssistantPanel& panel,
                                               std::atomic<bool>* entered,
                                               std::atomic<Qt::HANDLE>* handler_thread,
                                               QSemaphore* gate) {
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("download_file"),
            [entered, handler_thread, gate](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                *handler_thread = QThread::currentThreadId();
                *entered = true;
                gate->acquire();
                return QJsonObject{{QStringLiteral("success"), true},
                                   {QStringLiteral("path"), QStringLiteral("f")}};
            });
    }
};

}  // namespace sak

QTEST_MAIN(sak::AiAssistantPanelToolDispatchTest)
#include "test_ai_assistant_panel_tool_dispatch.moc"
