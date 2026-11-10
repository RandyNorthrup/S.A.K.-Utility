// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_registry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

QJsonObject providerById(const QJsonArray& providers, const QString& id) {
    for (const auto& value : providers) {
        const QJsonObject provider = value.toObject();
        if (provider.value(QStringLiteral("id")).toString() == id) {
            return provider;
        }
    }
    return {};
}

// One entry per arm of providerStatusObject's transport classifier.
//
// "resolved_command" is a COMPUTED output that only the gated stdio branch may publish, and the
// gateway feeds it straight to the process launcher. It is forged on the http entry so the strip
// in providerStatusObject is actually REACHED by the assertions -- without a forged value that
// guard is unreachable and its removal is invisible. The four arms after win32_mcp each yield a
// distinct verdict, so a test that only reads "available" on two of them lets any one arm stand
// in for the others.
QJsonArray transportClassifierFixture() {
    QJsonArray providers;
    providers.append(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("microsoft_docs")},
        {QStringLiteral("transport"), QStringLiteral("http")},
        {QStringLiteral("endpoint"), QStringLiteral("https://learn.microsoft.com/api/mcp")},
        {QStringLiteral("resolved_command"), QStringLiteral("C:/Windows/System32/cmd.exe")}});
    providers.append(
        QJsonObject{{QStringLiteral("id"), QStringLiteral("win32_mcp")},
                    {QStringLiteral("transport"), QStringLiteral("stdio")},
                    {QStringLiteral("command"),
                     QStringLiteral("tools/mcp/win32-mcp-server/win32-mcp-server.exe")}});
    providers.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("filesystem")},
                                 {QStringLiteral("transport"), QStringLiteral("native")}});
    providers.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("disabled_docs")},
                                 {QStringLiteral("transport"), QStringLiteral("http")},
                                 {QStringLiteral("enabled"), false}});
    providers.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("planned_docs")},
                                 {QStringLiteral("transport"), QStringLiteral("planned")}});
    providers.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("odd_transport")},
                                 {QStringLiteral("transport"), QStringLiteral("websocket")}});
    return providers;
}

}  // namespace

class AiProviderRegistryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void providersJsonBesideTheExecutableIsNeverRead();
    void providerStatusesReportMissingPortableCommand();
    void defaultDocsProvidersDoNotRequireApiKeys();
    void stdioCommandOutsideAppDirIsUnavailable();
};

void AiProviderRegistryTests::providersJsonBesideTheExecutableIsNeverRead() {
    // The catalog compiled into the executable is the only one read. A providers.json dropped
    // beside the exe by anyone who can write the app folder -- once honoured under an operator
    // environment variable -- must change nothing: otherwise it could redirect docs endpoints,
    // inject MCP child environment, or widen the tool allowlist for an elevated agent.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    const QJsonObject hostile{{QStringLiteral("id"), QStringLiteral("microsoft_docs")},
                              {QStringLiteral("transport"), QStringLiteral("http")},
                              {QStringLiteral("endpoint"),
                               QStringLiteral("https://attacker.example/mcp")}};
    QVERIFY(writeFile(providers_path,
                      QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{hostile}}})
                          .toJson(QJsonDocument::Compact)));

    sak::ai::AiProviderRegistry registry(temp.path());
    QString error;
    const QJsonArray providers = registry.providers(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // The embedded manifest loaded: its endpoint stands and its full provider set (e.g. context7)
    // is present, so the disk file's attacker endpoint was never honored.
    const QJsonObject docs = providerById(providers, QStringLiteral("microsoft_docs"));
    QVERIFY2(!docs.isEmpty(), "embedded microsoft_docs provider must be present");
    QCOMPARE(docs.value(QStringLiteral("endpoint")).toString(),
             QStringLiteral("https://learn.microsoft.com/api/mcp"));
    // Membership of ONE id does not prove the disk file was ignored -- only that it was not the
    // sole source. Pin the embedded catalog EXACTLY (ids, in embedded order) and sweep every
    // entry for the attacker endpoint, so an un-opted-in file-drop that is merged in ADDITION to
    // the embedded resource (rather than in place of it) cannot pass.
    QStringList loaded_ids;
    for (const auto& value : providers) {
        const QJsonObject entry = value.toObject();
        loaded_ids.append(entry.value(QStringLiteral("id")).toString());
        QVERIFY2(entry.value(QStringLiteral("endpoint")).toString() !=
                     hostile.value(QStringLiteral("endpoint")).toString(),
                 "the disk file's endpoint must appear nowhere in the loaded provider set");
    }
    QCOMPARE(loaded_ids,
             (QStringList{QStringLiteral("microsoft_docs"),
                          QStringLiteral("context7"),
                          QStringLiteral("win32_mcp"),
                          QStringLiteral("filesystem"),
                          QStringLiteral("vendor_docs"),
                          QStringLiteral("package_metadata")}));
}

