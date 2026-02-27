// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QVector>
#include <atomic>

class QTcpSocket;
class QFile;

#include "sak/network_transfer_types.h"

namespace sak {

/// @brief Low-level worker for sending and receiving file data
class NetworkTransferWorker : public QObject {
    Q_OBJECT

public:
    enum class Role {
        Sender,
        Receiver
    };

    /// @brief Options controlling a data transfer session
    struct DataOptions {
        QString transfer_id;
        bool encryption_enabled{true};
        bool compression_enabled{true};
        bool resume_enabled{true};
        int chunk_size{65536};
        int max_bandwidth_kbps{0};
        QString passphrase;
        QByteArray salt;
        QString destination_base;
        qint64 total_bytes{0};
        QMap<QString, PermissionMode> permission_modes; // username -> mode
        QMap<QString, QString> acl_overrides; // relative_path -> SDDL
    };

    explicit NetworkTransferWorker(QObject* parent = nullptr);

    void startSender(const QVector<TransferFileEntry>& files,
                     const QHostAddress& host,
                     quint16 port,
                     const DataOptions& options);

    void startReceiver(const QHostAddress& listenAddress,
                       quint16 port,
                       const DataOptions& options);

    void stop();
    void updateBandwidthLimit(int max_kbps);

Q_SIGNALS:
    void transferStarted();
    void fileStarted(const QString& fileId, const QString& path, qint64 size);
    void fileProgress(const QString& fileId, qint64 bytes, qint64 total);
    void overallProgress(qint64 bytes, qint64 total);
    void fileCompleted(const QString& fileId, const QString& path);
    void transferCompleted(bool success, const QString& message);
    void errorOccurred(const QString& message);

private:
    /// @brief Binary frame header for chunked data transfer
    struct FrameHeader {
        quint32 magic{0x53414B4E}; // SAKN
        quint8 version{1};
        quint8 frame_type{0};
        quint16 flags{0};
        quint32 chunk_id{0};
        quint32 payload_size{0};
        quint32 plain_size{0};
        quint32 crc32{0};
    };

    enum FrameType : quint8 {
        FrameFileHeader = 1,
        FrameDataChunk = 2,
        FrameFileEnd = 3,
        FrameTransferEnd = 4,
        FrameResumeInfo = 5
        ,FrameFileAck = 6
    };

    QByteArray serializeHeader(const FrameHeader& header) const;
    bool readHeader(QTcpSocket* socket, FrameHeader& header);
    QByteArray readExact(QTcpSocket* socket, qint64 size);

    bool sendFrame(QTcpSocket* socket, FrameType type, quint16 flags, quint32 chunkId,
                   const QByteArray& payload, quint32 plainSize, quint32 crc32);

    bool handleSender(QTcpSocket* socket, const QVector<TransferFileEntry>& files, const DataOptions& options);
    bool handleReceiver(QTcpSocket* socket, const DataOptions& options);

    /// @brief Sends the JSON file header frame for a single file.
    /// @return True if header was sent successfully.
    bool sendFileHeader(QTcpSocket* socket, const TransferFileEntry& file, const DataOptions& options);

    /// @brief Sends all data chunks for a single file with compression, encryption, and bandwidth throttling.
    /// @return True if all chunks were sent successfully.
    bool sendFileChunks(QTcpSocket* socket, QFile& source, const TransferFileEntry& file,
                       const DataOptions& options, const QByteArray& key,
                       const QVector<QPair<int, int>>& resumeRanges,
                       qint64& bytesSent, qint64 totalBytes,
                       QElapsedTimer& rateTimer, qint64& rateBytesSent);

    /// @brief Waits for and validates a file ACK frame from the receiver.
    /// @return True if ACK indicates success, false to retry or fail.
    bool awaitFileAck(QTcpSocket* socket, const TransferFileEntry& file);

    /// @brief State tracked across frames during a receive session
    struct ReceiverState {
        QByteArray key;
        QFile* current_file = nullptr;
        QString current_file_id;
        QString current_relative_path;
        QString current_checksum;
        QString current_acl_sddl;
        QString resume_path;
        qint64 current_size{0};
        int chunk_size{65536};
        qint64 bytes_received{0};
        qint64 overall_received{0};
        int total_chunks{0};
        QVector<QPair<int, int>> current_ranges;
    };

    bool processFileHeader(QTcpSocket* socket, const QByteArray& payload,
                           const DataOptions& options, ReceiverState& state);
    bool processDataChunk(QTcpSocket* socket, const FrameHeader& header,
                          const QByteArray& payload, const DataOptions& options,
                          ReceiverState& state);
    bool processFileEnd(QTcpSocket* socket, const DataOptions& options,
                        ReceiverState& state);

    QByteArray compressData(const QByteArray& data) const;
    QByteArray decompressData(const QByteArray& data) const;

    quint32 computeCrc32(const QByteArray& data) const;

    QByteArray buildResumePayload(const QString& fileId, const QVector<QPair<int,int>>& ranges, int totalChunks) const;
    QVector<QPair<int,int>> parseResumePayload(const QByteArray& payload, int& totalChunks) const;

    std::atomic<bool> m_stopRequested{false};
    std::atomic<int> m_dynamicMaxBandwidthKbps{0};
};

} // namespace sak
