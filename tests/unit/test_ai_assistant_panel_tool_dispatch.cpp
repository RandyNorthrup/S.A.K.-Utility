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
#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_skill.h"
#include "sak/ai/ai_skill_store.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_health_ledger.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/openai_response_types.h"
#include "sak/ai_assistant_panel.h"

#include <QComboBox>
#include <QJsonArray>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <memory>

namespace sak {

// Deterministic stand-in for the delegate sub-agent's model client. Records that
// it was invoked (on the async worker) and returns a complete result, so the
// delegate_subagent cert never touches the network.
class FakeSubagentModelClient : public ai::IAiModelClient {
public:
    explicit FakeSubagentModelClient(std::atomic<bool>* invoked) : m_invoked(invoked) {}

    Response invoke(const Request& /*request*/, const ai::CancellationToken& /*token*/) override {
        if (m_invoked != nullptr) {
            m_invoked->store(true);
        }
        Response response;
        response.success = true;
        response.text =
            QStringLiteral("{\"status\":\"complete\",\"summary\":\"delegated task done\"}");
        response.usage.total_tokens = 10;
        return response;
    }

private:
    std::atomic<bool>* m_invoked{nullptr};
};

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

    // Wave 2: repeated executor faults (hangs) trip the health circuit breaker for
    // a command tool, and the raw-command health gate then suppresses new calls
    // instead of launching into a known-bad executor.
    void healthCircuitBreakerSuppressesCommandTool() {
        AiAssistantPanel panel;
        resetHealthLedger(panel);
        const QString key = AiAssistantPanel::commandHealthKey(QStringLiteral("run_powershell"));
        panel.m_currentCommandHealthKey = key;

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::AiCommandToolPlan plan;
        plan.policy_request.tool_name = QStringLiteral("run_powershell");

        // Healthy to start: the gate lets the command through.
        ai::OpenAIFunctionOutput allow_output;
        QVERIFY(panel.applyCommandHealthGate(ctx, plan, &allow_output));

        // Three executor hangs trip the breaker...
        ai::AiCommandResult timed_out;
        timed_out.started = true;
        timed_out.timed_out = true;
        for (int i = 0; i < 3; ++i) {
            panel.recordCommandHealth(timed_out);
        }
        QVERIFY(!panel.m_toolHealthLedger->check(key).available);

        // ...and the gate now suppresses new calls (returns false).
        ai::OpenAIFunctionOutput deny_output;
        QVERIFY(!panel.applyCommandHealthGate(ctx, plan, &deny_output));
    }

    // A command that ran to completion is a healthy executor even if the command
    // itself exited nonzero -- that must not trip the breaker.
    void nonZeroExitKeepsCommandToolHealthy() {
        AiAssistantPanel panel;
        resetHealthLedger(panel);
        const QString key = AiAssistantPanel::commandHealthKey(QStringLiteral("run_cmd"));
        panel.m_currentCommandHealthKey = key;

        ai::AiCommandResult ran;
        ran.started = true;
        ran.exit_code = 1;
        for (int i = 0; i < 5; ++i) {
            panel.recordCommandHealth(ran);
        }
        QVERIFY(panel.m_toolHealthLedger->check(key).available);
    }

    // A user Stop is not an executor-health signal; cancels never trip the breaker.
    void cancelledCommandDoesNotTripBreaker() {
        AiAssistantPanel panel;
        resetHealthLedger(panel);
        const QString key = AiAssistantPanel::commandHealthKey(QStringLiteral("run_cmd"));
        panel.m_currentCommandHealthKey = key;

        ai::AiCommandResult cancelled;
        cancelled.started = true;
        cancelled.cancelled = true;
        for (int i = 0; i < 5; ++i) {
            panel.recordCommandHealth(cancelled);
        }
        QVERIFY(panel.m_toolHealthLedger->check(key).available);
    }

