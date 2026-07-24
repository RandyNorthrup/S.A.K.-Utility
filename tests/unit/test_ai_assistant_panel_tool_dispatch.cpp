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
#include "sak/ai/ai_tool_loop_detector.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/openai_response_types.h"
#include "sak/ai_assistant_panel.h"

#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
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

    // sak_app_action "list" returns the seeded built-in action catalog with risk
    // flags, so the assistant can enumerate the app's OWN technician actions headless.
    void appActionListReturnsSeededCatalogWithRiskFlags() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")},
                        {QStringLiteral("action_id"), QString()},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("operation")).toString(), QStringLiteral("list"));
        QVERIFY(result.value(QStringLiteral("action_count")).toInt() >= 7);

        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found_verify = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QStringLiteral("action.verify_system_files")) {
                found_verify = true;
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found_verify);
    }

    // Running a mutating app action in a chat/research (no-execution) session is
    // refused by the per-action gate before the controller is ever touched.
    void appActionRunBlockedInChatResearchSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)

        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("action.verify_system_files")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});

        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // An unknown app action id is reported cleanly, never dispatched.
    void appActionRunRejectsUnknownId() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("action.does_not_exist")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("app_action_not_found"));
    }

    // W1a: the read-only technician ops are listed alongside the QuickActions with
    // read_only=true and mutating=false, so the per-action human gate is skipped.
    void readOnlyOpsAreListedAsReadOnly() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")},
                        {QStringLiteral("action_id"), QString()},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        // 7 built-in QuickActions + 4 read-only ops.
        QVERIFY(result.value(QStringLiteral("action_count")).toInt() >= 11);

        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        QSet<QString> read_only_ids;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("read_only")).toBool()) {
                QVERIFY(!action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                read_only_ids.insert(action.value(QStringLiteral("id")).toString());
            }
        }
        QVERIFY(read_only_ids.contains(QStringLiteral("partition.list_inventory")));
        QVERIFY(read_only_ids.contains(QStringLiteral("security.list_installed_programs")));
        QVERIFY(read_only_ids.contains(QStringLiteral("security.scan_vulnerabilities")));
        QVERIFY(read_only_ids.contains(QStringLiteral("imaging.identify_image")));
    }

    // W1a: identify_image drives the app's FileImageSource detection on a real
    // file -- deterministic, reads no system or network state.
    void identifyImageDetectsGzip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sample.img.gz"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray gz = QByteArray::fromHex("1f8b0800000000000003") +
                                  QByteArray(64, '\0');
            QCOMPARE(file.write(gz), static_cast<qint64>(gz.size()));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.identify_image")},
            {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"%1\"}").arg(path)}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("format")).toString(), QStringLiteral("gzip"));
        QVERIFY(data.value(QStringLiteral("is_compressed")).toBool());
        QVERIFY(data.value(QStringLiteral("extension_recognized")).toBool());
        QCOMPARE(data.value(QStringLiteral("detection")).toString(), QStringLiteral("extension"));
        QCOMPARE(static_cast<qint64>(data.value(QStringLiteral("size_bytes")).toDouble()),
                 static_cast<qint64>(74));
    }

    // W1a: a read-only op runs even in a chat/research session -- the per-action
    // gate is skipped because it changes nothing (never policy_blocked).
    void readOnlyOpRunsInChatResearchSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("blank.img"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(512, '\0')), static_cast<qint64>(512));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.identify_image")},
            {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"%1\"}").arg(path)}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));
    }

    // W1a: identify_image with no path fails cleanly (never dereferences).
    void identifyImageMissingPathFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.identify_image")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("path")));
    }

    // W1b: search.find_in_files drives the real AdvancedSearchController + worker
    // (async, on its own thread) through the AsyncActionInvocation bridge, end to
    // end and deterministically: a temp tree with a known token yields exactly the
    // expected matches, proving completion is delivered onto the worker thread and
    // drained by the local loop.
    void searchFindsMatchesInTempDir() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&](const QString& name, const QByteArray& content) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
        };
        write(QStringLiteral("a.txt"), "hello NEEDLE world\nsecond line\n");
        write(QStringLiteral("b.txt"), "no match here\nanother NEEDLE token\n");
        write(QStringLiteral("c.txt"), "totally unrelated content\n");

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QStringLiteral("{\"root_path\":\"%1\",\"pattern\":\"NEEDLE\"}").arg(dir.path());
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("search.find_in_files")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("total_matches")).toInt(), 2);
        const QJsonArray matches = data.value(QStringLiteral("matches")).toArray();
        QCOMPARE(matches.size(), 2);
        for (const auto& value : matches) {
            QVERIFY(value.toObject()
                        .value(QStringLiteral("line_content"))
                        .toString()
                        .contains(QStringLiteral("NEEDLE")));
        }
    }

    // W1b: an ungated read-only search must refuse UNC/network roots, so prompt
    // injection cannot steer it into an SMB/NTLM handshake with an attacker host.
    void searchRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        // Windows treats ANY two leading separators as a UNC/device root: cover
        // homogeneous (\\, //) and mixed (\/, /\) forms -- all must be refused.
        const QStringList roots = {QStringLiteral("\\\\attacker.example.com\\share"),
                                   QStringLiteral("\\/attacker.example.com\\share"),
                                   QStringLiteral("/\\attacker.example.com/share"),
                                   QStringLiteral("//attacker.example.com/share")};
        for (const QString& root : roots) {
            const QString args = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("root_path"), root},
                                          {QStringLiteral("pattern"), QStringLiteral("x")}})
                    .toJson(QJsonDocument::Compact));
            const QJsonObject result = panel.runAppActionTool(
                QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                            {QStringLiteral("action_id"), QStringLiteral("search.find_in_files")},
                            {QStringLiteral("arguments"), args}});
            QVERIFY2(!result.value(QStringLiteral("success")).toBool(), qPrintable(root));
            QVERIFY2(result.value(QStringLiteral("message"))
                         .toString()
                         .contains(QStringLiteral("network")),
                     qPrintable(root));
        }
    }

    // W1b: a search missing a required argument fails cleanly (never starts a worker).
    void searchMissingArgsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("search.find_in_files")},
                        {QStringLiteral("arguments"), QStringLiteral("{\"pattern\":\"x\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("root_path")));
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

    // The catastrophic gate must also cover WORKFLOW tool phases: a catastrophic
    // phase (disk format) requires an explicit human confirm even in Unattended,
    // mirroring the command-tool gate. Previously authorizeWorkflowToolPhase
    // ignored catastrophic_change, so such a phase got only a skippable restore
    // point offer in Unattended.
    void catastrophicWorkflowPhaseForcesConfirmEvenInUnattended() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::WorkflowPhase phase;
        phase.id = QStringLiteral("p1");
        phase.tool = QStringLiteral("run_powershell");
        AiAssistantPanel::WorkflowToolDispatchPlan plan;
        plan.policy = ai::AiToolPolicy::MutatingRequiresLease;
        plan.request.tool_name = QStringLiteral("run_powershell");
        // Classified catastrophic by the policy regex (disk format), so
        // evaluateToolPolicy sets decision.catastrophic_change.
        plan.request.command_preview = QStringLiteral("format c: /y");
        plan.ok = true;

        QVERIFY(panel.authorizeWorkflowToolPhase(phase, plan));
        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 1);
    }

    // A merely-risky (non-catastrophic) workflow phase in Unattended keeps the
    // low-friction path: no mandatory confirm, just the restore-point offer.
    void nonCatastrophicRiskyWorkflowPhaseInUnattendedSkipsConfirm() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::WorkflowPhase phase;
        phase.id = QStringLiteral("p2");
        phase.tool = QStringLiteral("run_powershell");
        AiAssistantPanel::WorkflowToolDispatchPlan plan;
        plan.policy = ai::AiToolPolicy::MutatingRequiresLease;
        plan.request.tool_name = QStringLiteral("run_powershell");
        plan.request.command_preview = QStringLiteral("Restart-Service Spooler");
        plan.phase_risky = true;  // gate is needed, but the command is not catastrophic
        plan.ok = true;

        QVERIFY(panel.authorizeWorkflowToolPhase(phase, plan));
        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 0);
        QCOMPARE(countTitles(titles, QStringLiteral("Create Restore Point")), 1);
    }

    // Teardown safety: once shutdown has begun, the per-phase modal gates must
    // decline immediately WITHOUT showing a modal. Otherwise a workflow worker
    // blocked marshaling an approval onto the GUI thread would have that modal
    // shown by drainWorkflowRun's event-loop pump and block on exec() forever
    // (no user answers during teardown). The approval hook stands in for the
    // modal, so a zero hook count proves no modal was ever opened.
    void shutdownDeclinesPerPhaseModalWithoutShowingIt() {
        AiAssistantPanel panel;
        QStringList titles;
        installApprovalHook(&titles);
        panel.m_shuttingDown = true;

        QVERIFY(!panel.confirmCommandWithUser(
            QStringLiteral("PowerShell"), QStringLiteral("format c: /y"), true, true));
        QVERIFY(!panel.offerRestorePointIfNeeded(QStringLiteral("format c: /y"), true, true));
        QCOMPARE(titles.size(), 0);
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

    // run_workflow must fail closed on a missing or unknown workflow id before it
    // commits to any orchestration.
    void runWorkflowToolValidatesWorkflowId() {
        AiAssistantPanel panel;

        const QJsonObject no_id = panel.runRunWorkflowTool(QJsonObject{});
        QCOMPARE(no_id.value(QStringLiteral("success")).toBool(), false);
        QVERIFY(no_id.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("workflow_id")));

        const QJsonObject unknown = panel.runRunWorkflowTool(
            QJsonObject{{QStringLiteral("workflow_id"), QStringLiteral("does_not_exist_xyz")}});
        QVERIFY(unknown.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("Unknown workflow_id")));
    }

    // A sub-agent must never be able to delegate to another sub-agent or launch a
    // workflow -- a structural backstop beyond the executor allowlist, so
    // orchestration can never recurse.
    void subagentCannotDelegateOrRunWorkflow() {
        AiAssistantPanel panel;
        ai::AiToolCallRequest request;
        request.tool_name = QStringLiteral("delegate_subagent");
        const QJsonObject delegated = panel.dispatchSubagentToolCall(
            ai::AiToolPolicy::ReadOnlyPc, request, QJsonObject{}, QStringLiteral("sub"));
        QVERIFY(delegated.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("may not delegate")));

        request.tool_name = QStringLiteral("run_workflow");
        const QJsonObject workflowed = panel.dispatchSubagentToolCall(
            ai::AiToolPolicy::ReadOnlyPc, request, QJsonObject{}, QStringLiteral("sub"));
        QVERIFY(workflowed.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("may not delegate")));
    }

    // A workflow phase likewise may not use the orchestration tools (an authored
    // template could otherwise recurse unboundedly).
    void workflowPhaseCannotUseOrchestrationTools() {
        AiAssistantPanel panel;
        ai::WorkflowPhase phase;
        phase.id = QStringLiteral("p1");
        phase.type = QStringLiteral("tool_action");
        phase.tool = QStringLiteral("run_workflow");
        ai::AiWorkflowPhaseContext context;
        const QJsonObject result =
            panel.dispatchWorkflowToolPhase(phase, ai::AiToolPolicy::ReadOnlyPc, context);
        QVERIFY(result.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("may not use")));
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

    // Wave 5: the loop guard breaks a stuck model that re-emits the SAME tool with
    // byte-identical arguments. With the default threshold of 4, the first three
    // identical calls dispatch to the handler; the fourth is refused (handler not
    // invoked) and a corrective output is fed back instead -- long before the flat
    // per-message turn cap would burn the whole budget.
    void loopGuardRefusesIdenticalRepeatedToolCall() {
        AiAssistantPanel panel;
        std::atomic<int> handler_calls{0};
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("sak_skill"),
            [&handler_calls](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                handler_calls.fetch_add(1);
                return QJsonObject{{QStringLiteral("success"), true}};
            });
        allowLocalToolsAndValidRun(panel);

        // One response carrying four identical sak_skill calls; the loop is
        // processed inline because sak_skill is a synchronous built-in.
        panel.beginToolTurn(repeatedSkillResponse(4, /*identical=*/true));

        QCOMPARE(handler_calls.load(), 3);  // 4th refused before dispatch
        QVERIFY(panel.m_toolLoopDetector.isLooping());
        QCOMPARE(panel.m_toolLoopDetector.maxRepeatCount(),
                 ai::AiToolLoopDetector::kDefaultRepeatThreshold);
    }

    // The guard keys on tool name PLUS arguments: distinct arguments are distinct
    // work and must never be mistaken for a loop, even well past the threshold.
    void loopGuardAllowsDistinctArgumentsPastThreshold() {
        AiAssistantPanel panel;
        std::atomic<int> handler_calls{0};
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("sak_skill"),
            [&handler_calls](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                handler_calls.fetch_add(1);
                return QJsonObject{{QStringLiteral("success"), true}};
            });
        allowLocalToolsAndValidRun(panel);

        panel.beginToolTurn(repeatedSkillResponse(6, /*identical=*/false));

        QCOMPARE(handler_calls.load(), 6);  // every distinct call dispatched
        QVERIFY(!panel.m_toolLoopDetector.isLooping());
    }

    // Regression (adversarial-review finding): an identical repeat of an
    // UNRECOGNIZED tool name is the most wasteful loop (no useful work at all), so
    // the guard must observe it too rather than letting the "Unknown function"
    // path run all the way to the 12-turn hard cap. The detector must therefore
    // trip even though the tool is never dispatched.
    void loopGuardCountsUnrecognizedRepeatedToolCall() {
        AiAssistantPanel panel;
        allowLocalToolsAndValidRun(panel);

        // "no_such_tool" maps to AiToolCallKind::Unknown, so every call takes the
        // unrecognized path; the loop guard sits ahead of that split now.
        panel.beginToolTurn(repeatedNamedResponse(QStringLiteral("no_such_tool"),
                                                  4,
                                                  /*identical=*/true));

        QVERIFY(panel.m_toolLoopDetector.isLooping());
        QCOMPARE(panel.m_toolLoopDetector.maxRepeatCount(),
                 ai::AiToolLoopDetector::kDefaultRepeatThreshold);
    }

