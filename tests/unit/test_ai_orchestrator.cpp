// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_orchestrator.h"
#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/ai_workflow_template.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>

namespace {

class FakeModelClient : public sak::ai::IAiModelClient {
public:
    Response invoke(const Request& request, const sak::ai::CancellationToken& token) override {
        QMutexLocker lock(&m_mutex);
        ++concurrent_in_flight;
        if (concurrent_in_flight > peak_concurrent) {
            peak_concurrent = concurrent_in_flight;
        }
        last_objective = request.objective;
        Response response;
        if (cancel_on_invoke && token.isValid()) {
            token.cancel(QStringLiteral("test_cancelled"));
        }
        if (response_overrides.contains(request.objective)) {
            response = response_overrides.value(request.objective);
        } else {
            response = default_response;
        }
        lock.unlock();
        if (sleep_ms > 0) {
            QThread::msleep(sleep_ms);
        }
        QMutexLocker drop(&m_mutex);
        --concurrent_in_flight;
        return response;
    }

    int sleep_ms{0};
    int concurrent_in_flight{0};
    int peak_concurrent{0};
    QString last_objective;
    Response default_response;
    QHash<QString, Response> response_overrides;
    bool cancel_on_invoke{false};

private:
    QMutex m_mutex;
};

class FakeToolExecutor : public sak::ai::IAiToolExecutor {
public:
    QJsonObject runToolPhase(const sak::ai::WorkflowPhase& phase,
                             sak::ai::AiToolPolicy /*policy*/,
                             const sak::ai::AiWorkflowPhaseContext& context,
                             const sak::ai::CancellationToken& /*token*/) override {
        ++calls;
        last_phase_id = phase.id;
        last_context = context;
        if (result_sequences.contains(phase.id)) {
            auto& sequence = result_sequences[phase.id];
            if (!sequence.isEmpty()) {
                const QJsonObject next = sequence.takeFirst();
                return next;
            }
        }
        if (results.contains(phase.id)) {
            return results.value(phase.id);
        }
        QJsonObject ok;
        ok[QStringLiteral("success")] = true;
        return ok;
    }

    int calls{0};
    QString last_phase_id;
    sak::ai::AiWorkflowPhaseContext last_context;
    QHash<QString, QJsonObject> results;
    QHash<QString, QVector<QJsonObject>> result_sequences;
};

sak::ai::IAiModelClient::Response makeJsonResponse(const QString& status, const QString& summary) {
    sak::ai::IAiModelClient::Response response;
    response.success = true;
    QJsonObject payload;
    payload[QStringLiteral("status")] = status;
    payload[QStringLiteral("summary")] = summary;
    response.text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    response.usage.total_tokens = 100;
    return response;
}

sak::ai::WorkflowAgent makeAgent(const QString& id, const QString& policy) {
    sak::ai::WorkflowAgent agent;
    agent.id = id;
    agent.tool_policy = policy;
    agent.token_budget = 8000;
    return agent;
}

sak::ai::WorkflowPhase makeDelegate(const QString& id, const QString& agent_id) {
    sak::ai::WorkflowPhase phase;
    phase.id = id;
    phase.type = QStringLiteral("delegate");
    phase.agent = agent_id;
    phase.prompt = QStringLiteral("Do %1").arg(id);
    return phase;
}

}  // namespace

class AiOrchestratorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runsAllPhasesSequentially();
    void groupsReadOnlyDelegatePhasesInParallel();
    void serializesMutatingPhases();
    void skipsPhaseWhenConditionUnmet();
    void runsFallbackWhenPriorPhaseFlagged();
    void stopsOnPhaseFailureWhenConfigured();
    void cancelsRunWhenRootTokenCancelled();
    void overseerHandlerInvoked();
    void toolPhaseUsesExecutor();
    void cleanupPhaseAlwaysRunsAfterFailure();
    void cleanupFailureRecordedButRunCompletes();
    void retriesTransientToolFailureOnce();
    void continuesDegradedToFallbackOnOfflineDownloaderFailure();
    void waitsForHumanOnRiskyMutationFailure();
    void resumesAfterHumanGateWithoutRerunningPriorPhase();
    void reassignsCriticFailureToOverseer();
    void emitsPhaseStartedBeforeCompletion();
};