    // Wave 3: the sak_skill tool lists the catalog (no bodies), loads a full body
    // on demand by id, and reports an error for an unknown id.
    void skillToolListsLoadsAndRejectsUnknown() {
        AiAssistantPanel panel;
        QVERIFY(panel.m_skillStore != nullptr);
        const ai::Skill skill = ai::Skill::fromMarkdown(
            QByteArray("---\nid: cert-skill\ndescription: cert desc.\nwhen_to_use: x\n---\n"
                       "# Cert Skill\nthe full guidance body\n"),
            QStringLiteral("/x/cert-skill.md"));
        QVERIFY(panel.m_skillStore->addSkill(skill));

        const QJsonObject list =
            panel.runSkillTool(QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")},
                                           {QStringLiteral("skill_id"), QString()}});
        QVERIFY(list.value(QStringLiteral("success")).toBool());
        const QJsonArray catalog = list.value(QStringLiteral("skills")).toArray();
        bool found = false;
        for (const auto& entry : catalog) {
            const QJsonObject obj = entry.toObject();
            if (obj.value(QStringLiteral("id")).toString() == QLatin1String("cert-skill")) {
                found = true;
                QVERIFY(!obj.contains(QStringLiteral("body")));  // list never leaks bodies
            }
        }
        QVERIFY(found);

        const QJsonObject loaded = panel.runSkillTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("load")},
                        {QStringLiteral("skill_id"), QStringLiteral("cert-skill")}});
        QVERIFY(loaded.value(QStringLiteral("success")).toBool());
        QVERIFY(loaded.value(QStringLiteral("body"))
                    .toString()
                    .contains(QStringLiteral("the full guidance body")));

        const QJsonObject unknown = panel.runSkillTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("load")},
                        {QStringLiteral("skill_id"), QStringLiteral("does-not-exist")}});
        QVERIFY(!unknown.value(QStringLiteral("success")).toBool());
    }

    // End-to-end regression: a model-issued sak_skill call must travel the real
    // chat tool loop (router classify -> dispatchBuiltInToolCall -> dispatcher ->
    // handler). If the router misclassifies sak_skill as Unknown, the loop answers
    // "Unknown function" and the handler never runs -- so asserting the handler
    // fired proves the whole chain is wired, which the direct-call test cannot.
    // The model's delegate_subagent tool must route through the real chat tool
    // loop (router -> dispatcher -> handler) and actually run a scoped sub-agent.
    // Uses an injected fake model client so no network is touched.
    void delegateSubagentToolSpawnsScopedSubagentThroughRealLoop() {
        std::atomic<bool> subagent_invoked{false};
        AiAssistantPanel panel;
        panel.m_delegateSubagentModelFactoryOverride = [&subagent_invoked]() {
            return std::make_unique<FakeSubagentModelClient>(&subagent_invoked);
        };
        allowLocalToolsAndValidRun(panel);

        panel.beginToolTurn(delegateResponse());

        // delegate_subagent is an async built-in: it dispatched off the GUI thread,
        // ran the sub-agent runner, and invoked the sub-agent's (fake) model client.
        QTRY_VERIFY_WITH_TIMEOUT(subagent_invoked.load(), 5000);
    }

    void skillToolReachesHandlerThroughRealToolLoop() {
        AiAssistantPanel panel;
        std::atomic<bool> handler_ran{false};
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("sak_skill"),
            [&handler_ran](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
                handler_ran = true;
                return QJsonObject{{QStringLiteral("success"), true},
                                   {QStringLiteral("operation"),
                                    args.value(QStringLiteral("operation")).toString()}};
            });
        allowLocalToolsAndValidRun(panel);

        panel.beginToolTurn(skillResponse());

        // sak_skill is a synchronous built-in, dispatched inline in the tool loop.
        QTRY_VERIFY_WITH_TIMEOUT(handler_ran.load(), 5000);
    }

private:
    // Give the panel a fresh, non-persistent health ledger so a prior run's
    // persisted circuit state (test-mode data dir) cannot leak into these breaker
    // assertions. applyCommandHealthGate/recordCommandHealth use m_toolHealthLedger.
    static void resetHealthLedger(AiAssistantPanel& panel) {
        panel.m_toolHealthLedger = std::make_unique<ai::AiToolHealthLedger>();
    }

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

    static ai::OpenAIResponseResult skillResponse() {
        ai::OpenAIFunctionCall call;
        call.call_id = QStringLiteral("call_skill_1");
        call.name = QStringLiteral("sak_skill");
        call.arguments_json = QStringLiteral("{\"operation\":\"list\",\"skill_id\":\"\"}");
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_skill_1");
        response.function_calls.append(call);
        return response;
    }

    static ai::OpenAIResponseResult delegateResponse() {
        ai::OpenAIFunctionCall call;
        call.call_id = QStringLiteral("call_delegate_1");
        call.name = QStringLiteral("delegate_subagent");
        call.arguments_json = QStringLiteral(
            "{\"objective\":\"check disk space\",\"tool_policy\":\"read_only_pc\","
            "\"expected_output\":\"summary\"}");
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_delegate_1");
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
