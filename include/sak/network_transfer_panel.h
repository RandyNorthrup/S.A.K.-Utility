#pragma once

#include <QWidget>
#include <QVector>
#include <QMap>
#include <QSet>
#include <QQueue>
#include <memory>

#include "sak/user_profile_types.h"
#include "sak/network_transfer_types.h"
#include "sak/network_transfer_report.h"
#include "sak/user_profile_restore_worker.h"
#include "sak/mapping_engine.h"
#include "sak/deployment_history.h"
#include "sak/deployment_summary_report.h"
#include "sak/assignment_queue_store.h"

class QTableWidget;
class QPushButton;
class QTextEdit;
class QProgressBar;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QStackedWidget;
class QComboBox;
class QGroupBox;
class QMimeData;

namespace sak {

class WindowsUserScanner;
class NetworkTransferController;
class MigrationOrchestrator;
class ParallelTransferManager;
class MappingEngine;

class NetworkTransferPanel : public QWidget {
    Q_OBJECT

public:
    explicit NetworkTransferPanel(QWidget* parent = nullptr);
    ~NetworkTransferPanel() override;

Q_SIGNALS:
    void status_message(const QString& message, int timeout_ms);
    void progress_update(int current, int maximum);

private Q_SLOTS:
    void onModeChanged(int index);
    void onScanUsers();
    void onCustomizeUser();
    void onDiscoverPeers();
    void onStartSource();
    void onStartDestination();
    void onApproveTransfer();
    void onRejectTransfer();
    void onConnectOrchestrator();
    void onOrchestrationAssignment(const DeploymentAssignment& assignment);
    void onStartOrchestratorServer();
    void onScanOrchestratorUsers();
    void onStartDeployment();
    void onPauseDeployment();
    void onResumeDeployment();
    void onCancelDeployment();
    void onSaveDeploymentTemplate();
    void onLoadDeploymentTemplate();
    void onOrchestratorDestinationRegistered(const DestinationPC& destination);
    void onOrchestratorDestinationUpdated(const DestinationPC& destination);
    void onOrchestratorDestinationRemoved(const QString& destination_id);
    void onOrchestratorProgress(const DeploymentProgress& progress);
    void onOrchestratorCompletion(const DeploymentCompletion& completion);
    void onJobStartRequested(const QString& job_id,
                             const MappingEngine::SourceProfile& source,
                             const DestinationPC& destination);
    void onJobUpdated(const QString& job_id, int progress_percent);
    void onJobCompleted(const QString& job_id, bool success, const QString& error_message);
    void onAggregateProgress(int completed, int total, int percent);
    void onParallelDeploymentProgress(int completed, int total);
    void onPauseJob();
    void onResumeJob();
    void onRetryJob();
    void onCancelJob();
    void onExportDeploymentHistory();
    void onExportDeploymentSummaryCsv();
    void onExportDeploymentSummaryPdf();
    void onRecoverLastDeployment();
    void onParallelDeploymentCompleted(const QString& deployment_id, bool success);
    void onPeerDiscovered(const TransferPeerInfo& peer);
    void onManifestReceived(const TransferManifest& manifest);
    void onTransferProgress(qint64 bytes, qint64 total);
    void onTransferCompleted(bool success, const QString& message);

private:
    void setupUi();
    void setupConnections();
    void loadSettings();
    void buildManifest();
    QVector<TransferFileEntry> buildFileList();
    QVector<TransferFileEntry> buildFileListForUsers(const QVector<UserProfile>& users);
    TransferManifest buildManifestPayload(const QVector<TransferFileEntry>& files);
    TransferManifest buildManifestPayloadForUsers(const QVector<TransferFileEntry>& files,
                                                  const QVector<UserProfile>& users);
    QString destinationBase() const;
    void refreshOrchestratorDestinations();
    void refreshJobsTable();
    MappingEngine::DeploymentMapping buildDeploymentMapping();
    void refreshDeploymentHistory();
    void refreshAssignmentQueue();
    void refreshAssignmentStatus();
    void activateAssignment(const DeploymentAssignment& assignment);
    void persistAssignmentQueue() const;
    void onConnectionStateChanged(bool connected);
    bool eventFilter(QObject* obj, QEvent* event) override;
    void upsertCustomRule(const QString& sourceUser, const QString& destinationId);
    QString destinationIdForRow(int row) const;
    QString extractDraggedUserName(const QMimeData* mime) const;

    // UI elements
    QComboBox* m_modeCombo{nullptr};
    QStackedWidget* m_modeStack{nullptr};

    // Source UI
    QTableWidget* m_userTable{nullptr};
    QTableWidget* m_peerTable{nullptr};
    QPushButton* m_scanUsersButton{nullptr};
    QPushButton* m_customizeUserButton{nullptr};
    QPushButton* m_discoverPeersButton{nullptr};
    QPushButton* m_startSourceButton{nullptr};
    QLineEdit* m_manualIpEdit{nullptr};
    QSpinBox* m_manualPortSpin{nullptr};
    QLineEdit* m_passphraseEdit{nullptr};
    QLineEdit* m_destinationPassphraseEdit{nullptr};
    QCheckBox* m_encryptCheck{nullptr};
    QCheckBox* m_compressCheck{nullptr};
    QCheckBox* m_resumeCheck{nullptr};
    QSpinBox* m_chunkSizeSpin{nullptr};
    QSpinBox* m_bandwidthSpin{nullptr};
    QComboBox* m_permissionModeCombo{nullptr};

