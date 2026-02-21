#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTcpServer>
#include <QElapsedTimer>

#include "sak/network_transfer_controller.h"
#include "sak/network_transfer_types.h"
#include "sak/file_hash.h"

using namespace sak;

namespace {
quint16 pickFreePort() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = server.serverPort();
    server.close();
    return port;
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(data);
    return true;
}

QByteArray makeRepeatedData(const QByteArray& seed, int repeatCount) {
    QByteArray data;
    data.reserve(seed.size() * repeatCount);
    for (int i = 0; i < repeatCount; ++i) {
        data.append(seed);
    }
    return data;
}
}

class NetworkTransferWorkflowTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void transferEncryptedFiles();
    void transferMultipleFiles();
    void transferManySmallFiles();
    void resumeInterruptedTransfer();
    void throttledTransferRespectsLimit();
};

void NetworkTransferWorkflowTests::transferEncryptedFiles() {
    QTemporaryDir sourceDir;
    QTemporaryDir destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString relativePath = "TestUser/Documents/sample.txt";
    const QString sourcePath = QDir(sourceDir.path()).filePath(relativePath);

    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath("TestUser/Documents"));
    QVERIFY(writeFile(sourcePath, QByteArray("Hello Network Transfer")));

    file_hasher hasher(hash_algorithm::sha256);
    auto hashResult = hasher.calculateHash(std::filesystem::path(sourcePath.toStdString()));
    QVERIFY(hashResult.has_value());

    TransferFileEntry entry;
    entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.absolute_path = sourcePath;
    entry.relative_path = relativePath;
    entry.size_bytes = QFileInfo(sourcePath).size();
    entry.checksum_sha256 = QString::fromStdString(*hashResult);

    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = "TEST-SOURCE";
    manifest.source_os = "Windows";
    manifest.created = QDateTime::currentDateTimeUtc();
    manifest.files = {entry};
    manifest.total_files = 1;
    manifest.total_bytes = entry.size_bytes;

    BackupUserData user;
    user.username = "TestUser";
    user.permissions_mode = PermissionMode::StripAll;
    manifest.users.append(user);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings;
    settings.encryption_enabled = true;
    settings.compression_enabled = false;
    settings.resume_enabled = false;
    settings.auto_discovery_enabled = false;
    settings.chunk_size = 1024;
    settings.control_port = controlPort;
    settings.data_port = dataPort;

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);

    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);

    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    TransferPeerInfo peer;
    peer.ip_address = "127.0.0.1";
    peer.control_port = controlPort;
    peer.data_port = dataPort;
    peer.mode = "destination";

    source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(manifestSpy.wait(15000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1, "Destination transfer should complete", 30000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy.count() >= 1, "Source transfer should complete", 30000);

    const auto destArgs = destCompletedSpy.takeFirst();
    QVERIFY(destArgs.at(0).toBool());

    const auto sourceArgs = sourceCompletedSpy.takeFirst();
    QVERIFY(sourceArgs.at(0).toBool());

    const QString destPath = QDir(destDir.path()).filePath(relativePath);
    QFile destFile(destPath);
    QVERIFY(destFile.exists());
    QVERIFY(destFile.open(QIODevice::ReadOnly));
    QCOMPARE(destFile.readAll(), QByteArray("Hello Network Transfer"));

    source.stop();
    destination.stop();
}

