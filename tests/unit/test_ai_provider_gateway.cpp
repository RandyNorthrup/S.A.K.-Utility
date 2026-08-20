// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_gateway.h"
#include "sak/ai/ai_provider_gateway_authorization.h"

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
    void initTestCase();
    void cleanupTestCase();
    void docsQueryRequiresProviderId();
    void docsQueryRejectsNonHttpProvider();
    void docsQueryRejectsToolMissingFromProviderManifest();
    void classifiesWin32McpToolRisk();
    void classifiesRecipeDesktopInputAllowlist();
    void classifiesDesktopInputAndCloseToolRisk();
    void classifiesBatch3BrowserTools();
    void planWin32McpCallFlagsBrowserInputForConfirmation();
    void planWin32McpCallGatesExtensionTools();
    void win32McpEnvironmentIncludesProviderValues();
    void win32McpResultExtractsTextAndRiskFlags();
    void win32McpResultFlagsLogicalToolError();
    void planWin32McpCallBuildsReadOnlyPlan();
    void planWin32McpCallClampsTimeout();
    void checkAvailabilityRejectsUnsupportedAppAction();
    void checkAvailabilityAcceptsReadOnlyWin32Tool();
    void mutatingToolsAreNeverReadOnly();
    void authorizeWin32Mcp_browserInputRequiresConfirmEveryMode();
    void authorizeWin32Mcp_assistedMutatingRequiresConfirm();
    void authorizeWin32Mcp_unattendedHighRiskRequiresRestorePoint();
    void win32McpEnvironment_refusesProtectedAndNonStringVars();  // R5-G10-9
    void docsQuery_refusesNonHttpsEndpoint();                     // R5-G10-9
    void planWin32McpCall_refusesMalformedEnvelope();             // R5-G10-9
};

// The registry loads the embedded resource by default and honors an on-disk providers.json only
// under an out-of-band operator opt-in. Every gateway test here builds its provider set by writing
// a temp providers.json, so opt in for the whole suite; the fail-closed default is covered in
// test_ai_provider_registry.
void AiProviderGatewayTests::initTestCase() {
    qputenv("SAK_AI_POLICY_DISK_OVERRIDE", "1");
}

void AiProviderGatewayTests::cleanupTestCase() {
    qunsetenv("SAK_AI_POLICY_DISK_OVERRIDE");
}

void AiProviderGatewayTests::mutatingToolsAreNeverReadOnly() {
    // The fail-closed default (requires_confirmation unless read_only||high_risk) only protects a
    // tool that is MISSING from every classifier. It cannot protect a MUTATING tool wrongly added
    // to the read-only allowlist -- that would set requires_confirmation=false and auto-run
    // unattended. Pin the dangerous direction: none of these state-changing / sensitive tools may
    // be read-only, so a future mis-classification fails this test instead of shipping ungated.
    for (const QString& t : {QStringLiteral("clipboard_write"),
                             QStringLiteral("close_window"),
                             QStringLiteral("mouse_click"),
                             QStringLiteral("click_text"),
                             QStringLiteral("type_text"),
                             QStringLiteral("send_keys"),
                             QStringLiteral("uia_click_control"),
                             QStringLiteral("dismiss_dialog"),
                             QStringLiteral("focus_window"),
                             QStringLiteral("browser_click"),
                             QStringLiteral("browser_type"),
                             QStringLiteral("browser_set_value"),
                             QStringLiteral("browser_cookies"),
                             QStringLiteral("browser_http_auth"),
                             QStringLiteral("browser_download"),
                             QStringLiteral("browser_storage"),
                             QStringLiteral("browser_permission"),
                             QStringLiteral("browser_extension_install"),
                             QStringLiteral("browser_extension_uninstall")}) {
        QVERIFY2(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(t), qPrintable(t));
    }
}

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
    QCOMPARE(error,
             QStringLiteral("MCP provider tool is not in bundled provider manifest: "
                            "context7/query-docs"));
}

