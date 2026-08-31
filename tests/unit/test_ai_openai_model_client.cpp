// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_openai_model_client.h"

#include <QElapsedTimer>
#include <QTest>
#include <QThread>

namespace {

constexpr qint64 kImmediateFailureBudgetMs = 1000;
constexpr auto kThreadStartFailure = "Model invocation worker thread failed to start";

class AiOpenAIModelClientTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void startedThreadRunsWorkerAndReturnsClientFailure();
    void threadStartFailureReturnsImmediately();
    void cancellationOutranksThreadStartFailure();
};

void AiOpenAIModelClientTests::startedThreadRunsWorkerAndReturnsClientFailure() {
    sak::ai::OpenAIResponsesModelClient client;
    const auto token = sak::ai::CancellationToken::createRoot(QStringLiteral("worker-started"));

    const auto response = client.invoke({}, token);

    QVERIFY(!response.success);
    QCOMPARE(response.error_message, QStringLiteral("OpenAI API key is missing or too short"));
}

void AiOpenAIModelClientTests::threadStartFailureReturnsImmediately() {
    sak::ai::OpenAIResponsesModelClient client;
    int starter_calls = 0;
    client.setThreadStarterForTesting([&starter_calls](QThread&) { ++starter_calls; });
    const auto token = sak::ai::CancellationToken::createRoot(QStringLiteral("launch-failure"));
    QElapsedTimer elapsed;

    elapsed.start();
    const auto response = client.invoke({}, token);

    QCOMPARE(starter_calls, 1);
    QVERIFY2(elapsed.elapsed() < kImmediateFailureBudgetMs,
             "a failed QThread launch waited for the model timeout");
    QVERIFY(!response.success);
    QCOMPARE(response.error_message, QString::fromLatin1(kThreadStartFailure));
}

void AiOpenAIModelClientTests::cancellationOutranksThreadStartFailure() {
    sak::ai::OpenAIResponsesModelClient client;
    const auto token = sak::ai::CancellationToken::createRoot(QStringLiteral("cancel-priority"));
    int starter_calls = 0;
    client.setThreadStarterForTesting([&](QThread&) {
        ++starter_calls;
        token.cancel(QStringLiteral("operator_cancelled"));
    });

    const auto response = client.invoke({}, token);

    QCOMPARE(starter_calls, 1);
    QVERIFY(!response.success);
    QCOMPARE(response.error_message, QStringLiteral("operator_cancelled"));
}

}  // namespace

QTEST_GUILESS_MAIN(AiOpenAIModelClientTests)
#include "test_ai_openai_model_client.moc"
