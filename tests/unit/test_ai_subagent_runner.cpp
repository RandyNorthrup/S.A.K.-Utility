// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_subagent_runner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QtTest/QtTest>

#include <memory>

class FakeModelClient : public sak::ai::IAiModelClient {
public:
    Response invoke(const Request& request, const sak::ai::CancellationToken& token) override {
        last_request = request;
        ++invocation_count;
        if (invoke_sleep_ms > 0) {
            QThread::msleep(static_cast<unsigned long>(invoke_sleep_ms));
        }
        if (cancel_before_response && token.isValid()) {
            token.cancel(QStringLiteral("fake_cancelled"));
        }
        if (!scripted_responses.isEmpty()) {
            Response r = scripted_responses.takeFirst();
            return r;
        }
        return next_response;
    }

    Response continueWithToolOutputs(const Request& request,
                                     const QString& response_id,
                                     const QVector<sak::ai::AiSubagentToolOutput>& outputs,
                                     const sak::ai::CancellationToken&) override {
        last_request = request;
        last_continuation_response_id = response_id;
        received_outputs.append(outputs);
        ++continuation_count;
        if (!scripted_responses.isEmpty()) {
            return scripted_responses.takeFirst();
        }
        return next_response;
    }

    Request last_request;
    Response next_response;
    QList<Response> scripted_responses;
    bool cancel_before_response{false};
    int invoke_sleep_ms{0};
    int invocation_count{0};
    int continuation_count{0};
    QString last_continuation_response_id;
    QVector<sak::ai::AiSubagentToolOutput> received_outputs;
};

// Records every tool call it is asked to run and returns a fixed JSON output.
class RecordingToolExecutor : public sak::ai::IAiSubagentToolExecutor {
public:
    sak::ai::AiSubagentToolOutput executeToolCall(const sak::ai::AiSubagentTask& task,
                                                  const sak::ai::AiSubagentToolCall& call,
                                                  const sak::ai::CancellationToken&) override {
        executed_calls.append(call.name);
        last_policy = task.tool_policy;
        sak::ai::AiSubagentToolOutput out;
        out.call_id = call.call_id;
        out.output_json = QStringLiteral("{\"ok\":true}");
        return out;
    }

    QStringList executed_calls;
    sak::ai::AiToolPolicy last_policy{sak::ai::AiToolPolicy::NoLocalExecution};
};

// Cancels the agent token WHILE executing the first tool call, so a later call in the
// same batch must be skipped by the per-call cancellation recheck.
class CancelOnFirstToolExecutor : public sak::ai::IAiSubagentToolExecutor {
public:
    sak::ai::AiSubagentToolOutput executeToolCall(
        const sak::ai::AiSubagentTask&,
        const sak::ai::AiSubagentToolCall& call,
        const sak::ai::CancellationToken& token) override {
        executed_calls.append(call.name);
        token.cancel(QStringLiteral("cancelled mid-batch"));
        sak::ai::AiSubagentToolOutput out;
        out.call_id = call.call_id;
        out.output_json = QStringLiteral("{\"ok\":true}");
        return out;
    }

    QStringList executed_calls;
};

sak::ai::IAiModelClient::Response makeToolCallResponse(const QString& tool_name,
                                                       const QString& call_id,
                                                       const QString& response_id) {
    sak::ai::IAiModelClient::Response response;
    response.success = true;
    response.response_id = response_id;
    sak::ai::AiSubagentToolCall call;
    call.call_id = call_id;
    call.name = tool_name;
    call.arguments_json = QStringLiteral("{}");
    response.tool_calls.append(call);
    return response;
}

sak::ai::IAiModelClient::Response makeCompleteResponse(const QString& summary) {
    sak::ai::IAiModelClient::Response response;
    response.success = true;
    QJsonObject payload;
    payload[QStringLiteral("status")] = QStringLiteral("complete");
    payload[QStringLiteral("summary")] = summary;
    response.text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    return response;
}

class FactoryCountingModelClient : public sak::ai::IAiModelClient {
public:
    FactoryCountingModelClient(int* created, int* live, int* invocations)
        : m_created(created), m_live(live), m_invocations(invocations) {
        ++(*m_created);
        ++(*m_live);
    }

    ~FactoryCountingModelClient() override { --(*m_live); }

