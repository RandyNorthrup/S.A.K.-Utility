// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_gateway.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

}  // namespace

class AiProviderGatewayTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void docsQueryRequiresProviderId();
    void docsQueryRejectsNonHttpProvider();
    void docsQueryRejectsToolMissingFromProviderManifest();
    void classifiesWin32McpToolRisk();
    void planWin32McpCallFlagsBrowserInputForConfirmation();
    void win32McpEnvironmentIncludesProviderValues();
    void win32McpResultExtractsTextAndRiskFlags();
    void win32McpResultFlagsLogicalToolError();
    void planWin32McpCallBuildsReadOnlyPlan();
    void planWin32McpCallClampsTimeout();
    void checkAvailabilityRejectsUnsupportedAppAction();
    void checkAvailabilityAcceptsReadOnlyWin32Tool();
};

void AiProviderGatewayTests::docsQueryRequiresProviderId() {
    const sak::ai::AiProviderGateway gateway;
    QString error;
    const QJsonObject result = gateway.docsQuery(QJsonObject{}, &error);

    QVERIFY(result.isEmpty());
    QCOMPARE(error, QStringLiteral("docs_query requires provider_id"));
}

void AiProviderGatewayTests::docsQueryRejectsNonHttpProvider() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString command_path =
        QDir(temp.path()).filePath(QStringLiteral("tools/mcp/docs/server.exe"));
    QVERIFY(writeFile(command_path, QByteArray("stub")));
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject provider{{QStringLiteral("id"), QStringLiteral("microsoft_docs")},
                               {QStringLiteral("transport"), QStringLiteral("stdio")},
                               {QStringLiteral("command"),
                                QStringLiteral("tools/mcp/docs/server.exe")}};
    QVERIFY(
        writeFile(providers_path,
                  QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
                      .toJson(QJsonDocument::Compact)));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    QString error;
    const QJsonObject result = gateway.docsQuery(
        QJsonObject{{QStringLiteral("provider_id"), QStringLiteral("microsoft_docs")},
                    {QStringLiteral("query"), QStringLiteral("ui automation")}},
        &error);

    QVERIFY(result.isEmpty());
    QCOMPARE(error, QStringLiteral("docs_query supports HTTP MCP docs providers only"));
}

void AiProviderGatewayTests::docsQueryRejectsToolMissingFromProviderManifest() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject provider{
        {QStringLiteral("id"), QStringLiteral("context7")},
        {QStringLiteral("transport"), QStringLiteral("http")},
        {QStringLiteral("endpoint"), QStringLiteral("https://context7.com/mcp")},
        {QStringLiteral("tools"), QJsonArray{QStringLiteral("resolve-library-id")}}};
    QVERIFY(
        writeFile(providers_path,
                  QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
                      .toJson(QJsonDocument::Compact)));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    QString error;
    const QJsonObject result = gateway.docsQuery(
        QJsonObject{{QStringLiteral("provider_id"), QStringLiteral("context7")},
                    {QStringLiteral("query"), QStringLiteral("widgets")},
                    {QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("libraryId"), QStringLiteral("/qt/qtbase")}}}},
        &error);

    QVERIFY(result.isEmpty());
    QVERIFY(error.contains(QStringLiteral("not in bundled provider manifest")));
    QVERIFY(error.contains(QStringLiteral("query-docs")));
}

void AiProviderGatewayTests::classifiesWin32McpToolRisk() {
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("list_windows")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("capture_screen")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("clipboard_paste")));

    QVERIFY(sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("kill_process")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("start_process")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("click")));

    // Browser reads are read-only (ungated); browser input tools are the confirmation
    // tier and must NOT be misclassified as read-only.
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_snapshot")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_read")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_click")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_click_at")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_dialog")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_type")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_press_key")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_scroll")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_snapshot")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_click")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_click_at")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_dialog")));

    // The live-desktop input tools already shipped in the win32_mcp manifest must be in
    // the input tier too (they were fail-open ungated before).
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("uia_click_control")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("click_text")));
}

