// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_orchestrator.h"
#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/ai_workflow_template.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSet>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>

namespace {

class FakeModelClient : public sak::ai::IAiModelClient {
public:
    Response invoke(const Request& /*request*/, const sak::ai::CancellationToken& token) override {
        QMutexLocker lock(&m_mutex);
        ++calls;
        if (cancel_after_calls > 0 && calls >= cancel_after_calls && token.isValid()) {
            token.cancel(QStringLiteral("eval_cancel"));
        }
        Response response;
        response.success = true;
        QJsonObject payload;
        if (empty_failure) {
            payload[QStringLiteral("status")] = QStringLiteral("failed");
        } else {
            payload[QStringLiteral("status")] =
                (calls == fail_on_call) ? QStringLiteral("failed") : QStringLiteral("complete");
            payload[QStringLiteral("summary")] = QStringLiteral("ok");
        }
        response.text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        response.usage.total_tokens = 100;
        return response;
    }

    int calls{0};
    int cancel_after_calls{-1};
    int fail_on_call{-1};
    bool empty_failure{false};

private:
    QMutex m_mutex;
};

class TracingToolExecutor : public sak::ai::IAiToolExecutor {
public:
    QJsonObject runToolPhase(const sak::ai::WorkflowPhase& phase,
                             sak::ai::AiToolPolicy /*policy*/,
                             const sak::ai::AiWorkflowPhaseContext& context,
                             const sak::ai::CancellationToken& /*token*/) override {
        order.append(phase.id);
        last_context = context;
        if (fail_phase_ids.contains(phase.id)) {
            QJsonObject bad;
            bad[QStringLiteral("success")] = false;
            bad[QStringLiteral("error_message")] = QStringLiteral("simulated failure");
            return bad;
        }
        QJsonObject ok;
        ok[QStringLiteral("success")] = true;
        return ok;
    }

