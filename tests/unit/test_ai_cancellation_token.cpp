// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"

#include <QJsonArray>
#include <QtConcurrent>
#include <QtTest/QtTest>

#include <atomic>

class AiCancellationTokenTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parentCancelCancelsChildren();
    void childCancelDoesNotCancelParentOrSibling();
    void childCreatedAfterCancelStartsCancelled();
    void concurrentCancelAndChildCreationIsConsistent();
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

void AiCancellationTokenTests::concurrentCancelAndChildCreationIsConsistent() {
    // Exercise the synchronized paths: many worker threads create children and poll the
    // token while the "UI" thread cancels. This must not crash or corrupt state, and once
    // cancelled every subsequently created child must observe cancellation.
    for (int iteration = 0; iteration < 50; ++iteration) {
        auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("stress"));

        std::atomic_bool cancelled_flag{false};
        QList<QFuture<bool>> workers;
        for (int worker = 0; worker < 8; ++worker) {
            workers.append(QtConcurrent::run([root, &cancelled_flag]() {
                bool consistent = true;
                for (int i = 0; i < 200; ++i) {
                    auto child = root.createChild(QString());
                    (void)root.isCancellationRequested();
                    (void)child.toJson();
                    // Once cancellation is globally visible, a fresh child must be cancelled.
                    if (cancelled_flag.load() &&
                        !root.createChild(QString()).isCancellationRequested()) {
                        consistent = false;
                    }
                }
                return consistent;
            }));
        }

        root.cancel(QStringLiteral("stop"));
        cancelled_flag.store(true);

        for (auto& future : workers) {
            future.waitForFinished();
            QVERIFY(future.result());
        }
        QVERIFY(root.isCancellationRequested());
    }
}

QTEST_GUILESS_MAIN(AiCancellationTokenTests)
#include "test_ai_cancellation_token.moc"