void AiOrchestratorTests::runsAllPhasesSequentially() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("seq");
    workflow.agents << makeAgent(QStringLiteral("package_agent"),
                                 QStringLiteral("package_tools_only"));
    workflow.phases << makeDelegate(QStringLiteral("p1"), QStringLiteral("package_agent"));
    workflow.phases << makeDelegate(QStringLiteral("p2"), QStringLiteral("package_agent"));

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run1"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run1"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.phases.size(), 2);
    QVERIFY(result.phases[0].success);
    QVERIFY(result.phases[1].success);
    QCOMPARE(result.parallel_groups_executed, 0);
}

void AiOrchestratorTests::groupsReadOnlyDelegatePhasesInParallel() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    model.sleep_ms = 60;
    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);
    sak::ai::AiOrchestrationOptions options;
    options.max_parallel_subagents = 3;
    orchestrator.setOptions(options);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("parallel");
    workflow.agents << makeAgent(QStringLiteral("diagnostic_agent"),
                                 QStringLiteral("read_only_pc"));
    workflow.phases << makeDelegate(QStringLiteral("ro1"), QStringLiteral("diagnostic_agent"));
    workflow.phases << makeDelegate(QStringLiteral("ro2"), QStringLiteral("diagnostic_agent"));
    workflow.phases << makeDelegate(QStringLiteral("ro3"), QStringLiteral("diagnostic_agent"));

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run2"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run2"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.phases.size(), 3);
    QCOMPARE(result.parallel_groups_executed, 1);
    QVERIFY2(model.peak_concurrent >= 2,
             qPrintable(
                 QStringLiteral("expected >= 2 concurrent, got %1").arg(model.peak_concurrent)));
}

void AiOrchestratorTests::serializesMutatingPhases() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    model.sleep_ms = 40;
    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);
    sak::ai::AiOrchestrationOptions options;
    options.max_parallel_subagents = 4;
    orchestrator.setOptions(options);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("serialize");
    workflow.agents << makeAgent(QStringLiteral("repair_agent"),
                                 QStringLiteral("mutating_requires_lease"));
    workflow.phases << makeDelegate(QStringLiteral("mut1"), QStringLiteral("repair_agent"));
    workflow.phases << makeDelegate(QStringLiteral("mut2"), QStringLiteral("repair_agent"));

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run3"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run3"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.parallel_groups_executed, 0);
    QCOMPARE(model.peak_concurrent, 1);
}

void AiOrchestratorTests::skipsPhaseWhenConditionUnmet() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("cond");
    workflow.agents << makeAgent(QStringLiteral("a"), QStringLiteral("read_only_pc"));
    workflow.phases << makeDelegate(QStringLiteral("first"), QStringLiteral("a"));
    sak::ai::WorkflowPhase fallback = makeDelegate(QStringLiteral("fallback"), QStringLiteral("a"));
    fallback.condition = QStringLiteral("first_failed");
    workflow.phases << fallback;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run4"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run4"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.phases.size(), 2);
    QVERIFY(result.phases[0].success);
    QVERIFY(result.phases[1].skipped);
    QVERIFY(result.phases[1].skip_reason.contains(QStringLiteral("first_failed")));
}

void AiOrchestratorTests::runsFallbackWhenPriorPhaseFlagged() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    sak::ai::IAiModelClient::Response fail_response;
    fail_response.success = true;
    QJsonObject fail_payload;
    fail_payload[QStringLiteral("status")] = QStringLiteral("failed");
    fail_payload[QStringLiteral("summary")] = QStringLiteral("nope");
    fail_response.text =
        QString::fromUtf8(QJsonDocument(fail_payload).toJson(QJsonDocument::Compact));
    fail_response.usage.total_tokens = 10;
    model.response_overrides.insert(QStringLiteral("Do first"), fail_response);

    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);
    sak::ai::AiOrchestrationOptions options;
    options.stop_on_phase_failure = false;
    orchestrator.setOptions(options);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("cond_run");
    workflow.agents << makeAgent(QStringLiteral("a"), QStringLiteral("read_only_pc"));
    workflow.phases << makeDelegate(QStringLiteral("first"), QStringLiteral("a"));
    sak::ai::WorkflowPhase fallback = makeDelegate(QStringLiteral("fallback"), QStringLiteral("a"));
    fallback.condition = QStringLiteral("first_failed");
    workflow.phases << fallback;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run5"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run5"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.phases.size(), 2);
    QVERIFY(!result.phases[0].success);
    QVERIFY(result.phases[1].ran);
    QVERIFY(!result.phases[1].skipped);
    QVERIFY(result.flags.contains(QStringLiteral("first_failed")));
}

