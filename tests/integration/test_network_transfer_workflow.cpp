// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_hash.h"
#include "sak/network_transfer_controller.h"
#include "sak/network_transfer_types.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <optional>

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

TransferPeerInfo makeLocalDestinationPeer(quint16 controlPort, quint16 dataPort) {
    TransferPeerInfo peer;
    peer.ip_address = "127.0.0.1";
    peer.control_port = controlPort;
    peer.data_port = dataPort;
    peer.mode = "destination";
    return peer;
}

TransferSettings makeBasicSettings(quint16 controlPort,
                                   quint16 dataPort,
                                   bool resumeEnabled,
                                   int maxBandwidthKbps = 0) {
    TransferSettings settings;
    settings.encryption_enabled = true;
    settings.compression_enabled = false;
    settings.resume_enabled = resumeEnabled;
    settings.auto_discovery_enabled = false;
    settings.chunk_size = 1024;
    settings.max_bandwidth_kbps = maxBandwidthKbps;
    settings.control_port = controlPort;
    settings.data_port = dataPort;
    return settings;
}

BackupUserData makeTestUserData() {
    BackupUserData user;
    user.username = "TestUser";
    user.permissions_mode = PermissionMode::StripAll;
    return user;
}

std::optional<QString> computeSha256(const QString& absolutePath) {
    file_hasher hasher(hash_algorithm::sha256);
    auto hashResult = hasher.calculateHash(std::filesystem::path(absolutePath.toStdString()));
    if (!hashResult.has_value()) {
        return std::nullopt;
    }
    return QString::fromStdString(*hashResult);
}

std::optional<TransferFileEntry> makeEntryForFile(const QString& absolutePath,
                                                  const QString& relativePath) {
    const auto checksum = computeSha256(absolutePath);
    if (!checksum.has_value()) {
        return std::nullopt;
    }

    TransferFileEntry entry;
    entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.absolute_path = absolutePath;
    entry.relative_path = relativePath;
    entry.size_bytes = QFileInfo(absolutePath).size();
    entry.checksum_sha256 = *checksum;
    return entry;
}

TransferManifest makeTestManifest(const QVector<TransferFileEntry>& entries, qint64 totalBytes) {
    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = "TEST-SOURCE";
    manifest.source_os = "Windows";
    manifest.created = QDateTime::currentDateTimeUtc();
    manifest.files = entries;
    manifest.total_files = entries.size();
    manifest.total_bytes = totalBytes;
    manifest.users.append(makeTestUserData());
    return manifest;
}

struct GeneratedEntries {
    QStringList relativePaths;
    QVector<TransferFileEntry> entries;
    qint64 totalBytes = 0;
};

std::optional<GeneratedEntries> generateTextEntries(const QString& sourceDirPath,
                                                    const QString& userRoot,
                                                    const QString& fileStem,
                                                    int count,
                                                    int width,
                                                    const QByteArray& dataPrefix) {
    GeneratedEntries out;
    QDir sourceRoot(sourceDirPath);
    if (!sourceRoot.mkpath(userRoot)) {
        return std::nullopt;
    }

    out.relativePaths.reserve(count);
    out.entries.reserve(count);

    for (int i = 0; i < count; ++i) {
        const QString rel =
            QString("%1/%2_%3.txt").arg(userRoot).arg(fileStem).arg(i, width, 10, QLatin1Char('0'));
        out.relativePaths.append(rel);

        const QString sourcePath = QDir(sourceDirPath).filePath(rel);
        if (!writeFile(sourcePath, dataPrefix + rel.toUtf8())) {
            return std::nullopt;
        }

        const auto entry = makeEntryForFile(sourcePath, rel);
        if (!entry.has_value()) {
            return std::nullopt;
        }

        out.totalBytes += entry->size_bytes;
        out.entries.append(*entry);
    }

    return out;
}

struct TransferEndpoints {
    NetworkTransferController destination;
    NetworkTransferController source;
    QSignalSpy manifestSpy;
    QSignalSpy destCompletedSpy;
    QSignalSpy sourceCompletedSpy;

