#include <QtTest/QtTest>

#include "sak/assignment_queue_store.h"

#include <QTemporaryDir>

using namespace sak;

class AssignmentQueueStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void savesAndLoads();
};

void AssignmentQueueStoreTests::savesAndLoads() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.path() + "/queue.json";
    AssignmentQueueStore store(path);

    DeploymentAssignment active;
    active.deployment_id = "deploy-1";
    active.job_id = "job-1";
    active.source_user = "user";
    active.profile_size_bytes = 123;
    active.priority = "high";

    QQueue<DeploymentAssignment> queue;
    DeploymentAssignment queued;
    queued.deployment_id = "deploy-2";
    queued.job_id = "job-2";
    queued.source_user = "user2";
    queued.profile_size_bytes = 456;
    queued.priority = "normal";
    queue.enqueue(queued);

    QMap<QString, QString> statusByJob;
    statusByJob.insert("job-1", "active");
    QMap<QString, QString> eventByJob;
    eventByJob.insert("job-1", "Received");

    QVERIFY(store.save(active, queue, statusByJob, eventByJob));

    DeploymentAssignment loadedActive;
    QQueue<DeploymentAssignment> loadedQueue;
    QMap<QString, QString> loadedStatus;
    QMap<QString, QString> loadedEvent;
    QVERIFY(store.load(loadedActive, loadedQueue, loadedStatus, loadedEvent));

    QCOMPARE(loadedActive.deployment_id, active.deployment_id);
    QCOMPARE(loadedActive.job_id, active.job_id);
    QCOMPARE(loadedQueue.size(), 1);
    QCOMPARE(loadedQueue.first().deployment_id, queued.deployment_id);
    QCOMPARE(loadedStatus.value("job-1"), QString("active"));
    QCOMPARE(loadedEvent.value("job-1"), QString("Received"));
}

QTEST_MAIN(AssignmentQueueStoreTests)
#include "test_assignment_queue_store.moc"
