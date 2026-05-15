// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"

#include <QJsonArray>
#include <QtTest/QtTest>

class AiCancellationTokenTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parentCancelCancelsChildren();
    void childCancelDoesNotCancelParentOrSibling();
    void childCreatedAfterCancelStartsCancelled();
};

void AiCancellationTokenTests::parentCancelCancelsChildren() {
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_1"));
    auto phase = root.createChild(QStringLiteral("phase_1"));
    auto tool = phase.createChild(QStringLiteral("tool_1"));

    root.cancel(QStringLiteral("user_cancelled"));

    QVERIFY(root.isCancellationRequested());
    QVERIFY(phase.isCancellationRequested());
    QVERIFY(tool.isCancellationRequested());
    QCOMPARE(tool.cancelReason(), QStringLiteral("user_cancelled"));
    QVERIFY(root.cancelledAtUtc().isValid());

    const QJsonObject json = root.toJson();
    QCOMPARE(json.value(QStringLiteral("cancelled")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("children")).toArray().size(), 1);
}

void AiCancellationTokenTests::childCancelDoesNotCancelParentOrSibling() {
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_2"));
    auto phase_a = root.createChild(QStringLiteral("phase_a"));
    auto phase_b = root.createChild(QStringLiteral("phase_b"));

    phase_a.cancel(QStringLiteral("phase_failed"));

    QVERIFY(!root.isCancellationRequested());
    QVERIFY(phase_a.isCancellationRequested());
    QVERIFY(!phase_b.isCancellationRequested());
}

void AiCancellationTokenTests::childCreatedAfterCancelStartsCancelled() {
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_3"));
    root.cancel(QStringLiteral("timeout"));

    auto late_child = root.createChild(QStringLiteral("late_child"));

    QVERIFY(late_child.isCancellationRequested());
    QCOMPARE(late_child.cancelReason(), QStringLiteral("timeout"));
    QCOMPARE(root.childCount(), 1);
}

QTEST_GUILESS_MAIN(AiCancellationTokenTests)
#include "test_ai_cancellation_token.moc"
