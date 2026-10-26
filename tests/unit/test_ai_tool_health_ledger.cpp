#include "sak/ai/ai_tool_health_ledger.h"

#include <QFuture>
#include <QJsonArray>
#include <QtConcurrent>
#include <QtTest/QtTest>

class AiToolHealthLedgerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void recordSuccessClearsBackoff();
    void repeatedFailuresSuppressTemporarily();
    void resetForgetsEveryRecord();
    void classifyResultUsesSpecificFailureFields();
    void concurrentRecordAndReadIsThreadSafe();
    void snapshotOrdersRecordsByKey();
};

void AiToolHealthLedgerTests::recordSuccessClearsBackoff() {
    sak::ai::AiToolHealthLedger ledger(2, 1000, 10'000);
    const QDateTime now = QDateTime::currentDateTimeUtc();

    ledger.recordFailure(
        QStringLiteral("tool"), QStringLiteral("timeout"), QStringLiteral("slow"), 10, now);
    ledger.recordFailure(QStringLiteral("tool"),
                         QStringLiteral("timeout"),
                         QStringLiteral("slow"),
                         11,
                         now.addSecs(1));
    QVERIFY(!ledger.check(QStringLiteral("tool"), now.addSecs(1)).available);

    ledger.recordSuccess(QStringLiteral("tool"), 5, now.addSecs(2));
    const auto record = ledger.record(QStringLiteral("tool"));
    QCOMPARE(record.consecutive_failures, 0);
    QVERIFY(!record.disabled_until_utc.isValid());
    QVERIFY(ledger.check(QStringLiteral("tool"), now.addSecs(2)).available);
}

void AiToolHealthLedgerTests::repeatedFailuresSuppressTemporarily() {
    sak::ai::AiToolHealthLedger ledger(2, 1000, 10'000);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    ledger.recordFailure(QStringLiteral("provider"),
                         QStringLiteral("unavailable"),
                         QStringLiteral("missing"),
                         0,
                         now);
    QVERIFY(ledger.check(QStringLiteral("provider"), now).available);
    ledger.recordFailure(QStringLiteral("provider"),
                         QStringLiteral("unavailable"),
                         QStringLiteral("missing"),
                         0,
                         now);

    const auto availability = ledger.check(QStringLiteral("provider"), now.addMSecs(500));
    QVERIFY(!availability.available);
    QCOMPARE(availability.failure_class, QStringLiteral("health_backoff"));
    // "Tool/provider" already contains "provider", so the old check was vacuous. Pin the actual
    // %1 key substitution (startsWith) and the count + error tail (endsWith); only the %2 ISO
    // disabled-until instant in the middle is wall-clock-derived and left unpinned.
    QVERIFY(availability.reason.startsWith(
        QStringLiteral("Tool/provider 'provider' is temporarily disabled until ")));
    QVERIFY(
        availability.reason.endsWith(QStringLiteral(" after 2 consecutive failure(s): missing")));
}

// The breaker is per chat session (owner ruling 2026-10-24): reset() must forget a tripped
// tool so the next session starts with it available, and must forget the counters too, so a
// single failure in the new session does not re-trip a breaker the old session filled.
void AiToolHealthLedgerTests::resetForgetsEveryRecord() {
    sak::ai::AiToolHealthLedger ledger(2, 60'000, 60'000);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    ledger.recordFailure(
        QStringLiteral("tool"), QStringLiteral("timeout"), QStringLiteral("slow"), 1, now);
    ledger.recordFailure(
        QStringLiteral("tool"), QStringLiteral("timeout"), QStringLiteral("slow"), 1, now);
    ledger.recordSuccess(QStringLiteral("other"), 1, now);
    QVERIFY(!ledger.check(QStringLiteral("tool"), now).available);
    QCOMPARE(ledger.size(), 2);

    ledger.reset();

    QCOMPARE(ledger.size(), 0);
    QVERIFY(ledger.check(QStringLiteral("tool"), now).available);
    QCOMPARE(ledger.snapshot().value(QStringLiteral("record_count")).toInt(-1), 0);
    ledger.recordFailure(
        QStringLiteral("tool"), QStringLiteral("timeout"), QStringLiteral("slow"), 1, now);
    QCOMPARE(ledger.record(QStringLiteral("tool")).consecutive_failures, 1);
    QVERIFY(ledger.check(QStringLiteral("tool"), now).available);
}

void AiToolHealthLedgerTests::classifyResultUsesSpecificFailureFields() {
    QJsonObject explicit_failure{{QStringLiteral("success"), false},
                                 {QStringLiteral("failure_class"),
                                  QStringLiteral("bad_transport")}};
    QCOMPARE(sak::ai::AiToolHealthLedger::classifyResult(explicit_failure),
             QStringLiteral("bad_transport"));

    QJsonObject checksum{{QStringLiteral("success"), false},
                         {QStringLiteral("error_message"), QStringLiteral("Checksum mismatch")}};
    QCOMPARE(sak::ai::AiToolHealthLedger::classifyResult(checksum),
             QStringLiteral("checksum_mismatch"));
}

// Tool dispatch may record success/failure from a worker thread while the GUI
// thread reads the ledger (run-details snapshot). Before the mutex was added the
// shared QHash was written and iterated concurrently -- undefined behavior that
// crashes intermittently. This hammers both paths in parallel; it must not crash
// and every recorded tool must survive.
void AiToolHealthLedgerTests::concurrentRecordAndReadIsThreadSafe() {
    sak::ai::AiToolHealthLedger ledger(3, 1000, 10'000);
    constexpr int kToolCount = 32;
    constexpr int kIterations = 200;

    QVector<int> indices(kToolCount);
    for (int i = 0; i < kToolCount; ++i) {
        indices[i] = i;
    }

    QFuture<void> writers = QtConcurrent::map(indices, [&ledger](int tool) {
        const QString key = QStringLiteral("tool_%1").arg(tool);
        for (int i = 0; i < kIterations; ++i) {
            if ((i & 1) == 0) {
                ledger.recordSuccess(key, i);
            } else {
                ledger.recordFailure(key, QStringLiteral("timeout"), QStringLiteral("slow"), i);
            }
        }
    });

    // Concurrent readers on the calling thread while the pool writes.
    for (int i = 0; i < kIterations * 4; ++i) {
        const QJsonObject snap = ledger.snapshot();
        // record_count == records.size(): it grows monotonically toward the distinct
        // tool count and must never exceed it. A torn/garbage read or a duplicate-key
        // bug under the race would break the [0, kToolCount] bound; `>= 0` (always true
        // for a count) could not.
        const int record_count = snap.value(QStringLiteral("record_count")).toInt(-1);
        QVERIFY(record_count >= 0 && record_count <= kToolCount);
        (void)ledger.check(QStringLiteral("tool_%1").arg(i % kToolCount));
        (void)ledger.size();
    }
    writers.waitForFinished();

    QCOMPARE(ledger.size(), kToolCount);
    for (int tool = 0; tool < kToolCount; ++tool) {
        const auto record = ledger.record(QStringLiteral("tool_%1").arg(tool));
        // Even i -> recordSuccess, odd i -> recordFailure over [0, kIterations): exactly half
        // each. Pinning only the sum would miss a success/failure swap (sum stays kIterations).
        QCOMPARE(record.success_count, kIterations / 2);
        QCOMPARE(record.failure_count, kIterations / 2);
    }
}

void AiToolHealthLedgerTests::snapshotOrdersRecordsByKey() {
    sak::ai::AiToolHealthLedger ledger;
    ledger.recordSuccess(QStringLiteral("zeta"), 1);
    ledger.recordSuccess(QStringLiteral("alpha"), 1);
    const QJsonArray records = ledger.snapshot().value(QStringLiteral("records")).toArray();
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0).toObject().value(QStringLiteral("key")).toString(),
             QStringLiteral("alpha"));
    QCOMPARE(records.at(1).toObject().value(QStringLiteral("key")).toString(),
             QStringLiteral("zeta"));
}

QTEST_GUILESS_MAIN(AiToolHealthLedgerTests)
#include "test_ai_tool_health_ledger.moc"