void NetworkTransferWorkflowTests::transferMultipleFiles() {
    QTemporaryDir sourceDir;
    QTemporaryDir destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString userRoot = "TestUser/Documents";
    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath(userRoot));

    QStringList relativePaths;
    for (int i = 0; i < 25; ++i) {
        relativePaths.append(QString("%1/file_%2.txt").arg(userRoot).arg(i, 3, 10, QLatin1Char('0')));
    }

    QVector<TransferFileEntry> entries;
    entries.reserve(relativePaths.size());

    file_hasher hasher(hash_algorithm::sha256);
    qint64 totalBytes = 0;

    for (const auto& rel : relativePaths) {
        const QString sourcePath = QDir(sourceDir.path()).filePath(rel);
        QVERIFY(writeFile(sourcePath, QByteArray("data:") + rel.toUtf8()));

        auto hashResult = hasher.calculateHash(std::filesystem::path(sourcePath.toStdString()));
        QVERIFY(hashResult.has_value());

        TransferFileEntry entry;
        entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.absolute_path = sourcePath;
        entry.relative_path = rel;
        entry.size_bytes = QFileInfo(sourcePath).size();
        entry.checksum_sha256 = QString::fromStdString(*hashResult);
        totalBytes += entry.size_bytes;
        entries.append(entry);
    }

    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = "TEST-SOURCE";
    manifest.source_os = "Windows";
    manifest.created = QDateTime::currentDateTimeUtc();
    manifest.files = entries;
    manifest.total_files = entries.size();
    manifest.total_bytes = totalBytes;

    BackupUserData user;
    user.username = "TestUser";
    user.permissions_mode = PermissionMode::StripAll;
    manifest.users.append(user);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings;
    settings.encryption_enabled = true;
    settings.compression_enabled = false;
    settings.resume_enabled = false;
    settings.auto_discovery_enabled = false;
    settings.chunk_size = 1024;
    settings.control_port = controlPort;
    settings.data_port = dataPort;

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);

    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);

    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    TransferPeerInfo peer;
    peer.ip_address = "127.0.0.1";
    peer.control_port = controlPort;
    peer.data_port = dataPort;
    peer.mode = "destination";

    source.startSource(manifest, entries, peer, passphrase);

    QVERIFY(manifestSpy.wait(15000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1, "Destination transfer should complete", 60000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy.count() >= 1, "Source transfer should complete", 60000);

    const auto destArgs = destCompletedSpy.takeFirst();
    QVERIFY(destArgs.at(0).toBool());

    const auto sourceArgs = sourceCompletedSpy.takeFirst();
    QVERIFY(sourceArgs.at(0).toBool());

    for (const auto& rel : relativePaths) {
        const QString destPath = QDir(destDir.path()).filePath(rel);
        QFile destFile(destPath);
        QVERIFY(destFile.exists());
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        QCOMPARE(destFile.readAll(), QByteArray("data:") + rel.toUtf8());
    }

    source.stop();
    destination.stop();
}

void NetworkTransferWorkflowTests::transferManySmallFiles() {
    QTemporaryDir sourceDir;
    QTemporaryDir destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString userRoot = "TestUser/Documents";
    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath(userRoot));

    QStringList relativePaths;
    for (int i = 0; i < 200; ++i) {
        relativePaths.append(QString("%1/small_%2.txt").arg(userRoot).arg(i, 4, 10, QLatin1Char('0')));
    }

    QVector<TransferFileEntry> entries;
    entries.reserve(relativePaths.size());

    file_hasher hasher(hash_algorithm::sha256);
    qint64 totalBytes = 0;

    for (const auto& rel : relativePaths) {
        const QString sourcePath = QDir(sourceDir.path()).filePath(rel);
        QVERIFY(writeFile(sourcePath, QByteArray("s") + rel.toUtf8()));

        auto hashResult = hasher.calculateHash(std::filesystem::path(sourcePath.toStdString()));
        QVERIFY(hashResult.has_value());

        TransferFileEntry entry;
        entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.absolute_path = sourcePath;
        entry.relative_path = rel;
        entry.size_bytes = QFileInfo(sourcePath).size();
        entry.checksum_sha256 = QString::fromStdString(*hashResult);
        totalBytes += entry.size_bytes;
        entries.append(entry);
    }

    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = "TEST-SOURCE";
    manifest.source_os = "Windows";
    manifest.created = QDateTime::currentDateTimeUtc();
    manifest.files = entries;
    manifest.total_files = entries.size();
    manifest.total_bytes = totalBytes;

    BackupUserData user;
    user.username = "TestUser";
    user.permissions_mode = PermissionMode::StripAll;
    manifest.users.append(user);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings;
    settings.encryption_enabled = true;
    settings.compression_enabled = false;
    settings.resume_enabled = false;
    settings.auto_discovery_enabled = false;
    settings.chunk_size = 1024;
    settings.control_port = controlPort;
    settings.data_port = dataPort;

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);
    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);
    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    TransferPeerInfo peer;
    peer.ip_address = "127.0.0.1";
    peer.control_port = controlPort;
    peer.data_port = dataPort;
    peer.mode = "destination";

    source.startSource(manifest, entries, peer, passphrase);

    QVERIFY(manifestSpy.wait(15000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1, "Destination transfer should complete", 60000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy.count() >= 1, "Source transfer should complete", 60000);

    const auto destArgs = destCompletedSpy.takeFirst();
    QVERIFY(destArgs.at(0).toBool());

    const auto sourceArgs = sourceCompletedSpy.takeFirst();
    QVERIFY(sourceArgs.at(0).toBool());

    for (const auto& rel : relativePaths) {
        const QString destPath = QDir(destDir.path()).filePath(rel);
        QFile destFile(destPath);
        QVERIFY(destFile.exists());
    }

    source.stop();
    destination.stop();
}

