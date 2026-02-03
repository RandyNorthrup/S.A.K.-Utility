#include "sak/network_transfer_controller.h"

#include "sak/peer_discovery_service.h"
#include "sak/network_connection_manager.h"
#include "sak/network_transfer_worker.h"
#include "sak/network_transfer_protocol.h"
#include "sak/network_transfer_security.h"
#include "sak/orchestration_client.h"
#include "sak/orchestration_discovery_service.h"
#include "sak/logger.h"

#include <QJsonObject>
#include <QJsonDocument>
#include <QHostAddress>
#include <QHostInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QNetworkInterface>

namespace sak {

namespace {
QString firstNonLoopbackIPv4() {
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto& iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp)
            || !(iface.flags() & QNetworkInterface::IsRunning)
            || (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                return entry.ip().toString();
            }
        }
    }
    return QHostAddress(QHostAddress::LocalHost).toString();
}
}

NetworkTransferController::NetworkTransferController(QObject* parent)
    : QObject(parent)
    , m_discovery(new PeerDiscoveryService(this))
    , m_connection(new NetworkConnectionManager(this))
    , m_heartbeatTimer(new QTimer(this))
    , m_orchestratorClient(new OrchestrationClient(this))
    , m_orchestratorDiscovery(new OrchestrationDiscoveryService(this))
{
    connect(m_discovery, &PeerDiscoveryService::peerDiscovered, this, &NetworkTransferController::peerDiscovered);
    connect(m_discovery, &PeerDiscoveryService::discoveryError, this, [this](const QString& msg) {
        Q_EMIT errorMessage(msg);
    });

    connect(m_connection, &NetworkConnectionManager::dataReceived, this, &NetworkTransferController::onDataReceived);
    connect(m_connection, &NetworkConnectionManager::connected, this, &NetworkTransferController::onConnected);
    connect(m_connection, &NetworkConnectionManager::disconnected, this, &NetworkTransferController::onDisconnected);
    connect(m_connection, &NetworkConnectionManager::connectionError, this, [this](const QString& msg) {
        Q_EMIT errorMessage(msg);
    });

    m_heartbeatTimer->setInterval(5000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (m_connection->socket()) {
            TransferProtocol::writeMessage(m_connection->socket(),
                TransferProtocol::makeMessage(TransferMessageType::Heartbeat));
        }
    });

    connect(m_orchestratorClient, &OrchestrationClient::assignmentReceived, this, [this](const DeploymentAssignment& assignment) {
        m_orchestratorDeploymentId = assignment.deployment_id;
        m_orchestratorJobId = assignment.job_id;
        Q_EMIT orchestrationAssignmentReceived(assignment);
        Q_EMIT statusMessage(tr("Orchestration assignment received for %1").arg(assignment.source_user));

        if (!m_connection->isServerRunning() && !m_destinationBase.isEmpty() && !m_passphrase.isEmpty()) {
            startDestination(m_passphrase, m_destinationBase);
        }
    });

    connect(m_orchestratorClient, &OrchestrationClient::assignmentPaused, this,
        [this](const QString& deployment_id, const QString& job_id) {
            if (!m_orchestratorDeploymentId.isEmpty() && deployment_id != m_orchestratorDeploymentId) {
                return;
            }
            if (!m_orchestratorJobId.isEmpty() && !job_id.isEmpty() && job_id != m_orchestratorJobId) {
                return;
            }
            Q_EMIT orchestrationAssignmentPaused(job_id);
            pauseTransfer();
        });

    connect(m_orchestratorClient, &OrchestrationClient::assignmentResumed, this,
        [this](const QString& deployment_id, const QString& job_id) {
            if (!m_orchestratorDeploymentId.isEmpty() && deployment_id != m_orchestratorDeploymentId) {
                return;
            }
            if (!m_orchestratorJobId.isEmpty() && !job_id.isEmpty() && job_id != m_orchestratorJobId) {
                return;
            }
            Q_EMIT orchestrationAssignmentResumed(job_id);
            resumeTransfer();
        });

    connect(m_orchestratorClient, &OrchestrationClient::assignmentCanceled, this,
        [this](const QString& deployment_id, const QString& job_id) {
            if (!m_orchestratorDeploymentId.isEmpty() && deployment_id != m_orchestratorDeploymentId) {
                return;
            }
            if (!m_orchestratorJobId.isEmpty() && !job_id.isEmpty() && job_id != m_orchestratorJobId) {
                return;
            }
            Q_EMIT orchestrationAssignmentCanceled(job_id);
            cancelTransfer();
        });

    connect(m_orchestratorClient, &OrchestrationClient::connectionError, this, &NetworkTransferController::errorMessage);
    connect(m_orchestratorClient, &OrchestrationClient::statusMessage, this, &NetworkTransferController::statusMessage);

    connect(m_orchestratorDiscovery, &OrchestrationDiscoveryService::orchestratorDiscovered, this,
        [this](const QHostAddress& address, quint16 port) {
            if (m_mode != Mode::Destination || !m_settings.auto_discovery_enabled) {
                return;
            }

            if (m_orchestratorClient->isConnected()) {
                return;
            }

            m_orchestratorClient->setAutoReconnectEnabled(true);
            m_orchestratorClient->connectToServer(address, port);
            Q_EMIT statusMessage(tr("Discovered orchestrator at %1:%2").arg(address.toString()).arg(port));
        });
}

