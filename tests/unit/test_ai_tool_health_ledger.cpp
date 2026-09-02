#include "sak/ai/ai_tool_health_ledger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
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
    void loadRejectsFileTruncatedDuringRead();
    void loadRejectsFileGrownDuringRead();
    void loadRejectsNonObjectRootExplicitly();
    void loadRejectsForgedDisableWithoutFailure();
    void loadRejectsDirectorySwapBeforeFileOpen();
    void savePinsDirectoryAgainstSwap();
    void saveRejectsPayloadBeyondLoadCeiling();
    void snapshotOrdersRecordsByKey();
};

namespace {

QByteArray emptyLedgerJson() {
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
                                     {QStringLiteral("record_count"), 0},
                                     {QStringLiteral("records"), QJsonArray{}}})
        .toJson(QJsonDocument::Compact);
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

int createJunction(const QString& junction, const QString& target) {
    const QString command =
        QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/cmd.exe"));
    return QProcess::execute(command,
                             {QStringLiteral("/d"),
                              QStringLiteral("/c"),
                              QStringLiteral("mklink"),
                              QStringLiteral("/J"),
                              QDir::toNativeSeparators(junction),
                              QDir::toNativeSeparators(target)});
}

}  // namespace

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
        // Even i -> recordSuccess, odd i -> recordFailure over [0, kIterations): exactly half
        // each. Pinning only the sum would miss a success/failure swap (sum stays kIterations).
        QCOMPARE(record.success_count, kIterations / 2);
        QCOMPARE(record.failure_count, kIterations / 2);
    }
}

void AiToolHealthLedgerTests::loadRejectsFileTruncatedDuringRead() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = QDir(temp.path()).filePath(QStringLiteral("health.json"));
    QVERIFY(writeBytes(path, emptyLedgerJson()));
    sak::ai::AiToolHealthLedger::setBeforeFileReadHookForTesting([path](const QString&) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(2));
    });
    const auto reset = qScopeGuard([] {
        sak::ai::AiToolHealthLedger::setBeforeFileReadHookForTesting(
            std::function<void(const QString&)>());
    });

    sak::ai::AiToolHealthLedger ledger;
    ledger.setPersistencePath(path);
    QString error;
    QVERIFY(!ledger.load(&error));
    QVERIFY2(error.contains(QStringLiteral("changed during read")), qPrintable(error));
}

void AiToolHealthLedgerTests::loadRejectsFileGrownDuringRead() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = QDir(temp.path()).filePath(QStringLiteral("health.json"));
    QVERIFY(writeBytes(path, emptyLedgerJson()));
    sak::ai::AiToolHealthLedger::setBeforeFileReadHookForTesting([path](const QString&) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        QCOMPARE(file.write(QByteArray((8 * 1024 * 1024) + 1, 'x')), Q_INT64_C(8'388'609));
    });
    const auto reset = qScopeGuard([] {
        sak::ai::AiToolHealthLedger::setBeforeFileReadHookForTesting(
            std::function<void(const QString&)>());
    });

    sak::ai::AiToolHealthLedger ledger;
    ledger.setPersistencePath(path);
    QString error;
    QVERIFY(!ledger.load(&error));
    QVERIFY2(error.contains(QStringLiteral("changed during read")), qPrintable(error));
}

void AiToolHealthLedgerTests::loadRejectsNonObjectRootExplicitly() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = QDir(temp.path()).filePath(QStringLiteral("health.json"));
    QVERIFY(writeBytes(path, QByteArrayLiteral("[]")));

    sak::ai::AiToolHealthLedger ledger;
    ledger.setPersistencePath(path);
    QString error;
    QVERIFY(!ledger.load(&error));
    QCOMPARE(error, QStringLiteral("Invalid AI tool health ledger JSON root: expected object"));
}

void AiToolHealthLedgerTests::loadRejectsForgedDisableWithoutFailure() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = QDir(temp.path()).filePath(QStringLiteral("health.json"));
    const QJsonObject forged{
        {QStringLiteral("key"), QStringLiteral("tool")},
        {QStringLiteral("disabled_until_utc"),
         QDateTime::currentDateTimeUtc().addSecs(1).toString(Qt::ISODateWithMs)},
        {QStringLiteral("failure_count"), 0},
        {QStringLiteral("consecutive_failures"), 0}};
    const QJsonObject root{{QStringLiteral("schema_version"), 1},
                           {QStringLiteral("record_count"), 1},
                           {QStringLiteral("records"), QJsonArray{forged}}};
    QVERIFY(writeBytes(path, QJsonDocument(root).toJson(QJsonDocument::Compact)));

    sak::ai::AiToolHealthLedger ledger;
    ledger.setPersistencePath(path);
    QString error;
    QVERIFY(!ledger.load(&error));
    QCOMPARE(error,
             QStringLiteral(
                 "Refusing AI tool health ledger: a record is malformed or inconsistent"));
    QCOMPARE(ledger.size(), 0);
}

