// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_call_router.h"

#include <QJsonDocument>
#include <QtTest/QtTest>

namespace {

QJsonObject outputObject(const sak::ai::OpenAIFunctionOutput& output) {
    return QJsonDocument::fromJson(output.output.toUtf8()).object();
}

struct KindExpectation {
    sak::ai::AiToolCallKind kind;
    bool command;
    bool built_in;
};

// Literal expectations for EVERY AiToolCallKind enumerator -- hand-written values, not a
// re-derivation of the production one-liners: exactly Shell and Process are command tools,
// every other named kind is a built-in, and Unknown is neither.
constexpr KindExpectation kKindMatrix[] = {
    {sak::ai::AiToolCallKind::Unknown, false, false},
    {sak::ai::AiToolCallKind::Shell, true, false},
    {sak::ai::AiToolCallKind::Process, true, false},
    {sak::ai::AiToolCallKind::Screenshot, false, true},
    {sak::ai::AiToolCallKind::Download, false, true},
    {sak::ai::AiToolCallKind::PackageManager, false, true},
    {sak::ai::AiToolCallKind::OfflineDownloader, false, true},
    {sak::ai::AiToolCallKind::ProviderGateway, false, true},
    {sak::ai::AiToolCallKind::SessionSearch, false, true},
    {sak::ai::AiToolCallKind::Skill, false, true},
    {sak::ai::AiToolCallKind::DelegateSubagent, false, true},
    {sak::ai::AiToolCallKind::RunWorkflow, false, true},
    {sak::ai::AiToolCallKind::AppAction, false, true},
};

}  // namespace

class AiToolCallRouterTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void kindForNameMapsKnownTools();
    void kindForNameRejectsNearMissNames();
    void predicatesClassifyEveryKind();
    void prepareRecognizesSkillToolAsBuiltIn();
    void prepareBuildsMetadataAndUnknownError();
    void parseArgumentsAcceptsObject();
    void parseArgumentsRejectsInvalidJson();
    void cancelledOutputIsStructured();
};

void AiToolCallRouterTests::kindForNameMapsKnownTools() {
    using sak::ai::AiToolCallKind;
    using sak::ai::AiToolCallRouter;

    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("run_powershell")),
             AiToolCallKind::Shell);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("run_cmd")), AiToolCallKind::Shell);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("run_process")), AiToolCallKind::Process);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("take_screenshot")),
             AiToolCallKind::Screenshot);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("download_file")),
             AiToolCallKind::Download);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("sak_package_manager")),
             AiToolCallKind::PackageManager);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("sak_offline_downloader")),
             AiToolCallKind::OfflineDownloader);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("sak_provider_gateway")),
             AiToolCallKind::ProviderGateway);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("sak_session_search")),
             AiToolCallKind::SessionSearch);
    // sak_skill must be classified as a built-in tool, or the chat tool loop
    // rejects it as "Unknown function" before it reaches the handler.
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("sak_skill")), AiToolCallKind::Skill);
    // delegate_subagent must be a built-in tool too, or the chat tool loop rejects
    // it as "Unknown function" before the delegate handler runs.
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("delegate_subagent")),
             AiToolCallKind::DelegateSubagent);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("run_workflow")),
             AiToolCallKind::RunWorkflow);
    // sak_app_action must be a recognized built-in, or the tool loop rejects it as
    // "Unknown function" before the app-action handler runs.
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("sak_app_action")),
             AiToolCallKind::AppAction);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("missing_tool")),
             AiToolCallKind::Unknown);
    // Every name above is already lower-case and untrimmed, so the whole block stays green
    // against a lookup that dropped the trim/lower normalization. The dispatcher normalizes the
    // same model-supplied name and still reaches the registered handler, so a router that did
    // not would classify " RUN_PowerShell " as Unknown -- slipping it past the structural
    // command-tool refusal for sub-agents and the workflow-phase recursion guard.
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral(" RUN_PowerShell ")),
             AiToolCallKind::Shell);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("\tDelegate_Subagent\n")),
             AiToolCallKind::DelegateSubagent);

    QVERIFY(AiToolCallRouter::isCommandTool(AiToolCallKind::Shell));
    QVERIFY(AiToolCallRouter::isCommandTool(AiToolCallKind::Process));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::ProviderGateway));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::SessionSearch));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::Skill));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::DelegateSubagent));
    QVERIFY(!AiToolCallRouter::isCommandTool(AiToolCallKind::DelegateSubagent));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::RunWorkflow));
    QVERIFY(!AiToolCallRouter::isCommandTool(AiToolCallKind::RunWorkflow));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::AppAction));
    QVERIFY(!AiToolCallRouter::isCommandTool(AiToolCallKind::AppAction));
    QVERIFY(!AiToolCallRouter::isCommandTool(AiToolCallKind::Skill));
    QVERIFY(!AiToolCallRouter::isBuiltInTool(AiToolCallKind::Shell));
    QVERIFY(!AiToolCallRouter::isBuiltInTool(AiToolCallKind::Unknown));
}