NetworkTransferController::~NetworkTransferController() {
    stop();
}

void NetworkTransferController::configure(const TransferSettings& settings) {
    m_settings = settings;
}

TransferSettings NetworkTransferController::settings() const {
    return m_settings;
}

void NetworkTransferController::startSource(const TransferManifest& manifest,
                                            const QVector<TransferFileEntry>& files,
                                            const TransferPeerInfo& peer,
                                            const QString& passphrase) {
    stop();
    m_mode = Mode::Source;
    m_manifest = manifest;
    m_files = files;
    m_selectedPeer = peer;
    m_passphrase = passphrase;
    m_authenticated = false;
    m_auth_required = m_settings.encryption_enabled;

    log_info("NetworkTransferController startSource to {}:{} with {} files", peer.ip_address.toStdString(),
             m_settings.control_port, files.size());

    if (m_settings.encryption_enabled) {
        m_salt = TransferSecurityManager::generateRandomBytes(16);
    }

    if (m_settings.auto_discovery_enabled) {
        TransferPeerInfo info;
        info.peer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        info.hostname = QHostInfo::localHostName();
        info.os = "Windows";
        info.app_version = "";
        info.mode = "source";
        info.control_port = m_settings.control_port;
        info.data_port = m_settings.data_port;
        info.capabilities = {"user_profiles", "resume"};
        info.last_seen = QDateTime::currentDateTime();

        m_discovery->setPeerInfo(info);
        m_discovery->setPort(m_settings.discovery_port);
        m_discovery->start();
    }

    m_connection->connectToHost(QHostAddress(peer.ip_address), m_settings.control_port);
}

void NetworkTransferController::startDestination(const QString& passphrase, const QString& destinationBase) {
    stop();
    m_mode = Mode::Destination;
    m_passphrase = passphrase;
    m_destinationBase = destinationBase;
    m_authenticated = false;
    m_auth_required = m_settings.encryption_enabled;

    log_info("NetworkTransferController startDestination on port {}", m_settings.control_port);

    TransferPeerInfo info;
    info.peer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    info.hostname = QHostInfo::localHostName();
    info.os = "Windows";
    info.app_version = "";
    info.mode = "destination";
    info.control_port = m_settings.control_port;
    info.data_port = m_settings.data_port;
    info.capabilities = {"user_profiles", "resume"};
    info.last_seen = QDateTime::currentDateTime();

    if (m_settings.auto_discovery_enabled) {
        m_discovery->setPeerInfo(info);
        m_discovery->setPort(m_settings.discovery_port);
        m_discovery->start();
    }

    if (m_settings.auto_discovery_enabled) {
        DestinationPC destination;
        destination.destination_id = QHostInfo::localHostName();
        destination.hostname = QHostInfo::localHostName();
        destination.ip_address = firstNonLoopbackIPv4();
        destination.control_port = m_settings.control_port;
        destination.data_port = m_settings.data_port;
        destination.status = "ready";
        destination.last_seen = QDateTime::currentDateTimeUtc();
        m_orchestratorClient->setDestinationInfo(destination);
        m_orchestratorDiscovery->setDestinationInfo(destination);
        m_orchestratorDiscovery->setPort(m_settings.discovery_port);
        m_orchestratorDiscovery->startAsDestination();
    }

    m_connection->startServer(m_settings.control_port);
    Q_EMIT statusMessage(tr("Waiting for incoming connections"));
}

