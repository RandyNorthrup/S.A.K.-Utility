// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"

#include <QJsonArray>
#include <QtConcurrent>
#include <QtTest/QtTest>

#include <atomic>

namespace {

/// A generated child id is "<parent>_child_<n>", with n read under the same mutex that appends,
/// so it is deterministic and unique. @p previous_ordinal is updated in place: the ordinals a
/// single worker observes must strictly INCREASE, since the children vector only ever grows.
/// Gaps are expected -- other workers interleave -- but going backwards or repeating is not.
bool generatedChildIdIsWellFormed(const sak::ai::CancellationToken& child,
                                  qsizetype& previous_ordinal) {
    static const QString kPrefix = QStringLiteral("stress_child_");
    const QJsonObject child_json = child.toJson();
    const QString child_id = child_json.value(QStringLiteral("id")).toString();
    if (!child_json.value(QStringLiteral("valid")).toBool() || !child_id.startsWith(kPrefix)) {
        return false;
    }
    bool ordinal_ok = false;
    const qsizetype ordinal = child_id.mid(kPrefix.size()).toLongLong(&ordinal_ok);
    if (!ordinal_ok || ordinal <= previous_ordinal) {
        return false;
    }
    previous_ordinal = ordinal;
    return true;
}

}  // namespace

class AiCancellationTokenTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parentCancelCancelsChildren();
    void childCancelDoesNotCancelParentOrSibling();
    void childCreatedAfterCancelStartsCancelled();
    void concurrentCancelAndChildCreationIsConsistent();
    void cancelReachesGrandchildAfterIntermediateTokenDropped();
    void expiredChildrenArePrunedAndIdsStayUnique();
};

void AiCancellationTokenTests::cancelReachesGrandchildAfterIntermediateTokenDropped() {
    // R5-LEDGER: a real defect this suite could not see. The parent-to-child links are weak (so a
    // finished child does not pin memory), and the child-to-parent link used to be weak too. That
    // combination meant an intermediate token holding the only strong reference to its own state
    // destroyed that state the moment it went out of scope: the root's weak link to it expired,
    // cancel() walked past the expired entry, and a LIVE grandchild was never cancelled. The user
    // pressed cancel, the sub-operation kept running, and nothing reported it.
    //
    // Every existing test in this file keeps all three tokens alive in one scope, which is
    // exactly the shape that cannot reach the bug.
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run_1"));
    sak::ai::CancellationToken tool;
    {
        auto phase = root.createChild(QStringLiteral("phase_1"));
        tool = phase.createChild(QStringLiteral("tool_1"));
        // `phase` dies here. The worker still holds `tool` and is still polling it.
    }
    QVERIFY(tool.isValid());
    QVERIFY(!tool.isCancellationRequested());

    root.cancel(QStringLiteral("user_cancelled"));

    // The grandchild MUST see the cancellation: it is the token the running work polls.
    QVERIFY(tool.isCancellationRequested());
    QCOMPARE(tool.cancelReason(), QStringLiteral("user_cancelled"));
    // The intermediate is kept alive by its descendant, so the root still counts it -- proving
    // the chain survived rather than the grandchild being reached some other way.
    QCOMPARE(root.childCount(), 1);
}

void AiCancellationTokenTests::expiredChildrenArePrunedAndIdsStayUnique() {
    // The children vector used to grow forever: a long-lived root that creates a child per
    // operation made every cancel(), childCount() and toJson() walk every child it had ever had.
    // Pruning is only safe if generated ids do NOT come from children.size(), which would hand
    // two live children the same name once anything had been collected.
    auto root = sak::ai::CancellationToken::createRoot(QStringLiteral("run"));
    for (int i = 0; i < 50; ++i) {
        auto transient = root.createChild(QString());  // dies immediately
        QVERIFY(transient.isValid());
    }
    QCOMPARE(root.childCount(), 0);  // none of them are still alive

    // Two survivors created after 50 collected children must still get DISTINCT generated ids.
    auto first = root.createChild(QString());
    auto second = root.createChild(QString());
    QVERIFY(first.id() != second.id());
    QCOMPARE(root.childCount(), 2);
    // The generated names are index-based and monotonic, never reused after pruning.
    QCOMPARE(first.id(), QStringLiteral("run_child_51"));
    QCOMPARE(second.id(), QStringLiteral("run_child_52"));
}

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
    // ... and childCount() is not a plain size(): it counts only children whose weak_ptr has NOT
    // expired. Every token in this file is held alive in a local for the whole test, so that
    // filter was never observed by any assertion in the repo -- both exact counts were equally
    // satisfied by returning the raw vector size, which grows without bound because the children
    // vector is append-only. This is the difference between "how many children are still
    // running" and "how many were ever created".
    {
        auto scoped = root.createChild(QStringLiteral("scoped"));
        QCOMPARE(scoped.id(), QStringLiteral("scoped"));
        QCOMPARE(root.childCount(), 3);
    }
    QVERIFY2(root.childCount() == 2, "a destroyed child must stop being counted");
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
    // createChild() copies the parent's cancel stamp VERBATIM onto a child born cancelled. The
    // equality below is the whole claim, and QDateTime compares at millisecond resolution -- so
    // with the parent cancelled microseconds earlier in the same test, a child that stamped a
    // FRESH "now" instead of copying would compare equal anyway. Backdating the parent's stamp
    // forces the two candidate sources apart: a copied stamp is old, a fresh one is not.
    QVERIFY(late_child.cancelledAtUtc().isValid());
    QCOMPARE(late_child.cancelledAtUtc(), root.cancelledAtUtc());
    QCOMPARE(late_child.cancelReason(), QStringLiteral("timeout"));
    QCOMPARE(root.childCount(), 1);

    auto aged = sak::ai::CancellationToken::createRoot(QStringLiteral("run_3b"));
    aged.cancel(QStringLiteral("timeout"));
    const QDateTime aged_stamp = aged.cancelledAtUtc();
    QVERIFY(aged_stamp.isValid());
    // A second of separation is far beyond QDateTime's millisecond resolution, so a fresh stamp
    // cannot masquerade as the copied one.
    QTest::qWait(1100);
    auto aged_child = aged.createChild(QStringLiteral("aged_child"));
    QVERIFY(aged_child.isCancellationRequested());
    QCOMPARE(aged_child.cancelledAtUtc(), aged_stamp);
    QVERIFY2(aged_child.cancelledAtUtc().msecsTo(QDateTime::currentDateTimeUtc()) >= 1000,
             "the child stamped a fresh 'now' instead of copying the parent's cancel instant");
    QCOMPARE(aged_child.cancelReason(), QStringLiteral("timeout"));
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
                // This loop is the ONLY place in the repository that exercises createChild()'s
                // generated-id arm -- every other call site, in tests and in src/, passes an
                // explicit id. The generated form is "<parent>_child_<n>" where n is
                // children.size() + 1 read under the same mutex that appends, so the ordinal is
                // deterministic and unique; startsWith() constrained only the constant prefix,
                // leaving the entire counter half -- the uniqueness of trace ids, which is the
                // reason the counter exists -- unasserted. The ordinals a single worker observes
                // must strictly INCREASE: the children vector only grows, and size() is read
                // under the append lock. Gaps are expected (other workers interleave); going
                // backwards or repeating is not.
                qsizetype previous_ordinal = 0;
                for (int i = 0; i < 200; ++i) {
                    auto child = root.createChild(QString());
                    (void)root.isCancellationRequested();
                    if (!generatedChildIdIsWellFormed(child, previous_ordinal)) {
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