void AiToolCallRouterTests::kindForNameRejectsNearMissNames() {
    using sak::ai::AiToolCallKind;
    using sak::ai::AiToolCallRouter;

    // "missing_tool" shares no prefix, suffix or substring with any registered key, so it
    // returns Unknown under an exact, prefix, substring or fuzzy lookup alike -- it cannot
    // prove the lookup is EXACT. Each name below is exactly one leniency away from a real
    // key: an extension of one, a truncation shared by four, a wrapper containing one, a
    // one-character typo, and the empty name that every key starts with. kindForName() is
    // consumed as a bare existence check (workflowRequirementAvailable,
    // src/gui/ai_assistant_panel.cpp:1282) and as the phase/tool dispatch key
    // (ai_assistant_panel.cpp:10088, :10122), so a match here would declare an invented
    // sak_tool requirement satisfied and route a model-supplied name into a handler it was
    // never registered for.
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("sak_skill_reader")),
             AiToolCallKind::Unknown);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("run_")), AiToolCallKind::Unknown);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("please_run_powershell_now")),
             AiToolCallKind::Unknown);
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("run_powershel")),
             AiToolCallKind::Unknown);
    QCOMPARE(AiToolCallRouter::kindForName(QString()), AiToolCallKind::Unknown);
}

void AiToolCallRouterTests::predicatesClassifyEveryKind() {
    // Spot-checking a subset (the block above never passes Screenshot, Download,
    // PackageManager or OfflineDownloader to either predicate) lets a predicate that swallows
    // one extra kind ship green: isCommandTool() true for Screenshot makes isBuiltInTool()
    // false, so take_screenshot is never dispatched and the call falls through to the command
    // planner instead of its registered handler.
    // Pin the ENUM, not just this file's row count: 13 rows is a compile-time fact about the
    // literal table above and survives any edit to ai_tool_call_router.{h,cpp}. Pin VALUES, so
    // an enumerator inserted mid-list -- the shape that leaves a new kind unclassified while
    // every existing row still matches -- turns these red instead of shipping green and
    // dropping its tool past the isBuiltInTool() dispatch gate.
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::Unknown), 0);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::Shell), 1);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::Process), 2);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::Screenshot), 3);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::Download), 4);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::PackageManager), 5);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::OfflineDownloader), 6);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::ProviderGateway), 7);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::SessionSearch), 8);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::Skill), 9);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::DelegateSubagent), 10);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::RunWorkflow), 11);
    QCOMPARE(static_cast<int>(sak::ai::AiToolCallKind::AppAction), 12);
    QCOMPARE(static_cast<int>(sizeof(kKindMatrix) / sizeof(kKindMatrix[0])), 13);
    for (const KindExpectation& expected : kKindMatrix) {
        QCOMPARE(sak::ai::AiToolCallRouter::isCommandTool(expected.kind), expected.command);
        QCOMPARE(sak::ai::AiToolCallRouter::isBuiltInTool(expected.kind), expected.built_in);
    }
}

