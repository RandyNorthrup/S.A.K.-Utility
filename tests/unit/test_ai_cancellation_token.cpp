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
    // cancelState() stamps ONE instant and threads it down the tree, so every descendant
    // carries the byte-identical UTC timestamp rather than its own "now".
    QCOMPARE(root.cancelledAtUtc().offsetFromUtc(), 0);
    QCOMPARE(phase.cancelledAtUtc(), root.cancelledAtUtc());
    QCOMPARE(tool.cancelledAtUtc(), root.cancelledAtUtc());
    QCOMPARE(root.cancelReason(), QStringLiteral("user_cancelled"));
    QCOMPARE(phase.cancelReason(), QStringLiteral("user_cancelled"));

    const QJsonObject json = root.toJson();
    QCOMPARE(json.value(QStringLiteral("valid")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("id")).toString(), QStringLiteral("run_1"));
    QCOMPARE(json.value(QStringLiteral("cancelled")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("reason")).toString(), QStringLiteral("user_cancelled"));
    QCOMPARE(json.value(QStringLiteral("cancelled_at")).toString(),
             root.cancelledAtUtc().toString(Qt::ISODateWithMs));
    const QJsonArray children = json.value(QStringLiteral("children")).toArray();
    QCOMPARE(children.size(), 1);
    const QJsonObject phase_json = children.at(0).toObject();
    // Direct children only, each rendered as exactly {id, cancelled, reason}.
    QCOMPARE(phase_json.size(), 3);
    QCOMPARE(phase_json.value(QStringLiteral("id")).toString(), QStringLiteral("phase_1"));
    QCOMPARE(phase_json.value(QStringLiteral("cancelled")).toBool(), true);
    QCOMPARE(phase_json.value(QStringLiteral("reason")).toString(),
             QStringLiteral("user_cancelled"));
}

void AiCancellationTokenTests::childCancelDoesNotCancelParentOrSibling() {
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_2"));
    auto phase_a = root.createChild(QStringLiteral("phase_a"));
    auto phase_b = root.createChild(QStringLiteral("phase_b"));

    phase_a.cancel(QStringLiteral("phase_failed"));

    QVERIFY(!root.isCancellationRequested());
    // A child cancel must leave NO trace on the parent: no reason, no timestamp.
    QCOMPARE(root.cancelReason(), QString());
    QVERIFY(root.cancelledAtUtc().isNull());
    QVERIFY(phase_a.isCancellationRequested());
    QVERIFY(!phase_b.isCancellationRequested());
    QCOMPARE(phase_b.cancelReason(), QString());
    QVERIFY(phase_b.cancelledAtUtc().isNull());
    // The sibling must still be REGISTERED on the root, so a later root cancel reaches it...
    QCOMPARE(root.childCount(), 2);
    root.cancel(QStringLiteral("root_cancelled"));
    QVERIFY(phase_b.isCancellationRequested());
    QCOMPARE(phase_b.cancelReason(), QStringLiteral("root_cancelled"));
    // ...while the already-cancelled sibling keeps its own reason (cancelState() stops early).
    QCOMPARE(phase_a.cancelReason(), QStringLiteral("phase_failed"));
}

void AiCancellationTokenTests::childCreatedAfterCancelStartsCancelled() {
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_3"));
    root.cancel(QStringLiteral("timeout"));

    auto late_child = root.createChild(QStringLiteral("late_child"));

    QVERIFY(late_child.isCancellationRequested());
    QCOMPARE(late_child.id(), QStringLiteral("late_child"));
    // createChild() copies the parent's cancel stamp verbatim onto a child born cancelled.
    QVERIFY(late_child.cancelledAtUtc().isValid());
    QCOMPARE(late_child.cancelledAtUtc(), root.cancelledAtUtc());
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
                    const QJsonObject child_json = child.toJson();
                    const QString child_id = child_json.value(QStringLiteral("id")).toString();
                    if (!child_json.value(QStringLiteral("valid")).toBool() ||
                        !child_id.startsWith(QStringLiteral("stress_child_"))) {
                        consistent = false;
                    }
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
        // Concurrent child creation takes the same mutex as cancel(): it must not clobber
        // the recorded reason or timestamp.
        QCOMPARE(root.cancelReason(), QStringLiteral("stop"));
        QVERIFY(root.cancelledAtUtc().isValid());
    }
}

QTEST_GUILESS_MAIN(AiCancellationTokenTests)
#include "test_ai_cancellation_token.moc"