    QStringList order;
    sak::ai::AiWorkflowPhaseContext last_context;
    QSet<QString> fail_phase_ids;
};

QString locateWorkflowFile(const QString& filename) {
    const QString own = QFileInfo(__FILE__).absolutePath();
    QDir dir(own);
    for (int hop = 0; hop < 6; ++hop) {
        const QString candidate =
            dir.absoluteFilePath(QStringLiteral("resources/ai/workflows/") + filename);
        if (QFile::exists(candidate)) {
            return candidate;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

QString locateAiResourceFile(const QString& relative_path) {
    const QString own = QFileInfo(__FILE__).absolutePath();
    QDir dir(own);
    for (int hop = 0; hop < 6; ++hop) {
        const QString candidate =
            dir.absoluteFilePath(QStringLiteral("resources/ai/") + relative_path);
        if (QFile::exists(candidate)) {
            return candidate;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

sak::ai::WorkflowTemplate loadGoldenWorkflow(const QString& filename, QString* error_text) {
    const QString path = locateWorkflowFile(filename);
    if (path.isEmpty()) {
        if (error_text) {
            *error_text = QStringLiteral("workflow file not found: %1").arg(filename);
        }
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error_text) {
            *error_text = file.errorString();
        }
        return {};
    }
    QStringList errors;
    auto wf = sak::ai::WorkflowTemplate::fromJsonBytes(file.readAll(), path, &errors);
    if (!errors.isEmpty() && error_text) {
        *error_text = errors.join(QStringLiteral("; "));
    }
    return wf;
}

QString phaseSearchText(const sak::ai::WorkflowPhase& phase) {
    return QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8")
        .arg(phase.id,
             phase.type,
             phase.agent,
             phase.tool,
             phase.operation,
             phase.prompt,
             phase.expected_output,
             phase.completion)
        .toLower();
}

bool workflowHasPhaseText(const sak::ai::WorkflowTemplate& workflow, const QStringList& needles) {
    for (const auto& phase : workflow.phases) {
        const QString haystack = phaseSearchText(phase);
        for (const auto& needle : needles) {
            if (haystack.contains(needle)) {
                return true;
            }
        }
    }
    return false;
}

bool workflowHasCleanup(const sak::ai::WorkflowTemplate& workflow) {
    return std::any_of(
        workflow.phases.cbegin(), workflow.phases.cend(), [](const sak::ai::WorkflowPhase& phase) {
            return phase.type.compare(QStringLiteral("cleanup"), Qt::CaseInsensitive) == 0 &&
                   phase.always_run;
        });
}

bool workflowHasRiskyPhase(const sak::ai::WorkflowTemplate& workflow) {
    return std::any_of(workflow.phases.cbegin(),
                       workflow.phases.cend(),
                       [](const sak::ai::WorkflowPhase& phase) {
                           const QString risk = phase.risk.toLower();
                           const QString operation = phase.operation.toLower();
                           return risk.contains(QStringLiteral("system_change")) ||
                                  risk.contains(QStringLiteral("repair")) ||
                                  risk.contains(QStringLiteral("uninstall")) ||
                                  risk.contains(QStringLiteral("destructive")) ||
                                  operation == QLatin1String("install") ||
                                  operation == QLatin1String("uninstall") ||
                                  operation == QLatin1String("upgrade") ||
                                  operation == QLatin1String("install_bundle");
                       });
}

bool phaseTextContainsAny(const QString& text, const QStringList& needles) {
    for (const auto& needle : needles) {
        if (text.contains(needle)) {
            return true;
        }
    }
    return false;
}

int lastToolPhaseIndex(const sak::ai::WorkflowTemplate& workflow) {
    int last_tool_index = -1;
    for (int i = 0; i < workflow.phases.size(); ++i) {
        const auto& phase = workflow.phases.at(i);
        if (phase.type.compare(QStringLiteral("tool_action"), Qt::CaseInsensitive) == 0) {
            last_tool_index = i;
        }
    }
    return last_tool_index;
}

bool workflowHasVerificationAfterToolWork(const sak::ai::WorkflowTemplate& workflow) {
    const int last_tool_index = lastToolPhaseIndex(workflow);
    if (last_tool_index < 0) {
        return true;
    }
    const QStringList needles{QStringLiteral("verify"),
                              QStringLiteral("verification"),
                              QStringLiteral("review"),
                              QStringLiteral("critic"),
                              QStringLiteral("assess"),
                              QStringLiteral("report")};
    for (int i = last_tool_index + 1; i < workflow.phases.size(); ++i) {
        if (phaseTextContainsAny(phaseSearchText(workflow.phases.at(i)), needles)) {
            return true;
        }
    }
    return false;
}

bool phaseLooksLikeReport(const sak::ai::WorkflowPhase& phase, const QString& text) {
    return phase.id.contains(QStringLiteral("report"), Qt::CaseInsensitive) ||
           phase.expected_output.contains(QStringLiteral("report"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("handoff"));
}

bool reportPhaseMeetsTechnicianStandard(const sak::ai::WorkflowPhase& phase) {
    const QString text = phaseSearchText(phase);
    if (!phaseLooksLikeReport(phase, text)) {
        return false;
    }
    const bool readable = phaseTextContainsAny(text,
                                               {QStringLiteral("technician"),
                                                QStringLiteral("customer"),
                                                QStringLiteral("handoff"),
                                                QStringLiteral("readable")});
    const bool actions = phaseTextContainsAny(text,
                                              {QStringLiteral("action"), QStringLiteral("tool")});
    const bool verification =
        phaseTextContainsAny(text, {QStringLiteral("verification"), QStringLiteral("verify")});
    const bool artifacts = phaseTextContainsAny(text,
                                                {QStringLiteral("artifact"),
                                                 QStringLiteral("log"),
                                                 QStringLiteral("citation"),
                                                 QStringLiteral("source")});
    const bool risk_or_followup = phaseTextContainsAny(
        text, {QStringLiteral("risk"), QStringLiteral("follow-up"), QStringLiteral("follow up")});
    return readable && text.contains(QStringLiteral("evidence")) && actions && verification &&
           artifacts && text.contains(QStringLiteral("cleanup")) && risk_or_followup;
}

bool workflowHasTechnicianReportPhase(const sak::ai::WorkflowTemplate& workflow) {
    for (const auto& phase : workflow.phases) {
        if (reportPhaseMeetsTechnicianStandard(phase)) {
            return true;
        }
    }
    return false;
}

}  // namespace

class AiWorkflowEvalsTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void downloadOfflineInstallerWorkflowParses();
    void downloadOfflineInstallerExecutesSakFirstThenCleansUp();
    void downloadOfflineInstallerRunsFallbackAfterSakFailure();
    void installAppWorkflowFallsBackAfterPackageLookupFailure();
    void cancellationDuringWorkflowPropagatesToChildren();
    void cleanupFailureInSeededWorkflowIsRecordedNotFatal();
    void reportFailureRunsAlwaysCleanup();
    void seededWorkflowCatalogHasUniqueIdsAndResources();
    void seededWorkflowsParseAndExpose();
    void seededWorkflowsHaveReportVerifyCleanupShape();
    void seededWorkflowsVerifyAfterToolWork();
    void seededWorkflowReportsMeetTechnicianStandard();
    void seededPowerShellToolPhasesHaveExplicitCommands();
    void seededDelegatePhasesHaveStructuredOutputs();
    void seededReadOnlyToolPhasesStayReadOnly();
    void seededStorageReliabilityChecksDeclareFallbacks();
    void fullPcHealthCheckUsesLiveCpuSample();
    void technicianToolWorkflowDownloadsRunsVerifiesAndCleans();
    void seededWorkflowsContinueAfterEmptyDelegateFailure();
};

void AiWorkflowEvalsTests::downloadOfflineInstallerWorkflowParses() {
    QString err;
    const auto workflow = loadGoldenWorkflow(QStringLiteral("download_offline_installer.json"),
                                             &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(workflow.id, QStringLiteral("download_offline_installer"));
    QVERIFY(!workflow.phases.isEmpty());
    bool found_direct_download = false;
    bool found_cleanup = false;
    bool cleanup_always_run = false;
    for (const auto& phase : workflow.phases) {
        if (phase.id == QStringLiteral("direct_download")) {
            found_direct_download = true;
            QCOMPARE(phase.tool, QStringLiteral("sak_offline_downloader"));
            QCOMPARE(phase.operation, QStringLiteral("direct_download"));
        }
        if (phase.type == QStringLiteral("cleanup")) {
            found_cleanup = true;
            cleanup_always_run = phase.always_run;
        }
    }
    QVERIFY(found_direct_download);
    QVERIFY(found_cleanup);
    QVERIFY2(cleanup_always_run, "Cleanup phase should be marked always_run by parser default");
}

void AiWorkflowEvalsTests::downloadOfflineInstallerExecutesSakFirstThenCleansUp() {
    QString err;
    const auto workflow = loadGoldenWorkflow(QStringLiteral("download_offline_installer.json"),
                                             &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    FakeModelClient model;
    TracingToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("eval_run_1"));
    const auto result = orchestrator.run(workflow, QStringLiteral("eval_run_1"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QVERIFY(tool.order.contains(QStringLiteral("package_search")));
    QVERIFY(tool.order.contains(QStringLiteral("direct_download")));
    QVERIFY(tool.order.contains(QStringLiteral("cleanup")));
    const int search_idx = tool.order.indexOf(QStringLiteral("package_search"));
    const int direct_idx = tool.order.indexOf(QStringLiteral("direct_download"));
    const int cleanup_idx = tool.order.indexOf(QStringLiteral("cleanup"));
    QVERIFY(search_idx >= 0);
    QVERIFY(direct_idx >= 0);
    QVERIFY(direct_idx > search_idx);
    QVERIFY(cleanup_idx > direct_idx);
    QVERIFY(!result.flags.contains(QStringLiteral("fallback_research_succeeded")));
}

void AiWorkflowEvalsTests::downloadOfflineInstallerRunsFallbackAfterSakFailure() {
    QString err;
    const auto workflow = loadGoldenWorkflow(QStringLiteral("download_offline_installer.json"),
                                             &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    FakeModelClient model;
    TracingToolExecutor tool;
    tool.fail_phase_ids.insert(QStringLiteral("direct_download"));
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("eval_fallback_run"));
    const auto result = orchestrator.run(workflow, QStringLiteral("eval_fallback_run"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QVERIFY(result.flags.contains(QStringLiteral("direct_download_failed")));
    QVERIFY(result.flags.contains(QStringLiteral("fallback_research_succeeded")));
    QVERIFY(tool.order.contains(QStringLiteral("direct_download")));
    QVERIFY(tool.order.contains(QStringLiteral("cleanup")));

    const auto phase_index = [&](const QString& phase_id) {
        for (int i = 0; i < result.phases.size(); ++i) {
            if (result.phases.at(i).phase_id == phase_id) {
                return i;
            }
        }
        return -1;
    };
    const int direct_idx = phase_index(QStringLiteral("direct_download"));
    const int fallback_idx = phase_index(QStringLiteral("fallback_research"));
    const int cleanup_idx = phase_index(QStringLiteral("cleanup"));

    QVERIFY(direct_idx >= 0);
    QVERIFY(fallback_idx > direct_idx);
    QVERIFY(cleanup_idx > direct_idx);
}

void AiWorkflowEvalsTests::installAppWorkflowFallsBackAfterPackageLookupFailure() {
    QString err;
    const auto workflow = loadGoldenWorkflow(QStringLiteral("install_app_now.json"), &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    FakeModelClient model;
    TracingToolExecutor tool;
    tool.fail_phase_ids.insert(QStringLiteral("precheck"));
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("eval_install_fallback_run"));
    const auto result =
        orchestrator.run(workflow, QStringLiteral("eval_install_fallback_run"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QVERIFY(result.flags.contains(QStringLiteral("precheck_failed")));
    QVERIFY(result.flags.contains(QStringLiteral("fallback_research_succeeded")));
    QVERIFY(!result.flags.contains(QStringLiteral("install_succeeded")));
    QVERIFY(tool.order.contains(QStringLiteral("precheck")));
    QVERIFY(tool.order.contains(QStringLiteral("cleanup")));
    QVERIFY(!tool.order.contains(QStringLiteral("install")));

    const auto phase_index = [&](const QString& phase_id) {
        for (int i = 0; i < result.phases.size(); ++i) {
            if (result.phases.at(i).phase_id == phase_id) {
                return i;
            }
        }
        return -1;
    };
    const int precheck_idx = phase_index(QStringLiteral("precheck"));
    const int fallback_idx = phase_index(QStringLiteral("fallback_research"));
    const int install_idx = phase_index(QStringLiteral("install"));
    QVERIFY(precheck_idx >= 0);
    QVERIFY(fallback_idx > precheck_idx);
    QVERIFY(install_idx > fallback_idx);
    QVERIFY(result.phases.at(install_idx).skipped);
}

void AiWorkflowEvalsTests::cancellationDuringWorkflowPropagatesToChildren() {
    QString err;
    const auto workflow = loadGoldenWorkflow(QStringLiteral("download_offline_installer.json"),
                                             &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    FakeModelClient model;
    model.cancel_after_calls = 1;
    TracingToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("eval_cancel_run"));
    const auto result = orchestrator.run(workflow, QStringLiteral("eval_cancel_run"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Cancelled);
    QVERIFY(!result.error_message.isEmpty());
    QVERIFY(tool.order.contains(QStringLiteral("direct_download")));
}

void AiWorkflowEvalsTests::cleanupFailureInSeededWorkflowIsRecordedNotFatal() {
    QString err;
    const auto workflow = loadGoldenWorkflow(QStringLiteral("download_offline_installer.json"),
                                             &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    FakeModelClient model;
    TracingToolExecutor tool;
    tool.fail_phase_ids.insert(QStringLiteral("cleanup"));
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("eval_cleanup_fail"));
    const auto result = orchestrator.run(workflow, QStringLiteral("eval_cleanup_fail"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Completed);
    QVERIFY(result.cleanup_failures.contains(QStringLiteral("cleanup")));
    QVERIFY(result.flags.contains(QStringLiteral("cleanup_failed")));
}

void AiWorkflowEvalsTests::reportFailureRunsAlwaysCleanup() {
    QString err;
    const auto workflow = loadGoldenWorkflow(QStringLiteral("download_offline_installer.json"),
                                             &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    FakeModelClient model;
    model.fail_on_call = 1;
    TracingToolExecutor tool;
    sak::ai::AiSubagentRunner runner(&model);
    sak::ai::AiOrchestrator orchestrator(&runner, &tool);

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("eval_report_fail"));
    const auto result = orchestrator.run(workflow, QStringLiteral("eval_report_fail"), root);

    QCOMPARE(result.status, sak::ai::AiRunStatus::Failed);
    QVERIFY(result.error_message.contains(QStringLiteral("report")));
    QVERIFY(tool.order.contains(QStringLiteral("cleanup")));
}

QStringList seededWorkflowFiles() {
    return {
        QStringLiteral("full_pc_health_check.json"),
        QStringLiteral("drive_health_deep_check.json"),
        QStringLiteral("download_offline_installer.json"),
        QStringLiteral("install_app_now.json"),
        QStringLiteral("build_offline_deployment_bundle.json"),
        QStringLiteral("windows_update_repair.json"),
        QStringLiteral("bsod_investigation.json"),
        QStringLiteral("clean_uninstall.json"),
        QStringLiteral("security_advisory_check.json"),
        QStringLiteral("technician_service_report.json"),
        QStringLiteral("network_connectivity_repair.json"),
        QStringLiteral("startup_performance_triage.json"),
        QStringLiteral("printer_troubleshooting.json"),
        QStringLiteral("technician_tool_assisted_task.json"),
        QStringLiteral("malware_virus_removal.json"),
        QStringLiteral("pc_cleanup_bloatware_adware.json"),
        QStringLiteral("approved_bloatware_adware_removal.json"),
    };
}

void AiWorkflowEvalsTests::seededWorkflowCatalogHasUniqueIdsAndResources() {
    const QStringList files = seededWorkflowFiles();
    QVERIFY2(!files.isEmpty(), "Seeded workflow file list should not be empty");

    QSet<QString> listed;
    for (const auto& f : files) {
        QVERIFY2(!listed.contains(f),
                 qPrintable(QStringLiteral("Duplicate seeded workflow file: %1").arg(f)));
        listed.insert(f);
    }

    QDir workflow_dir(QFileInfo(locateWorkflowFile(files.first())).absolutePath());
    const QStringList actual_files =
        workflow_dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto& actual : actual_files) {
        QVERIFY2(listed.contains(actual),
                 qPrintable(QStringLiteral("Workflow JSON exists but is not covered by evals: %1")
                                .arg(actual)));
    }

    QSet<QString> workflow_ids;
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        QVERIFY2(!workflow_ids.contains(wf.id),
                 qPrintable(QStringLiteral("Duplicate workflow id: %1").arg(wf.id)));
        workflow_ids.insert(wf.id);

        QSet<QString> phase_ids;
        for (const auto& phase : wf.phases) {
            QVERIFY2(!phase_ids.contains(phase.id),
                     qPrintable(QStringLiteral("%1: duplicate phase id %2").arg(f, phase.id)));
            phase_ids.insert(phase.id);
        }

        QSet<QString> agent_ids;
        for (const auto& agent : wf.agents) {
            QVERIFY2(!agent_ids.contains(agent.id),
                     qPrintable(QStringLiteral("%1: duplicate agent id %2").arg(f, agent.id)));
            agent_ids.insert(agent.id);
        }
        for (const auto& phase : wf.phases) {
            if (!phase.agent.trimmed().isEmpty()) {
                QVERIFY2(agent_ids.contains(phase.agent),
                         qPrintable(QStringLiteral("%1/%2: phase references missing agent %3")
                                        .arg(f, phase.id, phase.agent)));
            }
        }

        const QStringList resources = wf.instructions + wf.skills;
        for (const auto& resource : resources) {
            QVERIFY2(!locateAiResourceFile(resource).isEmpty(),
                     qPrintable(
                         QStringLiteral("%1: missing workflow resource %2").arg(f, resource)));
        }
    }
}

void AiWorkflowEvalsTests::seededWorkflowsParseAndExpose() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        QVERIFY2(!wf.id.isEmpty(), qPrintable(QStringLiteral("%1: empty id").arg(f)));
        QVERIFY2(!wf.phases.isEmpty(), qPrintable(QStringLiteral("%1: empty phases").arg(f)));
        QStringList validation_errors;
        QVERIFY2(wf.isValid(&validation_errors),
                 qPrintable(QStringLiteral("%1: %2").arg(
                     f, validation_errors.join(QStringLiteral("; ")))));
    }
}

void AiWorkflowEvalsTests::seededWorkflowsHaveReportVerifyCleanupShape() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        QVERIFY2(workflowHasPhaseText(wf, {QStringLiteral("report")}),
                 qPrintable(QStringLiteral("%1: missing report phase").arg(f)));
        QVERIFY2(workflowHasPhaseText(wf,
                                      {QStringLiteral("verify"),
                                       QStringLiteral("review"),
                                       QStringLiteral("critic"),
                                       QStringLiteral("assess")}),
                 qPrintable(QStringLiteral("%1: missing verification/review phase").arg(f)));
        QVERIFY2(workflowHasCleanup(wf),
                 qPrintable(QStringLiteral("%1: missing always-run cleanup phase").arg(f)));
        QVERIFY2(!wf.acceptance_criteria.isEmpty(),
                 qPrintable(QStringLiteral("%1: missing acceptance criteria").arg(f)));

        if (workflowHasRiskyPhase(wf)) {
            QVERIFY2(
                workflowHasPhaseText(wf,
                                     {QStringLiteral("approval"),
                                      QStringLiteral("restore point"),
                                      QStringLiteral("restore-point")}),
                qPrintable(
                    QStringLiteral("%1: risky workflow missing approval/restore gate").arg(f)));
        }
    }
}

void AiWorkflowEvalsTests::seededWorkflowsVerifyAfterToolWork() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));

        QVERIFY2(workflowHasVerificationAfterToolWork(wf),
                 qPrintable(
                     QStringLiteral("%1: missing verification/review/report after final tool phase")
                         .arg(f)));
    }
}

void AiWorkflowEvalsTests::seededWorkflowReportsMeetTechnicianStandard() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        QVERIFY2(wf.instructions.contains(QStringLiteral("instructions/reporting_standard.md")),
                 qPrintable(QStringLiteral("%1: missing reporting standard instruction").arg(f)));
        QVERIFY2(wf.skills.contains(QStringLiteral("skills/cleanup-after-job.md")),
                 qPrintable(QStringLiteral("%1: missing cleanup-after-job skill").arg(f)));

        QVERIFY2(workflowHasTechnicianReportPhase(wf),
                 qPrintable(
                     QStringLiteral("%1: report phase does not meet technician handoff standard")
                         .arg(f)));
    }
}

void AiWorkflowEvalsTests::seededPowerShellToolPhasesHaveExplicitCommands() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        for (const auto& phase : wf.phases) {
            if (phase.tool != QLatin1String("run_powershell")) {
                continue;
            }
            const QString command =
                phase.arguments.value(QStringLiteral("command")).toString().trimmed();
            QVERIFY2(!command.isEmpty(),
                     qPrintable(
                         QStringLiteral("%1/%2: run_powershell phase needs arguments.command")
                             .arg(f, phase.id)));
            QVERIFY2(phase.arguments.value(QStringLiteral("timeout_seconds")).isDouble(),
                     qPrintable(
                         QStringLiteral("%1/%2: run_powershell phase needs numeric timeout_seconds")
                             .arg(f, phase.id)));
            const int timeout_seconds =
                phase.arguments.value(QStringLiteral("timeout_seconds")).toInt();
            QVERIFY2(timeout_seconds >= 5 && timeout_seconds <= 7200,
                     qPrintable(
                         QStringLiteral("%1/%2: run_powershell timeout_seconds out of bounds")
                             .arg(f, phase.id)));
            QVERIFY2(phase.arguments.value(QStringLiteral("max_output_bytes")).isDouble(),
                     qPrintable(QStringLiteral(
                                    "%1/%2: run_powershell phase needs numeric max_output_bytes")
                                    .arg(f, phase.id)));
            const int max_output_bytes =
                phase.arguments.value(QStringLiteral("max_output_bytes")).toInt();
            QVERIFY2(max_output_bytes > 0 && max_output_bytes <= 1'048'576,
                     qPrintable(
                         QStringLiteral("%1/%2: run_powershell max_output_bytes out of bounds")
                             .arg(f, phase.id)));
            QVERIFY2(phase.arguments.value(QStringLiteral("requires_admin")).isBool(),
                     qPrintable(
                         QStringLiteral(
                             "%1/%2: run_powershell phase needs explicit boolean requires_admin")
                             .arg(f, phase.id)));
        }
    }
}

