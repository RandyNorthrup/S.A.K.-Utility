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

}  // namespace

class AiProviderRegistryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void providerStatusesReportMissingPortableCommand();
    void providerRegistryRejectsInvalidJson();
    void providerRegistryCacheInvalidatesOnFileTimestampChange();
    void appManifestRejectsInvalidJson();
    void appCapabilitiesExposeRequestedActionPlan();
    void defaultDocsProvidersDoNotRequireApiKeys();
};

void AiProviderRegistryTests::providerStatusesReportMissingPortableCommand() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    QJsonArray providers;
    providers.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("microsoft_docs")},
                                 {QStringLiteral("transport"), QStringLiteral("http")},
                                 {QStringLiteral("endpoint"),
                                  QStringLiteral("https://learn.microsoft.com/api/mcp")}});
    providers.append(
        QJsonObject{{QStringLiteral("id"), QStringLiteral("win32_mcp")},
                    {QStringLiteral("transport"), QStringLiteral("stdio")},
                    {QStringLiteral("command"),
                     QStringLiteral("tools/mcp/win32-mcp-server/win32-mcp-server.exe")}});
    QVERIFY(writeFile(providers_path,
                      QJsonDocument(QJsonObject{{QStringLiteral("providers"), providers}})
                          .toJson(QJsonDocument::Compact)));

    sak::ai::AiProviderRegistry registry(temp.path());
    QString error;
    const QJsonObject statuses = registry.providerStatuses(&error);

    QVERIFY(error.isEmpty());
    QCOMPARE(statuses.value(QStringLiteral("provider_count")).toInt(), 2);
    const QJsonObject docs = providerById(statuses.value(QStringLiteral("providers")).toArray(),
                                          QStringLiteral("microsoft_docs"));
    const QJsonObject win32 = providerById(statuses.value(QStringLiteral("providers")).toArray(),
                                           QStringLiteral("win32_mcp"));
    QVERIFY(docs.value(QStringLiteral("available")).toBool(false));
    QVERIFY(!win32.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(win32.value(QStringLiteral("missing_reason")).toString(),
             QStringLiteral("Bundled MCP command missing"));
}

void AiProviderRegistryTests::providerRegistryRejectsInvalidJson() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    QVERIFY(writeFile(providers_path, QByteArray("{ bad json")));

    sak::ai::AiProviderRegistry registry(temp.path());
    QString error;
    const QJsonObject object = registry.providersObject(&error);

    QVERIFY(object.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Invalid JSON")));
}

void AiProviderRegistryTests::providerRegistryCacheInvalidatesOnFileTimestampChange() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString providers_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/providers/providers.json"));
    QVERIFY(writeFile(providers_path, R"({"providers":[{"id":"one","transport":"http"}]})"));

    sak::ai::AiProviderRegistry registry(temp.path());
    QString error;
    QCOMPARE(registry.providers(&error).first().toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("one"));
    QVERIFY(error.isEmpty());

    QTest::qWait(1100);
    QVERIFY(writeFile(providers_path, R"({"providers":[{"id":"two","transport":"http"}]})"));
    QCOMPARE(registry.providers(&error).first().toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("two"));
    QVERIFY(error.isEmpty());
}

void AiProviderRegistryTests::appManifestRejectsInvalidJson() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString manifest_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/app_manifests/bad_app.json"));
    QVERIFY(writeFile(manifest_path, QByteArray("{ bad json")));

    sak::ai::AiProviderRegistry registry(temp.path());
    QString error;
    const QJsonObject manifest = registry.appManifest(QStringLiteral("bad_app"), &error);

    QVERIFY(manifest.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Invalid JSON")));
}

void AiProviderRegistryTests::appCapabilitiesExposeRequestedActionPlan() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString manifest_path =
        QDir(temp.path()).filePath(QStringLiteral("data/ai/app_manifests/sample_app.json"));
    const QJsonObject quick_scan{{QStringLiteral("supported"), true},
                                 {QStringLiteral("control_type"), QStringLiteral("cli")},
                                 {QStringLiteral("command"),
                                  QStringLiteral("sample.exe --quick-scan")}};
    const QJsonObject full_scan{{QStringLiteral("supported"), false},
                                {QStringLiteral("reason"), QStringLiteral("Manual GUI only")}};
    const QJsonObject manifest{{QStringLiteral("id"), QStringLiteral("sample_app")},
                               {QStringLiteral("display_name"), QStringLiteral("Sample App")},
                               {QStringLiteral("actions"),
                                QJsonObject{{QStringLiteral("quick_scan"), quick_scan},
                                            {QStringLiteral("full_scan"), full_scan}}}};
    QVERIFY(writeFile(manifest_path, QJsonDocument(manifest).toJson(QJsonDocument::Compact)));

    sak::ai::AiProviderRegistry registry(temp.path());
    QString error;
    const QJsonObject quick = registry.appCapabilities(QStringLiteral("sample_app"),
                                                       QStringLiteral("quick_scan"),
                                                       &error);
    QVERIFY(error.isEmpty());
    QVERIFY(quick.value(QStringLiteral("requested_action_supported")).toBool(false));
    QCOMPARE(quick.value(QStringLiteral("requested_action")).toString(),
             QStringLiteral("quick_scan"));
    QCOMPARE(quick.value(QStringLiteral("requested_action_profile"))
                 .toObject()
                 .value(QStringLiteral("control_type"))
                 .toString(),
             QStringLiteral("cli"));

    const QJsonObject missing = registry.appCapabilities(QStringLiteral("sample_app"),
                                                         QStringLiteral("definition_update"),
                                                         &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!missing.value(QStringLiteral("requested_action_supported")).toBool(true));
}

void AiProviderRegistryTests::defaultDocsProvidersDoNotRequireApiKeys() {
    sak::ai::AiProviderRegistry registry;
    QString error;
    const QJsonObject object = registry.providersObject(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonArray providers = object.value(QStringLiteral("providers")).toArray();
    for (const QString& id : {QStringLiteral("context7"), QStringLiteral("microsoft_docs")}) {
        const QJsonObject provider = providerById(providers, id);
        QVERIFY2(!provider.isEmpty(), qPrintable(QStringLiteral("Missing provider %1").arg(id)));
        QVERIFY(!provider.value(QStringLiteral("requires_auth")).toBool(false));
        QVERIFY(!provider.contains(QStringLiteral("api_key")));
        QVERIFY(!provider.contains(QStringLiteral("auth_token")));
    }
}

QTEST_GUILESS_MAIN(AiProviderRegistryTests)
#include "test_ai_provider_registry.moc"
