#include "sak/network_transfer_worker.h"

#include "sak/network_transfer_security.h"
#include "sak/file_hash.h"
#include "sak/path_utils.h"
#include "sak/logger.h"
#include "sak/permission_manager.h"

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

QVector<QPair<int, int>> loadResumeInfo(const QString& resumePath, int& totalChunks) {
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

} // namespace

NetworkTransferWorker::NetworkTransferWorker(QObject* parent)
    : QObject(parent)
{
}

void NetworkTransferWorker::startSender(const QVector<TransferFileEntry>& files,
                                        const QHostAddress& host,
                                        quint16 port,
                                        const DataOptions& options) {
    m_stopRequested = false;
    m_dynamicMaxBandwidthKbps.store(options.max_bandwidth_kbps, std::memory_order_relaxed);

    log_info("NetworkTransferWorker sender connecting to {}:{}", host.toString().toStdString(), port);

    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(15000)) {
        log_error("NetworkTransferWorker sender connection failed: {}", socket.errorString().toStdString());
        Q_EMIT errorOccurred(tr("Failed to connect to %1:%2").arg(host.toString()).arg(port));
        Q_EMIT transferCompleted(false, tr("Connection failed"));
        return;
    }

    Q_EMIT transferStarted();

    if (!handleSender(&socket, files, options)) {
        log_error("NetworkTransferWorker sender failed during transfer");
        socket.disconnectFromHost();
        Q_EMIT transferCompleted(false, tr("Transfer failed"));
        return;
    }

    socket.disconnectFromHost();
    Q_EMIT transferCompleted(true, tr("Transfer completed"));
    log_info("NetworkTransferWorker sender completed transfer");
}

void NetworkTransferWorker::startReceiver(const QHostAddress& listenAddress,
                                          quint16 port,
                                          const DataOptions& options) {
    m_stopRequested = false;
    m_dynamicMaxBandwidthKbps.store(options.max_bandwidth_kbps, std::memory_order_relaxed);

    log_info("NetworkTransferWorker receiver listening on {}:{}", listenAddress.toString().toStdString(), port);

    QTcpServer server;
    if (!server.listen(listenAddress, port)) {
        log_error("NetworkTransferWorker receiver listen failed: {}", server.errorString().toStdString());
        Q_EMIT errorOccurred(tr("Failed to listen on %1:%2").arg(listenAddress.toString()).arg(port));
        Q_EMIT transferCompleted(false, tr("Listen failed"));
        return;
    }

    Q_EMIT transferStarted();

    if (!server.waitForNewConnection(60000)) {
        log_error("NetworkTransferWorker receiver timed out waiting for connection");
        Q_EMIT errorOccurred(tr("No incoming connection"));
        Q_EMIT transferCompleted(false, tr("No incoming connection"));
        return;
    }

    QTcpSocket* socket = server.nextPendingConnection();
    if (!socket) {
        log_error("NetworkTransferWorker receiver connection failed: no socket");
        Q_EMIT transferCompleted(false, tr("Connection failure"));
        return;
    }

    const bool success = handleReceiver(socket, options);
    socket->disconnectFromHost();
    socket->deleteLater();

    Q_EMIT transferCompleted(success, success ? tr("Transfer completed") : tr("Transfer failed"));
    log_info("NetworkTransferWorker receiver completed transfer: {}", success ? "success" : "failure");
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
    data.reserve(static_cast<int>(size));

    while (data.size() < size && !m_stopRequested) {
        if (!socket->waitForReadyRead(15000)) {
            break;
        }
        data.append(socket->read(size - data.size()));
    }

    return data;
}