void AiWorkflowEvalsTests::seededDelegatePhasesHaveStructuredOutputs() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        for (const auto& phase : wf.phases) {
            if (phase.type.compare(QStringLiteral("delegate"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            QVERIFY2(!phase.agent.trimmed().isEmpty(),
                     qPrintable(QStringLiteral("%1/%2: delegate missing agent").arg(f, phase.id)));
            QVERIFY2(!phase.expected_output.trimmed().isEmpty(),
                     qPrintable(
                         QStringLiteral("%1/%2: delegate missing expected_output schema label")
                             .arg(f, phase.id)));
            QVERIFY2(
                !phase.prompt.trimmed().isEmpty(),
                qPrintable(
                    QStringLiteral("%1/%2: delegate missing bounded prompt").arg(f, phase.id)));
        }
    }
}

void AiWorkflowEvalsTests::seededReadOnlyToolPhasesStayReadOnly() {
    const QStringList files = seededWorkflowFiles();
    const QSet<QString> allowed_read_only_operations{
        QStringLiteral("diagnose"),
        QStringLiteral("verify"),
        QStringLiteral("search"),
        QStringLiteral("is_installed"),
        QStringLiteral("installed_version"),
    };
    const QStringList mutating_powershell_markers{
        QStringLiteral("restart-service"),
        QStringLiteral("start-service"),
        QStringLiteral("stop-service"),
        QStringLiteral("set-service"),
        QStringLiteral("set-item"),
        QStringLiteral("new-item"),
        QStringLiteral("remove-item"),
        QStringLiteral("clear-dnsclientcache"),
        QStringLiteral("ipconfig /renew"),
        QStringLiteral("dism /online /cleanup-image /restorehealth"),
        QStringLiteral("sfc /scannow"),
        QStringLiteral("chkdsk /f"),
        QStringLiteral("chkdsk /r"),
        QStringLiteral("reg add"),
        QStringLiteral("reg delete"),
        QStringLiteral("sc config"),
        QStringLiteral("disable-scheduledtask"),
        QStringLiteral("enable-scheduledtask"),
    };
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        for (const auto& phase : wf.phases) {
            const QString risk = phase.risk.trimmed().toLower();
            if (phase.type.compare(QStringLiteral("tool_action"), Qt::CaseInsensitive) != 0 ||
                risk != QLatin1String("read_only")) {
                continue;
            }
            QVERIFY2(allowed_read_only_operations.contains(phase.operation.trimmed().toLower()),
                     qPrintable(
                         QStringLiteral(
                             "%1/%2: read-only tool phase uses mutating-looking operation '%3'")
                             .arg(f, phase.id, phase.operation)));
            QVERIFY2(!phase.arguments.value(QStringLiteral("requires_admin")).toBool(false),
                     qPrintable(
                         QStringLiteral(
                             "%1/%2: read-only tool phase should not require admin by default")
                             .arg(f, phase.id)));
            if (phase.tool == QLatin1String("run_powershell")) {
                const QString command =
                    phase.arguments.value(QStringLiteral("command")).toString().toLower();
                for (const auto& marker : mutating_powershell_markers) {
                    QVERIFY2(!command.contains(marker),
                             qPrintable(
                                 QStringLiteral(
                                     "%1/%2: read-only PowerShell contains mutating marker '%3'")
                                     .arg(f, phase.id, marker)));
                }
            }
        }
    }
}