void AiToolCallRouterTests::prepareRecognizesSkillToolAsBuiltIn() {
    sak::ai::OpenAIFunctionCall call;
    call.call_id = QStringLiteral("call_skill");
    call.name = QStringLiteral("sak_skill");

    const auto prepared = sak::ai::AiToolCallRouter::prepare(call, 0);

    QVERIFY(prepared.recognized);
    QCOMPARE(prepared.kind, sak::ai::AiToolCallKind::Skill);
    QVERIFY(sak::ai::AiToolCallRouter::isBuiltInTool(prepared.kind));
    // Recognized calls carry no pre-baked error output (that is the Unknown path)...
    QVERIFY(prepared.output.output.isEmpty());
    // ...but the call_id must still be stamped here. prepare() sets it unconditionally, and the
    // sync built-in dispatch fills only output.output, so a version that stamped the id in the
    // Unknown branch alone would hand the turn an empty call_id and abort it with "Tool output
    // missing call id".
    QCOMPARE(prepared.output.call_id, call.call_id);
    QCOMPARE(prepared.metadata.value(QStringLiteral("call_id")).toString(), call.call_id);
    QCOMPARE(prepared.metadata.value(QStringLiteral("name")).toString(), call.name);
    // toInt(-1), not toInt(): a missing or non-numeric index must not read as the 0 we passed.
    QCOMPARE(prepared.metadata.value(QStringLiteral("index")).toInt(-1), 0);
    QCOMPARE(prepared.metadata.size(), 3);

    // prepare()'s recognition rule is `kind != Unknown`, which is STRICTLY WIDER than
    // isBuiltInTool(): command tools are recognized but not built-in. Re-deriving
    // isBuiltInTool(Skill) above only re-proves what predicatesClassifyEveryKind() already pins
    // for every enumerator. What is unpinned is that Shell/Process -- built-in FALSE -- still
    // clear the recognition gate; narrow `recognized` to isBuiltInTool() and every shell and
    // process call is answered "Unknown function" before the command dispatch is ever reached.
    sak::ai::OpenAIFunctionCall shell_call;
    shell_call.call_id = QStringLiteral("call_shell");
    shell_call.name = QStringLiteral("run_powershell");
    const auto shell_prepared = sak::ai::AiToolCallRouter::prepare(shell_call, 1);
    QCOMPARE(shell_prepared.kind, sak::ai::AiToolCallKind::Shell);
    QVERIFY(!sak::ai::AiToolCallRouter::isBuiltInTool(shell_prepared.kind));
    QVERIFY(shell_prepared.recognized);
    // A recognized command tool must carry no pre-baked "Unknown function" output either.
    QVERIFY(shell_prepared.output.output.isEmpty());

    sak::ai::OpenAIFunctionCall process_call;
    process_call.call_id = QStringLiteral("call_process");
    process_call.name = QStringLiteral("run_process");
    const auto process_prepared = sak::ai::AiToolCallRouter::prepare(process_call, 2);
    QCOMPARE(process_prepared.kind, sak::ai::AiToolCallKind::Process);
    QVERIFY(!sak::ai::AiToolCallRouter::isBuiltInTool(process_prepared.kind));
    QVERIFY(process_prepared.recognized);
    QVERIFY(process_prepared.output.output.isEmpty());
}