void AiProviderGatewayTests::classifiesWin32McpToolRisk() {
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("list_windows")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("capture_screen")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("clipboard_paste")));

    QVERIFY(sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("close_window")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("click")));
    // B12-08: list_processes/kill_process/start_process were classified but never in the server
    // manifest (dead) -- removed, so they are no longer read-only or high-risk.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("list_processes")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("kill_process")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("start_process")));

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
    // Dropdown selection + tab grouping mutate the page/browser and take the input tier.
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_select")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_set_value")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_media")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_group_tabs")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_ungroup_tabs")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_drag")));
    // Hover moves the pointer and dispatches hover events -- it MUTATES UI state, so the win32
    // server excludes it from its read-only allowlist and refuses it under the read_only profile.
    // It must therefore not be read-only here either (that would report it safe yet send a profile
    // the server rejects); the fail-closed default gives it the interactive/confirm tier.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_hover")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_hover")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_select")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_snapshot")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_click")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_click_at")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_dialog")));

    // Clipboard: writing injects content (input tier); reading exposes cross-app data, so it
    // must not be on the read-only allowlist -- the fail-closed default then gates it.
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("clipboard_write")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("clipboard_write")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("clipboard_read")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("clipboard_read")));

    // Browser-extension setup: status is a read-only query (ungated); install/uninstall write
    // user Chrome policy that force-installs software and take the hard-confirm input tier.
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(
        QStringLiteral("browser_extension_status")));
    QVERIFY(
        !sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_extension_status")));
    QVERIFY(
        sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_extension_install")));
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(
        QStringLiteral("browser_extension_uninstall")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(
        QStringLiteral("browser_extension_install")));
}

void AiProviderGatewayTests::classifiesRecipeDesktopInputAllowlist() {
    // CODEX_REVIEW_4 M-B1-6: only the PHYSICAL-desktop input tools may auto-run inside a
    // win32_gui recipe. The desktop subset is true; browser/clipboard/extension input tools are
    // input-tier but NOT desktop-input, so they still require a per-call confirm in a recipe.
    for (const QString& desktop : {QStringLiteral("click_text"),
                                   QStringLiteral("uia_click_control"),
                                   QStringLiteral("dismiss_dialog"),
                                   QStringLiteral("mouse_click"),
                                   QStringLiteral("type_text"),
                                   QStringLiteral("send_keys"),
                                   QStringLiteral("focus_window")}) {
        QVERIFY2(sak::ai::AiProviderGateway::isWin32DesktopInputTool(desktop), qPrintable(desktop));
    }
    for (const QString& nonDesktop : {QStringLiteral("browser_click"),
                                      QStringLiteral("browser_type"),
                                      QStringLiteral("clipboard_write"),
                                      QStringLiteral("browser_extension_install")}) {
        QVERIFY2(sak::ai::AiProviderGateway::isWin32InputTool(nonDesktop), qPrintable(nonDesktop));
        QVERIFY2(!sak::ai::AiProviderGateway::isWin32DesktopInputTool(nonDesktop),
                 qPrintable(nonDesktop));
    }
}

void AiProviderGatewayTests::classifiesDesktopInputAndCloseToolRisk() {
    // The live-desktop input tools (UIA activate + text-click, already shipped) and the SendInput
    // physical-input tools (DC batch 5) all inject input and must be in the hard-confirm input
    // tier; none may be on the read-only allowlist.
    // dismiss_dialog invokes a pop-up's affirmative button (a UIA activation) -- input tier, and
    // deliberately NOT high-risk so a vetted win32_gui recipe may use it to close a completion
    // dialog without needing a dynamic ref.
    for (const QString& in : {QStringLiteral("uia_click_control"),
                              QStringLiteral("click_text"),
                              QStringLiteral("mouse_click"),
                              QStringLiteral("type_text"),
                              QStringLiteral("send_keys"),
                              QStringLiteral("focus_window"),
                              QStringLiteral("dismiss_dialog")}) {
        QVERIFY2(sak::ai::AiProviderGateway::isWin32InputTool(in), qPrintable(in));
        QVERIFY2(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(in), qPrintable(in));
        QVERIFY2(!sak::ai::AiProviderGateway::isWin32HighRiskTool(in), qPrintable(in));
    }

    // close_window (DC batch 6) is high-risk (confirms in Assisted, restore point in Unattended),
    // not read-only and not an input tool.
    QVERIFY(sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("close_window")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("close_window")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("close_window")));
}