void NetworkTransferController::approveTransfer(bool approved) {
    if (m_mode != Mode::Destination) {
        return;
    }

    if (!approved) {
        log_warning("NetworkTransferController transfer rejected by user");
        auto message = TransferProtocol::makeMessage(TransferMessageType::TransferReject, {
            {"reason", "Rejected by user"}
        });
        TransferProtocol::writeMessage(m_connection->socket(), message);
        return;
    }

    if (!m_connection->socket() || m_connection->socket()->state() != QAbstractSocket::ConnectedState) {
        Q_EMIT errorMessage(tr("No active control connection to approve transfer."));
        return;
    }

    // Prepare worker before approving
    m_pendingApprove = true;
    startWorkerReceiver();
}

void NetworkTransferController::stop() {
    if (m_mode != Mode::Idle) {
        log_info("NetworkTransferController stop requested");
    }
    if (m_discovery->isRunning()) {
        m_discovery->stop();
    }
    if (m_orchestratorDiscovery && m_orchestratorDiscovery->isRunning()) {
        m_orchestratorDiscovery->stop();
    }

    m_connection->disconnectFromHost();
    m_connection->stopServer();

    resetWorker();
    m_mode = Mode::Idle;
    m_pendingApprove = false;
    m_transferPaused = false;
}

void NetworkTransferController::pauseTransfer() {
    if (m_transferPaused) {
        return;
    }

    m_transferPaused = true;
    resetWorker();

    if (m_mode == Mode::Source) {
        m_connection->disconnectFromHost();
    }

    Q_EMIT statusMessage(tr("Transfer paused"));
}

void NetworkTransferController::resumeTransfer() {
    if (!m_transferPaused) {
        return;
    }

    m_transferPaused = false;

    if (m_mode == Mode::Source) {
        if (!m_manifest.transfer_id.isEmpty() && !m_selectedPeer.ip_address.isEmpty()) {
            startSource(m_manifest, m_files, m_selectedPeer, m_passphrase);
        }
    } else if (m_mode == Mode::Destination) {
        if (!m_pendingManifest.transfer_id.isEmpty()) {
            m_pendingApprove = true;
            startWorkerReceiver();
        }
    }

    Q_EMIT statusMessage(tr("Transfer resumed"));
}

void NetworkTransferController::cancelTransfer() {
    m_transferPaused = false;
    resetWorker();

    if (m_mode == Mode::Source) {
        stop();
    } else if (m_mode == Mode::Destination) {
        m_pendingManifest = TransferManifest{};
        m_pendingApprove = false;
        m_orchestratorDeploymentId.clear();
        m_orchestratorJobId.clear();
        Q_EMIT statusMessage(tr("Transfer canceled"));
    }
}

void NetworkTransferController::updateBandwidthLimit(int max_kbps) {
    m_settings.max_bandwidth_kbps = qMax(0, max_kbps);
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [this]() {
            if (m_worker) {
                m_worker->updateBandwidthLimit(m_settings.max_bandwidth_kbps);
            }
        }, Qt::QueuedConnection);
    }
}

void NetworkTransferController::startDiscovery(const QString& mode) {
    TransferPeerInfo info;
    info.peer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    info.hostname = QHostInfo::localHostName();
    info.os = "Windows";
    info.app_version = "";
    info.mode = mode;
    info.control_port = m_settings.control_port;
    info.data_port = m_settings.data_port;
    info.capabilities = {"user_profiles", "resume"};
    info.last_seen = QDateTime::currentDateTime();

    m_discovery->setPeerInfo(info);
    m_discovery->setPort(m_settings.discovery_port);
    m_discovery->start();
    log_info("NetworkTransferController discovery started in mode {}", mode.toStdString());
}

