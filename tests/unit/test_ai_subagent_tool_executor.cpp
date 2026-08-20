// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_subagent_tool_executor.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

namespace {

struct DispatchRecorder {
    int calls{0};
    sak::ai::AiToolPolicy policy{sak::ai::AiToolPolicy::NoLocalExecution};
    QString tool_name;
    QString operation;
    QString agent_id;
    QJsonObject arguments;
    QJsonObject result{{QStringLiteral("success"), true}};
};

sak::ai::AiSubagentToolExecutor::RawDispatch recordingDispatch(DispatchRecorder* recorder) {
    return [recorder](sak::ai::AiToolPolicy policy,
                      const sak::ai::AiToolCallRequest& request,
                      const QJsonObject& arguments,
                      const QString& agent_id,
                      const sak::ai::CancellationToken&) {
        ++recorder->calls;
        recorder->policy = policy;
        recorder->tool_name = request.tool_name;
        recorder->operation = request.operation;
        recorder->agent_id = agent_id;
        recorder->arguments = arguments;
        return recorder->result;
    };
}

sak::ai::AiSubagentToolCall makeCall(const QString& name, const QString& args_json) {
    sak::ai::AiSubagentToolCall call;
    call.call_id = QStringLiteral("call_1");
    call.name = name;
    call.arguments_json = args_json;
    return call;
}

sak::ai::AiSubagentTask makeTask(sak::ai::AiToolPolicy policy) {
    sak::ai::AiSubagentTask task;
    task.task_id = QStringLiteral("t1");
    task.agent_id = QStringLiteral("package_agent");
    task.tool_policy = policy;
    return task;
}

QJsonObject parseOutput(const sak::ai::AiSubagentToolOutput& output) {
    return QJsonDocument::fromJson(output.output_json.toUtf8()).object();
}

}  // namespace

class AiSubagentToolExecutorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void dispatchesAllowedToolWithPolicyAndArgs();
    void deniesToolNotInAllowlist();
    void deniesWhenCancelled();
    void failsClosedOnInvalidArguments();
    void emptyArgumentsDispatchWithEmptyObject();
    void missingDispatchCallbackFailsClosed();
};

void AiSubagentToolExecutorTests::dispatchesAllowedToolWithPolicyAndArgs() {
    DispatchRecorder recorder;
    recorder.result = QJsonObject{{QStringLiteral("success"), true},
                                  {QStringLiteral("packages"), 3}};
    sak::ai::AiSubagentToolExecutor executor(recordingDispatch(&recorder));
    executor.setAllowedTools({QStringLiteral("sak_package_manager")});

    const auto task = makeTask(sak::ai::AiToolPolicy::PackageToolsOnly);
    const auto call = makeCall(QStringLiteral("sak_package_manager"),
                               QStringLiteral("{\"operation\":\"list\"}"));
    const auto output = executor.executeToolCall(
        task, call, sak::ai::CancellationToken::createRoot(QStringLiteral("r")));

    QCOMPARE(recorder.calls, 1);
    QCOMPARE(recorder.policy, sak::ai::AiToolPolicy::PackageToolsOnly);
    QCOMPARE(recorder.tool_name, QStringLiteral("sak_package_manager"));
    QCOMPARE(recorder.operation, QStringLiteral("list"));
    QCOMPARE(recorder.agent_id, QStringLiteral("package_agent"));
    QCOMPARE(output.call_id, QStringLiteral("call_1"));
    const QJsonObject parsed = parseOutput(output);
    QCOMPARE(parsed.value(QStringLiteral("success")).toBool(), true);
    QCOMPARE(parsed.value(QStringLiteral("packages")).toInt(), 3);
}