void AiProviderGatewayTests::planWin32McpCallFlagsBrowserInputForConfirmation() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString command_path =
        QDir(temp.path()).filePath(QStringLiteral("tools/mcp/win32-mcp-server/server.exe"));
    QVERIFY(writeFile(command_path, QByteArray("stub")));
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject provider{{QStringLiteral("id"), QStringLiteral("win32_mcp")},
                               {QStringLiteral("transport"), QStringLiteral("stdio")},
                               {QStringLiteral("command"),
                                QStringLiteral("tools/mcp/win32-mcp-server/server.exe")},
                               {QStringLiteral("tools"),
                                QJsonArray{QStringLiteral("browser_click"),
                                           QStringLiteral("browser_snapshot"),
                                           QStringLiteral("mystery_tool")}}};
    QVERIFY(
        writeFile(providers_path,
                  QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
                      .toJson(QJsonDocument::Compact)));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    QString error;

    // Input tool: not read-only, and flagged for a mandatory human confirmation.
    const auto click = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("browser_click")},
                                 {QStringLiteral("ref"), QStringLiteral("e5")}}}},
        &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!click.read_only);
    QVERIFY(click.requires_confirmation);

    // Read tool: read-only and NOT gated for confirmation.
    const auto snapshot =
        gateway.planWin32McpCall(QJsonObject{{QStringLiteral("arguments"),
                                              QJsonObject{{QStringLiteral("tool_name"),
                                                           QStringLiteral("browser_snapshot")}}}},
                                 &error);
    QVERIFY(error.isEmpty());
    QVERIFY(snapshot.read_only);
    QVERIFY(!snapshot.requires_confirmation);

    // Fail CLOSED: a tool on none of the classifier lists must still require confirmation
    // rather than dropping into the ungated interactive tier and auto-running unattended.
    const auto mystery = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("mystery_tool")}}}},
        &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!mystery.read_only);
    QVERIFY(!mystery.high_risk);
    QVERIFY(mystery.requires_confirmation);
}

void AiProviderGatewayTests::win32McpEnvironmentIncludesProviderValues() {
    const QJsonObject provider{
        {QStringLiteral("environment"),
         QJsonObject{{QStringLiteral("CUSTOM_PROVIDER_ENV"), QStringLiteral("present")}}}};

    const QProcessEnvironment env =
        sak::ai::AiProviderGateway::win32McpEnvironment(QStringLiteral("read_only"), provider);

    QCOMPARE(env.value(QStringLiteral("CUSTOM_PROVIDER_ENV")), QStringLiteral("present"));
    QCOMPARE(env.value(QStringLiteral("WIN32_MCP_SECURITY_PROFILE")), QStringLiteral("read_only"));
    QCOMPARE(env.value(QStringLiteral("WIN32_MCP_RESULT_ENVELOPE")), QStringLiteral("true"));
    QCOMPARE(env.value(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT")),
             QStringLiteral("true"));
}

void AiProviderGatewayTests::win32McpResultExtractsTextAndRiskFlags() {
    const QJsonObject provider{{QStringLiteral("id"), QStringLiteral("win32_mcp")}};
    const QJsonObject mcp_message{
        {QStringLiteral("result"),
         QJsonObject{{QStringLiteral("content"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                             {QStringLiteral("text"),
                                              QStringLiteral("Window A\nWindow B")}}}}}}};

    const QJsonObject result = sak::ai::AiProviderGateway::win32McpResult(
        provider,
        QStringLiteral("list_windows"),
        QJsonObject{{QStringLiteral("filter"), QStringLiteral("SAK")}},
        QStringLiteral("read_only"),
        mcp_message);

    QCOMPARE(result.value(QStringLiteral("provider_id")).toString(), QStringLiteral("win32_mcp"));
    QCOMPARE(result.value(QStringLiteral("provider_tool")).toString(),
             QStringLiteral("list_windows"));
    QCOMPARE(result.value(QStringLiteral("security_profile")).toString(),
             QStringLiteral("read_only"));
    QVERIFY(result.value(QStringLiteral("read_only_tool")).toBool(false));
    QVERIFY(!result.value(QStringLiteral("high_risk_tool")).toBool(true));
    QCOMPARE(result.value(QStringLiteral("result_text")).toString(),
             QStringLiteral("Window A\nWindow B"));
    QVERIFY(result.contains(QStringLiteral("mcp_result")));
    // A successful result must not be flagged as a tool error.
    QVERIFY(!result.value(QStringLiteral("mcp_is_error")).toBool(true));
}

void AiProviderGatewayTests::win32McpResultFlagsLogicalToolError() {
    // An MCP tools/call result with isError:true is a logical failure (no transport
    // error); win32McpResult must surface it so the runner does not record a success.
    const QJsonObject provider{{QStringLiteral("id"), QStringLiteral("win32_mcp")}};
    const QJsonObject mcp_message{
        {QStringLiteral("result"),
         QJsonObject{{QStringLiteral("isError"), true},
                     {QStringLiteral("content"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                             {QStringLiteral("text"),
                                              QStringLiteral("element not found")}}}}}}};

    const QJsonObject result =
        sak::ai::AiProviderGateway::win32McpResult(provider,
                                                   QStringLiteral("click_element"),
                                                   QJsonObject{},
                                                   QStringLiteral("standard"),
                                                   mcp_message);

    QVERIFY(result.value(QStringLiteral("mcp_is_error")).toBool(false));
    QCOMPARE(result.value(QStringLiteral("result_text")).toString(),
             QStringLiteral("element not found"));
}