void AiProviderGatewayTests::classifiesBatch3BrowserTools() {
    // Batch 3 inspection tools are pure reads: read-only, ungated, and never in the input tier.
    for (const QString& ro : {QStringLiteral("browser_wait_for"),
                              QStringLiteral("browser_get_value"),
                              QStringLiteral("browser_get_attribute"),
                              QStringLiteral("browser_box")}) {
        QVERIFY2(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(ro), qPrintable(ro));
        QVERIFY2(!sak::ai::AiProviderGateway::isWin32InputTool(ro), qPrintable(ro));
    }
    // browser_focus/reveal are NOT pure reads: focusing an element and scrolling it into view
    // mutate UI state, so the win32 server drops them from its read-only allowlist and refuses
    // them under the read_only profile. They must be neither read-only (which would send that
    // rejected profile while reporting them safe) nor input-tier; the fail-closed default in
    // populateWin32Plan then gives them the interactive profile and a per-call human confirmation.
    for (const QString& mid : {QStringLiteral("browser_focus"), QStringLiteral("browser_reveal")}) {
        QVERIFY2(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(mid), qPrintable(mid));
        QVERIFY2(!sak::ai::AiProviderGateway::isWin32InputTool(mid), qPrintable(mid));
    }
    // js_click is a click: hard-confirm input tier, never read-only.
    QVERIFY(sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_js_click")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_js_click")));

    // Batch 3b: listing windows is read-only; browser_window (new/focus/close) mutates the
    // browser, so it is neither read-only nor input-tier -- the fail-closed default confirms it.
    QVERIFY(sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_windows")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_windows")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_window")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_window")));

    // Batch 4: browser_emulate mutates rendering/UA state -> middle tier (neither read-only nor
    // input); the fail-closed default confirms it.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_emulate")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_emulate")));

    // Batch 4: browser_print renders the page to a PDF file on disk -> middle tier (neither
    // read-only nor input); the fail-closed default confirms it before writing a file.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_print")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_print")));

    // Batch 4: browser_permission changes a site's privacy permissions (geo/cam/mic/...) ->
    // middle tier; the fail-closed default confirms it before granting or blocking.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_permission")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_permission")));

    // Batch 4: browser_storage can write site storage (and reading it can exfiltrate session
    // tokens) -> middle tier; the fail-closed default confirms it, reads included.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_storage")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_storage")));

    // Batch 4: browser_cookies reads/sets cookies (session tokens) -> middle tier; never
    // read-only (reading a cookie value is itself sensitive) and never high-risk (must not
    // auto-run unattended). The fail-closed default confirms every call.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_cookies")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_cookies")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("browser_cookies")));

    // Batch 4: browser_download fetches a url and writes a file to disk -> middle tier; the
    // fail-closed default confirms it before downloading.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_download")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_download")));

    // Batch 4: browser_http_auth feeds credentials to sites -> middle tier; never read-only,
    // never high-risk (must not auto-run unattended). The fail-closed default confirms it.
    QVERIFY(!sak::ai::AiProviderGateway::isWin32ReadOnlyTool(QStringLiteral("browser_http_auth")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32InputTool(QStringLiteral("browser_http_auth")));
    QVERIFY(!sak::ai::AiProviderGateway::isWin32HighRiskTool(QStringLiteral("browser_http_auth")));
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
                                           QStringLiteral("clipboard_read"),
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

    // clipboard_read is not on any classifier list, so the same fail-closed default gates it
    // -- reading the user's clipboard never auto-runs unattended.
    const auto clip = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("clipboard_read")}}}},
        &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!clip.read_only);
    QVERIFY(clip.requires_confirmation);
}

void AiProviderGatewayTests::planWin32McpCallGatesExtensionTools() {
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
                                QJsonArray{QStringLiteral("browser_extension_install"),
                                           QStringLiteral("browser_extension_status")}}};
    QVERIFY(
        writeFile(providers_path,
                  QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
                      .toJson(QJsonDocument::Compact)));

    const sak::ai::AiProviderGateway gateway{sak::ai::AiProviderRegistry(temp.path())};
    QString error;

    // Installing the extension writes Chrome policy: not read-only, mandatory confirmation.
    const auto install = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"),
                                  QStringLiteral("browser_extension_install")}}}},
        &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!install.read_only);
    QVERIFY(install.requires_confirmation);

    // Status only reads state: read-only and NOT gated.
    const auto status = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"),
                                  QStringLiteral("browser_extension_status")}}}},
        &error);
    QVERIFY(error.isEmpty());
    QVERIFY(status.read_only);
    QVERIFY(!status.requires_confirmation);
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
    QCOMPARE(plan.preview, QStringLiteral("Win32 MCP list_windows {\"filter\":\"SAK\"}"));
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
                                 {QStringLiteral("timeout_ms"), 9'999'999}}}},
        &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(high.timeout_ms, 7'200'000);  // clamped to the long-operation ceiling (2 hours)
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

