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

TransferSettings makeSettings(quint16 controlPort,
                              quint16 dataPort,
                              bool encryptionEnabled,
                              bool compressionEnabled,
                              bool resumeEnabled) {
    TransferSettings settings;
    settings.encryption_enabled = encryptionEnabled;
    settings.compression_enabled = compressionEnabled;
    settings.resume_enabled = resumeEnabled;
    settings.auto_discovery_enabled = false;
    settings.chunk_size = 1024;
    settings.max_bandwidth_kbps = 0;
    settings.control_port = controlPort;
    settings.data_port = dataPort;
    return settings;
}

TransferSettings makeBasicSettings(quint16 controlPort,
                                   quint16 dataPort,
                                   bool resumeEnabled,
                                   int maxBandwidthKbps = 0) {
    auto settings = makeSettings(controlPort, dataPort, true, false, resumeEnabled);
    settings.max_bandwidth_kbps = maxBandwidthKbps;
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
                                                    const QByteArray& dataPrefix) {
    const int width = QString::number(count).length() + 1;
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

private:
    void runSingleFileTransfer(bool encryption, bool compression);
    void runMultiFileTransfer(bool encryption, bool compression);

private Q_SLOTS:
    void transferEncryptedFiles();
    void transferMultipleFiles();
    void transferManySmallFiles();
    void resumeInterruptedTransfer();
    void throttledTransferRespectsLimit();

    // All four encryption/compression configurations — single file
    void transferEncryptedCompressed();
    void transferUnencryptedCompressed();
    void transferUnencryptedUncompressed();

    // All four encryption/compression configurations — multiple files
    void multiEncryptedCompressed();
    void multiEncryptedUncompressed();
    void multiUnencryptedCompressed();
    void multiUnencryptedUncompressed();

    // Resume with compression enabled
    void resumeWithCompression();

    // Receiver detects sender disconnect
    void receiverDetectsSenderDisconnect();
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
        generateTextEntries(sourceDir.path(), userRoot, "file", 25, QByteArray("data:"));
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
        generateTextEntries(sourceDir.path(), userRoot, "small", 200, QByteArray("s"));
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

// ======================================================================
// Parameterized helpers for configuration coverage
// ======================================================================

void NetworkTransferWorkflowTests::runSingleFileTransfer(bool encryption, bool compression) {
    QTemporaryDir sourceDir;
    QTemporaryDir destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString relativePath = "TestUser/Documents/sample.txt";
    const QString sourcePath = QDir(sourceDir.path()).filePath(relativePath);

    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath("TestUser/Documents"));
    QVERIFY(writeFile(sourcePath, QByteArray("Hello Network Transfer Config")));

    const auto entryOpt = makeEntryForFile(sourcePath, relativePath);
    QVERIFY(entryOpt.has_value());
    const TransferFileEntry entry = *entryOpt;
    const TransferManifest manifest = makeTestManifest({entry}, entry.size_bytes);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    const TransferSettings settings =
        makeSettings(controlPort, dataPort, encryption, compression, false);
    const QString passphrase = "test-passphrase";

    TransferEndpoints endpoints(settings);
    endpoints.destination.startDestination(passphrase, destDir.path());

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);
    endpoints.source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(endpoints.manifestSpy.wait(15'000));
    endpoints.destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(endpoints.destCompletedSpy.count() >= 1,
                              "Destination transfer should complete",
                              30'000);
    QTRY_VERIFY2_WITH_TIMEOUT(endpoints.sourceCompletedSpy.count() >= 1,
                              "Source transfer should complete",
                              30'000);

    const auto destArgs = endpoints.destCompletedSpy.takeFirst();
    QVERIFY2(destArgs.at(0).toBool(),
             qPrintable(QString("Destination failed: %1").arg(destArgs.at(1).toString())));

    const auto sourceArgs = endpoints.sourceCompletedSpy.takeFirst();
    QVERIFY2(sourceArgs.at(0).toBool(),
             qPrintable(QString("Source failed: %1").arg(sourceArgs.at(1).toString())));

    const QString destPath = QDir(destDir.path()).filePath(relativePath);
    QFile destFile(destPath);
    QVERIFY2(destFile.exists(), "Destination file must exist after transfer");
    QVERIFY(destFile.open(QIODevice::ReadOnly));
    QCOMPARE(destFile.readAll(), QByteArray("Hello Network Transfer Config"));

    endpoints.source.stop();
    endpoints.destination.stop();
}

void NetworkTransferWorkflowTests::runMultiFileTransfer(bool encryption, bool compression) {
    QTemporaryDir sourceDir;
    QTemporaryDir destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString userRoot = "TestUser/Documents";
    const auto generated =
        generateTextEntries(sourceDir.path(), userRoot, "cfg", 10, QByteArray("payload:"));
    QVERIFY(generated.has_value());

    const QStringList relativePaths = generated->relativePaths;
    const QVector<TransferFileEntry> entries = generated->entries;
    const TransferManifest manifest = makeTestManifest(entries, generated->totalBytes);

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    const TransferSettings settings =
        makeSettings(controlPort, dataPort, encryption, compression, false);
    const QString passphrase = "test-passphrase";

    TransferEndpoints endpoints(settings);
    endpoints.destination.startDestination(passphrase, destDir.path());

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);
    endpoints.source.startSource(manifest, entries, peer, passphrase);

    QVERIFY(endpoints.manifestSpy.wait(15'000));
    endpoints.destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(endpoints.destCompletedSpy.count() >= 1,
                              "Destination transfer should complete",
                              60'000);
    QTRY_VERIFY2_WITH_TIMEOUT(endpoints.sourceCompletedSpy.count() >= 1,
                              "Source transfer should complete",
                              60'000);

    const auto destArgs = endpoints.destCompletedSpy.takeFirst();
    QVERIFY2(destArgs.at(0).toBool(),
             qPrintable(QString("Destination failed: %1").arg(destArgs.at(1).toString())));

    const auto sourceArgs = endpoints.sourceCompletedSpy.takeFirst();
    QVERIFY2(sourceArgs.at(0).toBool(),
             qPrintable(QString("Source failed: %1").arg(sourceArgs.at(1).toString())));

    for (const auto& rel : relativePaths) {
        const QString destPath = QDir(destDir.path()).filePath(rel);
        QFile destFile(destPath);
        QVERIFY2(destFile.exists(), qPrintable(QString("Missing: %1").arg(rel)));
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        QCOMPARE(destFile.readAll(), QByteArray("payload:") + rel.toUtf8());
    }

    endpoints.source.stop();
    endpoints.destination.stop();
}

// ======================================================================
// Single-file config matrix
// ======================================================================

void NetworkTransferWorkflowTests::transferEncryptedCompressed() {
    runSingleFileTransfer(true, true);
}

void NetworkTransferWorkflowTests::transferUnencryptedCompressed() {
    runSingleFileTransfer(false, true);
}

void NetworkTransferWorkflowTests::transferUnencryptedUncompressed() {
    runSingleFileTransfer(false, false);
}

// ======================================================================
// Multi-file config matrix
// ======================================================================

void NetworkTransferWorkflowTests::multiEncryptedCompressed() {
    runMultiFileTransfer(true, true);
}

void NetworkTransferWorkflowTests::multiEncryptedUncompressed() {
    runMultiFileTransfer(true, false);
}

void NetworkTransferWorkflowTests::multiUnencryptedCompressed() {
    runMultiFileTransfer(false, true);
}

void NetworkTransferWorkflowTests::multiUnencryptedUncompressed() {
    runMultiFileTransfer(false, false);
}

// ======================================================================
// Resume with compression
// ======================================================================

void NetworkTransferWorkflowTests::resumeWithCompression() {
    QTemporaryDir sourceDir, destDir;
    QVERIFY(sourceDir.isValid());
    QVERIFY(destDir.isValid());

    const QString relativePath = "TestUser/Documents/large_compressed.bin";
    const QString sourcePath = QDir(sourceDir.path()).filePath(relativePath);

    QDir sourceRoot(sourceDir.path());
    QVERIFY(sourceRoot.mkpath("TestUser/Documents"));

    const QByteArray payload = makeRepeatedData("CMP", 200'000);
    QVERIFY(writeFile(sourcePath, payload));

    const auto entryOpt = makeEntryForFile(sourcePath, relativePath);
    QVERIFY(entryOpt.has_value());
    const TransferFileEntry entry = *entryOpt;
    const TransferManifest manifest = makeTestManifest({entry}, entry.size_bytes);

    const quint16 controlPort = pickFreePort(), dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings = makeSettings(controlPort, dataPort, true, true, true);
    settings.max_bandwidth_kbps = 32;
    const QString passphrase = "test-passphrase";

    TransferEndpoints endpoints(settings);
    endpoints.destination.startDestination(passphrase, destDir.path());
    QSignalSpy progressSpy(&endpoints.destination, &NetworkTransferController::transferProgress);

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);
    endpoints.source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(endpoints.manifestSpy.wait(15'000));
    endpoints.destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(progressSpy.count() >= 1, "Should receive progress update", 15'000);
    QTest::qWait(3000);
    endpoints.source.stop();
    endpoints.destination.stop();

    const QString partialPath = QDir(destDir.path()).filePath(relativePath + ".partial");
    const QString resumePath = QDir(destDir.path()).filePath(relativePath + ".resume.json");
    QVERIFY(QFileInfo::exists(partialPath));
    QVERIFY(QFileInfo::exists(resumePath));

    settings.max_bandwidth_kbps = 0;
    TransferEndpoints resumeEndpoints(settings);
    resumeEndpoints.destination.startDestination(passphrase, destDir.path());
    resumeEndpoints.source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(resumeEndpoints.manifestSpy.wait(15'000));
    resumeEndpoints.destination.approveTransfer(true);

    QTRY_VERIFY2_WITH_TIMEOUT(resumeEndpoints.destCompletedSpy.count() >= 1,
                              "Resume destination should complete",
                              60'000);
    QTRY_VERIFY2_WITH_TIMEOUT(resumeEndpoints.sourceCompletedSpy.count() >= 1,
                              "Resume source should complete",
                              60'000);

    const auto destArgs = resumeEndpoints.destCompletedSpy.takeFirst();
    QVERIFY2(destArgs.at(0).toBool(), "Destination must report success after resume");

    const QString destPath = QDir(destDir.path()).filePath(relativePath);
    QFile destFile(destPath);
    QVERIFY(destFile.exists());
    QVERIFY(destFile.open(QIODevice::ReadOnly));
    QCOMPARE(destFile.readAll(), payload);

    resumeEndpoints.source.stop();
    resumeEndpoints.destination.stop();
}

// ======================================================================
// Receiver detects sender disconnect (Bug #3 regression test)
// ======================================================================

void NetworkTransferWorkflowTests::receiverDetectsSenderDisconnect() {
    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());

    const quint16 controlPort = pickFreePort();
    const quint16 dataPort = pickFreePort();
    QVERIFY(controlPort != 0);
    QVERIFY(dataPort != 0);

    TransferSettings settings = makeSettings(controlPort, dataPort, false, false, false);
    settings.max_bandwidth_kbps = 8;
    const QString passphrase = "test-passphrase";

    NetworkTransferController destination;
    destination.configure(settings);
    QSignalSpy manifestSpy(&destination, &NetworkTransferController::manifestReceived);
    QSignalSpy destCompletedSpy(&destination, &NetworkTransferController::transferCompleted);

    destination.startDestination(passphrase, destDir.path());

    // Create source with a valid file, but stop it mid-transfer
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const QString relativePath = "TestUser/Documents/disconnect_test.bin";
    const QString sourcePath = QDir(sourceDir.path()).filePath(relativePath);
    QDir(sourceDir.path()).mkpath("TestUser/Documents");

    const QByteArray payload = makeRepeatedData("DISC", 200'000);
    QVERIFY(writeFile(sourcePath, payload));

    const auto entryOpt = makeEntryForFile(sourcePath, relativePath);
    QVERIFY(entryOpt.has_value());
    const TransferFileEntry entry = *entryOpt;
    const TransferManifest manifest = makeTestManifest({entry}, entry.size_bytes);

    NetworkTransferController source;
    source.configure(settings);
    QSignalSpy sourceCompletedSpy(&source, &NetworkTransferController::transferCompleted);

    const TransferPeerInfo peer = makeLocalDestinationPeer(controlPort, dataPort);
    source.startSource(manifest, {entry}, peer, passphrase);

    QVERIFY(manifestSpy.wait(15'000));
    destination.approveTransfer(true);

    // Wait for data transfer to start, then kill the source mid-stream
    QSignalSpy progressSpy(&destination, &NetworkTransferController::transferProgress);
    QTRY_VERIFY2_WITH_TIMEOUT(progressSpy.count() >= 1,
                              "Should see progress before stopping",
                              15'000);
    source.stop();

    QTRY_VERIFY2_WITH_TIMEOUT(destCompletedSpy.count() >= 1,
                              "Destination should complete after disconnect",
                              30'000);

    const auto destArgs = destCompletedSpy.takeFirst();
    QVERIFY2(!destArgs.at(0).toBool(), "Destination MUST report failure when sender disconnects");

    destination.stop();
}

QTEST_MAIN(NetworkTransferWorkflowTests)
#include "test_network_transfer_workflow.moc"
