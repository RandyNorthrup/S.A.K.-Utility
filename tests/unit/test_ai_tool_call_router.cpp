// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_call_router.h"

#include <QJsonDocument>
#include <QtTest/QtTest>

namespace {

QJsonObject outputObject(const sak::ai::OpenAIFunctionOutput& output) {
    return QJsonDocument::fromJson(output.output.toUtf8()).object();
}

}  // namespace

class AiToolCallRouterTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void kindForNameMapsKnownTools();
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
    QCOMPARE(AiToolCallRouter::kindForName(QStringLiteral("missing_tool")),
             AiToolCallKind::Unknown);

    QVERIFY(AiToolCallRouter::isCommandTool(AiToolCallKind::Shell));
    QVERIFY(AiToolCallRouter::isCommandTool(AiToolCallKind::Process));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::ProviderGateway));
    QVERIFY(AiToolCallRouter::isBuiltInTool(AiToolCallKind::SessionSearch));
    QVERIFY(!AiToolCallRouter::isBuiltInTool(AiToolCallKind::Shell));
    QVERIFY(!AiToolCallRouter::isBuiltInTool(AiToolCallKind::Unknown));
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
    QCOMPARE(outputObject(prepared.output).value(QStringLiteral("error")).toString(),
             QStringLiteral("Unknown function"));
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
}

void AiToolCallRouterTests::parseArgumentsRejectsInvalidJson() {
    sak::ai::OpenAIFunctionCall call;
    call.call_id = QStringLiteral("call_bad");
    call.name = QStringLiteral("run_powershell");
    call.arguments_json = QStringLiteral("[1,2,3]");

    const auto parsed = sak::ai::AiToolCallRouter::parseArguments(call);

    QVERIFY(!parsed.ok);
    QCOMPARE(parsed.output.call_id, call.call_id);
    QCOMPARE(outputObject(parsed.output).value(QStringLiteral("error")).toString(),
             QStringLiteral("Invalid run_powershell arguments"));
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
    QVERIFY(json.value(QStringLiteral("cancelled")).toBool(false));
}

QTEST_GUILESS_MAIN(AiToolCallRouterTests)
#include "test_ai_tool_call_router.moc"