void NetworkTransferController::stopDiscovery() {
    if (m_discovery->isRunning()) {
        m_discovery->stop();
        log_info("NetworkTransferController discovery stopped");
    }
}

void NetworkTransferController::connectToOrchestrator(const QHostAddress& host,
                                                      quint16 port,
                                                      const DestinationPC& destination) {
    m_orchestratorDestinationId = destination.destination_id;
    m_orchestratorClient->setDestinationInfo(destination);
    m_orchestratorClient->setAutoReconnectEnabled(true);
    m_orchestratorClient->connectToServer(host, port);
}

void NetworkTransferController::disconnectFromOrchestrator() {
    m_orchestratorClient->setAutoReconnectEnabled(false);
    m_orchestratorClient->disconnectFromServer();
}

NetworkTransferController::Mode NetworkTransferController::mode() const {
    return m_mode;
}

void NetworkTransferController::onConnected() {
    Q_EMIT connectionStateChanged(true);
    m_heartbeatTimer->start();
    log_info("NetworkTransferController control channel connected");

    QJsonObject helloPayload;
    helloPayload["peer_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    helloPayload["hostname"] = QHostInfo::localHostName();
    helloPayload["capabilities"] = QJsonArray{"user_profiles", "resume"};
    helloPayload["encryption"] = m_settings.encryption_enabled;
    if (m_settings.encryption_enabled) {
        helloPayload["salt"] = QString::fromUtf8(m_salt.toBase64());
    }

    TransferProtocol::writeMessage(m_connection->socket(), TransferProtocol::makeMessage(TransferMessageType::Hello, helloPayload));

    if (m_mode == Mode::Source && !m_auth_required) {
        m_authenticated = true;
        QJsonObject manifestPayload = m_manifest.toJson(false);
        manifestPayload["files_count"] = static_cast<int>(m_files.size());
        TransferProtocol::writeMessage(m_connection->socket(), TransferProtocol::makeMessage(TransferMessageType::TransferManifest, manifestPayload));
        Q_EMIT statusMessage(tr("Manifest sent. Awaiting approval..."));
    }
}

void NetworkTransferController::onDisconnected() {
    Q_EMIT connectionStateChanged(false);
    m_heartbeatTimer->stop();
    log_info("NetworkTransferController control channel disconnected");
}

void NetworkTransferController::onDataReceived(const QByteArray& data) {
    auto messages = TransferProtocol::readMessages(m_controlBuffer, data);
    for (const auto& message : messages) {
        const auto type = TransferProtocol::parseType(message.value("message_type").toString());
        if (!type.has_value()) {
            log_warning("NetworkTransferController received unknown message type");
            continue;
        }

        switch (*type) {
            case TransferMessageType::Hello: {
                if (message.contains("salt")) {
                    m_salt = QByteArray::fromBase64(message.value("salt").toString().toUtf8());
                }
                Q_EMIT statusMessage(tr("Handshake completed"));

                if (m_mode == Mode::Destination && m_auth_required) {
                    m_auth_nonce = TransferSecurityManager::generateRandomBytes(16);
                    QJsonObject challenge;
                    challenge["nonce"] = QString::fromUtf8(m_auth_nonce.toBase64());
                    TransferProtocol::writeMessage(m_connection->socket(),
                        TransferProtocol::makeMessage(TransferMessageType::AuthChallenge, challenge));
                }
                break;
            }
            case TransferMessageType::AuthChallenge: {
                if (!m_auth_required) {
                    break;
                }
                const auto nonce = QByteArray::fromBase64(message.value("nonce").toString().toUtf8());
                auto keyResult = TransferSecurityManager::deriveKey(m_passphrase, m_salt);
                if (!keyResult) {
                    Q_EMIT errorMessage(tr("Failed to derive authentication key"));
                    break;
                }
                QByteArray authData = nonce + *keyResult;
                QByteArray digest = QCryptographicHash::hash(authData, QCryptographicHash::Sha256);

                QJsonObject response;
                response["response"] = QString::fromUtf8(digest.toBase64());
                TransferProtocol::writeMessage(m_connection->socket(),
                    TransferProtocol::makeMessage(TransferMessageType::AuthResponse, response));
                break;
            }
            case TransferMessageType::AuthResponse: {
                if (!m_auth_required) {
                    break;
                }

                if (message.contains("status")) {
                    const auto status = message.value("status").toString();
                    if (status == "ok") {
                        m_authenticated = true;
                        if (m_mode == Mode::Source) {
                            QJsonObject manifestPayload = m_manifest.toJson(false);
                            manifestPayload["files_count"] = static_cast<int>(m_files.size());
                            TransferProtocol::writeMessage(m_connection->socket(),
                                TransferProtocol::makeMessage(TransferMessageType::TransferManifest, manifestPayload));
                            Q_EMIT statusMessage(tr("Manifest sent. Awaiting approval..."));
                        }
                    }
                    break;
                }

                if (m_mode == Mode::Destination) {
                    const auto response = QByteArray::fromBase64(message.value("response").toString().toUtf8());
                    auto keyResult = TransferSecurityManager::deriveKey(m_passphrase, m_salt);
                    if (!keyResult) {
                        Q_EMIT errorMessage(tr("Failed to derive authentication key"));
                        break;
                    }
                    QByteArray expected = QCryptographicHash::hash(m_auth_nonce + *keyResult, QCryptographicHash::Sha256);

                    if (response != expected) {
                        TransferProtocol::writeMessage(m_connection->socket(),
                            TransferProtocol::makeMessage(TransferMessageType::Error, { {"error", "Authentication failed"} }));
                        Q_EMIT errorMessage(tr("Authentication failed"));
                        break;
                    }

                    m_authenticated = true;
                    TransferProtocol::writeMessage(m_connection->socket(),
                        TransferProtocol::makeMessage(TransferMessageType::AuthResponse, { {"status", "ok"} }));
                    Q_EMIT statusMessage(tr("Authentication successful"));
                }
                break;
            }
            case TransferMessageType::TransferManifest: {
                if (m_mode == Mode::Destination) {
                    if (m_auth_required && !m_authenticated) {
                        Q_EMIT errorMessage(tr("Manifest received before authentication"));
                        break;
                    }
                    m_pendingManifest = TransferManifest::fromJson(message);
                    Q_EMIT manifestReceived(m_pendingManifest);
                    Q_EMIT statusMessage(tr("Transfer manifest received"));
                }
                break;
            }
            case TransferMessageType::TransferApprove: {
                if (m_mode == Mode::Source) {
                    Q_EMIT statusMessage(tr("Transfer approved. Starting data transfer..."));
                    startWorkerSender();
                }
                break;
            }
            case TransferMessageType::TransferReject: {
                Q_EMIT errorMessage(tr("Transfer rejected: %1").arg(message.value("reason").toString()));
                break;
            }
            case TransferMessageType::Error: {
                Q_EMIT errorMessage(message.value("error").toString());
                break;
            }
            case TransferMessageType::Heartbeat:
            default:
                break;
        }
    }
}

void NetworkTransferController::resetWorker() {
    if (m_worker) {
        m_worker->stop();
    }

    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
        m_workerThread->deleteLater();
        m_workerThread = nullptr;
    }

    m_worker = nullptr;
}