void AiToolHealthLedgerTests::loadRejectsDirectorySwapBeforeFileOpen() {
    QTemporaryDir parent;
    QTemporaryDir outside;
    QVERIFY(parent.isValid());
    QVERIFY(outside.isValid());
    const QString directory = QDir(parent.path()).filePath(QStringLiteral("ledger"));
    const QString moved = QDir(parent.path()).filePath(QStringLiteral("ledger-moved"));
    QVERIFY(QDir().mkpath(directory));
    const QString path = QDir(directory).filePath(QStringLiteral("health.json"));
    QVERIFY(writeBytes(path, emptyLedgerJson()));
    QVERIFY(writeBytes(QDir(outside.path()).filePath(QStringLiteral("health.json")),
                       emptyLedgerJson()));
    sak::ai::AiToolHealthLedger::setBeforeFileOpenHookForTesting([&](const QString&) {
        QVERIFY(QDir().rename(directory, moved));
        QCOMPARE(createJunction(directory, outside.path()), 0);
    });
    const auto reset = qScopeGuard([&] {
        sak::ai::AiToolHealthLedger::setBeforeFileOpenHookForTesting(
            std::function<void(const QString&)>());
        if (QFileInfo(directory).isJunction()) {
            QVERIFY(QDir().rmdir(directory));
        }
    });

    sak::ai::AiToolHealthLedger ledger;
    ledger.setPersistencePath(path);
    QString error;
    QVERIFY(!ledger.load(&error));
    QVERIFY2(error.contains(QStringLiteral("redirected")), qPrintable(error));
}

void AiToolHealthLedgerTests::savePinsDirectoryAgainstSwap() {
    QTemporaryDir parent;
    QTemporaryDir outside;
    QVERIFY(parent.isValid());
    QVERIFY(outside.isValid());
    const QString directory = QDir(parent.path()).filePath(QStringLiteral("ledger"));
    const QString moved = QDir(parent.path()).filePath(QStringLiteral("ledger-moved"));
    QVERIFY(QDir().mkpath(directory));
    const QString path = QDir(directory).filePath(QStringLiteral("health.json"));
    bool swap_blocked = false;
    sak::ai::AiToolHealthLedger::setBeforeTempOpenHookForTesting([&](const QString&) {
        swap_blocked = !QDir().rename(directory, moved);
        if (!swap_blocked) {
            QCOMPARE(createJunction(directory, outside.path()), 0);
        }
    });
    const auto reset = qScopeGuard([&] {
        sak::ai::AiToolHealthLedger::setBeforeTempOpenHookForTesting(
            std::function<void(const QString&)>());
        if (QFileInfo(directory).isJunction()) {
            QVERIFY(QDir().rmdir(directory));
        }
    });

    sak::ai::AiToolHealthLedger ledger;
    ledger.setPersistencePath(path);
    QString error;
    QVERIFY2(ledger.save(&error), qPrintable(error));
    QVERIFY(swap_blocked);
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!QFileInfo::exists(QDir(outside.path()).filePath(QStringLiteral("health.json"))));
}

void AiToolHealthLedgerTests::saveRejectsPayloadBeyondLoadCeiling() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::AiToolHealthLedger ledger;
    const QString long_text(1000, QLatin1Char('x'));
    for (int index = 0; index < sak::ai::kAiToolHealthMaxRecords; ++index) {
        ledger.recordFailure(QStringLiteral("tool-%1").arg(index), long_text, long_text, 1);
    }
    QCOMPARE(ledger.size(), sak::ai::kAiToolHealthMaxRecords);
    ledger.setPersistencePath(QDir(temp.path()).filePath(QStringLiteral("health.json")));
    QString error;
    QVERIFY(!ledger.save(&error));
    QVERIFY2(error.contains(QStringLiteral("payload exceeds")), qPrintable(error));
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
