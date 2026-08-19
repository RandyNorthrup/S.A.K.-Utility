#include "sak/ai/ai_tool_health_ledger.h"

#include <QDir>
#include <QFileInfo>
#include <QFuture>
#include <QtConcurrent>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class AiToolHealthLedgerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void recordSuccessClearsBackoff();
    void repeatedFailuresSuppressTemporarily();
    void persistsFreshRecordsAndPrunesExpired();
    void classifyResultUsesSpecificFailureFields();
    void concurrentRecordAndReadIsThreadSafe();
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
    QVERIFY(availability.reason.contains(QStringLiteral("provider")));
}

void AiToolHealthLedgerTests::persistsFreshRecordsAndPrunesExpired() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = QDir(temp.path()).filePath(QStringLiteral("tool_health.json"));
    const QDateTime now = QDateTime::currentDateTimeUtc();

    sak::ai::AiToolHealthLedger ledger(2, 1000, 10'000);
    ledger.setPersistencePath(path, 24);
    ledger.recordFailure(
        QStringLiteral("fresh"), QStringLiteral("timeout"), QStringLiteral("slow"), 7, now);
    QVERIFY(QFileInfo::exists(path));

    sak::ai::AiToolHealthLedger loaded(2, 1000, 10'000);
    loaded.setPersistencePath(path, 24);
    QString error;
    QVERIFY2(loaded.load(&error), qPrintable(error));
    QCOMPARE(loaded.record(QStringLiteral("fresh")).last_failure_class, QStringLiteral("timeout"));

    loaded.pruneExpired(now.addSecs(25 * 3600));
    QCOMPARE(loaded.size(), 0);
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
        QCOMPARE(record.success_count + record.failure_count, kIterations);
    }
}

QTEST_GUILESS_MAIN(AiToolHealthLedgerTests)
#include "test_ai_tool_health_ledger.moc"
