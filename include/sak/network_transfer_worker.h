#pragma once

#include <QObject>
#include <QHostAddress>
#include <QVector>
#include <atomic>

class QTcpSocket;

#include "sak/network_transfer_types.h"

namespace sak {

class NetworkTransferWorker : public QObject {
    Q_OBJECT

public:
    enum class Role {
        Sender,
        Receiver
    };

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

    QByteArray compressData(const QByteArray& data) const;
    QByteArray decompressData(const QByteArray& data) const;

    quint32 computeCrc32(const QByteArray& data) const;

    QByteArray buildResumePayload(const QString& fileId, const QVector<QPair<int,int>>& ranges, int totalChunks) const;
    QVector<QPair<int,int>> parseResumePayload(const QByteArray& payload, int& totalChunks) const;

    bool m_stopRequested{false};
    std::atomic<int> m_dynamicMaxBandwidthKbps{0};
};

} // namespace sak
