// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_transfer_worker.cpp
/// @brief Implements network file transfer operations over TCP sockets

#include "sak/network_transfer_worker.h"

#include "sak/network_transfer_security.h"
#include "sak/file_hash.h"
#include "sak/path_utils.h"
#include "sak/logger.h"
#include "sak/permission_manager.h"
#include "sak/layout_constants.h"

#include <QtGlobal>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QThread>
#include <QByteArray>
#include <QDataStream>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QScopeGuard>

#include <filesystem>
#include <algorithm>

namespace sak {

namespace {
constexpr quint32 kMagic = 0x53414B4E; // SAKN
constexpr int kHeaderSize = 4 + 1 + 1 + 2 + 4 + 4 + 4 + 4; // 24
constexpr quint16 kFlagEncrypted = 0x01;
constexpr quint16 kFlagCompressed = 0x02;
constexpr quint16 kFlagLastChunk = 0x04;

QByteArray toBigEndian(quint32 value) {
    QByteArray data(4, 0);
    data[0] = static_cast<char>((value >> 24) & 0xFF);
    data[1] = static_cast<char>((value >> 16) & 0xFF);
    data[2] = static_cast<char>((value >> 8) & 0xFF);
    data[3] = static_cast<char>(value & 0xFF);
    return data;
}

quint32 fromBigEndian(const QByteArray& data, int offset = 0) {
    return (static_cast<quint8>(data[offset]) << 24)
         | (static_cast<quint8>(data[offset + 1]) << 16)
         | (static_cast<quint8>(data[offset + 2]) << 8)
         | static_cast<quint8>(data[offset + 3]);
}

quint32 crc32(const QByteArray& data) {
    quint32 crc = 0xFFFFFFFF;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            const quint32 mask = (crc & 1U) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

QByteArray packEncrypted(const EncryptedPayload& payload) {
    QByteArray packed;
    packed.append(payload.iv);
    packed.append(payload.tag);
    packed.append(payload.ciphertext);
    return packed;
}

EncryptedPayload unpackEncrypted(const QByteArray& packed) {
    EncryptedPayload payload;
    if (packed.size() < 28) {
        return payload;
    }
    payload.iv = packed.left(12);
    payload.tag = packed.mid(12, 16);
    payload.ciphertext = packed.mid(28);
    return payload;
}

bool saveResumeInfo(const QString& resumePath, const QString& fileId,
                    const QVector<QPair<int, int>>& ranges, int totalChunks) {
    QJsonObject root;
    root["file_id"] = fileId;
    root["total_chunks"] = totalChunks;
    QJsonArray arr;
    for (const auto& range : ranges) {
        QJsonArray pair;
        pair.append(range.first);
        pair.append(range.second);
        arr.append(pair);
    }
    root["ranges"] = arr;
    QFile file(resumePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return true;
}

QVector<QPair<int, int>> loadResumeInfo(const QString& resumePath, QString& fileId, int& totalChunks) {
    QVector<QPair<int, int>> ranges;
    QFile file(resumePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return ranges;
    }
    QJsonParseError error{};
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return ranges;
    }
    auto obj = doc.object();
    fileId = obj.value("file_id").toString();
    totalChunks = obj.value("total_chunks").toInt(totalChunks);
    auto arr = obj.value("ranges").toArray();
    for (const auto& range : arr) {
        auto pair = range.toArray();
        if (pair.size() == 2) {
            ranges.append({pair[0].toInt(), pair[1].toInt()});
        }
    }
    return ranges;
}

void mergeChunkRange(QVector<QPair<int, int>>& ranges, int chunkId) {
    int start = chunkId;
    int end = chunkId;

    for (int i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        if (chunkId >= range.first && chunkId <= range.second) {
            return;
        }
        if (chunkId == range.second + 1) {
            start = range.first;
            ranges.removeAt(i);
            --i;
            continue;
        }
        if (chunkId + 1 == range.first) {
            end = range.second;
            ranges.removeAt(i);
            --i;
            continue;
        }
    }

    ranges.append({start, end});
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
}

bool isChunkInRanges(int chunkId, const QVector<QPair<int, int>>& ranges) {
    for (const auto& range : ranges) {
        if (chunkId >= range.first && chunkId <= range.second) {
            return true;
        }
    }
    return false;
}

} // namespace

NetworkTransferWorker::NetworkTransferWorker(QObject* parent)
    : QObject(parent)
{
}

void NetworkTransferWorker::startSender(const QVector<TransferFileEntry>& files,
                                        const QHostAddress& host,
                                        quint16 port,
                                        const DataOptions& options) {
    Q_ASSERT_X(!files.isEmpty(), "startSender", "files must not be empty");
    Q_ASSERT_X(port > 0, "startSender", "port must be positive");
    m_stopRequested = false;
    m_dynamicMaxBandwidthKbps.store(options.max_bandwidth_kbps, std::memory_order_relaxed);

    logInfo("NetworkTransferWorker sender connecting to {}:{}", host.toString().toStdString(), port);

    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(sak::kTimeoutNetworkReadMs)) {
        logError("NetworkTransferWorker sender connection failed: {}", socket.errorString().toStdString());
        Q_EMIT errorOccurred(tr("Failed to connect to %1:%2").arg(host.toString()).arg(port));
        Q_EMIT transferCompleted(false, tr("Connection failed"));
        return;
    }