// ============================================================================
// Win32 MCP authorization matrix (authorizeWin32McpCall, extracted to
// ai_provider_gateway_authorization.{h,cpp}). This is the SOLE gate on the plan's
// authorization flags -- callWin32Mcp does not re-derive them -- so each mode/flag
// combination is proven to demand its human step and to fail closed when the
// required callback is absent or declines. The AiProviderGatewayToolRunner routes
// every win32_mcp_call through exactly this function.
// ============================================================================

void AiProviderGatewayTests::authorizeWin32Mcp_browserInputRequiresConfirmEveryMode() {
    using Access = sak::ai::AiProviderGatewayToolAccess;
    sak::ai::AiProviderGateway::Win32McpCallPlan plan;
    plan.requires_confirmation = true;  // browser input injection (click/type/press-key/scroll)
    plan.read_only = false;
    plan.preview = QStringLiteral("click at (10,10)");

    // No confirm callback configured -> fail closed in BOTH non-chat modes.
    const sak::ai::AiProviderGatewayToolCallbacks noConfirm;
    for (const Access access : {Access::AssistedFullAccess, Access::UnattendedFullAccess}) {
        const QJsonObject result = sak::ai::authorizeWin32McpCall(plan, access, noConfirm);
        QVERIFY2(!result.isEmpty(), "browser input with no confirm callback must fail closed");
        QCOMPARE(result.value(QStringLiteral("success")).toBool(true), false);
        QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
                 QStringLiteral("Win32 MCP confirmation callback is not configured"));
    }

    // Confirm declines -> refused in BOTH modes. Unattended must NOT bypass the browser-input
    // confirm the way it lets ordinary win32 automation auto-run.
    int confirmCalls = 0;
    sak::ai::AiProviderGatewayToolCallbacks declines;
    declines.confirm = [&](const QString&, const QString&, bool) {
        ++confirmCalls;
        return false;
    };
    for (const Access access : {Access::AssistedFullAccess, Access::UnattendedFullAccess}) {
        const QJsonObject result = sak::ai::authorizeWin32McpCall(plan, access, declines);
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
                 QStringLiteral("User declined the browser input action"));
    }
    QCOMPARE(confirmCalls, 2);  // the confirm gate was actually consulted in each mode

    // Non-vacuity control: an accepted confirm authorizes the call (empty result) in both modes.
    sak::ai::AiProviderGatewayToolCallbacks accepts;
    accepts.confirm = [](const QString&, const QString&, bool) {
        return true;
    };
    for (const Access access : {Access::AssistedFullAccess, Access::UnattendedFullAccess}) {
        QVERIFY2(sak::ai::authorizeWin32McpCall(plan, access, accepts).isEmpty(),
                 "an accepted browser-input confirm must authorize the call");
    }
}