    Response invoke(const Request&, const sak::ai::CancellationToken&) override {
        ++(*m_invocations);
        Response response;
        response.success = true;
        QJsonObject payload;
        payload[QStringLiteral("status")] = QStringLiteral("complete");
        payload[QStringLiteral("summary")] = QStringLiteral("factory ok");
        response.text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        return response;
    }

private:
    int* m_created{nullptr};
    int* m_live{nullptr};
    int* m_invocations{nullptr};
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
    void lateSuccessAfterDeadlineDemotedToTimedOut();
    void perTaskTimeoutBoundsRun();
    void factoryCreatesFreshClientPerRun();
    void cancelledParentDoesNotCreateFactoryClient();
    void actingSubagentExecutesToolCallsThenCompletes();
    void toolLoopStopsAtIterationCapWithDegraded();
    void noLocalExecutionPolicyStaysSingleShot();
    void usageAccumulatesAcrossToolRoundTrips();
    void executedToolsRecordedWhenContinuationFails();
    void perCallCancellationStopsRemainingToolCalls();
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

    // A contentless "failed" subagent must be reported honestly as Degraded, not
    // laundered into Complete. It still continues (non-fatal) with a risk recorded.
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Degraded);
    // Pin the honest degradation summary. `!isEmpty()` could not tell the auditable
    // "recording this phase as degraded" message apart from any laundered success text.
    QCOMPARE(result.summary,
             QStringLiteral("Subagent returned a failed status without explanation or content; "
                            "recording this phase as degraded and continuing with available prior "
                            "workflow evidence."));
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
    result.usage.input_tokens = 100;
    result.usage.cached_input_tokens = 40;
    result.usage.output_tokens = 50;
    result.usage.reasoning_tokens = 30;
    result.usage.total_tokens = 220;
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
    // All four detailed token counts must survive the round trip, not just totals.
    QCOMPARE(result_copy.usage.input_tokens, 100);
    QCOMPARE(result_copy.usage.cached_input_tokens, 40);
    QCOMPARE(result_copy.usage.output_tokens, 50);
    QCOMPARE(result_copy.usage.reasoning_tokens, 30);
    QCOMPARE(result_copy.usage.total_tokens, 220);
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

void AiSubagentRunnerTests::lateSuccessAfterDeadlineDemotedToTimedOut() {
    // The model returns a valid "complete" result, but only AFTER the wall clock
    // has elapsed (the invoke() call ran past the deadline). A result produced past
    // budget must never be laundered into a clean success: it is demoted to
    // TimedOut. This is the within-file half of the deadline-enforcement fix (the
    // HTTP-wait bound itself needs an interface change and is deferred).
    FakeModelClient client;
    client.invoke_sleep_ms = 80;  // response arrives after the 20ms budget
    client.next_response = makeCompleteResponse(QStringLiteral("late but done"));

    sak::ai::AiSubagentRunner runner(&client);
    sak::ai::AiSubagentRunnerOptions opts;
    opts.wall_clock_timeout_ms = 20;  // shorter than the model's response latency
    runner.setOptions(opts);

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("late");
    task.agent_id = QStringLiteral("a");

    const auto result = runner.run(task, {});
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::TimedOut);
    QCOMPARE(client.invocation_count, 1);  // the model WAS invoked, then demoted
}

void AiSubagentRunnerTests::perTaskTimeoutBoundsRun() {
    // B12-06: the per-task timeout_seconds must bound the run even when the runner-wide wall
    // clock is unset (0 = unbounded). Before the fix the task timeout was parsed but ignored, so
    // this run would exhaust every retry and end Failed; now it ends TimedOut at the task budget.
    FakeModelClient client;
    sak::ai::IAiModelClient::Response fail;
    fail.success = false;
    fail.error_message = QStringLiteral("transient");
    for (int i = 0; i < 12; ++i) {
        client.scripted_responses << fail;
    }

    sak::ai::AiSubagentRunner runner(&client);
    sak::ai::AiSubagentRunnerOptions opts;
    opts.max_retries = 11;
    opts.retry_delay_ms = 300;       // 11 delays (~3.3s) far exceed the 1s task budget
    opts.wall_clock_timeout_ms = 0;  // runner-wide UNBOUNDED: only the per-task timeout can fire
    runner.setOptions(opts);

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("task_bound");
    task.agent_id = QStringLiteral("a");
    task.timeout_seconds = 1;  // 1s per-task budget

    const auto result = runner.run(task, {});
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::TimedOut);
    QVERIFY(client.invocation_count < 12);  // stopped before exhausting all retries
}