void AiOrchestratorTests::stopsOnPhaseFailureWhenConfigured() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    sak::ai::IAiModelClient::Response broken;
    broken.success = false;
    broken.error_message = QStringLiteral("HTTP 500");
    model.response_overrides.insert(QStringLiteral("Do p1"), broken);

    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("stop");
    workflow.agents << makeAgent(QStringLiteral("a"), QStringLiteral("package_tools_only"));
    workflow.phases << makeDelegate(QStringLiteral("p1"), QStringLiteral("a"));
    workflow.phases << makeDelegate(QStringLiteral("p2"), QStringLiteral("a"));

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run6"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run6"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Failed);
    QCOMPARE(result.phases.size(), 1);
    QVERIFY(result.error_message.contains(QStringLiteral("HTTP 500")));
    const auto recovery =
        result.phases.first().metadata.value(QStringLiteral("recovery_decision")).toObject();
    QCOMPARE(recovery.value(QStringLiteral("action")).toString(), QStringLiteral("abort"));
    QVERIFY(!recovery.value(QStringLiteral("reason")).toString().isEmpty());
}

void AiOrchestratorTests::cancelsRunWhenRootTokenCancelled() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    model.cancel_on_invoke = true;
    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("cancel");
    workflow.agents << makeAgent(QStringLiteral("a"), QStringLiteral("package_tools_only"));
    workflow.phases << makeDelegate(QStringLiteral("p1"), QStringLiteral("a"));
    workflow.phases << makeDelegate(QStringLiteral("p2"), QStringLiteral("a"));

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run7"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run7"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Cancelled);
    QVERIFY(result.error_message.contains(QStringLiteral("cancel")));
}

void AiOrchestratorTests::overseerHandlerInvoked() {
    FakeModelClient model;
    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);
    std::atomic<int> handler_calls{0};
    orchestrator.setOverseerHandler(
        [&handler_calls](const sak::ai::WorkflowPhase&, const sak::ai::CancellationToken&) {
            ++handler_calls;
            QJsonObject ok;
            ok[QStringLiteral("checked")] = true;
            return ok;
        });

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("overseer");
    sak::ai::WorkflowPhase phase;
    phase.id = QStringLiteral("clarify");
    phase.type = QStringLiteral("overseer");
    workflow.phases << phase;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run8"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run8"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(handler_calls.load(), 1);
    QVERIFY(result.phases.first().success);
}

void AiOrchestratorTests::toolPhaseUsesExecutor() {
    FakeModelClient model;
    FakeToolExecutor tool;
    QJsonObject tool_ok;
    tool_ok[QStringLiteral("success")] = true;
    tool_ok[QStringLiteral("artifact")] = QStringLiteral("a.zip");
    tool.results.insert(QStringLiteral("download"), tool_ok);

    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);
    sak::ai::AiOrchestrationOptions options;
    options.user_message = QStringLiteral("download an offline installer for vlc");
    options.input_values[QStringLiteral("app_name")] = QStringLiteral("vlc");
    orchestrator.setOptions(options);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("tool");
    sak::ai::WorkflowPhase phase;
    phase.id = QStringLiteral("download");
    phase.type = QStringLiteral("tool_action");
    phase.tool = QStringLiteral("sak_offline_downloader");
    phase.operation = QStringLiteral("direct_download");
    workflow.phases << phase;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run9"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run9"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(tool.calls, 1);
    QCOMPARE(tool.last_phase_id, QStringLiteral("download"));
    QCOMPARE(tool.last_context.user_message,
             QStringLiteral("download an offline installer for vlc"));
    QCOMPARE(tool.last_context.input_values.value(QStringLiteral("app_name")).toString(),
             QStringLiteral("vlc"));
    QCOMPARE(tool.last_context.workflow_id, QStringLiteral("tool"));
    QCOMPARE(tool.last_context.run_id, QStringLiteral("run9"));
    QCOMPARE(result.phases.first().tool_result.value(QStringLiteral("artifact")).toString(),
             QStringLiteral("a.zip"));
}