void AiProviderGatewayTests::authorizeWin32Mcp_assistedMutatingRequiresConfirm() {
    using Access = sak::ai::AiProviderGatewayToolAccess;
    sak::ai::AiProviderGateway::Win32McpCallPlan plan;
    plan.requires_confirmation = false;
    plan.read_only = false;  // mutating
    plan.high_risk = false;
    plan.preview = QStringLiteral("set_zoom 150%");

    // Assisted + mutating: a confirm is mandatory. No callback -> fail closed.
    const sak::ai::AiProviderGatewayToolCallbacks noConfirm;
    const QJsonObject noCb =
        sak::ai::authorizeWin32McpCall(plan, Access::AssistedFullAccess, noConfirm);
    QVERIFY(!noCb.isEmpty());
    QCOMPARE(noCb.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Win32 MCP confirmation callback is not configured"));

    // Confirm declines -> refused.
    sak::ai::AiProviderGatewayToolCallbacks declines;
    declines.confirm = [](const QString&, const QString&, bool) {
        return false;
    };
    const QJsonObject declined =
        sak::ai::authorizeWin32McpCall(plan, Access::AssistedFullAccess, declines);
    QVERIFY(!declined.isEmpty());
    QCOMPARE(declined.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("User declined Win32 MCP automation"));

    // Confirm accepts -> authorized.
    sak::ai::AiProviderGatewayToolCallbacks accepts;
    accepts.confirm = [](const QString&, const QString&, bool) {
        return true;
    };
    QVERIFY(sak::ai::authorizeWin32McpCall(plan, Access::AssistedFullAccess, accepts).isEmpty());

    // Guard-scope control: a READ-ONLY tool in Assisted needs NO confirm -- authorized even with an
    // empty callbacks struct. Proves the confirm gate is scoped to mutating calls, not all calls.
    sak::ai::AiProviderGateway::Win32McpCallPlan readOnly = plan;
    readOnly.read_only = true;
    QVERIFY2(sak::ai::authorizeWin32McpCall(readOnly, Access::AssistedFullAccess, {}).isEmpty(),
             "a read-only assisted call must not require confirmation");
}

void AiProviderGatewayTests::authorizeWin32Mcp_unattendedHighRiskRequiresRestorePoint() {
    using Access = sak::ai::AiProviderGatewayToolAccess;
    sak::ai::AiProviderGateway::Win32McpCallPlan plan;
    plan.requires_confirmation = false;
    plan.read_only = false;
    plan.high_risk = true;
    plan.preview = QStringLiteral("uninstall app");

    // Unattended + high risk: an offered restore point is mandatory. No callback -> fail closed.
    const sak::ai::AiProviderGatewayToolCallbacks noRp;
    const QJsonObject noCb =
        sak::ai::authorizeWin32McpCall(plan, Access::UnattendedFullAccess, noRp);
    QVERIFY(!noCb.isEmpty());
    QCOMPARE(noCb.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Win32 MCP restore-point callback is not configured"));

    // Restore point cancelled -> refused.
    sak::ai::AiProviderGatewayToolCallbacks cancels;
    cancels.offer_restore_point = [](const QString&, bool) {
        return false;
    };
    const QJsonObject cancelled =
        sak::ai::authorizeWin32McpCall(plan, Access::UnattendedFullAccess, cancels);
    QVERIFY(!cancelled.isEmpty());
    QCOMPARE(cancelled.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Restore point handling cancelled Win32 MCP high-risk automation"));

    // Restore point accepted -> authorized (no human confirm is required in unattended).
    sak::ai::AiProviderGatewayToolCallbacks accepts;
    accepts.offer_restore_point = [](const QString&, bool) {
        return true;
    };
    QVERIFY(sak::ai::authorizeWin32McpCall(plan, Access::UnattendedFullAccess, accepts).isEmpty());

    // Risk-scope control: a NON-high-risk mutating tool auto-runs in Unattended -- authorized even
    // with empty callbacks. Proves the restore-point gate is scoped to high-risk calls.
    sak::ai::AiProviderGateway::Win32McpCallPlan lowRisk = plan;
    lowRisk.high_risk = false;
    QVERIFY2(sak::ai::authorizeWin32McpCall(lowRisk, Access::UnattendedFullAccess, {}).isEmpty(),
             "a non-high-risk unattended call must not require a restore point");
}

// ============================================================================
// R5-G10-9 re-sweep fills: fail-closed negatives for the AI-layer trust boundaries
// on a disk-overridable providers.json manifest and a model-supplied win32 call
// envelope. Each drives the real hostile input at the real guard and carries a
// non-vacuity / guard-isolation control.
// ============================================================================

void AiProviderGatewayTests::win32McpEnvironment_refusesProtectedAndNonStringVars() {
    // A providers.json manifest must not set a PROTECTED child env var: it redirects where the
    // (possibly elevated) MCP child resolves code (PATH / Qt plugin paths) or which user dirs it
    // reads/writes. mergeManifestEnvironment fails closed -- the returned env never reaches the
    // success block that sets WIN32_MCP_RESULT_ENVELOPE, and the error names the offending var.
    for (const QString& name : {QStringLiteral("PATH"),
                                QStringLiteral("QT_PLUGIN_PATH"),
                                QStringLiteral("LD_PRELOAD"),
                                QStringLiteral("COMSPEC")}) {
        const QJsonObject provider{
            {QStringLiteral("environment"), QJsonObject{{name, QStringLiteral("C:/attacker")}}}};
        QString error;
        const QProcessEnvironment env = sak::ai::AiProviderGateway::win32McpEnvironment(
            QStringLiteral("read_only"), provider, &error);
        QCOMPARE(error,
                 QStringLiteral("Provider manifest may not set MCP child environment variable: %1")
                     .arg(name));
        QVERIFY2(!env.contains(QStringLiteral("WIN32_MCP_RESULT_ENVELOPE")),
                 "a refused manifest env must fail closed before the success markers are set");
    }

    // A non-string value is refused too, rather than partially applied with a coerced value.
    QString nsError;
    const QJsonObject nonString{
        {QStringLiteral("environment"), QJsonObject{{QStringLiteral("CUSTOM"), 123}}}};
    const QProcessEnvironment nsEnv = sak::ai::AiProviderGateway::win32McpEnvironment(
        QStringLiteral("read_only"), nonString, &nsError);
    QCOMPARE(nsError,
             QStringLiteral("Provider manifest environment value is not a string: CUSTOM"));
    QVERIFY(!nsEnv.contains(QStringLiteral("WIN32_MCP_RESULT_ENVELOPE")));

    // Non-vacuity control: a benign string var is accepted and the success markers ARE set, so the
    // rejections above are scoped to protected/non-string vars, not to every manifest environment.
    QString okError = QStringLiteral("preset");
    const QJsonObject benign{{QStringLiteral("environment"),
                              QJsonObject{{QStringLiteral("CUSTOM"), QStringLiteral("v")}}}};
    const QProcessEnvironment okEnv = sak::ai::AiProviderGateway::win32McpEnvironment(
        QStringLiteral("read_only"), benign, &okError);
    QVERIFY(okError.isEmpty());
    QCOMPARE(okEnv.value(QStringLiteral("CUSTOM")), QStringLiteral("v"));
    QVERIFY(okEnv.contains(QStringLiteral("WIN32_MCP_RESULT_ENVELOPE")));
}

void AiProviderGatewayTests::docsQuery_refusesNonHttpsEndpoint() {
    // A providers.json docs provider endpoint is untrusted disk-overridable text. An http-transport
    // provider whose endpoint is not an absolute https URL must be refused so the model's docs
    // query is never POSTed to an attacker host or over cleartext.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject provider{{QStringLiteral("id"), QStringLiteral("microsoft_docs")},
                               {QStringLiteral("transport"), QStringLiteral("http")},
                               {QStringLiteral("endpoint"),
                                QStringLiteral("http://attacker.example/mcp")}};  // non-TLS
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
    QCOMPARE(error,
             QStringLiteral("docs_query provider endpoint is not an absolute https URL: "
                            "microsoft_docs"));
}

void AiProviderGatewayTests::planWin32McpCall_refusesMalformedEnvelope() {
    // Qt would silently coerce a non-object tool_arguments to {} and run the tool on server
    // defaults (active tab/window) instead of the model's explicit, constrained arguments; a
    // non-integer timeout_ms would silently become the default. planWin32McpCall fails closed on
    // both. The envelope guard runs before any provider lookup, so no registry fixture is needed.
    const sak::ai::AiProviderGateway gateway;

    QString argsError;
    [[maybe_unused]] const auto argsPlan = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("browser_click")},
                                 {QStringLiteral("tool_arguments"), QStringLiteral("e5")}}}},
        &argsError);
    QCOMPARE(argsError,
             QStringLiteral("win32_mcp_call arguments.tool_arguments must be an object"));

    QString timeoutError;
    [[maybe_unused]] const auto timeoutPlan = gateway.planWin32McpCall(
        QJsonObject{{QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("browser_snapshot")},
                                 {QStringLiteral("timeout_ms"), QStringLiteral("soon")}}}},
        &timeoutError);
    QCOMPARE(timeoutError,
             QStringLiteral("win32_mcp_call arguments.timeout_ms must be an integer"));

    // Guard-isolation control: a well-formed envelope does NOT yield either envelope error (any
    // later error would be a registry matter, not the envelope guard), so the guards are scoped to
    // the malformed shapes rather than firing on every call.
    QString okError;
    [[maybe_unused]] const auto okPlan =
        gateway.planWin32McpCall(QJsonObject{{QStringLiteral("arguments"),
                                              QJsonObject{{QStringLiteral("tool_name"),
                                                           QStringLiteral("browser_snapshot")}}}},
                                 &okError);
    QVERIFY(!okError.contains(QStringLiteral("must be an object")));
    QVERIFY(!okError.contains(QStringLiteral("must be an integer")));
}

QTEST_GUILESS_MAIN(AiProviderGatewayTests)
#include "test_ai_provider_gateway.moc"