void AiSubagentRunnerTests::factoryCreatesFreshClientPerRun() {
    int created = 0;
    int live = 0;
    int invocations = 0;
    sak::ai::AiSubagentRunner runner(nullptr);
    runner.setModelClientFactory([&created, &live, &invocations]() {
        return std::make_unique<FactoryCountingModelClient>(&created, &live, &invocations);
    });

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("factory");
    task.agent_id = QStringLiteral("a");

    const auto first = runner.run(task, {});
    const auto second = runner.run(task, {});

    QCOMPARE(first.status, sak::ai::AiSubagentStatus::Complete);
    QCOMPARE(second.status, sak::ai::AiSubagentStatus::Complete);
    QCOMPARE(created, 2);
    QCOMPARE(invocations, 2);
    QCOMPARE(live, 0);
}

void AiSubagentRunnerTests::cancelledParentDoesNotCreateFactoryClient() {
    int created = 0;
    int live = 0;
    int invocations = 0;
    sak::ai::AiSubagentRunner runner(nullptr);
    runner.setModelClientFactory([&created, &live, &invocations]() {
        return std::make_unique<FactoryCountingModelClient>(&created, &live, &invocations);
    });

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("factory_cancel");
    task.agent_id = QStringLiteral("a");
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run"));
    root.cancel(QStringLiteral("already_cancelled"));

    const auto result = runner.run(task, root);

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Cancelled);
    QCOMPARE(created, 0);
    QCOMPARE(invocations, 0);
    QCOMPARE(live, 0);
}

void AiSubagentRunnerTests::actingSubagentExecutesToolCallsThenCompletes() {
    FakeModelClient client;
    // Turn 1: model asks to run a tool. Turn 2 (continuation): final answer.
    client.scripted_responses.append(makeToolCallResponse(
        QStringLiteral("run_powershell"), QStringLiteral("call_1"), QStringLiteral("resp_1")));
    client.scripted_responses.append(makeCompleteResponse(QStringLiteral("Did the work")));

    RecordingToolExecutor executor;
    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("act_1");
    task.agent_id = QStringLiteral("repair_agent");
    task.tool_policy = sak::ai::AiToolPolicy::ReadOnlyPc;  // != NoLocalExecution -> tools allowed

    sak::ai::AiSubagentRunner runner(&client);
    runner.setToolExecutor(&executor);
    const auto result = runner.run(task,
                                   sak::ai::CancellationToken::createRoot(QStringLiteral("run")));

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Complete);
    // The tool was actually executed under the task's policy.
    QCOMPARE(executor.executed_calls, QStringList{QStringLiteral("run_powershell")});
    QCOMPARE(executor.last_policy, sak::ai::AiToolPolicy::ReadOnlyPc);
    // The continuation carried the tool output back keyed to the model's response_id.
    QCOMPARE(client.continuation_count, 1);
    QCOMPARE(client.last_continuation_response_id, QStringLiteral("resp_1"));
    QCOMPARE(client.received_outputs.size(), 1);
    QCOMPARE(client.received_outputs.first().call_id, QStringLiteral("call_1"));
    // actions_taken reflects the REAL executed tool, not just a model claim.
    QVERIFY(result.actions_taken.contains(QStringLiteral("executed tool: run_powershell")));
    // Tools were offered to the model because the policy permits local execution.
    QVERIFY(client.last_request.enable_local_tools);
}

void AiSubagentRunnerTests::toolLoopStopsAtIterationCapWithDegraded() {
    FakeModelClient client;
    // Model never stops asking for tools; every response requests another call.
    client.next_response = makeToolCallResponse(QStringLiteral("run_powershell"),
                                                QStringLiteral("loop_call"),
                                                QStringLiteral("resp_loop"));

    RecordingToolExecutor executor;
    sak::ai::AiSubagentRunner runner(&client);
    runner.setToolExecutor(&executor);
    sak::ai::AiSubagentRunnerOptions options;
    options.max_tool_iterations = 2;
    runner.setOptions(options);

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("act_cap");
    task.tool_policy = sak::ai::AiToolPolicy::ReadOnlyPc;

    const auto result = runner.run(task,
                                   sak::ai::CancellationToken::createRoot(QStringLiteral("run")));

    // Hitting the cap yields an honest Degraded status, not a fake Complete.
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Degraded);
    QVERIFY(result.risks.join(QStringLiteral("\n")).contains(QStringLiteral("iteration cap")));
    // Exactly the cap number of tool round trips ran before stopping.
    QCOMPARE(executor.executed_calls.size(), 2);
}