void AiOrchestratorTests::cleanupPhaseAlwaysRunsAfterFailure() {
    FakeModelClient model;
    sak::ai::IAiModelClient::Response fail;
    fail.success = false;
    fail.error_message = QStringLiteral("HTTP 500");
    model.response_overrides.insert(QStringLiteral("Do work"), fail);

    FakeToolExecutor tool;
    QJsonObject cleanup_ok;
    cleanup_ok[QStringLiteral("success")] = true;
    cleanup_ok[QStringLiteral("cleaned")] = true;
    tool.results.insert(QStringLiteral("cleanup"), cleanup_ok);

    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("always_run");
    workflow.agents << makeAgent(QStringLiteral("a"), QStringLiteral("package_tools_only"));
    workflow.phases << makeDelegate(QStringLiteral("work"), QStringLiteral("a"));
    workflow.phases.last().prompt = QStringLiteral("Do work");
    sak::ai::WorkflowPhase cleanup;
    cleanup.id = QStringLiteral("cleanup");
    cleanup.type = QStringLiteral("cleanup");
    cleanup.always_run = true;
    workflow.phases << cleanup;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_cleanup"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run_cleanup"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Failed);
    QCOMPARE(result.phases.size(), 2);
    QCOMPARE(result.phases[1].phase_id, QStringLiteral("cleanup"));
    QVERIFY(result.phases[1].success);
    QCOMPARE(tool.calls, 1);
}

void AiOrchestratorTests::cleanupFailureRecordedButRunCompletes() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));

    FakeToolExecutor tool;
    QJsonObject bad_cleanup;
    bad_cleanup[QStringLiteral("success")] = false;
    bad_cleanup[QStringLiteral("error_message")] = QStringLiteral("locked file");
    tool.results.insert(QStringLiteral("cleanup"), bad_cleanup);

    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);
    sak::ai::AiOrchestrationOptions opts;
    opts.stop_on_phase_failure = false;
    orchestrator.setOptions(opts);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("cleanup_fail");
    workflow.agents << makeAgent(QStringLiteral("a"), QStringLiteral("read_only_pc"));
    workflow.phases << makeDelegate(QStringLiteral("work"), QStringLiteral("a"));
    sak::ai::WorkflowPhase cleanup;
    cleanup.id = QStringLiteral("cleanup");
    cleanup.type = QStringLiteral("cleanup");
    cleanup.always_run = true;
    workflow.phases << cleanup;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_cleanup_fail"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run_cleanup_fail"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.cleanup_failures.size(), 1);
    QCOMPARE(result.cleanup_failures.first(), QStringLiteral("cleanup"));
    const auto recovery =
        result.phases.last().metadata.value(QStringLiteral("recovery_decision")).toObject();
    QCOMPARE(recovery.value(QStringLiteral("action")).toString(),
             QStringLiteral("continue_degraded"));
    QVERIFY(recovery.value(QStringLiteral("safe_to_continue")).toBool(false));
}

void AiOrchestratorTests::retriesTransientToolFailureOnce() {
    FakeModelClient model;
    FakeToolExecutor tool;

    QJsonObject transient_failure;
    transient_failure[QStringLiteral("success")] = false;
    transient_failure[QStringLiteral("error_message")] = QStringLiteral("Connection closed");
    QJsonObject retry_success;
    retry_success[QStringLiteral("success")] = true;
    retry_success[QStringLiteral("artifact")] = QStringLiteral("vlc-offline.exe");
    tool.result_sequences.insert(QStringLiteral("download"), {transient_failure, retry_success});

    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("retry_download");
    sak::ai::WorkflowPhase phase;
    phase.id = QStringLiteral("download");
    phase.type = QStringLiteral("tool_action");
    phase.tool = QStringLiteral("sak_offline_downloader");
    phase.operation = QStringLiteral("direct_download");
    phase.risk = QStringLiteral("download_only");
    workflow.phases << phase;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_retry"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run_retry"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(tool.calls, 2);
    QCOMPARE(result.phases.size(), 1);
    QVERIFY(result.phases.first().success);
    QCOMPARE(result.phases.first().metadata.value(QStringLiteral("retry_count")).toInt(), 1);
    QVERIFY(result.phases.first().metadata.contains(QStringLiteral("first_failure")));
    QVERIFY(result.phases.first().metadata.value(QStringLiteral("recovered")).toBool(false));
    const auto recovery =
        result.phases.first().metadata.value(QStringLiteral("recovery_decision")).toObject();
    QCOMPARE(recovery.value(QStringLiteral("action")).toString(), QStringLiteral("retry"));
    QCOMPARE(result.phases.first().tool_result.value(QStringLiteral("artifact")).toString(),
             QStringLiteral("vlc-offline.exe"));
}

