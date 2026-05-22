#include "sak/ai/ai_tool_health_ledger.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class AiToolHealthLedgerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void recordSuccessClearsBackoff();
    void repeatedFailuresSuppressTemporarily();
    void persistsFreshRecordsAndPrunesExpired();
    void classifyResultUsesSpecificFailureFields();
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

QTEST_GUILESS_MAIN(AiToolHealthLedgerTests)
#include "test_ai_tool_health_ledger.moc"