void AiSubagentRunnerTests::noLocalExecutionPolicyStaysSingleShot() {
    FakeModelClient client;
    // The model requests a tool, but the task policy forbids local execution.
    client.next_response = makeToolCallResponse(QStringLiteral("run_powershell"),
                                                QStringLiteral("call_x"),
                                                QStringLiteral("resp_x"));

    RecordingToolExecutor executor;
    sak::ai::AiSubagentRunner runner(&client);
    runner.setToolExecutor(&executor);

    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("no_exec");
    task.tool_policy = sak::ai::AiToolPolicy::NoLocalExecution;  // fail closed

    const auto result = runner.run(task,
                                   sak::ai::CancellationToken::createRoot(QStringLiteral("run")));

    // The executor is never invoked and tools are not offered to the model.
    QVERIFY(executor.executed_calls.isEmpty());
    QVERIFY(!client.last_request.enable_local_tools);
    QCOMPARE(client.continuation_count, 0);
    // The tool-call-only response has no final JSON, so it surfaces as a failure
    // rather than being silently accepted.
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Failed);
}

void AiSubagentRunnerTests::usageAccumulatesAcrossToolRoundTrips() {
    FakeModelClient client;
    auto tool_turn = makeToolCallResponse(QStringLiteral("run_powershell"),
                                          QStringLiteral("c1"),
                                          QStringLiteral("r1"));
    tool_turn.usage.total_tokens = 100;
    auto final_turn = makeCompleteResponse(QStringLiteral("done"));
    final_turn.usage.total_tokens = 50;
    client.scripted_responses.append(tool_turn);
    client.scripted_responses.append(final_turn);

    RecordingToolExecutor executor;
    sak::ai::AiSubagentRunner runner(&client);
    runner.setToolExecutor(&executor);
    sak::ai::AiSubagentTask task;
    task.tool_policy = sak::ai::AiToolPolicy::ReadOnlyPc;

    const auto result = runner.run(task,
                                   sak::ai::CancellationToken::createRoot(QStringLiteral("run")));

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Complete);
    // Both the tool turn (100) and the final turn (50) are counted, not just one.
    QCOMPARE(result.usage.total_tokens, static_cast<qint64>(150));
}

void AiSubagentRunnerTests::executedToolsRecordedWhenContinuationFails() {
    FakeModelClient client;
    client.scripted_responses.append(makeToolCallResponse(
        QStringLiteral("run_powershell"), QStringLiteral("c1"), QStringLiteral("r1")));
    sak::ai::IAiModelClient::Response failed_continuation;
    failed_continuation.success = false;
    failed_continuation.error_message = QStringLiteral("boom");
    client.scripted_responses.append(failed_continuation);

    RecordingToolExecutor executor;
    sak::ai::AiSubagentRunner runner(&client);
    runner.setToolExecutor(&executor);
    sak::ai::AiSubagentTask task;
    task.tool_policy = sak::ai::AiToolPolicy::ReadOnlyPc;

    const auto result = runner.run(task,
                                   sak::ai::CancellationToken::createRoot(QStringLiteral("run")));

    // The turn failed, but the tool that DID run must still be recorded so the
    // ground-truth "what did it do" survives on the failure path.
    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Failed);
    QVERIFY(result.actions_taken.contains(QStringLiteral("executed tool: run_powershell")));
}

void AiSubagentRunnerTests::perCallCancellationStopsRemainingToolCalls() {
    // A single response asks for TWO tool calls. The first call cancels the agent token;
    // the second must NOT execute -- cancellation is rechecked BEFORE each call, not just
    // once per response. Regression for the mid-batch cancellation fix.
    FakeModelClient client;
    sak::ai::IAiModelClient::Response two_calls;
    two_calls.success = true;
    two_calls.response_id = QStringLiteral("resp_multi");
    for (const auto& id : {QStringLiteral("call_1"), QStringLiteral("call_2")}) {
        sak::ai::AiSubagentToolCall call;
        call.call_id = id;
        call.name = QStringLiteral("run_powershell");
        call.arguments_json = QStringLiteral("{}");
        two_calls.tool_calls.append(call);
    }
    client.next_response = two_calls;

    CancelOnFirstToolExecutor executor;
    sak::ai::AiSubagentRunner runner(&client);
    runner.setToolExecutor(&executor);
    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("mid_batch");
    task.tool_policy = sak::ai::AiToolPolicy::ReadOnlyPc;

    const auto result = runner.run(task,
                                   sak::ai::CancellationToken::createRoot(QStringLiteral("run")));

    QCOMPARE(result.status, sak::ai::AiSubagentStatus::Cancelled);
    // Only the first call ran; the second was skipped by the per-call recheck.
    QCOMPARE(executor.executed_calls.size(), 1);
    // The batch was abandoned before continuing the model with tool outputs.
    QCOMPARE(client.continuation_count, 0);
}

QTEST_GUILESS_MAIN(AiSubagentRunnerTests)
#include "test_ai_subagent_runner.moc"