void NetworkTransferController::startWorkerSender() {
    resetWorker();

    m_workerThread = new QThread(this);
    m_worker = new NetworkTransferWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &NetworkTransferWorker::overallProgress, this, &NetworkTransferController::transferProgress);
    connect(m_worker, &NetworkTransferWorker::transferCompleted, this, &NetworkTransferController::transferCompleted);
    connect(m_worker, &NetworkTransferWorker::errorOccurred, this, &NetworkTransferController::errorMessage);

    m_workerThread->start();

    NetworkTransferWorker::DataOptions options;
    options.transfer_id = m_manifest.transfer_id;
    options.encryption_enabled = m_settings.encryption_enabled;
    options.compression_enabled = m_settings.compression_enabled;
    options.resume_enabled = m_settings.resume_enabled;
    options.chunk_size = m_settings.chunk_size;
    options.max_bandwidth_kbps = m_settings.max_bandwidth_kbps;
    options.passphrase = m_passphrase;
    options.salt = m_salt;
    options.total_bytes = m_manifest.total_bytes;

    QMetaObject::invokeMethod(m_worker, [this, options]() {
        m_worker->startSender(m_files, QHostAddress(m_selectedPeer.ip_address), m_settings.data_port, options);
    }, Qt::QueuedConnection);
}