bool NetworkTransferWorker::sendFrame(QTcpSocket* socket, FrameType type, quint16 flags, quint32 chunkId,
                                      const QByteArray& payload, quint32 plainSize, quint32 crc) {
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

bool NetworkTransferWorker::handleSender(QTcpSocket* socket,
                                         const QVector<TransferFileEntry>& files,
                                         const DataOptions& options) {
    QByteArray key;
    if (options.encryption_enabled) {
        auto keyResult = TransferSecurityManager::deriveKey(options.passphrase, options.salt);
        if (!keyResult) {
            log_error("NetworkTransferWorker sender failed to derive encryption key");
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
        log_info("NetworkTransferWorker sender starting file {}", file.relative_path.toStdString());
        int attempts = 0;
        bool sent = false;
        while (attempts < 3 && !sent) {
            attempts++;

            QFile source(file.absolute_path);
            if (!source.open(QIODevice::ReadOnly)) {
                log_error("NetworkTransferWorker sender failed to open file {}", file.absolute_path.toStdString());
                Q_EMIT errorOccurred(tr("Failed to open file %1").arg(file.absolute_path));
                return false;
            }

            Q_EMIT fileStarted(file.file_id, file.relative_path, file.size_bytes);

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
                log_error("NetworkTransferWorker sender failed to send file header");
                Q_EMIT errorOccurred(tr("Failed to send file header"));
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

            auto isChunkSkipped = [&](int chunkId) {
                for (const auto& range : resumeRanges) {
                    if (chunkId >= range.first && chunkId <= range.second) {
                        return true;
                    }
                }
                return false;
            };

            int chunkId = 0;
            while (!source.atEnd()) {
                if (m_stopRequested) {
                    return false;
                }

                QByteArray chunk = source.read(options.chunk_size);
                if (chunk.isEmpty()) {
                    break;
                }

                if (options.resume_enabled && isChunkSkipped(chunkId)) {
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

                if (options.encryption_enabled) {
                    auto encrypted = TransferSecurityManager::encryptAesGcm(payload, key, options.transfer_id.toUtf8());
                    if (!encrypted) {
                        log_error("NetworkTransferWorker sender encryption failed for file {}", file.relative_path.toStdString());
                        Q_EMIT errorOccurred(tr("Encryption failed"));
                        return false;
                    }
                    payload = packEncrypted(*encrypted);
                    flags |= kFlagEncrypted;
                }

                const bool isLast = source.atEnd();
                if (isLast) {
                    flags |= kFlagLastChunk;
                }

                const quint32 crc = computeCrc32(chunk);
                if (!sendFrame(socket, FrameDataChunk, flags, static_cast<quint32>(chunkId), payload, chunk.size(), crc)) {
                    log_error("NetworkTransferWorker sender failed to send data chunk");
                    Q_EMIT errorOccurred(tr("Failed to send data chunk"));
                    return false;
                }

                bytesSent += chunk.size();
                Q_EMIT fileProgress(file.file_id, bytesSent, file.size_bytes);
                Q_EMIT overallProgress(bytesSent, totalBytes);

                const int maxBandwidthKbps = m_dynamicMaxBandwidthKbps.load(std::memory_order_relaxed);
                if (maxBandwidthKbps > 0) {
                    rateBytesSent += chunk.size();
                    const qint64 expectedMs = (rateBytesSent * 1000) / (static_cast<qint64>(maxBandwidthKbps) * 1024);
                    const qint64 elapsedMs = rateTimer.elapsed();
                    if (expectedMs > elapsedMs) {
                        QThread::msleep(static_cast<unsigned long>(expectedMs - elapsedMs));
                    }
                    if (elapsedMs >= 1000) {
                        rateTimer.restart();
                        rateBytesSent = 0;
                    }
                }

                chunkId++;
            }

            if (!sendFrame(socket, FrameFileEnd, 0, 0, {}, 0, 0)) {
                log_error("NetworkTransferWorker sender failed to finalize file {}", file.relative_path.toStdString());
                Q_EMIT errorOccurred(tr("Failed to finalize file"));
                return false;
            }

            FrameHeader ackHeader;
            if (!readHeader(socket, ackHeader) || ackHeader.frame_type != FrameFileAck) {
                log_warning("NetworkTransferWorker sender did not receive file ACK for {}", file.relative_path.toStdString());
                Q_EMIT errorOccurred(tr("No file ACK received"));
                continue;
            }

            QByteArray ackPayload = readExact(socket, ackHeader.payload_size);
            QJsonParseError err{};
            QJsonDocument ackDoc = QJsonDocument::fromJson(ackPayload, &err);
            if (err.error != QJsonParseError::NoError || !ackDoc.isObject()) {
                log_warning("NetworkTransferWorker sender received invalid file ACK for {}", file.relative_path.toStdString());
                Q_EMIT errorOccurred(tr("Invalid file ACK"));
                continue;
            }

            auto ackObj = ackDoc.object();
            const QString status = ackObj.value("status").toString();
            if (status == "ok") {
                sent = true;
            } else {
                log_warning("NetworkTransferWorker sender file verification failed for {}, retrying", file.relative_path.toStdString());
                Q_EMIT errorOccurred(tr("File verification failed, retrying..."));
            }
        }

        if (!sent) {
            log_error("NetworkTransferWorker sender failed to transfer file {} after retries", file.relative_path.toStdString());
            Q_EMIT errorOccurred(tr("Failed to transfer file after retries"));
            return false;
        }

        Q_EMIT fileCompleted(file.file_id, file.relative_path);
    }

    return sendFrame(socket, FrameTransferEnd, 0, 0, {}, 0, 0);
}

bool NetworkTransferWorker::handleReceiver(QTcpSocket* socket, const DataOptions& options) {
    QByteArray key;
    if (options.encryption_enabled) {
        auto keyResult = TransferSecurityManager::deriveKey(options.passphrase, options.salt);
        if (!keyResult) {
            log_error("NetworkTransferWorker receiver failed to derive encryption key");
            Q_EMIT errorOccurred(tr("Failed to derive encryption key"));
            return false;
        }
        key = *keyResult;
    }

    QFile currentFile;
    QString currentFileId;
    QString currentRelativePath;
    QString currentChecksum;
    QString currentAclSddl;
    qint64 currentSize = 0;
    int chunkSize = options.chunk_size;
    qint64 bytesReceived = 0;
    qint64 totalBytes = options.total_bytes;
    qint64 overallReceived = 0;
    QVector<QPair<int, int>> currentRanges;
    QString resumePath;
    int totalChunks = 0;
    QElapsedTimer resumeTimer;
    resumeTimer.start();

    while (!m_stopRequested) {
        FrameHeader header;
        if (!readHeader(socket, header)) {
            break;
        }

        QByteArray payload = readExact(socket, header.payload_size);
        if (payload.size() != header.payload_size) {
            return false;
        }

        if (header.frame_type == FrameFileHeader) {
            QJsonParseError error{};
            QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                log_error("NetworkTransferWorker receiver invalid file header");
                Q_EMIT errorOccurred(tr("Invalid file header"));
                return false;
            }
            auto obj = doc.object();
            currentFileId = obj.value("file_id").toString();
            currentRelativePath = obj.value("relative_path").toString();
            currentSize = obj.value("size_bytes").toVariant().toLongLong();
            currentChecksum = obj.value("checksum_sha256").toString();
            chunkSize = obj.value("chunk_size").toInt(chunkSize);
            currentAclSddl = obj.value("acl_sddl").toString();

            totalChunks = static_cast<int>((currentSize + chunkSize - 1) / chunkSize);

            const QString destinationPath = QDir(options.destination_base).filePath(currentRelativePath);
            auto nativePath = QDir::toNativeSeparators(destinationPath);

            std::filesystem::path destPath = nativePath.toStdString();
            auto safe = path_utils::is_safe_path(destPath, std::filesystem::path(options.destination_base.toStdString()));
            if (!safe || !(*safe)) {
                log_error("NetworkTransferWorker receiver detected unsafe path: {}", destinationPath.toStdString());
                Q_EMIT errorOccurred(tr("Unsafe path detected"));
                return false;
            }

            QDir destDir = QFileInfo(destinationPath).absoluteDir();
            if (!destDir.exists()) {
                destDir.mkpath(".");
            }

            QString tempPath = destinationPath + ".partial";
            currentFile.setFileName(tempPath);
            if (!currentFile.open(QIODevice::ReadWrite)) {
                log_error("NetworkTransferWorker receiver failed to open destination file {}", destinationPath.toStdString());
                Q_EMIT errorOccurred(tr("Failed to open destination file"));
                return false;
            }

            if (options.resume_enabled) {
                resumePath = destinationPath + ".resume.json";
                currentRanges = loadResumeInfo(resumePath, totalChunks);
                if (currentRanges.isEmpty()) {
                    qint64 existing = currentFile.size();
                    int completedChunks = static_cast<int>(existing / chunkSize);
                    if (completedChunks > 0) {
                        currentRanges.append({0, completedChunks - 1});
                    }
                }

                QByteArray resumePayload = buildResumePayload(currentFileId, currentRanges, totalChunks);
                sendFrame(socket, FrameResumeInfo, 0, 0, resumePayload, resumePayload.size(), computeCrc32(resumePayload));
            }

            bytesReceived = 0;
            Q_EMIT fileStarted(currentFileId, currentRelativePath, currentSize);
            log_info("NetworkTransferWorker receiver starting file {}", currentRelativePath.toStdString());
            continue;
        }

        if (header.frame_type == FrameDataChunk) {
            QByteArray plainPayload = payload;

            if (header.flags & kFlagEncrypted) {
                EncryptedPayload encrypted = unpackEncrypted(payload);
                auto decrypted = TransferSecurityManager::decryptAesGcm(encrypted, key, options.transfer_id.toUtf8());
                if (!decrypted) {
                    log_error("NetworkTransferWorker receiver decryption failed for {}", currentRelativePath.toStdString());
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

            const qint64 offset = static_cast<qint64>(header.chunk_id) * chunkSize;
            if (!currentFile.seek(offset)) {
                Q_EMIT errorOccurred(tr("Failed to seek in destination file"));
                return false;
            }

            if (currentFile.write(plainPayload) != plainPayload.size()) {
                Q_EMIT errorOccurred(tr("Failed to write data"));
                return false;
            }

            if (options.resume_enabled) {
                mergeChunkRange(currentRanges, static_cast<int>(header.chunk_id));
                if (resumeTimer.elapsed() > 2000) {
                    saveResumeInfo(resumePath, currentFileId, currentRanges, totalChunks);
                    resumeTimer.restart();
                }
            }

            bytesReceived += plainPayload.size();
            overallReceived += plainPayload.size();
            Q_EMIT fileProgress(currentFileId, bytesReceived, currentSize);
            Q_EMIT overallProgress(overallReceived, totalBytes);
            continue;
        }

        if (header.frame_type == FrameFileEnd) {
            currentFile.flush();
            currentFile.close();

            QString finalPath = QDir(options.destination_base).filePath(currentRelativePath);
            QString tempPath = finalPath + ".partial";

            QJsonObject ackPayload;
            ackPayload["file_id"] = currentFileId;

            // Verify checksum
            if (!currentChecksum.isEmpty()) {
                file_hasher hasher(hash_algorithm::sha256);
                auto hashResult = hasher.calculate_hash(std::filesystem::path(tempPath.toStdString()));
                if (!hashResult || QString::fromStdString(*hashResult) != currentChecksum) {
                    Q_EMIT errorOccurred(tr("Checksum mismatch for %1").arg(finalPath));
                    ackPayload["status"] = "retry";
                    sendFrame(socket, FrameFileAck, 0, 0,
                        QJsonDocument(ackPayload).toJson(QJsonDocument::Compact), 0, 0);
                    QFile::remove(tempPath);
                    continue;
                }
            }

            QFile::remove(finalPath);
            if (!QFile::rename(tempPath, finalPath)) {
                Q_EMIT errorOccurred(tr("Failed to finalize file"));
                ackPayload["status"] = "retry";
                sendFrame(socket, FrameFileAck, 0, 0,
                    QJsonDocument(ackPayload).toJson(QJsonDocument::Compact), 0, 0);
                continue;
            }

            PermissionManager perms;
            if (!currentAclSddl.isEmpty()) {
                perms.setSecurityDescriptorSddl(finalPath, currentAclSddl);
            } else if (options.acl_overrides.contains(currentRelativePath)) {
                perms.setSecurityDescriptorSddl(finalPath, options.acl_overrides.value(currentRelativePath));
            } else if (options.permission_modes.contains(currentRelativePath.section('/', 0, 0))) {
                const auto mode = options.permission_modes.value(currentRelativePath.section('/', 0, 0));
                perms.applyPermissionStrategy(finalPath, mode);
            } else {
                perms.stripPermissions(finalPath);
            }

            ackPayload["status"] = "ok";
            sendFrame(socket, FrameFileAck, 0, 0,
                QJsonDocument(ackPayload).toJson(QJsonDocument::Compact), 0, 0);

            if (options.resume_enabled && !resumePath.isEmpty()) {
                QFile::remove(resumePath);
            }

            Q_EMIT fileCompleted(currentFileId, currentRelativePath);
            continue;
        }

        if (header.frame_type == FrameTransferEnd) {
            return true;
        }
    }

    return !m_stopRequested;
}

} // namespace sak