    explicit TransferEndpoints(const TransferSettings& settings)
        : manifestSpy(&destination, &NetworkTransferController::manifestReceived)
        , destCompletedSpy(&destination, &NetworkTransferController::transferCompleted)
        , sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted) {
        destination.configure(settings);
        source.configure(settings);
    }
};
}  // namespace

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

    const auto entryOpt = makeEntryForFile(sourcePath, relativePath);
    QVERIFY(entryOpt.has_value());
    const TransferFileEntry entry = *entryOpt;
    const TransferManifest manifest = makeTestManifest({entry}, entry.size_bytes);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    const TransferSettings settings = makeBasicSettings(controlPort, dataPort, false);

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);

    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);

    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);

    source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(manifestSpy.wait(15'000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1,
                              "Destination transfer should complete",
                              30'000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy.count() >= 1,
                              "Source transfer should complete",
                              30'000);

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
    const auto generated =
        generateTextEntries(sourceDir.path(), userRoot, "file", 25, 3, QByteArray("data:"));
    QVERIFY(generated.has_value());

    const QStringList relativePaths = generated->relativePaths;
    const QVector<TransferFileEntry> entries = generated->entries;
    const TransferManifest manifest = makeTestManifest(entries, generated->totalBytes);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    const TransferSettings settings = makeBasicSettings(controlPort, dataPort, false);

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);

    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);

    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);

    source.startSource(manifest, entries, peer, passphrase);

    QVERIFY(manifestSpy.wait(15'000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1,
                              "Destination transfer should complete",
                              60'000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy.count() >= 1,
                              "Source transfer should complete",
                              60'000);

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
    const auto generated =
        generateTextEntries(sourceDir.path(), userRoot, "small", 200, 4, QByteArray("s"));
    QVERIFY(generated.has_value());

    const QStringList relativePaths = generated->relativePaths;
    const QVector<TransferFileEntry> entries = generated->entries;
    const TransferManifest manifest = makeTestManifest(entries, generated->totalBytes);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    const TransferSettings settings = makeBasicSettings(controlPort, dataPort, false);

    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);
    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    NetworkTransferController source;
    source.configure(settings);
    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);

    source.startSource(manifest, entries, peer, passphrase);

    QVERIFY(manifestSpy.wait(15'000));
    destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1,
                              "Destination transfer should complete",
                              60'000);
    QTRY_VERIFY2_WITH_TIMEOUT(sourceCompletedSpy.count() >= 1,
                              "Source transfer should complete",
                              60'000);

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
    QTemporaryDir sourceDir, destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString relativePath = "TestUser/Documents/large.bin";
    const QString sourcePath = QDir(sourceDir.path()).filePath(relativePath);

    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath("TestUser/Documents"));

    const QByteArray payload = makeRepeatedData("SAK", 200'000);  // ~600 KB
    QVERIFY(writeFile(sourcePath, payload));

    const auto entryOpt = makeEntryForFile(sourcePath, relativePath);
    QVERIFY(entryOpt.has_value());
    const TransferFileEntry entry = *entryOpt;
    const TransferManifest manifest = makeTestManifest({entry}, entry.size_bytes);

    const quint16 controlPort = pickFreePort(), dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings = makeBasicSettings(controlPort, dataPort, true, 32);

    const QString passphrase = "test-passphrase";
    TransferEndpoints endpoints(settings);
    endpoints.destination.startDestination(passphrase, destDir.path());
    QSignalSpy progressSpy(&endpoints.destination, &NetworkTransferController::transferProgress);

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);
    endpoints.source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(endpoints.manifestSpy.wait(15'000));
    endpoints.destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(progressSpy.count() >= 1, "Should receive progress update", 15'000);
    QTest::qWait(3000);  // Wait enough for resume timer (2s interval) to save state
    endpoints.source.stop();
    endpoints.destination.stop();

    const QString partialPath = QDir(destDir.path()).filePath(relativePath + ".partial");
    const QString resumePath = QDir(destDir.path()).filePath(relativePath + ".resume.json");
    QVERIFY(QFileInfo::exists(partialPath));
    QVERIFY(QFileInfo::exists(resumePath));

    settings.max_bandwidth_kbps = 0;  // No throttle for resume transfer
    TransferEndpoints resumeEndpoints(settings);
    resumeEndpoints.destination.startDestination(passphrase, destDir.path());
    resumeEndpoints.source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(resumeEndpoints.manifestSpy.wait(15'000));
    resumeEndpoints.destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(resumeEndpoints.destCompletedSpy.count() >= 1,
                              "Destination resume transfer should complete",
                              60'000);
    QTRY_VERIFY2_WITH_TIMEOUT(resumeEndpoints.sourceCompletedSpy.count() >= 1,
                              "Source resume transfer should complete",
                              60'000);

    const auto destArgs = resumeEndpoints.destCompletedSpy.takeFirst();
    QVERIFY2(destArgs.at(0).toBool(), "Destination must report success after resume");

    // Destination file integrity is the authoritative resume success signal.

    const QString destPath = QDir(destDir.path()).filePath(relativePath);
    QFile destFile(destPath);
    QVERIFY(destFile.exists());
    QVERIFY(destFile.open(QIODevice::ReadOnly));
    QCOMPARE(destFile.readAll(), payload);

    resumeEndpoints.source.stop();
    resumeEndpoints.destination.stop();
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

    const QByteArray payload = makeRepeatedData("THROTTLE", 20'000);  // ~160 KB
    QVERIFY(writeFile(sourcePath, payload));

    const auto entryOpt = makeEntryForFile(sourcePath, relativePath);
    QVERIFY(entryOpt.has_value());
    const TransferFileEntry entry = *entryOpt;
    const TransferManifest manifest = makeTestManifest({entry}, entry.size_bytes);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    const TransferSettings settings = makeBasicSettings(controlPort, dataPort, false, 32);

    const QString passphrase = "test-passphrase";

    TransferEndpoints endpoints(settings);
    endpoints.destination.startDestination(passphrase, destDir.path());

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);

    QElapsedTimer timer;
    timer.start();

    endpoints.source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(endpoints.manifestSpy.wait(15'000));
    endpoints.destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(endpoints.destCompletedSpy.count() >= 1,
                              "Destination throttled transfer should complete",
                              60'000);
    QTRY_VERIFY2_WITH_TIMEOUT(endpoints.sourceCompletedSpy.count() >= 1,
                              "Source throttled transfer should complete",
                              60'000);

    const auto destArgs = endpoints.destCompletedSpy.takeFirst();
    QVERIFY(destArgs.at(0).toBool());

    const auto sourceArgs = endpoints.sourceCompletedSpy.takeFirst();
    QVERIFY(sourceArgs.at(0).toBool());

    QVERIFY(timer.elapsed() >= 1000);

    endpoints.source.stop();
    endpoints.destination.stop();
}

QTEST_MAIN(NetworkTransferWorkflowTests)
#include "test_network_transfer_workflow.moc"
