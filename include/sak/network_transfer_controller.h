#pragma once

#include <QObject>
#include <QMap>
#include <QByteArray>
#include <QHostAddress>
#include <QThread>
#include <QTimer>

#include "sak/network_transfer_types.h"
#include "sak/orchestration_types.h"

namespace sak {

class PeerDiscoveryService;
class NetworkConnectionManager;
class NetworkTransferWorker;
class OrchestrationClient;
class OrchestrationDiscoveryService;

class NetworkTransferController : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        Idle,
        Source,
        Destination
    };

    explicit NetworkTransferController(QObject* parent = nullptr);
    ~NetworkTransferController() override;

    void configure(const TransferSettings& settings);
    TransferSettings settings() const;

    void startSource(const TransferManifest& manifest,
                     const QVector<TransferFileEntry>& files,
                     const TransferPeerInfo& peer,
                     const QString& passphrase);

    void startDestination(const QString& passphrase, const QString& destinationBase);

    void approveTransfer(bool approved);
    void stop();

    void pauseTransfer();
    void resumeTransfer();
    void cancelTransfer();

    void updateBandwidthLimit(int max_kbps);

    void startDiscovery(const QString& mode);
    void stopDiscovery();

    void connectToOrchestrator(const QHostAddress& host,
                               quint16 port,
                               const DestinationPC& destination);
    void disconnectFromOrchestrator();

    Mode mode() const;

Q_SIGNALS:
    void statusMessage(const QString& message);
    void errorMessage(const QString& message);
    void peerDiscovered(const TransferPeerInfo& peer);
    void connectionStateChanged(bool connected);
    void manifestReceived(const TransferManifest& manifest);
    void transferProgress(qint64 bytes, qint64 total);
    void transferCompleted(bool success, const QString& message);
    void orchestrationAssignmentReceived(const DeploymentAssignment& assignment);
    void orchestrationAssignmentPaused(const QString& job_id);
    void orchestrationAssignmentResumed(const QString& job_id);
    void orchestrationAssignmentCanceled(const QString& job_id);

private Q_SLOTS:
    void onDataReceived(const QByteArray& data);
    void onConnected();
    void onDisconnected();

private:
    void resetWorker();
    void startWorkerSender();
    void startWorkerReceiver();

    TransferSettings m_settings;
    Mode m_mode{Mode::Idle};

    PeerDiscoveryService* m_discovery{nullptr};
    NetworkConnectionManager* m_connection{nullptr};
    QTimer* m_heartbeatTimer{nullptr};

    QByteArray m_controlBuffer;

    TransferPeerInfo m_selectedPeer;
    TransferManifest m_manifest;
    QVector<TransferFileEntry> m_files;
    TransferManifest m_pendingManifest;

    bool m_authenticated{false};
    bool m_auth_required{false};

    QString m_passphrase;
    QString m_destinationBase;

    QByteArray m_salt;
    QByteArray m_auth_nonce;

    NetworkTransferWorker* m_worker{nullptr};
    QThread* m_workerThread{nullptr};

    OrchestrationClient* m_orchestratorClient{nullptr};
    OrchestrationDiscoveryService* m_orchestratorDiscovery{nullptr};
    QString m_orchestratorDeploymentId;
    QString m_orchestratorDestinationId;
    QString m_orchestratorJobId;
    bool m_pendingApprove{false};
    bool m_transferPaused{false};
};

} // namespace sak