void AiToolCallRouterTests::prepareBuildsMetadataAndUnknownError() {
    sak::ai::OpenAIFunctionCall call;
    call.call_id = QStringLiteral("call_123");
    call.name = QStringLiteral("missing_tool");

    const auto prepared = sak::ai::AiToolCallRouter::prepare(call, 4);

    QVERIFY(!prepared.recognized);
    QCOMPARE(prepared.kind, sak::ai::AiToolCallKind::Unknown);
    QCOMPARE(prepared.metadata.value(QStringLiteral("call_id")).toString(), call.call_id);
    QCOMPARE(prepared.metadata.value(QStringLiteral("name")).toString(), call.name);
    QCOMPARE(prepared.metadata.value(QStringLiteral("index")).toInt(), 4);
    QCOMPARE(prepared.output.call_id, call.call_id);
    const QJsonObject unknown_json = outputObject(prepared.output);
    QCOMPARE(unknown_json.value(QStringLiteral("error")).toString(),
             QStringLiteral("Unknown function"));
    // The Unknown-function refusal shares errorOutput() with the cancellation path, whose
    // "extra" object is seeded before "error" is stamped over it. So the refusal must carry the
    // error key ALONE: a stray "cancelled" flag here would report every unrecognized tool name
    // to the model as a cancellation and quietly falsify the discrimination
    // cancelledOutputIsStructured() asserts.
    QVERIFY(!unknown_json.contains(QStringLiteral("cancelled")));
    QCOMPARE(unknown_json.size(), 1);
}

void AiToolCallRouterTests::parseArgumentsAcceptsObject() {
    sak::ai::OpenAIFunctionCall call;
    call.call_id = QStringLiteral("call_args");
    call.name = QStringLiteral("run_powershell");
    call.arguments_json = QStringLiteral(R"({"command":"Get-Date","timeout_seconds":5})");

    const auto parsed = sak::ai::AiToolCallRouter::parseArguments(call);

    QVERIFY(parsed.ok);
    QCOMPARE(parsed.arguments.value(QStringLiteral("command")).toString(),
             QStringLiteral("Get-Date"));
    QCOMPARE(parsed.arguments.value(QStringLiteral("timeout_seconds")).toInt(), 5);
    QCOMPARE(parsed.output.call_id, call.call_id);
    QVERIFY(parsed.output.output.isEmpty());
    // A successful parse carries no error message and exactly the caller's two keys -- nothing
    // injected, nothing dropped.
    QCOMPARE(parsed.arguments.size(), 2);
    QCOMPARE(parsed.error_message, QString());

    // Empty / whitespace-only arguments are a NO-ARGUMENT tool call, not a parse failure: the
    // turn validator admits them, so the router must agree or every no-arg call is refused at
    // dispatch with "Invalid <tool> arguments".
    sak::ai::OpenAIFunctionCall no_args;
    no_args.call_id = QStringLiteral("call_empty");
    no_args.name = QStringLiteral("take_screenshot");
    no_args.arguments_json = QStringLiteral("   ");
    const auto empty_parsed = sak::ai::AiToolCallRouter::parseArguments(no_args);
    QVERIFY(empty_parsed.ok);
    QVERIFY(empty_parsed.arguments.isEmpty());
    QCOMPARE(empty_parsed.error_message, QString());
    QCOMPARE(empty_parsed.output.call_id, no_args.call_id);
    QVERIFY(empty_parsed.output.output.isEmpty());
}