    // Destination UI
    QLabel* m_destinationInfo{nullptr};
    QLineEdit* m_destinationBaseEdit{nullptr};
    QPushButton* m_startDestinationButton{nullptr};
    QLineEdit* m_orchestratorHostEdit{nullptr};
    QSpinBox* m_orchestratorPortSpin{nullptr};
    QPushButton* m_connectOrchestratorButton{nullptr};
    QCheckBox* m_autoApproveOrchestratorCheck{nullptr};
    QCheckBox* m_applyRestoreCheck{nullptr};
    QTextEdit* m_manifestText{nullptr};
    QPushButton* m_approveButton{nullptr};
    QPushButton* m_rejectButton{nullptr};
    QLabel* m_activeAssignmentLabel{nullptr};
    QTableWidget* m_assignmentQueueTable{nullptr};
    QTableWidget* m_assignmentStatusTable{nullptr};
    QLabel* m_assignmentBandwidthLabel{nullptr};

    // Orchestrator UI
    QSpinBox* m_orchestratorListenPortSpin{nullptr};
    QPushButton* m_orchestratorListenButton{nullptr};
    QLabel* m_orchestratorStatusLabel{nullptr};
    QPushButton* m_orchestratorScanUsersButton{nullptr};
    QTableWidget* m_orchestratorUserTable{nullptr};
    QTableWidget* m_orchestratorDestTable{nullptr};
    QComboBox* m_mappingTypeCombo{nullptr};
    QComboBox* m_mappingStrategyCombo{nullptr};
    QSpinBox* m_maxConcurrentSpin{nullptr};
    QSpinBox* m_globalBandwidthSpin{nullptr};
    QSpinBox* m_perJobBandwidthSpin{nullptr};
    QCheckBox* m_useTemplateCheck{nullptr};
    QLabel* m_templateStatusLabel{nullptr};
    QPushButton* m_saveTemplateButton{nullptr};
    QPushButton* m_loadTemplateButton{nullptr};
    QPushButton* m_startDeploymentButton{nullptr};
    QPushButton* m_pauseDeploymentButton{nullptr};
    QPushButton* m_resumeDeploymentButton{nullptr};
    QPushButton* m_cancelDeploymentButton{nullptr};
    QTableWidget* m_customRulesTable{nullptr};
    QTableWidget* m_jobsTable{nullptr};
    QPushButton* m_pauseJobButton{nullptr};
    QPushButton* m_resumeJobButton{nullptr};
    QPushButton* m_retryJobButton{nullptr};
    QPushButton* m_cancelJobButton{nullptr};
    QLabel* m_deploymentSummaryLabel{nullptr};
    QProgressBar* m_deploymentProgressBar{nullptr};
    QLabel* m_deploymentEtaLabel{nullptr};
    QPushButton* m_exportHistoryButton{nullptr};
    QPushButton* m_exportSummaryCsvButton{nullptr};
    QPushButton* m_exportSummaryPdfButton{nullptr};
    QPushButton* m_recoverDeploymentButton{nullptr};
    QTableWidget* m_historyTable{nullptr};

    // Common progress/log
    QProgressBar* m_overallProgress{nullptr};
    QPushButton* m_stopTransferButton{nullptr};
    QTextEdit* m_logText{nullptr};

    QVector<UserProfile> m_users;
    QMap<QString, TransferPeerInfo> m_peers;

    std::unique_ptr<WindowsUserScanner> m_userScanner;
    NetworkTransferController* m_controller{nullptr};
    UserProfileRestoreWorker* m_restoreWorker{nullptr};
    QMap<QString, NetworkTransferController*> m_jobSourceControllers;
    MigrationOrchestrator* m_orchestrator{nullptr};
    ParallelTransferManager* m_parallelManager{nullptr};
    MappingEngine* m_mappingEngine{nullptr};
    std::unique_ptr<DeploymentHistoryManager> m_historyManager;

    TransferSettings m_settings;
    TransferManifest m_currentManifest;
    QVector<TransferFileEntry> m_currentFiles;

    QDateTime m_transferStarted;
    QStringList m_transferErrors;
    bool m_isSourceTransfer{false};
    bool m_orchestrationAssignmentPending{false};
    bool m_destinationTransferActive{false};
    bool m_manifestValidated{false};

    DeploymentAssignment m_activeAssignment;
    QQueue<DeploymentAssignment> m_assignmentQueue;
    std::unique_ptr<AssignmentQueueStore> m_assignmentQueueStore;
    QMap<QString, QString> m_assignmentStatusByJob;
    QMap<QString, QString> m_assignmentEventByJob;

    MappingEngine::DeploymentMapping m_loadedMapping;
    QMap<QString, QString> m_destinationToJobId;
    QMap<QString, QString> m_jobToDestinationId;
    QMap<QString, QString> m_jobToDeploymentId;
    QMap<QString, int> m_destinationProgress;
    QMap<QString, QStringList> m_destinationStatusHistory;
    QSet<QString> m_knownJobIds;
    bool m_orchestratorServerRunning{false};
    QString m_activeDeploymentId;
    QDateTime m_deploymentStartedAt;
    QString m_loadedTemplatePath;
};

} // namespace sak