void AiProviderGatewayTests::planWin32McpCallBuildsReadOnlyPlan() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString command_path =
        QDir(temp.path()).filePath(QStringLiteral("tools/mcp/win32-mcp-server/server.exe"));
    QVERIFY(writeFile(command_path, QByteArray("stub")));

    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject provider{
        {QStringLiteral("id"), QStringLiteral("win32_mcp")},
        {QStringLiteral("transport"), QStringLiteral("stdio")},
        {QStringLiteral("command"), QStringLiteral("tools/mcp/win32-mcp-server/server.exe")},
        {QStringLiteral("tools"),
         QJsonArray{QStringLiteral("list_windows"), QStringLiteral("click")}}};
    QVERIFY(
        writeFile(providers_path,
                  QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
                      .toJson(QJsonDocument::Compact)));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    QString error;
    const QJsonObject args{
        {QStringLiteral("arguments"),
         QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("list_windows")},
                     {QStringLiteral("filter"), QStringLiteral("SAK")},
                     {QStringLiteral("timeout_ms"), 1500}}}};

    const sak::ai::AiProviderGateway::Win32McpCallPlan plan = gateway.planWin32McpCall(args,
                                                                                       &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(plan.provider.value(QStringLiteral("id")).toString(), QStringLiteral("win32_mcp"));
    QCOMPARE(plan.provider.value(QStringLiteral("resolved_command")).toString(),
             QDir::cleanPath(command_path));
    QCOMPARE(plan.tool_name, QStringLiteral("list_windows"));
    QCOMPARE(plan.tool_arguments.value(QStringLiteral("filter")).toString(), QStringLiteral("SAK"));
    QVERIFY(!plan.tool_arguments.contains(QStringLiteral("tool_name")));
    QVERIFY(!plan.tool_arguments.contains(QStringLiteral("timeout_ms")));
    QCOMPARE(plan.security_profile, QStringLiteral("read_only"));
    QCOMPARE(plan.timeout_ms, 1500);
    QVERIFY(plan.read_only);
    QVERIFY(!plan.high_risk);
    QVERIFY(plan.preview.contains(QStringLiteral("list_windows")));
}

void AiProviderGatewayTests::planWin32McpCallClampsTimeout() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString command_path =
        QDir(temp.path()).filePath(QStringLiteral("tools/mcp/win32-mcp-server/server.exe"));
    QVERIFY(writeFile(command_path, QByteArray("stub")));

    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject provider{
        {QStringLiteral("id"), QStringLiteral("win32_mcp")},
        {QStringLiteral("transport"), QStringLiteral("stdio")},
        {QStringLiteral("command"), QStringLiteral("tools/mcp/win32-mcp-server/server.exe")},
        {QStringLiteral("tools"), QJsonArray{QStringLiteral("list_windows")}}};
    QVERIFY(
        writeFile(providers_path,
                  QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
                      .toJson(QJsonDocument::Compact)));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    QString error;
    const auto low = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("list_windows")},
                                 {QStringLiteral("timeout_ms"), 10}}}},
        &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(low.timeout_ms, 1000);

    const auto high = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("list_windows")},
                                 {QStringLiteral("timeout_ms"), 999'999}}}},
        &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(high.timeout_ms, 120'000);
}

void AiProviderGatewayTests::checkAvailabilityRejectsUnsupportedAppAction() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    QVERIFY(writeFile(providers_path, R"({"providers":[]})"));
    const QString manifest_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/app_manifests/testapp.json"));
    QVERIFY(writeFile(manifest_path, R"({"id":"testapp","actions":{"scan":{"supported":false}}})"));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    const QJsonObject result = gateway.checkAvailability(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("app_run_action")},
                    {QStringLiteral("app_id"), QStringLiteral("testapp")},
                    {QStringLiteral("action"), QStringLiteral("scan")}});

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
             QStringLiteral("app_action_unsupported"));
}

void AiProviderGatewayTests::checkAvailabilityAcceptsReadOnlyWin32Tool() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString command_path =
        QDir(temp.path()).filePath(QStringLiteral("tools/mcp/win32-mcp-server/server.exe"));
    QVERIFY(writeFile(command_path, QByteArray("stub")));

    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject provider{
        {QStringLiteral("id"), QStringLiteral("win32_mcp")},
        {QStringLiteral("transport"), QStringLiteral("stdio")},
        {QStringLiteral("command"), QStringLiteral("tools/mcp/win32-mcp-server/server.exe")},
        {QStringLiteral("tools"), QJsonArray{QStringLiteral("list_windows")}}};
    QVERIFY(
        writeFile(providers_path,
                  QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
                      .toJson(QJsonDocument::Compact)));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    const QJsonObject result = gateway.checkAvailability(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("win32_mcp_call")},
                    {QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("list_windows")}}}});

    QVERIFY(result.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(result.value(QStringLiteral("provider_id")).toString(), QStringLiteral("win32_mcp"));
    QVERIFY(result.value(QStringLiteral("read_only_tool")).toBool(false));
}

QTEST_GUILESS_MAIN(AiProviderGatewayTests)
#include "test_ai_provider_gateway.moc"
