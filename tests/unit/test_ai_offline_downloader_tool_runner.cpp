// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_offline_downloader_tool_runner.h"

#include <QtTest/QtTest>

class AiOfflineDownloaderToolRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rejectsMissingOperation();
    void failsFastWhenCallbackMissing();
    void routesSearch();
    void blocksRunWhenWorkerBusy();
    void routesSupportedRunOperation();
};

void AiOfflineDownloaderToolRunnerTests::rejectsMissingOperation() {
    const QJsonObject result = sak::ai::AiOfflineDownloaderToolRunner::run(QJsonObject{}, {});

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QVERIFY(result.value(QStringLiteral("error_message"))
                .toString()
                .contains(QStringLiteral("requires operation")));
}

void AiOfflineDownloaderToolRunnerTests::failsFastWhenCallbackMissing() {
    const QJsonObject result = sak::ai::AiOfflineDownloaderToolRunner::run(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("presets")}}, {});

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QVERIFY(result.value(QStringLiteral("error_message"))
                .toString()
                .contains(QStringLiteral("callback is not configured")));
}

void AiOfflineDownloaderToolRunnerTests::routesSearch() {
    QString seen_query;
    sak::ai::AiOfflineDownloaderToolCallbacks callbacks;
    callbacks.search_result = [&seen_query](const QJsonObject&, const QString& query) {
        seen_query = query;
        return QJsonObject{{QStringLiteral("success"), true}, {QStringLiteral("query"), query}};
    };

    const QJsonObject result = sak::ai::AiOfflineDownloaderToolRunner::run(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral(" search ")},
                    {QStringLiteral("query"), QStringLiteral(" 7zip ")}},
        callbacks);

    QVERIFY(result.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(seen_query, QStringLiteral("7zip"));
    QCOMPARE(result.value(QStringLiteral("query")).toString(), QStringLiteral("7zip"));
}

void AiOfflineDownloaderToolRunnerTests::blocksRunWhenWorkerBusy() {
    sak::ai::AiOfflineDownloaderToolCallbacks callbacks;
    callbacks.is_running = [] {
        return true;
    };
    callbacks.run_operation = [](const QJsonObject&, const QString&) {
        return QJsonObject{{QStringLiteral("success"), true}};
    };

    const QJsonObject result = sak::ai::AiOfflineDownloaderToolRunner::run(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("direct_download")}}, callbacks);

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QVERIFY(result.value(QStringLiteral("error_message"))
                .toString()
                .contains(QStringLiteral("already running")));
}

void AiOfflineDownloaderToolRunnerTests::routesSupportedRunOperation() {
    QString seen_operation;
    sak::ai::AiOfflineDownloaderToolCallbacks callbacks;
    callbacks.is_running = [] {
        return false;
    };
    callbacks.run_operation = [&seen_operation](const QJsonObject&, const QString& operation) {
        seen_operation = operation;
        return QJsonObject{{QStringLiteral("success"), true},
                           {QStringLiteral("operation"), operation}};
    };

    const QJsonObject result = sak::ai::AiOfflineDownloaderToolRunner::run(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral(" BUILD_BUNDLE ")}}, callbacks);

    QVERIFY(result.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(seen_operation, QStringLiteral("build_bundle"));
    QCOMPARE(result.value(QStringLiteral("operation")).toString(), QStringLiteral("build_bundle"));
}

QTEST_GUILESS_MAIN(AiOfflineDownloaderToolRunnerTests)
#include "test_ai_offline_downloader_tool_runner.moc"
