// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

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
class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QStackedWidget;
class QGroupBox;
class QMimeData;

namespace sak {

class WindowsUserScanner;
class NetworkTransferController;
class MigrationOrchestrator;
class ParallelTransferManager;
class MappingEngine;
class DetachableLogWindow;
class LogToggleSwitch;

/// @brief UI panel for peer-to-peer network data transfers
class NetworkTransferPanel : public QWidget {
    Q_OBJECT

public:
    explicit NetworkTransferPanel(QWidget* parent = nullptr);
    ~NetworkTransferPanel() override;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    /** @brief Switch between Source / Destination / Orchestrator modes */
    void onModeChanged(int index);
    /** @brief Scan the local machine for Windows user profiles */
    void onScanUsers();
    /** @brief Open the customization dialog for the selected user */
    void onCustomizeUser();
    /** @brief Broadcast a peer-discovery request on the LAN */
    void onDiscoverPeers();
    /** @brief Begin sending data as the source machine */
    void onStartSource();
    /** @brief Begin receiving data as the destination machine */
    void onStartDestination();
    /** @brief Approve an incoming transfer manifest */
    void onApproveTransfer();
    /** @brief Reject an incoming transfer manifest */
    void onRejectTransfer();
    /** @brief Connect to a remote orchestrator server */
    void onConnectOrchestrator();
    /** @brief Handle a deployment assignment received from orchestrator */
    void onOrchestrationAssignment(const DeploymentAssignment& assignment);
    /** @brief Start a local orchestrator server for multi-PC deployments */
    void onStartOrchestratorServer();
    /** @brief Scan user profiles on the orchestrator source machine */
    void onScanOrchestratorUsers();
    /** @brief Launch the orchestrated deployment to all destinations */
    void onStartDeployment();
    /** @brief Pause the running deployment */
    void onPauseDeployment();
    /** @brief Resume a previously paused deployment */
    void onResumeDeployment();
    /** @brief Cancel the deployment and clean up */
    void onCancelDeployment();
    /** @brief Persist the current mapping configuration as a template */
    void onSaveDeploymentTemplate();
    /** @brief Load a previously saved deployment template */
    void onLoadDeploymentTemplate();
    /** @brief Insert or update a destination row when it registers */
    void onOrchestratorDestinationRegistered(const DestinationPC& destination);
    /** @brief Update a destination row with new metadata */
    void onOrchestratorDestinationUpdated(const DestinationPC& destination);
    /** @brief Remove a destination row when it disconnects */
    void onOrchestratorDestinationRemoved(const QString& destination_id);
    /** @brief Update progress display for orchestrated deployments */
    void onOrchestratorProgress(const DeploymentProgress& progress);
    /** @brief Handle orchestrated deployment completion */
    void onOrchestratorCompletion(const DeploymentCompletion& completion);
    /** @brief Start a single job transferring from source to destination */
    void onJobStartRequested(const QString& job_id,
                             const MappingEngine::SourceProfile& source,
                             const DestinationPC& destination);
    /** @brief Update the jobs table progress for a running job */
    void onJobUpdated(const QString& job_id, int progress_percent);
    /** @brief Handle a single job completing (success or failure) */
    void onJobCompleted(const QString& job_id, bool success, const QString& error_message);
    /** @brief Update the aggregate progress bar and ETA */
    void onAggregateProgress(int completed, int total, int percent);
    /** @brief Update the parallel deployment progress summary */
    void onParallelDeploymentProgress(int completed, int total);
    /** @brief Pause the selected job */
    void onPauseJob();
    /** @brief Resume the selected paused job */
    void onResumeJob();
    /** @brief Retry the selected failed job */
    void onRetryJob();
    /** @brief Cancel the selected running job */
    void onCancelJob();
    /** @brief Export full deployment history to JSON */
    void onExportDeploymentHistory();
    /** @brief Export deployment summary as CSV */
    void onExportDeploymentSummaryCsv();
    /** @brief Export deployment summary as PDF */
    void onExportDeploymentSummaryPdf();
    /** @brief Recover and resume the last interrupted deployment */
    void onRecoverLastDeployment();
    /** @brief Handle parallel deployment completion event */
    void onParallelDeploymentCompleted(const QString& deployment_id, bool success);
    /** @brief Handle a newly discovered LAN peer */
    void onPeerDiscovered(const TransferPeerInfo& peer);
    /** @brief Handle a received transfer manifest from a peer */
    void onManifestReceived(const TransferManifest& manifest);
    /** @brief Update byte-level transfer progress */
    void onTransferProgress(qint64 bytes, qint64 total);
    /** @brief Handle direct peer-to-peer transfer completion */
    void onTransferCompleted(bool success, const QString& message);
    /** @brief Open the security settings dialog */
    void onSecuritySettings();
    /** @brief Open the network settings dialog */
    void onNetworkSettings();

private:
    /** @brief Build the panel layout with mode selector and stacked pages */
    void setupUi();
    /** @brief Wire signals/slots between widgets and backend objects */
    void setupConnections();
    /** @brief Load persisted transfer settings from QSettings */
    void loadSettings();
    /** @brief Build a transfer manifest from the selected user profiles */
    void buildManifest();
    /** @brief Enumerate files to transfer for all selected users */
    QVector<TransferFileEntry> buildFileList();
    /** @brief Enumerate files for a specific set of user profiles */
    QVector<TransferFileEntry> buildFileListForUsers(const QVector<UserProfile>& users);
    /** @brief Create a manifest payload from the enumerated file list */
    TransferManifest buildManifestPayload(const QVector<TransferFileEntry>& files);
    /** @brief Create a manifest payload for specific user profiles */
    TransferManifest buildManifestPayloadForUsers(const QVector<TransferFileEntry>& files,
                                                  const QVector<UserProfile>& users);
    /** @brief Return the destination base directory from user input */
    QString destinationBase() const;
    /** @brief Reload the orchestrator destination table from server state */
    void refreshOrchestratorDestinations();
    /** @brief Reload the jobs table from the parallel transfer manager */
    void refreshJobsTable();
    /** @brief Build a deployment mapping from the current UI configuration */
    MappingEngine::DeploymentMapping buildDeploymentMapping();
    /** @brief Reload the deployment history table from the history manager */
    void refreshDeploymentHistory();
    /** @brief Reload the assignment queue table from the queue store */
    void refreshAssignmentQueue();
    /** @brief Reload the assignment status table with current job states */
    void refreshAssignmentStatus();
    /** @brief Activate a queued assignment for execution */
    void activateAssignment(const DeploymentAssignment& assignment);
    /** @brief Save the current assignment queue to persistent storage */
    void persistAssignmentQueue() const;
    /** @brief Update UI when orchestrator connection state changes */
    void onConnectionStateChanged(bool connected);
    /** @brief Enable or disable the transfer button based on current state */
    void updateTransferButton();
    /** @brief Toggle the pause/resume button label and icon */
    void updatePauseResumeButton();
    /** @brief Event filter for drag-and-drop user-to-destination mapping */
    bool eventFilter(QObject* obj, QEvent* event) override;
    /** @brief Insert or update a custom user-to-destination mapping rule */
    void upsertCustomRule(const QString& sourceUser, const QString& destinationId);
    /** @brief Extract the destination ID from a table row */
    QString destinationIdForRow(int row) const;
    /** @brief Parse a dragged user name from MIME data */
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
    QPushButton* m_transferButton{nullptr};
    QPushButton* m_pauseResumeButton{nullptr};
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
    QLabel* m_deploymentEtaLabel{nullptr};
    QPushButton* m_exportHistoryButton{nullptr};
    QPushButton* m_exportSummaryCsvButton{nullptr};
    QPushButton* m_exportSummaryPdfButton{nullptr};
    QPushButton* m_recoverDeploymentButton{nullptr};
    QTableWidget* m_historyTable{nullptr};

    // Common progress/log
    LogToggleSwitch* m_logToggle{nullptr};

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
    bool m_sourceTransferActive{false};
    bool m_sourceTransferPaused{false};
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