void AiSubagentToolExecutorTests::deniesToolNotInAllowlist() {
    DispatchRecorder recorder;
    sak::ai::AiSubagentToolExecutor executor(recordingDispatch(&recorder));
    executor.setAllowedTools({QStringLiteral("sak_package_manager")});

    const auto task = makeTask(sak::ai::AiToolPolicy::ReadOnlyPc);
    const auto call = makeCall(QStringLiteral("run_powershell"), QStringLiteral("{}"));
    const auto output = executor.executeToolCall(
        task, call, sak::ai::CancellationToken::createRoot(QStringLiteral("r")));

    // Fail closed: never dispatched, error names the tool.
    QCOMPARE(recorder.calls, 0);
    const QJsonObject parsed = parseOutput(output);
    QCOMPARE(parsed.value(QStringLiteral("error")).toString(),
             QStringLiteral("Tool 'run_powershell' is not permitted for subagents"));
    QCOMPARE(parsed.value(QStringLiteral("tool")).toString(), QStringLiteral("run_powershell"));
}

void AiSubagentToolExecutorTests::deniesWhenCancelled() {
    DispatchRecorder recorder;
    sak::ai::AiSubagentToolExecutor executor(recordingDispatch(&recorder));
    executor.setAllowedTools({QStringLiteral("sak_package_manager")});

    auto token = sak::ai::CancellationToken::createRoot(QStringLiteral("r"));
    token.cancel(QStringLiteral("stopped"));
    const auto output = executor.executeToolCall(makeTask(sak::ai::AiToolPolicy::PackageToolsOnly),
                                                 makeCall(QStringLiteral("sak_package_manager"),
                                                          QStringLiteral("{}")),
                                                 token);

    QCOMPARE(recorder.calls, 0);
    QCOMPARE(parseOutput(output).value(QStringLiteral("error")).toString(),
             QStringLiteral("Cancelled before tool dispatch"));
}

void AiSubagentToolExecutorTests::failsClosedOnInvalidArguments() {
    DispatchRecorder recorder;
    sak::ai::AiSubagentToolExecutor executor(recordingDispatch(&recorder));
    executor.setAllowedTools({QStringLiteral("sak_package_manager")});

    const auto output = executor.executeToolCall(
        makeTask(sak::ai::AiToolPolicy::PackageToolsOnly),
        makeCall(QStringLiteral("sak_package_manager"), QStringLiteral("{not valid json")),
        sak::ai::CancellationToken::createRoot(QStringLiteral("r")));

    QCOMPARE(recorder.calls, 0);
    // The %1 tail is QJsonParseError::errorString() (Qt-version-dependent), so pin only the
    // deterministic prefix -- but anchored with startsWith, including the "JSON:" token.
    QVERIFY(parseOutput(output)
                .value(QStringLiteral("error"))
                .toString()
                .startsWith(QStringLiteral("Invalid tool arguments JSON: ")));
}

void AiSubagentToolExecutorTests::emptyArgumentsDispatchWithEmptyObject() {
    DispatchRecorder recorder;
    sak::ai::AiSubagentToolExecutor executor(recordingDispatch(&recorder));
    executor.setAllowedTools({QStringLiteral("sak_session_search")});

    const auto output =
        executor.executeToolCall(makeTask(sak::ai::AiToolPolicy::ReadOnlyPc),
                                 makeCall(QStringLiteral("sak_session_search"), QString()),
                                 sak::ai::CancellationToken::createRoot(QStringLiteral("r")));

    QCOMPARE(recorder.calls, 1);
    QVERIFY(recorder.arguments.isEmpty());
    QCOMPARE(parseOutput(output).value(QStringLiteral("success")).toBool(), true);
}

void AiSubagentToolExecutorTests::missingDispatchCallbackFailsClosed() {
    sak::ai::AiSubagentToolExecutor executor(nullptr);
    executor.setAllowedTools({QStringLiteral("sak_package_manager")});

    const auto output = executor.executeToolCall(
        makeTask(sak::ai::AiToolPolicy::PackageToolsOnly),
        makeCall(QStringLiteral("sak_package_manager"), QStringLiteral("{}")),
        sak::ai::CancellationToken::createRoot(QStringLiteral("r")));

    QCOMPARE(parseOutput(output).value(QStringLiteral("error")).toString(),
             QStringLiteral("No subagent tool dispatcher configured"));
}

QTEST_GUILESS_MAIN(AiSubagentToolExecutorTests)
#include "test_ai_subagent_tool_executor.moc"
