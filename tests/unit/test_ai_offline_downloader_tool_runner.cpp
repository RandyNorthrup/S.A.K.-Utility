// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_offline_downloader_tool_runner.h"

#include <QSemaphore>
#include <QtTest/QtTest>

#include <atomic>
#include <thread>

class AiOfflineDownloaderToolRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rejectsMissingOperation();
    void failsFastWhenCallbackMissing();
    void routesSearch();
    void blocksRunWhenWorkerBusy();
    void routesSupportedRunOperation();
    void serializesConcurrentRunOperationsProcessWide();
};

void AiOfflineDownloaderToolRunnerTests::rejectsMissingOperation() {
    const QJsonObject result = sak::ai::AiOfflineDownloaderToolRunner::run(QJsonObject{}, {});

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Offline downloader requires operation"));
}

void AiOfflineDownloaderToolRunnerTests::failsFastWhenCallbackMissing() {
    const QJsonObject result = sak::ai::AiOfflineDownloaderToolRunner::run(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("presets")}}, {});

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Offline downloader presets callback is not configured"));
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
    QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("An offline deployment operation is already running"));
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

void AiOfflineDownloaderToolRunnerTests::serializesConcurrentRunOperationsProcessWide() {
    QSemaphore first_callback_entered;
    QSemaphore release_first_callback;
    std::atomic_int run_callback_count{0};

    sak::ai::AiOfflineDownloaderToolCallbacks first_callbacks;
    first_callbacks.is_running = [] {
        return false;
    };
    first_callbacks.run_operation = [&](const QJsonObject&, const QString&) {
        run_callback_count.fetch_add(1, std::memory_order_relaxed);
        first_callback_entered.release();
        release_first_callback.acquire();
        return QJsonObject{{QStringLiteral("success"), true}};
    };

    sak::ai::AiOfflineDownloaderToolCallbacks contender_callbacks;
    contender_callbacks.is_running = [] {
        return false;
    };
    contender_callbacks.run_operation = [&](const QJsonObject&, const QString&) {
        run_callback_count.fetch_add(1, std::memory_order_relaxed);
        return QJsonObject{{QStringLiteral("success"), true}};
    };

    const QJsonObject args{{QStringLiteral("operation"), QStringLiteral("direct_download")}};
    QJsonObject first_result;
    std::thread first_thread([&]() {
        first_result = sak::ai::AiOfflineDownloaderToolRunner::run(args, first_callbacks);
    });

    first_callback_entered.acquire();
    const QJsonObject contender_result =
        sak::ai::AiOfflineDownloaderToolRunner::run(args, contender_callbacks);
    release_first_callback.release();
    first_thread.join();

    const QJsonObject after_release_result =
        sak::ai::AiOfflineDownloaderToolRunner::run(args, contender_callbacks);

    QVERIFY(first_result.value(QStringLiteral("success")).toBool(false));
    QVERIFY(!contender_result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(contender_result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("An offline deployment operation is already running"));
    QVERIFY(after_release_result.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(run_callback_count.load(std::memory_order_relaxed), 2);
}

QTEST_GUILESS_MAIN(AiOfflineDownloaderToolRunnerTests)
#include "test_ai_offline_downloader_tool_runner.moc"