    Q_EMIT transferStarted();

    if (!handleSender(&socket, files, options)) {
        logError("NetworkTransferWorker sender failed during transfer");
        socket.disconnectFromHost();
        Q_EMIT transferCompleted(false, tr("Transfer failed"));
        return;
    }

    socket.disconnectFromHost();
    Q_EMIT transferCompleted(true, tr("Transfer completed"));
    logInfo("NetworkTransferWorker sender completed transfer");
}

void NetworkTransferWorker::startReceiver(const QHostAddress& listenAddress,
                                          quint16 port,
                                          const DataOptions& options) {
    Q_ASSERT_X(port > 0, "startReceiver", "port must be positive");
    m_stopRequested = false;
    m_dynamicMaxBandwidthKbps.store(options.max_bandwidth_kbps, std::memory_order_relaxed);

    logInfo("NetworkTransferWorker receiver listening on {}:{}", listenAddress.toString().toStdString(), port);

    QTcpServer server;
    if (!server.listen(listenAddress, port)) {
        logError("NetworkTransferWorker receiver listen failed: {}", server.errorString().toStdString());
        Q_EMIT errorOccurred(tr("Failed to listen on %1:%2").arg(listenAddress.toString()).arg(port));
        Q_EMIT transferCompleted(false, tr("Listen failed"));
        return;
    }

    Q_EMIT transferStarted();

    if (!server.waitForNewConnection(60000)) {
        logError("NetworkTransferWorker receiver timed out waiting for connection");
        Q_EMIT errorOccurred(tr("No incoming connection"));
        Q_EMIT transferCompleted(false, tr("No incoming connection"));
        return;
    }

    QTcpSocket* socket = server.nextPendingConnection();
    if (!socket) {
        logError("NetworkTransferWorker receiver connection failed: no socket");
        Q_EMIT transferCompleted(false, tr("Connection failure"));
        return;
    }

    const bool success = handleReceiver(socket, options);
    socket->disconnectFromHost();
    socket->deleteLater();

    Q_EMIT transferCompleted(success, success ? tr("Transfer completed") : tr("Transfer failed"));
    logInfo("NetworkTransferWorker receiver completed transfer: {}", success ? "success" : "failure");
}

void NetworkTransferWorker::stop() {
    m_stopRequested = true;
}

void NetworkTransferWorker::updateBandwidthLimit(int max_kbps) {
    m_dynamicMaxBandwidthKbps.store(qMax(0, max_kbps), std::memory_order_relaxed);
}

QByteArray NetworkTransferWorker::serializeHeader(const FrameHeader& header) const {
    QByteArray data;
    data.reserve(kHeaderSize);
    data.append(toBigEndian(header.magic));
    data.append(static_cast<char>(header.version));
    data.append(static_cast<char>(header.frame_type));
    data.append(static_cast<char>((header.flags >> 8) & 0xFF));
    data.append(static_cast<char>(header.flags & 0xFF));
    data.append(toBigEndian(header.chunk_id));
    data.append(toBigEndian(header.payload_size));
    data.append(toBigEndian(header.plain_size));
    data.append(toBigEndian(header.crc32));
    return data;
}

bool NetworkTransferWorker::readHeader(QTcpSocket* socket, FrameHeader& header) {
    Q_ASSERT_X(socket != nullptr, "readHeader", "socket must not be null");
    QByteArray headerBytes = readExact(socket, kHeaderSize);
    if (headerBytes.size() != kHeaderSize) {
        return false;
    }

    header.magic = fromBigEndian(headerBytes, 0);
    header.version = static_cast<quint8>(headerBytes[4]);
    header.frame_type = static_cast<quint8>(headerBytes[5]);
    header.flags = (static_cast<quint8>(headerBytes[6]) << 8) | static_cast<quint8>(headerBytes[7]);
    header.chunk_id = fromBigEndian(headerBytes, 8);
    header.payload_size = fromBigEndian(headerBytes, 12);
    header.plain_size = fromBigEndian(headerBytes, 16);
    header.crc32 = fromBigEndian(headerBytes, 20);
    return header.magic == kMagic;
}

QByteArray NetworkTransferWorker::readExact(QTcpSocket* socket, qint64 size) {
    QByteArray data;
    data.reserve(static_cast<int>(qMin(size, static_cast<qint64>(INT_MAX))));

    while (static_cast<qint64>(data.size()) < size && !m_stopRequested) {
        if (socket->bytesAvailable() <= 0) {
            if (!socket->waitForReadyRead(sak::kTimeoutNetworkReadMs)) {
                break;
            }
        }
        data.append(socket->read(size - data.size()));
    }

    return data;
}

bool NetworkTransferWorker::sendFrame(QTcpSocket* socket, FrameType type, quint16 flags, quint32 chunkId,
                                      const QByteArray& payload, quint32 plainSize, quint32 crc) {
    Q_ASSERT_X(socket != nullptr, "sendFrame", "socket must not be null");
    FrameHeader header;
    header.frame_type = static_cast<quint8>(type);
    header.flags = flags;
    header.chunk_id = chunkId;
    header.payload_size = payload.size();
    header.plain_size = plainSize;
    header.crc32 = crc;

    QByteArray headerBytes = serializeHeader(header);
    if (socket->write(headerBytes) != headerBytes.size()) {
        return false;
    }
    if (socket->write(payload) != payload.size()) {
        return false;
    }
    return socket->flush();
}

QByteArray NetworkTransferWorker::compressData(const QByteArray& data) const {
    return qCompress(data, 6);
}

QByteArray NetworkTransferWorker::decompressData(const QByteArray& data) const {
    return qUncompress(data);
}

quint32 NetworkTransferWorker::computeCrc32(const QByteArray& data) const {
    return crc32(data);
}

QByteArray NetworkTransferWorker::buildResumePayload(const QString& fileId,
                                                    const QVector<QPair<int, int>>& ranges,
                                                    int totalChunks) const {
    QJsonObject root;
    root["file_id"] = fileId;
    root["total_chunks"] = totalChunks;
    QJsonArray arr;
    for (const auto& range : ranges) {
        QJsonArray pair;
        pair.append(range.first);
        pair.append(range.second);
        arr.append(pair);
    }
    root["ranges"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QVector<QPair<int, int>> NetworkTransferWorker::parseResumePayload(const QByteArray& payload, int& totalChunks) const {
    QVector<QPair<int, int>> ranges;
    QJsonParseError error{};
    QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return ranges;
    }
    auto obj = doc.object();
    totalChunks = obj.value("total_chunks").toInt(0);
    auto arr = obj.value("ranges").toArray();
    for (const auto& range : arr) {
        auto pair = range.toArray();
        if (pair.size() == 2) {
            ranges.append({pair[0].toInt(), pair[1].toInt()});
        }
    }
    return ranges;
}

bool NetworkTransferWorker::trySendSingleFile(QTcpSocket* socket,
                                               const TransferFileEntry& file,
                                               const DataOptions& options,
                                               const QByteArray& key,
                                               qint64& bytesSent, qint64 totalBytes,
                                               QElapsedTimer& rateTimer, qint64& rateBytesSent) {
    Q_ASSERT_X(socket != nullptr, "trySendSingleFile", "socket must not be null");
    QFile source(file.absolute_path);
    if (!source.open(QIODevice::ReadOnly)) {
        logError("NetworkTransferWorker sender failed to open file {}", file.absolute_path.toStdString());
        Q_EMIT errorOccurred(tr("Failed to open file %1").arg(file.absolute_path));
        return false;
    }

    Q_EMIT fileStarted(file.file_id, file.relative_path, file.size_bytes);

    if (!sendFileHeader(socket, file, options)) {
        return false;
    }

    // Resume info
    QVector<QPair<int, int>> resumeRanges;
    if (options.resume_enabled) {
        FrameHeader resumeHeader;
        if (readHeader(socket, resumeHeader) && resumeHeader.frame_type == FrameResumeInfo) {
            QByteArray resumePayload = readExact(socket, resumeHeader.payload_size);
            int totalChunks = 0;
            resumeRanges = parseResumePayload(resumePayload, totalChunks);
        }
    }

    if (!sendFileChunks(socket, source, file, options, key,
                       resumeRanges, bytesSent, totalBytes,
                       rateTimer, rateBytesSent)) {
        return false;
    }

    if (!sendFrame(socket, FrameFileEnd, 0, 0, {}, 0, 0)) {
        logError("NetworkTransferWorker sender failed to finalize file {}", file.relative_path.toStdString());
        Q_EMIT errorOccurred(tr("Failed to finalize file"));
        return false;
    }

    return awaitFileAck(socket, file);
}

bool NetworkTransferWorker::handleSender(QTcpSocket* socket,
                                         const QVector<TransferFileEntry>& files,
                                         const DataOptions& options) {
    Q_ASSERT_X(socket != nullptr, "handleSender", "socket must not be null");
    Q_ASSERT_X(!files.isEmpty(), "handleSender", "files must not be empty");
    QByteArray key;
    // Scope guard to securely wipe the derived key on all exit paths
    auto keyGuard = qScopeGuard([&key]() {
        if (!key.isEmpty()) {
            SecureZeroMemory(key.data(), key.size());
        }
    });

    if (options.encryption_enabled) {
        auto keyResult = TransferSecurityManager::deriveKey(options.passphrase, options.salt);
        if (!keyResult) {
            logError("NetworkTransferWorker sender failed to derive encryption key");
            Q_EMIT errorOccurred(tr("Failed to derive encryption key"));
            return false;
        }
        key = *keyResult;
    }

    qint64 totalBytes = 0;
    for (const auto& file : files) {
        totalBytes += file.size_bytes;
    }

    qint64 bytesSent = 0;
    QElapsedTimer rateTimer;
    rateTimer.start();
    qint64 rateBytesSent = 0;

    for (const auto& file : files) {
        if (m_stopRequested) {
            return false;
        }
        logInfo("NetworkTransferWorker sender starting file {}", file.relative_path.toStdString());

        bool sent = false;
        for (int attempt = 0; attempt < 3 && !sent; ++attempt) {
            sent = trySendSingleFile(socket, file, options, key,
                                     bytesSent, totalBytes, rateTimer, rateBytesSent);
        }

        if (!sent) {
            logError("NetworkTransferWorker sender failed to transfer file {} after retries", file.relative_path.toStdString());
            Q_EMIT errorOccurred(tr("Failed to transfer file after retries"));
            return false;
        }

        Q_EMIT fileCompleted(file.file_id, file.relative_path);
    }

    return sendFrame(socket, FrameTransferEnd, 0, 0, {}, 0, 0);
}

bool NetworkTransferWorker::sendFileHeader(QTcpSocket* socket,
                                           const TransferFileEntry& file,
                                           const DataOptions& options) {
    Q_ASSERT_X(socket != nullptr, "sendFileHeader", "socket must not be null");
    QJsonObject fileHeader;
    fileHeader["file_id"] = file.file_id;
    fileHeader["relative_path"] = file.relative_path;
    fileHeader["size_bytes"] = static_cast<qint64>(file.size_bytes);
    fileHeader["checksum_sha256"] = file.checksum_sha256;
    fileHeader["chunk_size"] = options.chunk_size;
    if (!file.acl_sddl.isEmpty()) {
        fileHeader["acl_sddl"] = file.acl_sddl;
    }

    QByteArray headerPayload = QJsonDocument(fileHeader).toJson(QJsonDocument::Compact);
    if (!sendFrame(socket, FrameFileHeader, 0, 0, headerPayload, headerPayload.size(), computeCrc32(headerPayload))) {
        logError("NetworkTransferWorker sender failed to send file header");
        Q_EMIT errorOccurred(tr("Failed to send file header"));
        return false;
    }
    return true;
}

bool NetworkTransferWorker::sendFileChunks(QTcpSocket* socket, QFile& source,
                                           const TransferFileEntry& file,
                                           const DataOptions& options,
                                           const QByteArray& key,
                                           const QVector<QPair<int, int>>& resumeRanges,
                                           qint64& bytesSent, qint64 totalBytes,
                                           QElapsedTimer& rateTimer, qint64& rateBytesSent) {
    Q_ASSERT_X(socket != nullptr, "sendFileChunks", "socket must not be null");

    int chunkId = 0;
    while (!source.atEnd()) {
        if (m_stopRequested) {
            return false;
        }

        QByteArray chunk = source.read(options.chunk_size);
        if (chunk.isEmpty()) {
            break;
        }

        if (options.resume_enabled && isChunkInRanges(chunkId, resumeRanges)) {
            bytesSent += chunk.size();
            Q_EMIT fileProgress(file.file_id, bytesSent, file.size_bytes);
            Q_EMIT overallProgress(bytesSent, totalBytes);
            chunkId++;
            continue;
        }

        QByteArray payload = chunk;
        quint16 flags = 0;

        if (options.compression_enabled) {
            payload = compressData(payload);
            flags |= kFlagCompressed;
        }

        if (!encryptPayloadIfNeeded(payload, flags, key, options, file.relative_path)) {
            return false;
        }

        const bool isLast = source.atEnd();
        if (isLast) {
            flags |= kFlagLastChunk;
        }

        const quint32 crc = computeCrc32(chunk);
        if (!sendFrame(socket, FrameDataChunk, flags, static_cast<quint32>(chunkId), payload, chunk.size(), crc)) {
            logError("NetworkTransferWorker sender failed to send data chunk");
            Q_EMIT errorOccurred(tr("Failed to send data chunk"));
            return false;
        }

        bytesSent += chunk.size();
        Q_EMIT fileProgress(file.file_id, bytesSent, file.size_bytes);
        Q_EMIT overallProgress(bytesSent, totalBytes);

        throttleBandwidth(chunk.size(), rateTimer, rateBytesSent);

        chunkId++;
    }

    return true;
}

bool NetworkTransferWorker::encryptPayloadIfNeeded(QByteArray& payload, quint16& flags,
                                                    const QByteArray& key,
                                                    const DataOptions& options,
                                                    const QString& relativePath) {
    if (!options.encryption_enabled) {
        return true;
    }
    auto encrypted = TransferSecurityManager::encryptAesGcm(payload, key, options.transfer_id.toUtf8());
    if (!encrypted) {
        logError("NetworkTransferWorker sender encryption failed for file {}", relativePath.toStdString());
        Q_EMIT errorOccurred(tr("Encryption failed"));
        return false;
    }
    payload = packEncrypted(*encrypted);
    flags |= kFlagEncrypted;
    return true;
}

void NetworkTransferWorker::throttleBandwidth(qint64 chunkSize, QElapsedTimer& rateTimer,
                                              qint64& rateBytesSent) {
    const int maxBandwidthKbps = m_dynamicMaxBandwidthKbps.load(std::memory_order_relaxed);
    if (maxBandwidthKbps <= 0) {
        return;
    }
    rateBytesSent += chunkSize;
    const qint64 expectedMs = (rateBytesSent * 1000) / (static_cast<qint64>(maxBandwidthKbps) * sak::kBytesPerKB);
    const qint64 elapsedMs = rateTimer.elapsed();
    if (expectedMs > elapsedMs) {
        QThread::msleep(static_cast<unsigned long>(expectedMs - elapsedMs));
    }
    if (elapsedMs >= 1000) {
        rateTimer.restart();
        rateBytesSent = 0;
    }
}

bool NetworkTransferWorker::awaitFileAck(QTcpSocket* socket, const TransferFileEntry& file) {
    Q_ASSERT_X(socket != nullptr, "awaitFileAck", "socket must not be null");
    FrameHeader ackHeader;
    if (!readHeader(socket, ackHeader) || ackHeader.frame_type != FrameFileAck) {
        logWarning("NetworkTransferWorker sender did not receive file ACK for {}", file.relative_path.toStdString());
        Q_EMIT errorOccurred(tr("No file ACK received"));
        return false;
    }

    QByteArray ackPayload = readExact(socket, ackHeader.payload_size);
    QJsonParseError err{};
    QJsonDocument ackDoc = QJsonDocument::fromJson(ackPayload, &err);
    if (err.error != QJsonParseError::NoError || !ackDoc.isObject()) {
        logWarning("NetworkTransferWorker sender received invalid file ACK for {}", file.relative_path.toStdString());
        Q_EMIT errorOccurred(tr("Invalid file ACK"));
        return false;
    }

    auto ackObj = ackDoc.object();
    const QString status = ackObj.value("status").toString();
    if (status == "ok") {
        return true;
    }

    logWarning("NetworkTransferWorker sender file verification failed for {}, retrying", file.relative_path.toStdString());
    Q_EMIT errorOccurred(tr("File verification failed, retrying..."));
    return false;
}

bool NetworkTransferWorker::deriveReceiverKey(const DataOptions& options, ReceiverState& state) {
    if (!options.encryption_enabled) {
        return true;
    }
    auto keyResult = TransferSecurityManager::deriveKey(options.passphrase, options.salt);
    if (!keyResult) {
        logError("NetworkTransferWorker receiver failed to derive encryption key");
        Q_EMIT errorOccurred(tr("Failed to derive encryption key"));
        return false;
    }
    state.key = *keyResult;
    return true;
}

void NetworkTransferWorker::emitReceiverProgress(
    const DataOptions& options, ReceiverState& state, QElapsedTimer& resumeTimer)
{
    if (options.resume_enabled && resumeTimer.elapsed() > 2000) {
        saveResumeInfo(state.resume_path, state.current_file_id,
                       state.current_ranges, state.total_chunks);
        resumeTimer.restart();
    }
    Q_EMIT fileProgress(state.current_file_id, state.bytes_received, state.current_size);
    Q_EMIT overallProgress(state.overall_received, options.total_bytes);
}

bool NetworkTransferWorker::handleReceiver(QTcpSocket* socket, const DataOptions& options) {
    Q_ASSERT_X(socket != nullptr, "handleReceiver", "socket must not be null");
    ReceiverState state;
    state.chunk_size = options.chunk_size;
    state.overall_received = 0;

    // Scope guard to securely wipe the derived key on all exit paths
    auto keyGuard = qScopeGuard([&state]() {
        if (!state.key.isEmpty()) {
            SecureZeroMemory(state.key.data(), state.key.size());
        }
    });

    if (!deriveReceiverKey(options, state)) {
        return false;
    }

    QElapsedTimer resumeTimer;
    resumeTimer.start();

    while (!m_stopRequested) {
        FrameHeader header;
        if (!readHeader(socket, header)) {
            break;
        }

        // Validate payload size before allocating memory (BUG-02: prevent OOM DoS).
        if (header.payload_size > kMaxPayloadSize) {
            logError("NetworkTransferWorker receiver payload size {} exceeds maximum {}",
                     header.payload_size, kMaxPayloadSize);
            Q_EMIT errorOccurred(tr("Payload too large — possible protocol violation"));
            return false;
        }

        QByteArray payload = readExact(socket, header.payload_size);
        if (payload.size() != static_cast<qint64>(header.payload_size)) {
            return false;
        }

        switch (static_cast<FrameType>(header.frame_type)) {
        case FrameFileHeader:
            if (!processFileHeader(socket, payload, options, state)) {
                return false;
            }
            continue;

        case FrameDataChunk:
            if (!processDataChunk(socket, header, payload, options, state)) {
                return false;
            }
            emitReceiverProgress(options, state, resumeTimer);
            continue;

        case FrameFileEnd:
            if (!processFileEnd(socket, options, state)) {
                continue;  // retry signaled via ACK
            }
            continue;

        case FrameTransferEnd:
            return true;

        default:
            continue;
        }
    }

    return !m_stopRequested;
}

// ─── Private Helpers ────────────────────────────────────────────────────────────

bool NetworkTransferWorker::processFileHeader(QTcpSocket* socket,
                                               const QByteArray& payload,
                                               const DataOptions& options,
                                               ReceiverState& state) {
    Q_ASSERT_X(socket != nullptr, "processFileHeader", "socket must not be null");
    QJsonParseError error{};
    QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        logError("NetworkTransferWorker receiver invalid file header");
        Q_EMIT errorOccurred(tr("Invalid file header"));
        return false;
    }

    auto obj = doc.object();
    state.current_file_id = obj.value("file_id").toString();
    state.current_relative_path = obj.value("relative_path").toString();
    state.current_size = obj.value("size_bytes").toVariant().toLongLong();
    state.current_checksum = obj.value("checksum_sha256").toString();
    state.chunk_size = obj.value("chunk_size").toInt(state.chunk_size);
    if (state.chunk_size <= 0) {
        logError("NetworkTransferWorker receiver invalid chunk_size: {}", state.chunk_size);
        Q_EMIT errorOccurred(tr("Invalid chunk size in file header"));
        return false;
    }
    state.current_acl_sddl = obj.value("acl_sddl").toString();
    state.total_chunks = static_cast<int>((state.current_size + state.chunk_size - 1) / state.chunk_size);

    const QString destinationPath = QDir(options.destination_base).filePath(state.current_relative_path);
    auto nativePath = QDir::toNativeSeparators(destinationPath);

    std::filesystem::path destPath = nativePath.toStdString();
    auto safe = path_utils::isSafePath(destPath, std::filesystem::path(options.destination_base.toStdString()));
    if (!safe || !(*safe)) {
        logError("NetworkTransferWorker receiver detected unsafe path: {}", destinationPath.toStdString());
        Q_EMIT errorOccurred(tr("Unsafe path detected"));
        return false;
    }

    QDir destDir = QFileInfo(destinationPath).absoluteDir();
    if (!destDir.exists() && !destDir.mkpath(".")) {
        logError("NetworkTransferWorker receiver failed to create destination directory {}",
                  destDir.path().toStdString());
        Q_EMIT errorOccurred(tr("Failed to create destination directory"));
        return false;
    }

    QString tempPath = destinationPath + ".partial";
    state.current_file = std::make_unique<QFile>(tempPath);
    if (!state.current_file->open(QIODevice::ReadWrite)) {
        logError("NetworkTransferWorker receiver failed to open destination file {}", destinationPath.toStdString());
        Q_EMIT errorOccurred(tr("Failed to open destination file"));
        return false;
    }

    if (options.resume_enabled) {
        initReceiverResume(socket, options, state);
    }

    state.bytes_received = 0;
    Q_EMIT fileStarted(state.current_file_id, state.current_relative_path, state.current_size);
    logInfo("NetworkTransferWorker receiver starting file {}", state.current_relative_path.toStdString());
    return true;
}

void NetworkTransferWorker::initReceiverResume(QTcpSocket* socket, const DataOptions& options,
                                               ReceiverState& state) {
    const QString destinationPath = QDir(options.destination_base).filePath(state.current_relative_path);
    state.resume_path = destinationPath + ".resume.json";
    QString resumeFileId;
    int resumeTotalChunks = state.total_chunks;
    state.current_ranges = loadResumeInfo(state.resume_path, resumeFileId, resumeTotalChunks);

    if (!resumeFileId.isEmpty() && resumeFileId != state.current_file_id) {
        state.current_ranges.clear();
        QFile::remove(state.resume_path);
    } else if (resumeTotalChunks > 0 && resumeTotalChunks != state.total_chunks) {
        state.current_ranges.clear();
        QFile::remove(state.resume_path);
    }

    if (state.current_ranges.isEmpty()) {
        const qint64 existing = state.current_file->size();
        const int completedChunks = static_cast<int>(existing / state.chunk_size);
        if (completedChunks > 0) {
            state.current_ranges.append({0, completedChunks - 1});
        }
    }

    QByteArray resumePayload = buildResumePayload(state.current_file_id, state.current_ranges, state.total_chunks);
    if (!sendFrame(socket, FrameResumeInfo, 0, 0, resumePayload, resumePayload.size(), computeCrc32(resumePayload))) {
        logWarning("NetworkTransferWorker failed to send resume info frame");
    }
}

bool NetworkTransferWorker::processDataChunk(QTcpSocket* /*socket*/,
                                              const FrameHeader& header,
                                              const QByteArray& payload,
                                              const DataOptions& options,
                                              ReceiverState& state) {
    QByteArray plainPayload = payload;

    if (header.flags & kFlagEncrypted) {
        EncryptedPayload encrypted = unpackEncrypted(payload);
        auto decrypted = TransferSecurityManager::decryptAesGcm(encrypted, state.key, options.transfer_id.toUtf8());
        if (!decrypted) {
            logError("NetworkTransferWorker receiver decryption failed for {}", state.current_relative_path.toStdString());
            Q_EMIT errorOccurred(tr("Decryption failed"));
            return false;
        }
        plainPayload = *decrypted;
    }

    if (header.flags & kFlagCompressed) {
        plainPayload = decompressData(plainPayload);
    }

    if (computeCrc32(plainPayload) != header.crc32) {
        Q_EMIT errorOccurred(tr("CRC mismatch"));
        return false;
    }

    const qint64 offset = static_cast<qint64>(header.chunk_id) * state.chunk_size;
    if (!state.current_file->seek(offset)) {
        Q_EMIT errorOccurred(tr("Failed to seek in destination file"));
        return false;
    }

    if (state.current_file->write(plainPayload) != plainPayload.size()) {
        Q_EMIT errorOccurred(tr("Failed to write data"));
        return false;
    }

    if (options.resume_enabled) {
        mergeChunkRange(state.current_ranges, static_cast<int>(header.chunk_id));
    }

    state.bytes_received += plainPayload.size();
    state.overall_received += plainPayload.size();
    return true;
}

bool NetworkTransferWorker::processFileEnd(QTcpSocket* socket,
                                            const DataOptions& options,
                                            ReceiverState& state) {
    Q_ASSERT_X(socket != nullptr, "processFileEnd", "socket must not be null");
    state.current_file->flush();
    state.current_file->close();
    state.current_file.reset();

    QString finalPath = QDir(options.destination_base).filePath(state.current_relative_path);
    QString tempPath = finalPath + ".partial";

    QJsonObject ackPayload;
    ackPayload["file_id"] = state.current_file_id;

    // Verify checksum
    if (!state.current_checksum.isEmpty()) {
        file_hasher hasher(hash_algorithm::sha256);
        auto hashResult = hasher.calculateHash(std::filesystem::path(tempPath.toStdString()));
        if (!hashResult || QString::fromStdString(*hashResult) != state.current_checksum) {
            Q_EMIT errorOccurred(tr("Checksum mismatch for %1").arg(finalPath));
            ackPayload["status"] = "retry";
            sendFrame(socket, FrameFileAck, 0, 0,
                QJsonDocument(ackPayload).toJson(QJsonDocument::Compact), 0, 0);
            QFile::remove(tempPath);
            // Remove stale resume metadata so retries start from scratch
            // instead of skipping chunks that no longer exist on disk.
            QFile::remove(state.resume_path);
            state.current_ranges.clear();
            return false;
        }
    }

    QFile::remove(finalPath);
    if (!QFile::rename(tempPath, finalPath)) {
        Q_EMIT errorOccurred(tr("Failed to finalize file"));
        ackPayload["status"] = "retry";
        sendFrame(socket, FrameFileAck, 0, 0,
            QJsonDocument(ackPayload).toJson(QJsonDocument::Compact), 0, 0);
        return false;
    }

    PermissionManager perms;
    if (!state.current_acl_sddl.isEmpty()) {
        perms.setSecurityDescriptorSddl(finalPath, state.current_acl_sddl);
    } else if (options.acl_overrides.contains(state.current_relative_path)) {
        perms.setSecurityDescriptorSddl(finalPath, options.acl_overrides.value(state.current_relative_path));
    } else if (options.permission_modes.contains(state.current_relative_path.section('/', 0, 0))) {
        const auto mode = options.permission_modes.value(state.current_relative_path.section('/', 0, 0));
        perms.applyPermissionStrategy(finalPath, mode);
    } else {
        perms.stripPermissions(finalPath);
    }

    ackPayload["status"] = "ok";
    sendFrame(socket, FrameFileAck, 0, 0,
        QJsonDocument(ackPayload).toJson(QJsonDocument::Compact), 0, 0);

    if (options.resume_enabled && !state.resume_path.isEmpty()) {
        QFile::remove(state.resume_path);
    }

    Q_EMIT fileCompleted(state.current_file_id, state.current_relative_path);
    return true;
}

} // namespace sak