private:
    // A single response carrying `count` calls of an arbitrary tool `name`. When
    // `identical`, every call has byte-identical arguments (trips the loop guard);
    // otherwise each carries a distinct index so no two signatures match.
    static ai::OpenAIResponseResult repeatedNamedResponse(const QString& name,
                                                          int count,
                                                          bool identical) {
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_named_1");
        for (int i = 0; i < count; ++i) {
            ai::OpenAIFunctionCall call;
            call.call_id = QStringLiteral("call_named_%1").arg(i);
            call.name = name;
            const QString value = identical ? QStringLiteral("same")
                                            : QStringLiteral("v_%1").arg(i);
            call.arguments_json = QStringLiteral("{\"x\":\"%1\"}").arg(value);
            response.function_calls.append(call);
        }
        return response;
    }

    // A single response carrying `count` sak_skill calls. When `identical`, every
    // call has byte-identical arguments (trips the loop guard); otherwise each
    // carries a distinct skill_id so no two signatures match.
    static ai::OpenAIResponseResult repeatedSkillResponse(int count, bool identical) {
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_loop_1");
        for (int i = 0; i < count; ++i) {
            ai::OpenAIFunctionCall call;
            call.call_id = QStringLiteral("call_loop_%1").arg(i);
            call.name = QStringLiteral("sak_skill");
            const QString skill_id = identical ? QStringLiteral("same")
                                               : QStringLiteral("id_%1").arg(i);
            call.arguments_json =
                QStringLiteral("{\"operation\":\"load\",\"skill_id\":\"%1\"}").arg(skill_id);
            response.function_calls.append(call);
        }
        return response;
    }

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