void AiOrchestratorTests::continuesDegradedToFallbackOnOfflineDownloaderFailure() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"),
                                              QStringLiteral("fallback ok"));

    FakeToolExecutor tool;
    QJsonObject unavailable;
    unavailable[QStringLiteral("success")] = false;
    unavailable[QStringLiteral("error_message")] =
        QStringLiteral("package unavailable in offline catalog");
    tool.results.insert(QStringLiteral("direct_download"), unavailable);

    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("offline_fallback");
    workflow.agents << makeAgent(QStringLiteral("research_agent"), QStringLiteral("read_only_pc"));
    sak::ai::WorkflowPhase direct;
    direct.id = QStringLiteral("direct_download");
    direct.type = QStringLiteral("tool_action");
    direct.tool = QStringLiteral("sak_offline_downloader");
    direct.operation = QStringLiteral("direct_download");
    direct.risk = QStringLiteral("download_only");
    workflow.phases << direct;
    sak::ai::WorkflowPhase fallback = makeDelegate(QStringLiteral("official_source_fallback"),
                                                   QStringLiteral("research_agent"));
    fallback.condition = QStringLiteral("direct_download_failed");
    workflow.phases << fallback;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_fallback"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run_fallback"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.phases.size(), 2);
    QVERIFY(!result.phases.first().success);
    QVERIFY(result.phases.last().success);
    QVERIFY(result.flags.contains(QStringLiteral("direct_download_failed")));
    QVERIFY(result.flags.contains(QStringLiteral("official_source_fallback_succeeded")));
    const auto recovery =
        result.phases.first().metadata.value(QStringLiteral("recovery_decision")).toObject();
    QCOMPARE(recovery.value(QStringLiteral("action")).toString(),
             QStringLiteral("continue_degraded"));
}

void AiOrchestratorTests::waitsForHumanOnRiskyMutationFailure() {
    FakeModelClient model;
    FakeToolExecutor tool;
    QJsonObject install_failure;
    install_failure[QStringLiteral("success")] = false;
    install_failure[QStringLiteral("error_message")] =
        QStringLiteral("installer failed after system change");
    tool.results.insert(QStringLiteral("install"), install_failure);

    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("install_gate");
    sak::ai::WorkflowPhase install;
    install.id = QStringLiteral("install");
    install.type = QStringLiteral("tool_action");
    install.tool = QStringLiteral("sak_package_manager");
    install.operation = QStringLiteral("install");
    install.risk = QStringLiteral("system_change");
    workflow.phases << install;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_human"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run_human"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::WaitingForHuman);
    QCOMPARE(result.phases.size(), 1);
    QVERIFY(result.error_message.contains(QStringLiteral("needs human input")));
    const auto recovery =
        result.phases.first().metadata.value(QStringLiteral("recovery_decision")).toObject();
    QCOMPARE(recovery.value(QStringLiteral("action")).toString(), QStringLiteral("ask_human"));
    QVERIFY(recovery.value(QStringLiteral("requires_human")).toBool(false));
}

void AiOrchestratorTests::resumesAfterHumanGateWithoutRerunningPriorPhase() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    FakeToolExecutor failing_tool;
    QJsonObject install_failure;
    install_failure[QStringLiteral("success")] = false;
    install_failure[QStringLiteral("error_message")] =
        QStringLiteral("installer failed after system change");
    failing_tool.results.insert(QStringLiteral("install"), install_failure);

    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator first_orchestrator(&runner, &failing_tool);

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("resume_gate");
    sak::ai::WorkflowPhase install;
    install.id = QStringLiteral("install");
    install.type = QStringLiteral("tool_action");
    install.tool = QStringLiteral("sak_package_manager");
    install.operation = QStringLiteral("install");
    install.risk = QStringLiteral("system_change");
    workflow.phases << install;

    sak::ai::WorkflowPhase fallback;
    fallback.id = QStringLiteral("fallback");
    fallback.type = QStringLiteral("delegate");
    fallback.agent = QStringLiteral("research_agent");
    fallback.condition = QStringLiteral("install_failed");
    fallback.prompt = QStringLiteral("Find manual repair path");
    workflow.agents << makeAgent(QStringLiteral("research_agent"), QStringLiteral("read_only_pc"));
    workflow.phases << fallback;

    auto first_root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_resume"));
    const auto paused = first_orchestrator.run(workflow, QStringLiteral("run_resume"), first_root);
    QCOMPARE(paused.status, sak::ai::AiRunStatus::WaitingForHuman);
    QCOMPARE(paused.phases.size(), 1);
    QCOMPARE(failing_tool.calls, 1);

    FakeToolExecutor resume_tool;
    sak::ai::AiOrchestrator resume_orchestrator(&runner, &resume_tool);
    sak::ai::AiOrchestrationOptions options;
    options.resume_enabled = true;
    options.resume_start_phase_index = 1;
    options.resume_prior_phases = paused.phases;
    options.resume_flags = paused.flags;
    resume_orchestrator.setOptions(options);

    auto resume_root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_resume"));
    const auto resumed =
        resume_orchestrator.run(workflow, QStringLiteral("run_resume"), resume_root);

    QCOMPARE(resumed.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(resumed.phases.size(), 2);
    QCOMPARE(resumed.phases.at(0).phase_id, QStringLiteral("install"));
    QCOMPARE(resumed.phases.at(1).phase_id, QStringLiteral("fallback"));
    QVERIFY(resumed.phases.at(1).success);
    QCOMPARE(resume_tool.calls, 0);
}