void NetworkTransferController::startWorkerReceiver() {
    resetWorker();

    m_workerThread = new QThread(this);
    m_worker = new NetworkTransferWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &NetworkTransferWorker::overallProgress, this, &NetworkTransferController::transferProgress);
    connect(m_worker, &NetworkTransferWorker::transferCompleted, this, &NetworkTransferController::transferCompleted);
    connect(m_worker, &NetworkTransferWorker::errorOccurred, this, &NetworkTransferController::errorMessage);

    connect(m_worker, &NetworkTransferWorker::transferStarted, this, [this]() {
        if (!m_pendingApprove) {
            return;
        }
        auto message = TransferProtocol::makeMessage(TransferMessageType::TransferApprove, {
            {"data_port", static_cast<int>(m_settings.data_port)}
        });
        TransferProtocol::writeMessage(m_connection->socket(), message);
        log_info("NetworkTransferController transfer approved, waiting for data channel");
        Q_EMIT statusMessage(tr("Transfer approved. Awaiting data connection..."));
        m_pendingApprove = false;
    });

    connect(m_worker, &NetworkTransferWorker::overallProgress, this, [this](qint64 bytes, qint64 total) {
        if (m_orchestratorDeploymentId.isEmpty()) {
            return;
        }
        DeploymentProgress progress;
        progress.deployment_id = m_orchestratorDeploymentId;
        progress.job_id = m_orchestratorJobId;
        progress.destination_id = m_orchestratorDestinationId;
        progress.bytes_transferred = bytes;
        progress.bytes_total = total;
        if (total > 0) {
            progress.progress_percent = static_cast<int>((bytes * 100) / total);
        }
        m_orchestratorClient->sendProgress(progress);
    });

    connect(m_worker, &NetworkTransferWorker::transferCompleted, this, [this](bool success, const QString& message) {
        if (m_orchestratorDeploymentId.isEmpty()) {
            return;
        }
        DeploymentCompletion completion;
        completion.deployment_id = m_orchestratorDeploymentId;
        completion.job_id = m_orchestratorJobId;
        completion.destination_id = m_orchestratorDestinationId;
        completion.status = success ? "success" : "failed";
        completion.summary = QJsonObject{{"message", message}};
        m_orchestratorClient->sendCompletion(completion);
        m_pendingApprove = false;
    });

    m_workerThread->start();

    NetworkTransferWorker::DataOptions options;
    options.transfer_id = m_pendingManifest.transfer_id;
    options.encryption_enabled = m_settings.encryption_enabled;
    options.compression_enabled = m_settings.compression_enabled;
    options.resume_enabled = m_settings.resume_enabled;
    options.chunk_size = m_settings.chunk_size;
    options.max_bandwidth_kbps = m_settings.max_bandwidth_kbps;
    options.passphrase = m_passphrase;
    options.salt = m_salt;
    options.destination_base = m_destinationBase;
    options.total_bytes = m_pendingManifest.total_bytes;
    for (const auto& user : m_pendingManifest.users) {
        options.permission_modes.insert(user.username, user.permissions_mode);
    }
    for (const auto& file : m_pendingManifest.files) {
        if (!file.acl_sddl.isEmpty()) {
            options.acl_overrides.insert(file.relative_path, file.acl_sddl);
        }
    }

    QMetaObject::invokeMethod(m_worker, [this, options]() {
        m_worker->startReceiver(QHostAddress::AnyIPv4, m_settings.data_port, options);
    }, Qt::QueuedConnection);
}

} // namespace sak
