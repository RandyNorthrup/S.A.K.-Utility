#include <QtTest/QtTest>

#include "sak/deployment_history.h"

#include <QTemporaryDir>

using namespace sak;

class DeploymentHistoryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appendsAndLoads();
    void exportsCsv();
};

void DeploymentHistoryTests::appendsAndLoads() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString historyPath = tempDir.path() + "/history.json";
    DeploymentHistoryManager manager(historyPath);

    DeploymentHistoryEntry entry;
    entry.deployment_id = "deploy-1";
    entry.started_at = QDateTime::currentDateTimeUtc();
    entry.completed_at = entry.started_at.addSecs(5);
    entry.total_jobs = 2;
    entry.completed_jobs = 2;
    entry.failed_jobs = 0;
    entry.status = "success";

    QVERIFY(manager.appendEntry(entry));

    auto entries = manager.loadEntries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries[0].deployment_id, entry.deployment_id);
}

void DeploymentHistoryTests::exportsCsv() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString historyPath = tempDir.path() + "/history.json";
    DeploymentHistoryManager manager(historyPath);

    DeploymentHistoryEntry entry;
    entry.deployment_id = "deploy-2";
    entry.started_at = QDateTime::currentDateTimeUtc();
    entry.completed_at = entry.started_at.addSecs(10);
    entry.total_jobs = 1;
    entry.completed_jobs = 1;
    entry.failed_jobs = 0;
    entry.status = "success";

    QVERIFY(manager.appendEntry(entry));

    const QString csvPath = tempDir.path() + "/history.csv";
    QVERIFY(manager.exportCsv(csvPath));

    QFile csv(csvPath);
    QVERIFY(csv.open(QIODevice::ReadOnly));
    const auto content = csv.readAll();
    QVERIFY(content.contains("deploy-2"));
}

QTEST_MAIN(DeploymentHistoryTests)
#include "test_deployment_history.moc"
