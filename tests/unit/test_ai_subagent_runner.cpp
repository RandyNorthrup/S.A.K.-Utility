// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_subagent_runner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

class FakeModelClient : public sak::ai::IAiModelClient {
public:
    Response invoke(const Request& request, const sak::ai::CancellationToken& token) override {
        last_request = request;
        ++invocation_count;
        if (cancel_before_response && token.isValid()) {
            token.cancel(QStringLiteral("fake_cancelled"));
        }
        if (!scripted_responses.isEmpty()) {
            Response r = scripted_responses.takeFirst();
            return r;
        }
        return next_response;
    }

    Request last_request;
    Response next_response;
    QList<Response> scripted_responses;
    bool cancel_before_response{false};
    int invocation_count{0};
};

class AiSubagentRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void completeRoundTripPopulatesFields();
    void malformedJsonReportsFailure();
    void modelFailureSurfaces();
    void cancelledParentTokenSkipsInvoke();
    void cancellationDuringInvokeReportsCancelled();
    void exceededTokenBudgetRecordsRisk();
    void emptyFailedModelStatusContinuesDegraded();
    void taskAndResultJsonRoundTrip();
    void retriesUntilSuccess();
    void retriesExhaustedReturnsLastFailure();
    void wallClockTimeoutMarksTimedOut();
};

void AiSubagentRunnerTests::completeRoundTripPopulatesFields() {
    FakeModelClient client;
    QJsonObject payload;
    payload[QStringLiteral("status")] = QStringLiteral("complete");
    payload[QStringLiteral("summary")] = QStringLiteral("Drive healthy");
    QJsonArray findings;
    QJsonObject finding;
    finding[QStringLiteral("severity")] = QStringLiteral("info");
    finding[QStringLiteral("title")] = QStringLiteral("SMART OK");
    finding[QStringLiteral("recommendation")] = QStringLiteral("None");
    findings.append(finding);
    payload[QStringLiteral("findings")] = findings;
    payload[QStringLiteral("confidence")] = 0.91;
    client.next_response.success = true;
    client.next_response.text =
        QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    client.next_response.usage.input_tokens = 100;
    client.next_response.usage.output_tokens = 50;
    client.next_response.usage.total_tokens = 150;

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_1");
    task.agent_id = QStringLiteral("diagnostic_agent");
    task.objective = QStringLiteral("Check drive");
    task.token_budget = 8000;

    sak::ai::AiSubagentRunner runner(&client);
    const auto result = runner.run(task,
                                   sak::ai::CancellationToken::createRoot(QStringLiteral("run")));

    QCOMPARE(result.task_id, QStringLiteral("task_1"));
    QCOMPARE(result.agent_id, QStringLiteral("diagnostic_agent"));
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Complete);
    QCOMPARE(result.summary, QStringLiteral("Drive healthy"));
    QCOMPARE(result.findings.size(), 1);
    QCOMPARE(result.findings.first().title, QStringLiteral("SMART OK"));
    QCOMPARE(result.confidence, 0.91);
    QCOMPARE(result.usage.total_tokens, 150);
    QVERIFY(client.last_request.context.contains(QStringLiteral("Tool policy:")));
}

void AiSubagentRunnerTests::malformedJsonReportsFailure() {
    FakeModelClient client;
    client.next_response.success = true;
    client.next_response.text = QStringLiteral("not json at all");

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_2");
    task.agent_id = QStringLiteral("research_agent");

    sak::ai::AiSubagentRunner runner(&client);
    const auto result = runner.run(task, {});

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Failed);
    QVERIFY(result.error_message.contains(QStringLiteral("non-JSON")));
}

void AiSubagentRunnerTests::modelFailureSurfaces() {
    FakeModelClient client;
    client.next_response.success = false;
    client.next_response.error_message = QStringLiteral("HTTP 429");
    client.next_response.usage.total_tokens = 0;

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_3");
    task.agent_id = QStringLiteral("research_agent");

    sak::ai::AiSubagentRunner runner(&client);
    const auto result = runner.run(task, {});

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Failed);
    QVERIFY(result.error_message.contains(QStringLiteral("HTTP 429")));
}

void AiSubagentRunnerTests::cancelledParentTokenSkipsInvoke() {
    FakeModelClient client;
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run"));
    root.cancel(QStringLiteral("user_cancelled"));

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_4");
    task.agent_id = QStringLiteral("repair_agent");

    sak::ai::AiSubagentRunner runner(&client);
    const auto result = runner.run(task, root);

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Cancelled);
    QVERIFY(client.last_request.objective.isEmpty());
}

void AiSubagentRunnerTests::cancellationDuringInvokeReportsCancelled() {
    FakeModelClient client;
    client.cancel_before_response = true;
    client.next_response.success = true;
    client.next_response.text = QStringLiteral("{\"status\":\"complete\"}");

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_5");
    task.agent_id = QStringLiteral("repair_agent");

    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run"));
    sak::ai::AiSubagentRunner runner(&client);
    const auto result = runner.run(task, root);

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Cancelled);
}

void AiSubagentRunnerTests::exceededTokenBudgetRecordsRisk() {
    FakeModelClient client;
    QJsonObject payload;
    payload[QStringLiteral("status")] = QStringLiteral("complete");
    payload[QStringLiteral("summary")] = QStringLiteral("Big output");
    client.next_response.success = true;
    client.next_response.text =
        QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    client.next_response.usage.total_tokens = 9001;

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_6");
    task.agent_id = QStringLiteral("research_agent");
    task.token_budget = 1000;

    sak::ai::AiSubagentRunner runner(&client);
    const auto result = runner.run(task, {});

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Complete);
    QVERIFY(result.error_message.isEmpty());
    QVERIFY(result.risks.join(QStringLiteral("\n")).contains(QStringLiteral("token budget")));
}