void AiWorkflowEvalsTests::seededStorageReliabilityChecksDeclareFallbacks() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));
        for (const auto& phase : wf.phases) {
            if (phase.tool != QLatin1String("run_powershell")) {
                continue;
            }
            const QString command =
                phase.arguments.value(QStringLiteral("command")).toString().toLower();
            if (!command.contains(QStringLiteral("get-storagereliabilitycounter"))) {
                continue;
            }
            QVERIFY2(command.contains(QStringLiteral("[warning]")) &&
                         command.contains(QStringLiteral("unavailable")),
                     qPrintable(QStringLiteral("%1/%2: storage reliability command must "
                                               "make permission-limited evidence explicit")
                                    .arg(f, phase.id)));
            QVERIFY2(command.contains(QStringLiteral("fallback")),
                     qPrintable(QStringLiteral("%1/%2: storage reliability command needs "
                                               "fallback evidence when counters are denied")
                                    .arg(f, phase.id)));
        }
    }
}

void AiWorkflowEvalsTests::fullPcHealthCheckUsesLiveCpuSample() {
    QString err;
    const auto wf = loadGoldenWorkflow(QStringLiteral("full_pc_health_check.json"), &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    bool saw_live_sample = false;
    for (const auto& phase : wf.phases) {
        if (phase.tool != QLatin1String("run_powershell")) {
            continue;
        }
        const QString command =
            phase.arguments.value(QStringLiteral("command")).toString().toLower();
        QVERIFY2(!command.contains(QStringLiteral("sort-object cpu -descending")),
                 qPrintable(QStringLiteral("%1: full PC health check must not rank "
                                           "lifetime process CPU as live load")
                                .arg(phase.id)));
        if (command.contains(QStringLiteral("top cpu live sample")) &&
            command.contains(QStringLiteral("start-sleep")) &&
            command.contains(QStringLiteral("cpusecondsdelta"))) {
            saw_live_sample = true;
        }
    }
    QVERIFY2(saw_live_sample, "full_pc_health_check must collect a short live CPU sample");
}

void AiWorkflowEvalsTests::technicianToolWorkflowDownloadsRunsVerifiesAndCleans() {
    QString err;
    const auto wf = loadGoldenWorkflow(QStringLiteral("technician_tool_assisted_task.json"), &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(wf.id, QStringLiteral("technician_tool_assisted_task"));
    QVERIFY(wf.skills.contains(QStringLiteral("skills/artifact-verification.md")));
    QVERIFY(wf.skills.contains(QStringLiteral("skills/cleanup-after-job.md")));

    const auto phase_index = [&](const QString& phase_id) {
        for (int i = 0; i < wf.phases.size(); ++i) {
            if (wf.phases.at(i).id == phase_id) {
                return i;
            }
        }
        return -1;
    };
    const int download_idx = phase_index(QStringLiteral("download_tool"));
    const int verify_download_idx = phase_index(QStringLiteral("verify_download"));
    const int run_idx = phase_index(QStringLiteral("run_tool"));
    const int verify_result_idx = phase_index(QStringLiteral("verify_result"));
    const int report_idx = phase_index(QStringLiteral("report"));
    const int cleanup_idx = phase_index(QStringLiteral("cleanup"));
    QVERIFY(download_idx >= 0);
    QVERIFY(verify_download_idx > download_idx);
    QVERIFY(run_idx > verify_download_idx);
    QVERIFY(verify_result_idx > run_idx);
    QVERIFY(report_idx > verify_result_idx);
    QVERIFY(cleanup_idx > report_idx);

    const auto& download = wf.phases.at(download_idx);
    QCOMPARE(download.tool, QStringLiteral("download_file"));
    QCOMPARE(download.risk, QStringLiteral("download_only"));
    const auto& run = wf.phases.at(run_idx);
    QVERIFY(run.arguments.value(QStringLiteral("command"))
                .toString()
                .contains(QStringLiteral("${result_download_tool_path}")));
    const auto& cleanup = wf.phases.at(cleanup_idx);
    QCOMPARE(cleanup.type, QStringLiteral("cleanup"));
    QVERIFY(cleanup.arguments.value(QStringLiteral("command"))
                .toString()
                .contains(QStringLiteral("Remove-Item")));
}

void AiWorkflowEvalsTests::seededWorkflowsContinueAfterEmptyDelegateFailure() {
    const QStringList files = seededWorkflowFiles();
    for (const auto& f : files) {
        QString err;
        const auto wf = loadGoldenWorkflow(f, &err);
        QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(f, err)));

        FakeModelClient model;
        model.empty_failure = true;
        TracingToolExecutor tool;
        sak::ai::AiSubagentRunner runner(&model);
        sak::ai::AiOrchestrator orchestrator(&runner, &tool);
        sak::ai::AiOrchestrationOptions opts;
        opts.stop_on_phase_failure = false;
        orchestrator.setOptions(opts);

        const QString run_id = QStringLiteral("eval_empty_delegate_%1").arg(wf.id);
        auto root = sak::ai::CancellationToken::createRoot(run_id);
        const auto result = orchestrator.run(wf, run_id, root);

        QVERIFY2(
            result.status == sak::ai::AiRunStatus::Completed ||
                result.status == sak::ai::AiRunStatus::WaitingForHuman,
            qPrintable(
                QStringLiteral("%1: empty delegate failure should not abort workflow, got %2: %3")
                    .arg(f, sak::ai::runStatusToString(result.status), result.error_message)));
    }
}

QTEST_GUILESS_MAIN(AiWorkflowEvalsTests)
#include "test_ai_workflow_evals.moc"
