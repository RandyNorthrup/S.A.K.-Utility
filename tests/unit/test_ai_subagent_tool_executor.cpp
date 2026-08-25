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
    // The executor derives TWO fields from the argument object; only `operation` was recorded.
    // command_preview is what the policy layer classifies (read-only / catastrophic / obfuscated)
    // and what the result recorder logs as the operator-facing preview, so a mis-sourced or
    // deleted command_preview failed nothing at all.
    QString command_preview;
    // The forwarded cancellation token: the lambda used to drop this parameter entirely, so no
    // test could see WHICH token the dispatcher was handed -- and the panel's real dispatch
    // re-checks it, which is the only cancellation point once a long-running tool has started.
    bool token_valid = false;
    bool token_cancelled = false;
    QJsonObject result{{QStringLiteral("success"), true}};
};

sak::ai::AiSubagentToolExecutor::RawDispatch recordingDispatch(DispatchRecorder* recorder) {
    return [recorder](sak::ai::AiToolPolicy policy,
                      const sak::ai::AiToolCallRequest& request,
                      const QJsonObject& arguments,
                      const QString& agent_id,
                      const sak::ai::CancellationToken& token) {
        ++recorder->calls;
        recorder->policy = policy;
        recorder->tool_name = request.tool_name;
        recorder->operation = request.operation;
        recorder->command_preview = request.command_preview;
        recorder->agent_id = agent_id;
        recorder->arguments = arguments;
        recorder->token_valid = token.isValid();
        recorder->token_cancelled = token.isValid() && token.isCancellationRequested();
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
    void argumentSizeCapIsExactAtBothSides();
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
    // The arguments carry a "query" key as well: the executor derives command_preview from it,
    // and no fixture in the file had one at all. Note the asymmetry the pin closes -- mis-sourcing
    // `operation` from "query" would fail the operation assertion, while mis-sourcing or deleting
    // command_preview failed nothing.
    const auto call = makeCall(
        QStringLiteral("sak_package_manager"),
        QStringLiteral("{\"operation\":\"list\",\"query\":\"winget list --id Foo\",\"extra\":7}"));
    const auto output = executor.executeToolCall(
        task, call, sak::ai::CancellationToken::createRoot(QStringLiteral("r")));

    QCOMPARE(recorder.calls, 1);
    QCOMPARE(recorder.policy, sak::ai::AiToolPolicy::PackageToolsOnly);
    QCOMPARE(recorder.tool_name, QStringLiteral("sak_package_manager"));
    QCOMPARE(recorder.operation, QStringLiteral("list"));
    QCOMPARE(recorder.command_preview, QStringLiteral("winget list --id Foo"));
    QCOMPARE(recorder.agent_id, QStringLiteral("package_agent"));
    // The parsed object must REACH the dispatcher, whole. The operation assertion above proves
    // only that the executor parsed it -- that field is read inside the executor before the
    // dispatch call -- and the panel hands this object straight to the tool dispatcher, so it is
    // the entire tool payload.
    QCOMPARE(recorder.arguments.value(QStringLiteral("operation")).toString(),
             QStringLiteral("list"));
    QCOMPARE(recorder.arguments.value(QStringLiteral("query")).toString(),
             QStringLiteral("winget list --id Foo"));
    QCOMPARE(recorder.arguments.value(QStringLiteral("extra")).toInt(), 7);
    QCOMPARE(recorder.arguments.size(), 3);
    // The live token must be forwarded, not a default-constructed one: the real dispatch re-checks
    // it, and it is the only cancellation point once a long-running tool has started.
    QVERIFY2(recorder.token_valid, "the caller's cancellation token must be forwarded");
    QVERIFY(!recorder.token_cancelled);
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
    // The correlation id on the REFUSAL path. Only the happy path asserted call_id, so the id
    // errorOutput copies was unconstrained on every fail-closed path -- and it is contractual:
    // the runner rejects an output whose call_id does not match the call it dispatched and turns
    // the whole turn into a hard broken-execution, so a refusal that loses its id stops being a
    // refusal the model can see.
    QCOMPARE(output.call_id, QStringLiteral("call_1"));

    // NEAR MISSES of the allowlist. `run_powershell` shares no prefix, suffix or substring with
    // the allowed name, so any loosening of the exact QSet::contains compare still refuses it.
    // The real allowlist is five sak_-prefixed names -- exactly the shape where a loosened
    // compare admits sak_package_manager<suffix>.
    const QStringList near_misses{QStringLiteral("sak_package_manager2"),
                                  QStringLiteral("sak_package_manage"),
                                  QStringLiteral("xsak_package_manager"),
                                  QStringLiteral("sak_package"),
                                  QStringLiteral("sak_")};
    for (const QString& name : near_misses) {
        const auto refused =
            executor.executeToolCall(task,
                                     makeCall(name, QStringLiteral("{}")),
                                     sak::ai::CancellationToken::createRoot(QStringLiteral("r")));
        QVERIFY2(parseOutput(refused)
                     .value(QStringLiteral("error"))
                     .toString()
                     .endsWith(QStringLiteral("is not permitted for subagents")),
                 qPrintable(QStringLiteral("'%1' was admitted by the allowlist").arg(name)));
    }
    QCOMPARE(recorder.calls, 0);

    // The gate reads the NORMALIZED name while the dispatch and the error echo the RAW one, and
    // every fixture passed a name already trimmed and lowercase -- so the two sources were the
    // same string everywhere and the normalization was unproven in both directions. A model
    // emitting mixed case must still be admitted, and the audit trail must still show what the
    // model actually asked for.
    DispatchRecorder cased_recorder;
    sak::ai::AiSubagentToolExecutor cased(recordingDispatch(&cased_recorder));
    cased.setAllowedTools({QStringLiteral("sak_package_manager")});
    const auto cased_output = cased.executeToolCall(
        makeTask(sak::ai::AiToolPolicy::PackageToolsOnly),
        makeCall(QStringLiteral("  SAK_Package_Manager  "), QStringLiteral("{}")),
        sak::ai::CancellationToken::createRoot(QStringLiteral("r")));
    QCOMPARE(cased_recorder.calls, 1);
    QCOMPARE(cased_recorder.tool_name, QStringLiteral("  SAK_Package_Manager  "));
    QCOMPARE(parseOutput(cased_output).value(QStringLiteral("success")).toBool(), true);
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
    QCOMPARE(output.call_id, QStringLiteral("call_1"));
    QCOMPARE(parseOutput(output).value(QStringLiteral("tool")).toString(),
             QStringLiteral("sak_package_manager"));
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
    QCOMPARE(output.call_id, QStringLiteral("call_1"));

    // The refusal has TWO arms: a parse error OR a document that is not an object. Only the first
    // was ever fed. With the second gone, doc.object() on a top-level array returns an EMPTY
    // object and the call DISPATCHES with no arguments instead of failing closed -- a fail-open on
    // model-supplied argument text.
    const QStringList valid_json_non_objects{QStringLiteral("[1,2,3]"),
                                             QStringLiteral("\"a string\""),
                                             QStringLiteral("42"),
                                             QStringLiteral("true"),
                                             QStringLiteral("null")};
    for (const QString& args : valid_json_non_objects) {
        DispatchRecorder shape_recorder;
        sak::ai::AiSubagentToolExecutor shape_executor(recordingDispatch(&shape_recorder));
        shape_executor.setAllowedTools({QStringLiteral("sak_package_manager")});
        const auto refused = shape_executor.executeToolCall(
            makeTask(sak::ai::AiToolPolicy::PackageToolsOnly),
            makeCall(QStringLiteral("sak_package_manager"), args),
            sak::ai::CancellationToken::createRoot(QStringLiteral("r")));
        QVERIFY2(shape_recorder.calls == 0,
                 qPrintable(QStringLiteral("'%1' was dispatched instead of refused").arg(args)));
        QVERIFY2(parseOutput(refused)
                     .value(QStringLiteral("error"))
                     .toString()
                     .startsWith(QStringLiteral("Invalid tool arguments JSON: ")),
                 qPrintable(args));
    }
}

/// The 1 MiB argument cap: its own error message, the DoS guard for untrusted model output, and
/// the largest arguments_json anywhere in the file was sixteen characters. No test reached it, so
/// it could be deleted outright or flipped from > to >= with the suite green -- and because the
/// compare is exact, an off-by-one either way was equally invisible. Both sides are pinned.
void AiSubagentToolExecutorTests::argumentSizeCapIsExactAtBothSides() {
    constexpr qsizetype kCap = 1 << 20;
    DispatchRecorder cap_recorder;
    sak::ai::AiSubagentToolExecutor cap_executor(recordingDispatch(&cap_recorder));
    cap_executor.setAllowedTools({QStringLiteral("sak_package_manager")});

    // Exactly AT the cap: still accepted. The wrapper {"k":"..."} is 8 characters.
    const QString at_cap = QStringLiteral("{\"k\":\"") + QString(kCap - 8, QLatin1Char('x')) +
                           QStringLiteral("\"}");
    QCOMPARE(at_cap.size(), kCap);
    const auto accepted =
        cap_executor.executeToolCall(makeTask(sak::ai::AiToolPolicy::PackageToolsOnly),
                                     makeCall(QStringLiteral("sak_package_manager"), at_cap),
                                     sak::ai::CancellationToken::createRoot(QStringLiteral("r")));
    QCOMPARE(cap_recorder.calls, 1);
    QCOMPARE(parseOutput(accepted).value(QStringLiteral("success")).toBool(), true);

    // One character OVER: refused, before any parse is attempted.
    const QString over_cap = at_cap + QLatin1Char(' ');
    QCOMPARE(over_cap.size(), kCap + 1);
    const auto refused =
        cap_executor.executeToolCall(makeTask(sak::ai::AiToolPolicy::PackageToolsOnly),
                                     makeCall(QStringLiteral("sak_package_manager"), over_cap),
                                     sak::ai::CancellationToken::createRoot(QStringLiteral("r")));
    QCOMPARE(cap_recorder.calls, 1);  // unchanged: never dispatched
    QCOMPARE(parseOutput(refused).value(QStringLiteral("error")).toString(),
             QStringLiteral("Tool arguments JSON exceeds the size limit"));
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
    QCOMPARE(output.call_id, QStringLiteral("call_1"));
    // This refusal is also the only witness for errorOutput's `if (!tool.isEmpty())` branch, and
    // the field it guards was never checked on any refusal path.
    QCOMPARE(parseOutput(output).value(QStringLiteral("tool")).toString(),
             QStringLiteral("sak_package_manager"));
}

QTEST_GUILESS_MAIN(AiSubagentToolExecutorTests)
#include "test_ai_subagent_tool_executor.moc"