void AiSubagentRunnerTests::emptyFailedModelStatusContinuesDegraded() {
    FakeModelClient client;
    QJsonObject payload;
    payload[QStringLiteral("status")] = QStringLiteral("failed");
    client.next_response.success = true;
    client.next_response.text =
        QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    client.next_response.usage.total_tokens = 1200;

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_degraded");
    task.agent_id = QStringLiteral("diagnostic_agent");
    task.expected_output_schema = QStringLiteral("drive_evidence");

    sak::ai::AiSubagentRunner runner(&client);
    const auto result = runner.run(task, {});

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Complete);
    QVERIFY(!result.summary.isEmpty());
    QVERIFY(result.risks.join(QStringLiteral("\n")).contains(QStringLiteral("failed status")));
    QVERIFY(client.last_request.context.contains(
        QStringLiteral("Output schema label: drive_evidence")));
    QVERIFY(client.last_request.context.contains(QStringLiteral("Use status 'complete'")));
}

void AiSubagentRunnerTests::taskAndResultJsonRoundTrip() {
    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_7");
    task.agent_id = QStringLiteral("package_agent");
    task.objective = QStringLiteral("Find chrome");
    task.tool_policy = sak::ai::AiToolPolicy::PackageToolsOnly;
    task.token_budget = 6000;
    task.context_refs << QStringLiteral("ctx_001");
    task.instructions_refs << QStringLiteral("software-package-selection");

    const auto task_copy = sak::ai::AiSubagentTask::fromJson(task.toJson());
    QCOMPARE(task_copy.task_id, task.task_id);
    QCOMPARE(task_copy.tool_policy, sak::ai::AiToolPolicy::PackageToolsOnly);
    QCOMPARE(task_copy.token_budget, 6000);
    QCOMPARE(task_copy.context_refs, task.context_refs);
    QCOMPARE(task_copy.instructions_refs, task.instructions_refs);

    sak::ai::AiSubagentResult result;
    result.task_id = task.task_id;
    result.agent_id = task.agent_id;
    result.status = sak::ai::AiSubagentStatus::Complete;
    result.summary = QStringLiteral("googlechrome candidate");
    result.confidence = 0.75;
    result.artifacts << QStringLiteral("artifacts/manifest.json");
    sak::ai::AiSubagentFinding finding;
    finding.severity = QStringLiteral("info");
    finding.title = QStringLiteral("Candidate found");
    finding.recommendation = QStringLiteral("direct_download");
    result.findings.append(finding);

    const auto result_copy = sak::ai::AiSubagentResult::fromJson(result.toJson());
    QCOMPARE(result_copy.status, sak::ai::AiSubagentStatus::Complete);
    QCOMPARE(result_copy.summary, result.summary);
    QCOMPARE(result_copy.artifacts, result.artifacts);
    QCOMPARE(result_copy.findings.size(), 1);
    QCOMPARE(result_copy.findings.first().title, finding.title);
    QCOMPARE(result_copy.confidence, 0.75);
}

void AiSubagentRunnerTests::retriesUntilSuccess() {
    FakeModelClient client;
    sak::ai::IAiModelClient::Response fail;
    fail.success = false;
    fail.error_message = QStringLiteral("transient");
    client.scripted_responses << fail;
    sak::ai::IAiModelClient::Response ok;
    ok.success = true;
    QJsonObject payload;
    payload[QStringLiteral("status")] = QStringLiteral("complete");
    payload[QStringLiteral("summary")] = QStringLiteral("ok after retry");
    ok.text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    client.scripted_responses << ok;

    sak::ai::AiSubagentRunner runner(&client);
    sak::ai::AiSubagentRunnerOptions opts;
    opts.max_retries = 2;
    runner.setOptions(opts);

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("retry_ok");
    task.agent_id = QStringLiteral("a");

    const auto result = runner.run(task, {});
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Complete);
    QCOMPARE(client.invocation_count, 2);
}

void AiSubagentRunnerTests::retriesExhaustedReturnsLastFailure() {
    FakeModelClient client;
    sak::ai::IAiModelClient::Response fail;
    fail.success = false;
    fail.error_message = QStringLiteral("HTTP 500");
    client.scripted_responses << fail << fail << fail;

    sak::ai::AiSubagentRunner runner(&client);
    sak::ai::AiSubagentRunnerOptions opts;
    opts.max_retries = 2;
    runner.setOptions(opts);

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("retry_fail");
    task.agent_id = QStringLiteral("a");

    const auto result = runner.run(task, {});
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Failed);
    QVERIFY(result.error_message.contains(QStringLiteral("HTTP 500")));
    QCOMPARE(client.invocation_count, 3);
}

void AiSubagentRunnerTests::wallClockTimeoutMarksTimedOut() {
    FakeModelClient client;
    sak::ai::IAiModelClient::Response fail;
    fail.success = false;
    fail.error_message = QStringLiteral("transient");
    for (int i = 0; i < 6; ++i) {
        client.scripted_responses << fail;
    }

    sak::ai::AiSubagentRunner runner(&client);
    sak::ai::AiSubagentRunnerOptions opts;
    opts.max_retries = 5;
    opts.retry_delay_ms = 40;
    opts.wall_clock_timeout_ms = 50;
    runner.setOptions(opts);

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("timeout");
    task.agent_id = QStringLiteral("a");

    const auto result = runner.run(task, {});
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::TimedOut);
    QVERIFY(result.error_message.contains(QStringLiteral("timeout")));
    QVERIFY(client.invocation_count < 6);
}

QTEST_GUILESS_MAIN(AiSubagentRunnerTests)
#include "test_ai_subagent_runner.moc"