void AiToolCallRouterTests::parseArgumentsRejectsInvalidJson() {
    sak::ai::OpenAIFunctionCall call;
    call.call_id = QStringLiteral("call_bad");
    call.name = QStringLiteral("run_powershell");
    call.arguments_json = QStringLiteral("[1,2,3]");

    const auto parsed = sak::ai::AiToolCallRouter::parseArguments(call);

    QVERIFY(!parsed.ok);
    QVERIFY(parsed.arguments.isEmpty());
    QCOMPARE(parsed.error_message, QStringLiteral("Invalid run_powershell arguments"));
    QCOMPARE(parsed.output.call_id, call.call_id);
    // The refusal object must be a PLAIN error -- exactly {"error": ...}. errorOutput() seeds
    // the object from its `extra` argument before writing "error", so a producer that passed an
    // extra map here would keep this error text intact while telling the model something else
    // entirely, and echoing the raw argument blob back would splice unbounded model text into
    // the transcript.
    const QJsonObject error_json = outputObject(parsed.output);
    QCOMPARE(error_json.value(QStringLiteral("error")).toString(),
             QStringLiteral("Invalid run_powershell arguments"));
    QVERIFY(!error_json.contains(QStringLiteral("cancelled")));
    QCOMPARE(error_json.size(), 1);

    // "[1,2,3]" is WELL-FORMED JSON that merely is not an object, so it exercises only the
    // isObject() half of the two ANDed guards. Syntactically broken JSON must be refused by the
    // parse-error half too -- otherwise a fail-open that treated unparseable arguments as a
    // no-argument call would run the tool with silently empty arguments.
    sak::ai::OpenAIFunctionCall malformed;
    malformed.call_id = QStringLiteral("call_malformed");
    malformed.name = QStringLiteral("run_process");
    malformed.arguments_json = QStringLiteral(R"({"command":"whoami")");
    const auto malformed_parsed = sak::ai::AiToolCallRouter::parseArguments(malformed);
    QVERIFY(!malformed_parsed.ok);
    QVERIFY(malformed_parsed.arguments.isEmpty());
    QCOMPARE(malformed_parsed.error_message, QStringLiteral("Invalid run_process arguments"));
    QCOMPARE(malformed_parsed.output.call_id, malformed.call_id);
    const QJsonObject malformed_json = outputObject(malformed_parsed.output);
    QCOMPARE(malformed_json.value(QStringLiteral("error")).toString(),
             QStringLiteral("Invalid run_process arguments"));
    QVERIFY(!malformed_json.contains(QStringLiteral("cancelled")));
    QCOMPARE(malformed_json.size(), 1);
}

void AiToolCallRouterTests::cancelledOutputIsStructured() {
    sak::ai::OpenAIFunctionCall call;
    call.call_id = QStringLiteral("call_cancel");
    call.name = QStringLiteral("run_process");

    const auto output = sak::ai::AiToolCallRouter::cancelledOutput(call);
    const QJsonObject json = outputObject(output);

    QCOMPARE(output.call_id, call.call_id);
    QCOMPARE(json.value(QStringLiteral("error")).toString(),
             QStringLiteral("Cancelled before dispatch"));
    QCOMPARE(json.value(QStringLiteral("cancelled")).toBool(false), true);
    QCOMPARE(json.size(), 2);
    // The flag must be DISCRIMINATING: it comes from the caller-supplied extra object, not
    // stamped on every error. A plain errorOutput carries "error" alone, so the model can tell
    // a cancellation from the failures that share this helper -- "Unknown function", "Invalid
    // <tool> arguments" and the loop-guard refusal.
    const QJsonObject plain_error =
        outputObject(sak::ai::AiToolCallRouter::errorOutput(call, QStringLiteral("boom")));
    QCOMPARE(plain_error.value(QStringLiteral("error")).toString(), QStringLiteral("boom"));
    QVERIFY(!plain_error.contains(QStringLiteral("cancelled")));
    QCOMPARE(plain_error.size(), 1);
    // The message must survive compactJson() BYTE-EXACT, not merely ASCII-exact. Every message
    // this file pushes through errorOutput() is 7-bit, so a helper that decoded toJson()'s UTF-8
    // bytes as Latin-1 would round-trip all of them green. Two real callers feed non-ASCII
    // through this exact helper -- the tr()-translated loop-guard refusal, and "Invalid %1
    // arguments" interpolating a model-supplied tool name -- and mojibake there is what the
    // model reads back as the reason its call failed. The characters are built from QChar code
    // points rather than written literally, both because the repo's ASCII gate rejects any source
    // byte above 0x7F and because that keeps the expected value independent of how the compiler
    // happens to encode a non-ASCII source literal.
    const QString unicode_message = QStringLiteral("Invalid caf") + QChar(0x00E9) +
                                    QStringLiteral("_") + QChar(0x4E2D) + QChar(0x6587) +
                                    QStringLiteral(" arguments");
    const QJsonObject unicode_error =
        outputObject(sak::ai::AiToolCallRouter::errorOutput(call, unicode_message));
    QCOMPARE(unicode_error.value(QStringLiteral("error")).toString(), unicode_message);
}

QTEST_GUILESS_MAIN(AiToolCallRouterTests)
#include "test_ai_tool_call_router.moc"