void NetworkTransferWorkflowTests::resumeInterruptedTransfer() {
    QTemporaryDir sourceDir;
    QTemporaryDir destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString relativePath = "TestUser/Documents/large.bin";
    const QString sourcePath = QDir(sourceDir.path()).filePath(relativePath);

    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath("TestUser/Documents"));

    const QByteArray payload = makeRepeatedData("SAK", 200000); // ~600 KB
    QVERIFY(writeFile(sourcePath, payload));

    file_hasher hasher(hash_algorithm::sha256);
    auto hashResult = hasher.calculateHash(std::filesystem::path(sourcePath.toStdString()));
    QVERIFY(hashResult.has_value());

    TransferFileEntry entry;
    entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.absolute_path = sourcePath;
    entry.relative_path = relativePath;
    entry.size_bytes = QFileInfo(sourcePath).size();
    entry.checksum_sha256 = QString::fromStdString(*hashResult);

    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = "TEST-SOURCE";
    manifest.source_os = "Windows";
    manifest.created = QDateTime::currentDateTimeUtc();
    manifest.files = {entry};
    manifest.total_files = 1;
    manifest.total_bytes = entry.size_bytes;

    BackupUserData user;
    user.username = "TestUser";
    user.permissions_mode = PermissionMode::StripAll;
    manifest.users.append(user);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings;
    settings.encryption_enabled = true;
    settings.compression_enabled = false;
    settings.resume_enabled = true;
    settings.auto_discovery_enabled = false;
    settings.chunk_size = 1024;
    settings.max_bandwidth_kbps = 32; // Throttle to 32 KB/s so transfer can be interrupted
    settings.control_port = controlPort;
    settings.data_port = dataPort;

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);
    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);
    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    QSignalSpy progressSpy(&destination, &NetworkTransferController::transferProgress);

    TransferPeerInfo peer;
    peer.ip_address = "127.0.0.1";
    peer.control_port = controlPort;
    peer.data_port = dataPort;
    peer.mode = "destination";

    source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(manifestSpy.wait(15000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(progressSpy.count() >= 1, "Should receive progress update", 15000);
    QTest::qWait(3000); // Wait enough for resume timer (2s interval) to save state
    source.stop();
    destination.stop();

    const QString partialPath = QDir(destDir.path()).filePath(relativePath + ".partial");
    const QString resumePath = QDir(destDir.path()).filePath(relativePath + ".resume.json");
    QVERIFY(QFileInfo::exists(partialPath));
    QVERIFY(QFileInfo::exists(resumePath));

    NetworkTransferController destinationResume;
    settings.max_bandwidth_kbps = 0; // No throttle for resume transfer
    destinationResume.configure(settings);
    QSignalSpy manifestSpy2(&destinationResume, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy2(&destinationResume, &NetworkTransferController::transferCompleted);
    destinationResume.startDestination(passphrase, destDir.path());

    NetworkTransferController sourceResume;
    sourceResume.configure(settings);
    QSignalSpy sourceCompletedSpy2(&sourceResume, &NetworkTransferController::transferCompleted);

    sourceResume.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(manifestSpy2.wait(15000));
    destinationResume.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy2.count() >= 1, "Destination resume transfer should complete", 60000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy2.count() >= 1, "Source resume transfer should complete", 60000);

    const auto destArgs = destCompletedSpy2.takeFirst();
    QVERIFY(destArgs.at(0).toBool());

    const auto sourceArgs = sourceCompletedSpy2.takeFirst();
    QVERIFY(sourceArgs.at(0).toBool());

    const QString destPath = QDir(destDir.path()).filePath(relativePath);
    QFile destFile(destPath);
    QVERIFY(destFile.exists());
    QVERIFY(destFile.open(QIODevice::ReadOnly));
    QCOMPARE(destFile.readAll(), payload);

    sourceResume.stop();
    destinationResume.stop();
}

void NetworkTransferWorkflowTests::throttledTransferRespectsLimit() {
    QTemporaryDir sourceDir;
    QTemporaryDir destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString relativePath = "TestUser/Documents/throttle.bin";
    const QString sourcePath = QDir(sourceDir.path()).filePath(relativePath);

    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath("TestUser/Documents"));

    const QByteArray payload = makeRepeatedData("THROTTLE", 20000); // ~160 KB
    QVERIFY(writeFile(sourcePath, payload));

    file_hasher hasher(hash_algorithm::sha256);
    auto hashResult = hasher.calculateHash(std::filesystem::path(sourcePath.toStdString()));
    QVERIFY(hashResult.has_value());

    TransferFileEntry entry;
    entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.absolute_path = sourcePath;
    entry.relative_path = relativePath;
    entry.size_bytes = QFileInfo(sourcePath).size();
    entry.checksum_sha256 = QString::fromStdString(*hashResult);

    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = "TEST-SOURCE";
    manifest.source_os = "Windows";
    manifest.created = QDateTime::currentDateTimeUtc();
    manifest.files = {entry};
    manifest.total_files = 1;
    manifest.total_bytes = entry.size_bytes;

    BackupUserData user;
    user.username = "TestUser";
    user.permissions_mode = PermissionMode::StripAll;
    manifest.users.append(user);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings;
    settings.encryption_enabled = true;
    settings.compression_enabled = false;
    settings.resume_enabled = false;
    settings.auto_discovery_enabled = false;
    settings.chunk_size = 1024;
    settings.max_bandwidth_kbps = 32; // 32 KB/s
    settings.control_port = controlPort;
    settings.data_port = dataPort;

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);
    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);
    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    TransferPeerInfo peer;
    peer.ip_address = "127.0.0.1";
    peer.control_port = controlPort;
    peer.data_port = dataPort;
    peer.mode = "destination";

    QElapsedTimer timer;
    timer.start();

    source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(manifestSpy.wait(15000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1, "Destination throttled transfer should complete", 60000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy.count() >= 1, "Source throttled transfer should complete", 60000);

    const auto destArgs = destCompletedSpy.takeFirst();
    QVERIFY(destArgs.at(0).toBool());

    const auto sourceArgs = sourceCompletedSpy.takeFirst();
    QVERIFY(sourceArgs.at(0).toBool());

    QVERIFY(timer.elapsed() >= 1000);

    source.stop();
    destination.stop();
}

QTEST_MAIN(NetworkTransferWorkflowTests)
#include "test_network_transfer_workflow.moc"