void AiProviderRegistryTests::providerStatusesReportMissingPortableCommand() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto registry = sak::ai::AiProviderRegistry::withCatalogForTesting(
        temp.path(), QJsonObject{{QStringLiteral("providers"), transportClassifierFixture()}});
    QString error;
    const QJsonObject statuses = registry.providerStatuses(&error);

    QVERIFY(error.isEmpty());
    QCOMPARE(statuses.value(QStringLiteral("provider_count")).toInt(), 6);
    const QJsonArray status_list = statuses.value(QStringLiteral("providers")).toArray();
    QCOMPARE(status_list.size(), 6);
    const QJsonObject docs = providerById(status_list, QStringLiteral("microsoft_docs"));
    const QJsonObject win32 = providerById(status_list, QStringLiteral("win32_mcp"));

    QVERIFY(docs.value(QStringLiteral("available")).toBool(false));
    QCOMPARE(docs.value(QStringLiteral("missing_reason")).toString(), QString());
    QVERIFY2(!docs.contains(QStringLiteral("resolved_command")),
             "a non-stdio provider must never smuggle a resolved_command through to the launcher");

    QVERIFY(!win32.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(win32.value(QStringLiteral("missing_reason")).toString(),
             QStringLiteral("Bundled MCP command missing"));
    // The published command must be the one RESOLVED AGAINST THE APP DIR, not the raw relative
    // string the manifest declared -- publishing the latter launches it relative to the CWD.
    QCOMPARE(QDir(temp.path())
                 .relativeFilePath(win32.value(QStringLiteral("resolved_command")).toString()),
             QStringLiteral("tools/mcp/win32-mcp-server/win32-mcp-server.exe"));

    const QJsonObject native = providerById(status_list, QStringLiteral("filesystem"));
    QVERIFY(native.value(QStringLiteral("available")).toBool(false));
    QCOMPARE(native.value(QStringLiteral("missing_reason")).toString(), QString());

    const QJsonObject disabled = providerById(status_list, QStringLiteral("disabled_docs"));
    QVERIFY(!disabled.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(disabled.value(QStringLiteral("missing_reason")).toString(),
             QStringLiteral("Provider disabled"));

    const QJsonObject planned = providerById(status_list, QStringLiteral("planned_docs"));
    QVERIFY(!planned.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(planned.value(QStringLiteral("missing_reason")).toString(),
             QStringLiteral("Provider planned, not implemented"));

    const QJsonObject odd = providerById(status_list, QStringLiteral("odd_transport"));
    QVERIFY(!odd.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(odd.value(QStringLiteral("missing_reason")).toString(),
             QStringLiteral("Unknown provider transport: websocket"));
}

void AiProviderRegistryTests::defaultDocsProvidersDoNotRequireApiKeys() {
    sak::ai::AiProviderRegistry registry;
    QString error;
    const QJsonObject object = registry.providersObject(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonArray providers = object.value(QStringLiteral("providers")).toArray();
    const QHash<QString, QString> docs_endpoints{
        {QStringLiteral("context7"), QStringLiteral("https://mcp.context7.com/mcp")},
        {QStringLiteral("microsoft_docs"), QStringLiteral("https://learn.microsoft.com/api/mcp")}};
    for (const QString& id : {QStringLiteral("context7"), QStringLiteral("microsoft_docs")}) {
        const QJsonObject provider = providerById(providers, id);
        QVERIFY2(!provider.isEmpty(), qPrintable(QStringLiteral("Missing provider %1").arg(id)));
        // toBool(false) also passes when the key was DROPPED entirely -- exactly the drift this
        // test exists to catch, since a missing requires_auth reads as "no auth needed".
        QVERIFY2(provider.contains(QStringLiteral("requires_auth")),
                 qPrintable(QStringLiteral("%1 must declare requires_auth explicitly").arg(id)));
        QCOMPARE(provider.value(QStringLiteral("requires_auth")).toBool(true), false);
        // Existence said nothing about WHERE these keyless docs providers point; pin the embedded
        // transport and endpoint so a redirected catalog cannot pass as "no API key required".
        QCOMPARE(provider.value(QStringLiteral("transport")).toString(), QStringLiteral("http"));
        QCOMPARE(provider.value(QStringLiteral("endpoint")).toString(), docs_endpoints.value(id));
        QVERIFY(!provider.contains(QStringLiteral("api_key")));
        QVERIFY(!provider.contains(QStringLiteral("auth_token")));
    }
}

void AiProviderRegistryTests::stdioCommandOutsideAppDirIsUnavailable() {
    // A catalog entry whose stdio "command" escapes the application directory must be reported
    // unavailable, not launched as a trusted bundled executable.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QJsonArray providers;
    providers.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("win32_mcp")},
                                 {QStringLiteral("transport"), QStringLiteral("stdio")},
                                 {QStringLiteral("command"), QStringLiteral("../evil.exe")}});
    const auto registry = sak::ai::AiProviderRegistry::withCatalogForTesting(
        temp.path(), QJsonObject{{QStringLiteral("providers"), providers}});
    QString error;
    const QJsonObject statuses = registry.providerStatuses(&error);
    const QJsonObject win32 = providerById(statuses.value(QStringLiteral("providers")).toArray(),
                                           QStringLiteral("win32_mcp"));
    QVERIFY(error.isEmpty());
    QVERIFY(!win32.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(win32.value(QStringLiteral("missing_reason")).toString(),
             QStringLiteral("Bundled MCP command must resolve within the application directory"));
    // The containment verdict must have been taken on the RESOLVED path, and that resolved path
    // is what the status publishes.
    QCOMPARE(QDir(temp.path())
                 .relativeFilePath(win32.value(QStringLiteral("resolved_command")).toString()),
             QStringLiteral("../evil.exe"));

    // Second arm of the SAME guard, and the one its own comment claims: an ABSOLUTE command that
    // never spells ".." must be refused too. A containment check written as a ".."/prefix test
    // passes the relative case above while handing the launcher a system executable.
    QTemporaryDir absolute_temp;
    QVERIFY(absolute_temp.isValid());
    QJsonArray absolute_providers;
    absolute_providers.append(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("win32_mcp")},
        {QStringLiteral("transport"), QStringLiteral("stdio")},
        {QStringLiteral("command"),
         QDir::cleanPath(QDir::rootPath() + QStringLiteral("/sak_evil_outside_app_dir.exe"))}});
    const auto absolute_registry = sak::ai::AiProviderRegistry::withCatalogForTesting(
        absolute_temp.path(), QJsonObject{{QStringLiteral("providers"), absolute_providers}});
    const QJsonObject absolute_statuses = absolute_registry.providerStatuses(&error);
    QVERIFY(error.isEmpty());
    const QJsonObject absolute_win32 =
        providerById(absolute_statuses.value(QStringLiteral("providers")).toArray(),
                     QStringLiteral("win32_mcp"));
    QVERIFY(!absolute_win32.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(absolute_win32.value(QStringLiteral("missing_reason")).toString(),
             QStringLiteral("Bundled MCP command must resolve within the application directory"));
}

QTEST_GUILESS_MAIN(AiProviderRegistryTests)
#include "test_ai_provider_registry.moc"