void AiOrchestratorTests::reassignsCriticFailureToOverseer() {
    FakeModelClient model;
    sak::ai::IAiModelClient::Response critic_failure;
    critic_failure.success = false;
    critic_failure.error_message = QStringLiteral("review model failed");
    model.response_overrides.insert(QStringLiteral("Do quality_gate"), critic_failure);

    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);
    std::atomic<int> overseer_calls{0};
    orchestrator.setOverseerHandler(
        [&overseer_calls](const sak::ai::WorkflowPhase& phase, const sak::ai::CancellationToken&) {
            ++overseer_calls;
            QJsonObject ok;
            ok[QStringLiteral("summary")] = QStringLiteral("overseer accepted degraded review");
            ok[QStringLiteral("phase_id")] = phase.id;
            return ok;
        });

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("critic_reassign");
    workflow.agents << makeAgent(QStringLiteral("critic_agent"), QStringLiteral("read_only_pc"));
    workflow.phases << makeDelegate(QStringLiteral("quality_gate"), QStringLiteral("critic_agent"));

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_reassign"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run_reassign"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(result.phases.size(), 1);
    QVERIFY(!result.phases.first().success);
    QCOMPARE(overseer_calls.load(), 1);
    const auto recovery =
        result.phases.first().metadata.value(QStringLiteral("recovery_decision")).toObject();
    QCOMPARE(recovery.value(QStringLiteral("action")).toString(), QStringLiteral("reassign"));
    QCOMPARE(result.phases.first().metadata.value(QStringLiteral("reassigned_to")).toString(),
             QStringLiteral("overseer"));
    QVERIFY(result.phases.first().metadata.contains(QStringLiteral("reassignment")));
}

void AiOrchestratorTests::emitsPhaseStartedBeforeCompletion() {
    FakeModelClient model;
    model.default_response = makeJsonResponse(QStringLiteral("complete"), QStringLiteral("ok"));
    FakeToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    QStringList events;
    orchestrator.setPhaseStartedCallback([&events](const sak::ai::WorkflowPhase& phase) {
        events << QStringLiteral("start:%1").arg(phase.id);
    });
    orchestrator.setPhaseCompletedCallback([&events](const sak::ai::AiPhaseExecution& execution) {
        events << QStringLiteral("done:%1").arg(execution.phase_id);
    });

    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("callbacks");
    workflow.agents << makeAgent(QStringLiteral("a"), QStringLiteral("package_tools_only"));
    workflow.phases << makeDelegate(QStringLiteral("inspect"), QStringLiteral("a"));
    sak::ai::WorkflowPhase tool_phase;
    tool_phase.id = QStringLiteral("collect_logs");
    tool_phase.type = QStringLiteral("tool_action");
    tool_phase.tool = QStringLiteral("sak_powershell");
    workflow.phases << tool_phase;

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_callbacks"));
    const auto result = orchestrator.run(workflow, QStringLiteral("run_callbacks"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QCOMPARE(events.size(), 4);
    QCOMPARE(events.at(0), QStringLiteral("start:inspect"));
    QCOMPARE(events.at(1), QStringLiteral("done:inspect"));
    QCOMPARE(events.at(2), QStringLiteral("start:collect_logs"));
    QCOMPARE(events.at(3), QStringLiteral("done:collect_logs"));
}

QTEST_GUILESS_MAIN(AiOrchestratorTests)
#include "test_ai_orchestrator.moc"
