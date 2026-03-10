// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/config_manager.h"
#include "sak/file_hash.h"
#include "sak/file_scanner.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/mapping_engine.h"
#include "sak/migration_orchestrator.h"
#include "sak/network_constants.h"
#include "sak/network_transfer_controller.h"
#include "sak/network_transfer_panel.h"
#include "sak/parallel_transfer_manager.h"
#include "sak/path_utils.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/permission_manager.h"
#include "sak/smart_file_filter.h"
#include "sak/version.h"
#include "sak/widget_helpers.h"
#include "sak/windows_user_scanner.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkInterface>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QtConcurrent>
#include <QTextEdit>
#include <QTime>
#include <QUuid>
#include <QVBoxLayout>

#include <filesystem>

namespace sak {

void NetworkTransferPanel::onOrchestrationAssignment(const DeploymentAssignment& assignment) {
    if (!assignment.job_id.isEmpty()) {
        m_assignmentStatusByJob[assignment.job_id] = tr("queued");
        m_assignmentEventByJob[assignment.job_id] = tr("Received assignment");
    }

    if (m_destinationTransferActive || !m_activeAssignment.deployment_id.isEmpty()) {
        m_assignmentQueue.enqueue(assignment);
        refreshAssignmentQueue();
        refreshAssignmentStatus();
        persistAssignmentQueue();
        Q_EMIT logOutput(tr("Queued assignment %1 for %2")
                             .arg(assignment.deployment_id, assignment.source_user));
        return;
    }

    activateAssignment(assignment);
}

void NetworkTransferPanel::onStartOrchestratorServer() {
    Q_ASSERT(m_orchestratorListenPortSpin);
    Q_ASSERT(m_orchestrator);
    if (!m_orchestrator) {
        return;
    }

    if (!m_orchestratorServerRunning) {
        const auto port = static_cast<quint16>(m_orchestratorListenPortSpin->value());
        if (!m_orchestrator->startServer(port)) {
            sak::logWarning("Failed to start orchestration server.");
            QMessageBox::warning(this,
                                 tr("Orchestrator Error"),
                                 tr("Failed to start orchestration server."));
            return;
        }
        m_orchestrator->startHealthPolling(10'000);
        m_orchestrator->startDiscovery(m_settings.discovery_port);
        m_orchestratorServerRunning = true;
        m_orchestratorListenButton->setText(tr("Stop Server"));
        m_orchestratorStatusLabel->setText(tr("Listening on %1").arg(port));
        Q_EMIT logOutput(tr("Orchestrator server started on port %1").arg(port));
    } else {
        m_orchestrator->stopHealthPolling();
        m_orchestrator->stopDiscovery();
        m_orchestrator->stopServer();
        m_orchestratorServerRunning = false;
        m_orchestratorListenButton->setText(tr("Start Server"));
        m_orchestratorStatusLabel->setText(tr("Stopped"));
        Q_EMIT logOutput(tr("Orchestrator server stopped"));
    }
}

void NetworkTransferPanel::onScanOrchestratorUsers() {
    Q_ASSERT(m_orchestratorUserTable);
    Q_ASSERT(m_userScanner);
    m_users = m_userScanner->scanUsers();
    m_orchestratorUserTable->setRowCount(0);

    for (int i = 0; i < m_users.size(); ++i) {
        const auto& user = m_users[i];
        m_orchestratorUserTable->insertRow(i);

        auto* selectItem = new QTableWidgetItem();
        selectItem->setCheckState(Qt::Checked);
        m_orchestratorUserTable->setItem(i, 0, selectItem);

        auto* userItem = new QTableWidgetItem(user.username);
        userItem->setData(Qt::UserRole, i);
        m_orchestratorUserTable->setItem(i, 1, userItem);
        m_orchestratorUserTable->setItem(
            i, 2, new QTableWidgetItem(formatBytes(user.total_size_estimated)));
    }

    Q_EMIT logOutput(tr("Scanned %1 users for deployment").arg(m_users.size()));
}

void NetworkTransferPanel::onStartDeployment() {
    Q_ASSERT(m_mappingEngine);
    Q_ASSERT(m_orchestrator);
    if (!m_parallelManager || !m_orchestrator) {
        return;
    }

    auto mapping = buildDeploymentMapping();
    if (mapping.sources.isEmpty() || mapping.destinations.isEmpty()) {
        sak::logWarning("Deployment Error: Select source profiles and destinations first.");
        QMessageBox::warning(this,
                             tr("Deployment Error"),
                             tr("Select source profiles and destinations first."));
        return;
    }

    if (mapping.deployment_id.isEmpty()) {
        mapping.deployment_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    QString validationError;
    if (!m_mappingEngine->validateMapping(mapping, validationError)) {
        sak::logWarning(validationError.toStdString());
        QMessageBox::warning(this, tr("Deployment Error"), validationError);
        return;
    }

    if (!m_mappingEngine->checkDestinationReadiness(mapping)) {
        sak::logWarning("Deployment Error: One or more destinations are not ready.");
        QMessageBox::warning(this,
                             tr("Deployment Error"),
                             tr("One or more destinations are not ready."));
        return;
    }

    if (!m_mappingEngine->checkDiskSpace(mapping)) {
        sak::logWarning("Deployment Error: Insufficient disk space on one or more destinations.");
        QMessageBox::warning(this,
                             tr("Deployment Error"),
                             tr("Insufficient disk space on one or more destinations."));
        return;
    }

    m_activeDeploymentId = mapping.deployment_id;
    m_deploymentStartedAt = QDateTime::currentDateTimeUtc();

    auto& config = ConfigManager::instance();
    config.setValue("orchestration/last_deployment_id", m_activeDeploymentId);
    config.setValue("orchestration/last_deployment_started",
                    m_deploymentStartedAt.toString(Qt::ISODate));
    config.setValue("orchestration/mapping_type", m_mappingTypeCombo->currentIndex());
    config.setValue("orchestration/mapping_strategy", m_mappingStrategyCombo->currentIndex());
    config.setValue("orchestration/max_concurrent", m_maxConcurrentSpin->value());
    config.setValue("orchestration/global_bandwidth", m_globalBandwidthSpin->value());
    config.setValue("orchestration/per_job_bandwidth", m_perJobBandwidthSpin->value());
    config.setValue("orchestration/use_template", m_useTemplateCheck->isChecked());
    m_orchestrator->setMappingStrategy(m_mappingStrategyCombo->currentIndex() == 0
                                           ? MappingEngine::Strategy::LargestFree
                                           : MappingEngine::Strategy::RoundRobin);
    m_parallelManager->setMaxConcurrentTransfers(m_maxConcurrentSpin->value());
    m_parallelManager->setGlobalBandwidthLimit(m_globalBandwidthSpin->value());
    m_parallelManager->setPerJobBandwidthLimit(m_perJobBandwidthSpin->value());

    m_destinationToJobId.clear();
    m_jobToDestinationId.clear();
    m_jobToDeploymentId.clear();
    m_knownJobIds.clear();

    m_parallelManager->startDeployment(mapping);
    Q_EMIT logOutput(tr("Deployment %1 started").arg(mapping.deployment_id));
}

void NetworkTransferPanel::onPauseDeployment() {
    if (m_parallelManager) {
        m_parallelManager->pauseDeployment();
    }
}

void NetworkTransferPanel::onResumeDeployment() {
    if (m_parallelManager) {
        m_parallelManager->resumeDeployment();
    }
}

void NetworkTransferPanel::onCancelDeployment() {
    if (m_parallelManager) {
        m_parallelManager->cancelDeployment();
    }
}

void NetworkTransferPanel::onSaveDeploymentTemplate() {
    Q_ASSERT(m_mappingEngine);
    Q_ASSERT(m_templateStatusLabel);
    auto mapping = buildDeploymentMapping();
    if (mapping.sources.isEmpty() || mapping.destinations.isEmpty()) {
        sak::logWarning("Template Error: Select sources and destinations first.");
        QMessageBox::warning(this,
                             tr("Template Error"),
                             tr("Select sources and destinations first."));
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("Save Template"), QDir::homePath(), tr("JSON Files (*.json)"));
    if (filePath.isEmpty()) {
        return;
    }

    if (!m_mappingEngine->saveTemplate(mapping, filePath)) {
        sak::logWarning("Template Error: Failed to save template.");
        QMessageBox::warning(this, tr("Template Error"), tr("Failed to save template."));
        return;
    }

    m_loadedTemplatePath = filePath;
    m_templateStatusLabel->setText(tr("Template saved: %1").arg(QFileInfo(filePath).fileName()));
    ConfigManager::instance().setValue("orchestration/last_template_path", filePath);
}

void NetworkTransferPanel::onLoadDeploymentTemplate() {
    Q_ASSERT(m_mappingEngine);
    Q_ASSERT(m_templateStatusLabel);
    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("Load Template"), QDir::homePath(), tr("JSON Files (*.json)"));
    if (filePath.isEmpty()) {
        return;
    }

    m_loadedMapping = m_mappingEngine->loadTemplate(filePath);
    if (m_loadedMapping.sources.isEmpty() || m_loadedMapping.destinations.isEmpty()) {
        sak::logWarning("Template Error: Template is invalid or empty.");
        QMessageBox::warning(this, tr("Template Error"), tr("Template is invalid or empty."));
        return;
    }

    m_loadedTemplatePath = filePath;
    m_templateStatusLabel->setText(tr("Loaded template: %1").arg(QFileInfo(filePath).fileName()));
    m_useTemplateCheck->setChecked(true);
    ConfigManager::instance().setValue("orchestration/last_template_path", filePath);
}

void NetworkTransferPanel::onOrchestratorDestinationRegistered(const DestinationPC& destination) {
    if (!destination.destination_id.isEmpty()) {
        m_destinationStatusHistory[destination.destination_id].append(tr("Registered"));
    }
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorDestinationUpdated(const DestinationPC& destination) {
    if (!destination.destination_id.isEmpty()) {
        m_destinationStatusHistory[destination.destination_id].append(
            tr("Updated: %1").arg(destination.status));
    }
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorDestinationRemoved(const QString& destination_id) {
    if (!destination_id.isEmpty()) {
        m_destinationStatusHistory[destination_id].append(tr("Removed"));
    }
    m_destinationProgress.remove(destination_id);
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorProgress(const DeploymentProgress& progress) {
    Q_ASSERT(!m_destinationProgress.isEmpty());
    Q_ASSERT(m_parallelManager);
    if (!progress.destination_id.isEmpty()) {
        m_destinationProgress.insert(progress.destination_id, progress.progress_percent);
        m_destinationStatusHistory[progress.destination_id].append(
            tr("Progress %1%").arg(progress.progress_percent));
    }

    QString jobId = progress.job_id;
    if (jobId.isEmpty()) {
        jobId = m_destinationToJobId.value(progress.destination_id);
    }

    if (!jobId.isEmpty() && m_parallelManager) {
        m_parallelManager->updateJobProgress(jobId,
                                             progress.progress_percent,
                                             progress.bytes_transferred,
                                             progress.bytes_total,
                                             progress.transfer_speed_mbps,
                                             progress.current_file);
    }

    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorCompletion(const DeploymentCompletion& completion) {
    Q_ASSERT(m_parallelManager);
    if (!completion.destination_id.isEmpty()) {
        m_destinationStatusHistory[completion.destination_id].append(
            tr("Completed: %1").arg(completion.status));
    }
    QString jobId = completion.job_id;
    if (jobId.isEmpty()) {
        jobId = m_destinationToJobId.value(completion.destination_id);
    }
    if (!jobId.isEmpty() && m_parallelManager) {
        const bool success = completion.status == "success";
        m_parallelManager->markJobComplete(jobId, success, success ? QString() : completion.status);
    }
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onJobStartRequested(const QString& job_id,
                                               const MappingEngine::SourceProfile& source,
                                               const DestinationPC& destination) {
    if (!m_orchestrator) {
        return;
    }

    DeploymentAssignment assignment;
    assignment.deployment_id = m_activeDeploymentId;
    assignment.job_id = job_id;
    assignment.source_user = source.username;
    assignment.profile_size_bytes = source.profile_size_bytes;
    assignment.priority = "normal";
    if (m_perJobBandwidthSpin && m_perJobBandwidthSpin->value() > 0) {
        assignment.max_bandwidth_kbps = m_perJobBandwidthSpin->value() * sak::kBytesPerKB;
    }

    m_destinationToJobId.insert(destination.destination_id, job_id);
    m_jobToDestinationId.insert(job_id, destination.destination_id);
    m_jobToDeploymentId.insert(job_id, assignment.deployment_id);
    m_knownJobIds.insert(job_id);
    if (!destination.destination_id.isEmpty()) {
        m_destinationStatusHistory[destination.destination_id].append(
            tr("Job started: %1").arg(job_id));
    }

    m_orchestrator->assignDeploymentToDestination(destination.destination_id,
                                                  assignment,
                                                  assignment.profile_size_bytes);

    const auto it =
        std::find_if(m_users.begin(), m_users.end(), [&source](const UserProfile& user) {
            return user.username == source.username;
        });

    if (it == m_users.end()) {
        if (m_parallelManager) {
            m_parallelManager->markJobComplete(job_id, false, tr("Source user not found"));
        }
        refreshJobsTable();
        return;
    }

    if (m_settings.encryption_enabled && m_passphraseEdit->text().isEmpty()) {
        if (m_parallelManager) {
            m_parallelManager->markJobComplete(job_id,
                                               false,
                                               tr("Missing passphrase for encrypted transfer"));
        }
        refreshJobsTable();
        return;
    }

    QVector<UserProfile> selectedUsers{*it};
    const auto files = buildFileListForUsers(selectedUsers);
    const auto manifest = buildManifestPayloadForUsers(files, selectedUsers);

    TransferPeerInfo peer;
    peer.ip_address = destination.ip_address;
    peer.control_port = destination.control_port;
    peer.data_port = destination.data_port;
    peer.hostname = destination.hostname;

    auto* controller = new NetworkTransferController(this);
    TransferSettings settings = m_settings;
    settings.control_port = destination.control_port;
    settings.data_port = destination.data_port;
    settings.max_bandwidth_kbps = assignment.max_bandwidth_kbps > 0 ? assignment.max_bandwidth_kbps
                                                                    : settings.max_bandwidth_kbps;
    controller->configure(settings);

    connect(controller,
            &NetworkTransferController::transferCompleted,
            this,
            [this, job_id, controller](bool success, const QString& message) {
                if (m_parallelManager) {
                    m_parallelManager->markJobComplete(job_id, success, message);
                    refreshJobsTable();
                }
                controller->deleteLater();
                m_jobSourceControllers.remove(job_id);
            });

    m_jobSourceControllers.insert(job_id, controller);
    controller->startSource(manifest, files, peer, m_passphraseEdit->text());

    refreshJobsTable();
}

void NetworkTransferPanel::onJobUpdated(const QString& job_id, int progress_percent) {
    Q_UNUSED(progress_percent);
    m_knownJobIds.insert(job_id);
    refreshJobsTable();
}

void NetworkTransferPanel::onJobCompleted(const QString& job_id,
                                          bool success,
                                          const QString& error_message) {
    m_knownJobIds.insert(job_id);
    refreshJobsTable();

    if (success) {
        Q_EMIT logOutput(tr("Job %1 completed successfully.").arg(job_id));
    } else {
        Q_EMIT logOutput(tr("Job %1 failed: %2").arg(job_id, error_message));
        Q_EMIT statusMessage(tr("Transfer job failed: %1").arg(error_message), 5000);
    }
}

void NetworkTransferPanel::onAggregateProgress(int completed, int total, int percent) {
    if (m_deploymentSummaryLabel) {
        m_deploymentSummaryLabel->setText(tr("%1 of %2 complete").arg(completed).arg(total));
    }
    Q_EMIT progressUpdate(percent, 100);
}

void NetworkTransferPanel::onParallelDeploymentProgress(int completed, int total) {
    const int percent = total > 0 ? static_cast<int>((completed * 100) / total) : 0;
    onAggregateProgress(completed, total, percent);
}

void NetworkTransferPanel::onPauseJob() {
    Q_ASSERT(m_jobsTable);
    Q_ASSERT(m_parallelManager);
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->pauseJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onResumeJob() {
    Q_ASSERT(m_jobsTable);
    Q_ASSERT(m_parallelManager);
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->resumeJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onRetryJob() {
    Q_ASSERT(m_jobsTable);
    Q_ASSERT(m_parallelManager);
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->retryJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onCancelJob() {
    Q_ASSERT(m_jobsTable);
    Q_ASSERT(m_parallelManager);
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->cancelJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onExportDeploymentHistory() {
    Q_ASSERT(m_historyManager);
    if (!m_historyManager) {
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("Export Deployment History"), QDir::homePath(), tr("CSV Files (*.csv)"));
    if (filePath.isEmpty()) {
        return;
    }

    if (!m_historyManager->exportCsv(filePath)) {
        sak::logWarning("Export Error: Failed to export deployment history.");
        QMessageBox::warning(this, tr("Export Error"), tr("Failed to export deployment history."));
        return;
    }

    Q_EMIT logOutput(tr("Deployment history exported to %1").arg(filePath));
}

void NetworkTransferPanel::onExportDeploymentSummaryCsv() {
    Q_ASSERT(m_orchestrator);
    Q_ASSERT(m_parallelManager);
    if (!m_parallelManager || !m_orchestrator || !m_orchestrator->registry()) {
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("Export Deployment Summary"), QDir::homePath(), tr("CSV Files (*.csv)"));
    if (filePath.isEmpty()) {
        return;
    }

    QVector<DeploymentJobSummary> jobs;
    for (const auto& job : m_parallelManager->allJobs()) {
        DeploymentJobSummary summary;
        summary.job_id = job.job_id;
        summary.source_user = job.source.username;
        summary.destination_id = job.destination.destination_id;
        summary.status = job.status;
        summary.bytes_transferred = job.bytes_transferred;
        summary.total_bytes = job.total_bytes;
        summary.error_message = job.error_message;
        jobs.push_back(summary);
    }

    QVector<DeploymentDestinationSummary> destinations;
    const auto registryDestinations = m_orchestrator->registry()->destinations();
    for (const auto& destination : registryDestinations) {
        DeploymentDestinationSummary summary;
        summary.destination_id = destination.destination_id;
        summary.hostname = destination.hostname;
        summary.ip_address = destination.ip_address;
        summary.status = destination.status;
        summary.progress_percent = m_destinationProgress.value(destination.destination_id, 0);
        summary.last_seen = destination.last_seen;
        summary.status_events = m_destinationStatusHistory.value(destination.destination_id);
        destinations.push_back(summary);
    }

    const QDateTime completedAt = QDateTime::currentDateTimeUtc();
    if (!DeploymentSummaryReport::exportCsv(filePath,
                                            m_activeDeploymentId,
                                            m_deploymentStartedAt,
                                            completedAt,
                                            jobs,
                                            destinations)) {
        sak::logWarning("Export Error: Failed to export deployment summary.");
        QMessageBox::warning(this, tr("Export Error"), tr("Failed to export deployment summary."));
        return;
    }

    Q_EMIT logOutput(tr("Deployment summary exported to %1").arg(filePath));
}

void NetworkTransferPanel::onExportDeploymentSummaryPdf() {
    Q_ASSERT(m_orchestrator);
    Q_ASSERT(m_parallelManager);
    if (!m_parallelManager || !m_orchestrator || !m_orchestrator->registry()) {
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("Export Deployment Summary"), QDir::homePath(), tr("PDF Files (*.pdf)"));
    if (filePath.isEmpty()) {
        return;
    }

    QVector<DeploymentJobSummary> jobs;
    for (const auto& job : m_parallelManager->allJobs()) {
        DeploymentJobSummary summary;
        summary.job_id = job.job_id;
        summary.source_user = job.source.username;
        summary.destination_id = job.destination.destination_id;
        summary.status = job.status;
        summary.bytes_transferred = job.bytes_transferred;
        summary.total_bytes = job.total_bytes;
        summary.error_message = job.error_message;
        jobs.push_back(summary);
    }

    QVector<DeploymentDestinationSummary> destinations;
    const auto registryDestinations = m_orchestrator->registry()->destinations();
    for (const auto& destination : registryDestinations) {
        DeploymentDestinationSummary summary;
        summary.destination_id = destination.destination_id;
        summary.hostname = destination.hostname;
        summary.ip_address = destination.ip_address;
        summary.status = destination.status;
        summary.progress_percent = m_destinationProgress.value(destination.destination_id, 0);
        summary.last_seen = destination.last_seen;
        summary.status_events = m_destinationStatusHistory.value(destination.destination_id);
        destinations.push_back(summary);
    }

    const QDateTime completedAt = QDateTime::currentDateTimeUtc();
    if (!DeploymentSummaryReport::exportPdf(filePath,
                                            m_activeDeploymentId,
                                            m_deploymentStartedAt,
                                            completedAt,
                                            jobs,
                                            destinations)) {
        sak::logWarning("Export Error: Failed to export deployment summary.");
        QMessageBox::warning(this, tr("Export Error"), tr("Failed to export deployment summary."));
        return;
    }

    Q_EMIT logOutput(tr("Deployment summary exported to %1").arg(filePath));
}

void NetworkTransferPanel::onRecoverLastDeployment() {
    Q_ASSERT(m_mappingTypeCombo);
    Q_ASSERT(m_mappingStrategyCombo);
    auto& config = ConfigManager::instance();
    const QString deploymentId = config.getValue("orchestration/last_deployment_id").toString();
    const QString status = config.getValue("orchestration/last_deployment_status").toString();
    const QString startedAt = config.getValue("orchestration/last_deployment_started").toString();
    const QString completedAt =
        config.getValue("orchestration/last_deployment_completed").toString();

    if (deploymentId.isEmpty()) {
        QMessageBox::information(this,
                                 tr("Recover Deployment"),
                                 tr("No previous deployment state found."));
        return;
    }

    m_activeDeploymentId = deploymentId;
    if (!startedAt.isEmpty()) {
        m_deploymentStartedAt = QDateTime::fromString(startedAt, Qt::ISODate);
    }

    m_mappingTypeCombo->setCurrentIndex(config.getValue("orchestration/mapping_type", 0).toInt());
    m_mappingStrategyCombo->setCurrentIndex(
        config.getValue("orchestration/mapping_strategy", 0).toInt());
    m_maxConcurrentSpin->setValue(
        config.getValue("orchestration/max_concurrent", sak::kMaxConcurrentScrape).toInt());
    m_globalBandwidthSpin->setValue(config.getValue("orchestration/global_bandwidth", 0).toInt());
    m_perJobBandwidthSpin->setValue(config.getValue("orchestration/per_job_bandwidth", 0).toInt());
    m_useTemplateCheck->setChecked(config.getValue("orchestration/use_template", false).toBool());

    const QString templatePath = config.getValue("orchestration/last_template_path").toString();
    if (!templatePath.isEmpty() && QFileInfo::exists(templatePath)) {
        m_loadedMapping = m_mappingEngine->loadTemplate(templatePath);
        if (!m_loadedMapping.sources.isEmpty()) {
            m_loadedTemplatePath = templatePath;
            m_templateStatusLabel->setText(
                tr("Loaded template: %1").arg(QFileInfo(templatePath).fileName()));
        }
    }

    refreshDeploymentHistory();

    Q_EMIT logOutput(tr("Recovered deployment %1 (status: %2, started: %3, completed: %4)")
                         .arg(deploymentId)
                         .arg(status.isEmpty() ? tr("unknown") : status)
                         .arg(startedAt.isEmpty() ? tr("n/a") : startedAt)
                         .arg(completedAt.isEmpty() ? tr("n/a") : completedAt));
}

void NetworkTransferPanel::onParallelDeploymentCompleted(const QString& deployment_id,
                                                         bool success) {
    Q_UNUSED(deployment_id);

    if (!m_historyManager || !m_parallelManager) {
        return;
    }

    DeploymentHistoryEntry entry;
    entry.deployment_id = m_activeDeploymentId.isEmpty() ? deployment_id : m_activeDeploymentId;
    entry.started_at = m_deploymentStartedAt;
    entry.completed_at = QDateTime::currentDateTimeUtc();
    entry.total_jobs = m_parallelManager->totalJobs();
    entry.completed_jobs = m_parallelManager->completedJobs();
    entry.failed_jobs = m_parallelManager->failedJobs();
    entry.status = success ? "success" : "failed";
    entry.template_path = m_loadedTemplatePath;

    m_historyManager->appendEntry(entry);

    auto& config = ConfigManager::instance();
    config.setValue("orchestration/last_deployment_completed",
                    entry.completed_at.toString(Qt::ISODate));
    config.setValue("orchestration/last_deployment_status", entry.status);

    Q_EMIT logOutput(tr("Deployment %1 %2. %3/%4 complete, %5 failed.")
                         .arg(entry.deployment_id)
                         .arg(success ? tr("completed") : tr("failed"))
                         .arg(entry.completed_jobs)
                         .arg(entry.total_jobs)
                         .arg(entry.failed_jobs));
    refreshDeploymentHistory();
}

void NetworkTransferPanel::refreshOrchestratorDestinations() {
    Q_ASSERT(m_orchestrator);
    Q_ASSERT(m_orchestratorDestTable);
    if (!m_orchestrator || !m_orchestrator->registry() || !m_orchestratorDestTable) {
        return;
    }

    const auto destinations = m_orchestrator->registry()->destinations();
    m_orchestratorDestTable->setRowCount(0);

    int row = 0;
    for (const auto& destination : destinations) {
        m_orchestratorDestTable->insertRow(row);

        auto* selectItem = new QTableWidgetItem();
        selectItem->setCheckState(Qt::Checked);
        selectItem->setData(Qt::UserRole, destination.destination_id);
        m_orchestratorDestTable->setItem(row, 0, selectItem);

        auto* hostItem = new QTableWidgetItem(destination.hostname);
        hostItem->setData(Qt::UserRole, destination.destination_id);
        m_orchestratorDestTable->setItem(row, 1, hostItem);
        m_orchestratorDestTable->setItem(row, 2, new QTableWidgetItem(destination.ip_address));
        auto* statusItem = new QTableWidgetItem(destination.status);
        applyStatusColors(statusItem, statusColor(destination.status));
        m_orchestratorDestTable->setItem(row, 3, statusItem);
        m_orchestratorDestTable->setItem(
            row, 4, new QTableWidgetItem(formatBytes(destination.health.free_disk_bytes)));
        m_orchestratorDestTable->setItem(
            row, 5, new QTableWidgetItem(QString::number(destination.health.cpu_usage_percent)));
        m_orchestratorDestTable->setItem(
            row, 6, new QTableWidgetItem(QString::number(destination.health.ram_usage_percent)));
        m_orchestratorDestTable->setItem(
            row, 7, new QTableWidgetItem(destination.last_seen.toString("hh:mm:ss")));

        const int progress = m_destinationProgress.value(destination.destination_id, 0);
        auto* progressItem = new QTableWidgetItem(QString::number(progress) + "%");
        applyStatusColors(progressItem, progressColor(progress));
        m_orchestratorDestTable->setItem(row, 8, progressItem);
        row++;
    }
}

void NetworkTransferPanel::refreshJobsTable() {
    Q_ASSERT(m_jobsTable);
    Q_ASSERT(m_parallelManager);
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    m_jobsTable->setRowCount(0);
    int row = 0;
    qint64 remainingBytes = 0;
    double totalSpeedMbps = 0.0;
    for (const auto& jobId : m_knownJobIds) {
        const auto job = m_parallelManager->getJobStatus(jobId);
        if (job.job_id.isEmpty()) {
            continue;
        }

        m_jobsTable->insertRow(row);
        m_jobsTable->setItem(row, 0, new QTableWidgetItem(job.job_id));
        m_jobsTable->setItem(row, 1, new QTableWidgetItem(m_jobToDeploymentId.value(jobId)));
        m_jobsTable->setItem(row, 2, new QTableWidgetItem(job.source.username));
        m_jobsTable->setItem(row, 3, new QTableWidgetItem(job.destination.destination_id));
        auto* statusItem = new QTableWidgetItem(job.status);
        applyStatusColors(statusItem, statusColor(job.status));
        m_jobsTable->setItem(row, 4, statusItem);

        int percent = 0;
        if (job.total_bytes > 0) {
            percent = static_cast<int>((job.bytes_transferred * 100) / job.total_bytes);
        }
        auto* progressItem = new QTableWidgetItem(QString::number(percent) + "%");
        applyStatusColors(progressItem, progressColor(percent));
        m_jobsTable->setItem(row, 5, progressItem);
        auto* errorItem = new QTableWidgetItem(job.error_message);
        if (!job.error_message.isEmpty()) {
            applyStatusColors(errorItem, QColor(198, 40, 40));
        }
        m_jobsTable->setItem(row, 6, errorItem);
        row++;

        if (job.total_bytes > 0 && job.bytes_transferred < job.total_bytes) {
            remainingBytes += (job.total_bytes - job.bytes_transferred);
        }
        if (job.speed_mbps > 0.0 && job.status == "transferring") {
            totalSpeedMbps += job.speed_mbps;
        }
    }

    if (m_deploymentEtaLabel) {
        if (remainingBytes > 0 && totalSpeedMbps > 0.0) {
            const double bytesPerSecond = (totalSpeedMbps * sak::kBytesPerMBf) / 8.0;
            const qint64 etaSeconds = static_cast<qint64>(remainingBytes / bytesPerSecond);
            const QTime etaTime(0, 0, 0);
            m_deploymentEtaLabel->setText(tr("ETA: %1").arg(
                etaTime.addSecs(static_cast<int>(etaSeconds)).toString("hh:mm:ss")));
        } else {
            m_deploymentEtaLabel->setText(tr("ETA: --"));
        }
    }
}

MappingEngine::DeploymentMapping NetworkTransferPanel::buildDeploymentMapping() {
    Q_ASSERT(m_useTemplateCheck);
    Q_ASSERT(m_mappingTypeCombo);
    if (m_useTemplateCheck && m_useTemplateCheck->isChecked() &&
        !m_loadedMapping.sources.isEmpty()) {
        return m_loadedMapping;
    }

    MappingEngine::DeploymentMapping mapping;
    mapping.deployment_id = m_activeDeploymentId;

    mapping.sources = collectSelectedSources();
    mapping.destinations = collectSelectedDestinations();

    const int mappingTypeIndex = m_mappingTypeCombo->currentIndex();
    if (mappingTypeIndex == 1) {
        mapping.type = MappingEngine::MappingType::ManyToMany;
    } else if (mappingTypeIndex == 2) {
        mapping.type = MappingEngine::MappingType::CustomMapping;
    } else {
        mapping.type = MappingEngine::MappingType::OneToMany;
    }

    if (mapping.type == MappingEngine::MappingType::CustomMapping) {
        mapping.custom_rules = collectCustomMappingRules();
    }

    return mapping;
}

/// @brief Return the first non-loopback IPv4 address from one interface
static QString firstIpv4Address(const QNetworkInterface& iface) {
    for (const auto& entry : iface.addressEntries()) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
            return entry.ip().toString();
        }
    }
    return {};
}

/// @brief Discover the local machine's IPv4 address
static QString findLocalIpAddress() {
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        QString ip = firstIpv4Address(iface);
        if (!ip.isEmpty()) {
            return ip;
        }
    }
    return {};
}

QVector<MappingEngine::SourceProfile> NetworkTransferPanel::collectSelectedSources() {
    Q_ASSERT(!m_users.isEmpty());
    Q_ASSERT(m_orchestratorUserTable);
    QVector<MappingEngine::SourceProfile> sources;

    const QString localIp = findLocalIpAddress();

    for (int row = 0; row < m_orchestratorUserTable->rowCount(); ++row) {
        auto* selectItem = m_orchestratorUserTable->item(row, 0);
        if (!selectItem || selectItem->checkState() != Qt::Checked) {
            continue;
        }
        auto* userItem = m_orchestratorUserTable->item(row, 1);
        if (!userItem) {
            continue;
        }
        const int index = userItem->data(Qt::UserRole).toInt();
        if (index < 0 || index >= m_users.size()) {
            continue;
        }

        const auto& user = m_users[index];
        MappingEngine::SourceProfile source;
        source.username = user.username;
        source.source_hostname = QHostInfo::localHostName();
        source.source_ip = localIp;
        source.profile_size_bytes = user.total_size_estimated;
        sources.push_back(source);
    }
    return sources;
}

QVector<DestinationPC> NetworkTransferPanel::collectSelectedDestinations() {
    Q_ASSERT(m_orchestrator);
    Q_ASSERT(m_orchestratorDestTable);
    QVector<DestinationPC> destinations;

    QMap<QString, DestinationPC> destinationMap;
    if (m_orchestrator && m_orchestrator->registry()) {
        const auto dests = m_orchestrator->registry()->destinations();
        for (const auto& dest : dests) {
            destinationMap.insert(dest.destination_id, dest);
        }
    }

    for (int row = 0; row < m_orchestratorDestTable->rowCount(); ++row) {
        auto* selectItem = m_orchestratorDestTable->item(row, 0);
        if (!selectItem || selectItem->checkState() != Qt::Checked) {
            continue;
        }

        const QString destinationId = selectItem->data(Qt::UserRole).toString();
        if (destinationId.isEmpty() || !destinationMap.contains(destinationId)) {
            continue;
        }
        destinations.push_back(destinationMap.value(destinationId));
    }
    return destinations;
}

QMap<QString, QString> NetworkTransferPanel::collectCustomMappingRules() {
    Q_ASSERT(m_customRulesTable);
    QMap<QString, QString> rules;
    for (int row = 0; row < m_customRulesTable->rowCount(); ++row) {
        auto* sourceItem = m_customRulesTable->item(row, 0);
        auto* destinationItem = m_customRulesTable->item(row, 1);
        if (!sourceItem || !destinationItem) {
            continue;
        }
        const QString sourceUser = sourceItem->text().trimmed();
        const QString destinationId = destinationItem->text().trimmed();
        if (!sourceUser.isEmpty() && !destinationId.isEmpty()) {
            rules.insert(sourceUser, destinationId);
        }
    }
    return rules;
}

void NetworkTransferPanel::refreshAssignmentQueue() {
    Q_ASSERT(m_assignmentQueueTable);
    if (!m_assignmentQueueTable) {
        return;
    }

    m_assignmentQueueTable->setRowCount(0);
    int row = 0;
    for (const auto& assignment : m_assignmentQueue) {
        m_assignmentQueueTable->insertRow(row);
        m_assignmentQueueTable->setItem(row, 0, new QTableWidgetItem(assignment.deployment_id));
        m_assignmentQueueTable->setItem(row, 1, new QTableWidgetItem(assignment.job_id));
        m_assignmentQueueTable->setItem(row, 2, new QTableWidgetItem(assignment.source_user));
        m_assignmentQueueTable->setItem(
            row, 3, new QTableWidgetItem(formatBytes(assignment.profile_size_bytes)));
        m_assignmentQueueTable->setItem(row, 4, new QTableWidgetItem(assignment.priority));
        const QString bandwidthText = assignment.max_bandwidth_kbps > 0
                                          ? tr("%1 KB/s").arg(assignment.max_bandwidth_kbps)
                                          : tr("default");
        m_assignmentQueueTable->setItem(row, 5, new QTableWidgetItem(bandwidthText));
        row++;
    }
}

bool NetworkTransferPanel::handleDragEnterEvent(QDragEnterEvent* dragEvent) {
    if (extractDraggedUserName(dragEvent->mimeData()).isEmpty()) {
        return false;
    }
    dragEvent->acceptProposedAction();
    return true;
}

bool NetworkTransferPanel::handleDropEvent(QDropEvent* dropEvent) {
    Q_ASSERT(m_orchestratorDestTable);
    Q_ASSERT(dropEvent);
    const QString user = extractDraggedUserName(dropEvent->mimeData());
    if (user.isEmpty()) {
        return false;
    }

    const QPoint pos = dropEvent->position().toPoint();
    auto* item = m_orchestratorDestTable->itemAt(pos);
    int row = item ? item->row() : m_orchestratorDestTable->currentRow();
    if (row < 0) {
        return false;
    }

    const QString destinationId = destinationIdForRow(row);
    if (destinationId.isEmpty()) {
        return false;
    }

    auto* selectItem = m_orchestratorDestTable->item(row, 0);
    if (selectItem && selectItem->checkState() != Qt::Checked) {
        selectItem->setCheckState(Qt::Checked);
    }

    upsertCustomRule(user, destinationId);
    dropEvent->acceptProposedAction();
    return true;
}

bool NetworkTransferPanel::eventFilter(QObject* obj, QEvent* event) {
    Q_ASSERT(obj);
    if (obj != m_orchestratorDestTable) {
        return QWidget::eventFilter(obj, event);
    }

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        return handleDragEnterEvent(static_cast<QDragEnterEvent*>(event));
    }
    if (event->type() == QEvent::Drop) {
        return handleDropEvent(static_cast<QDropEvent*>(event));
    }

    return QWidget::eventFilter(obj, event);
}

void NetworkTransferPanel::upsertCustomRule(const QString& sourceUser,
                                            const QString& destinationId) {
    if (!m_customRulesTable || sourceUser.isEmpty() || destinationId.isEmpty()) {
        return;
    }

    for (int row = 0; row < m_customRulesTable->rowCount(); ++row) {
        auto* sourceItem = m_customRulesTable->item(row, 0);
        if (!sourceItem || sourceItem->text().trimmed() != sourceUser) {
            continue;
        }

        if (!m_customRulesTable->item(row, 1)) {
            m_customRulesTable->setItem(row, 1, new QTableWidgetItem());
        }
        m_customRulesTable->item(row, 1)->setText(destinationId);
        if (m_mappingTypeCombo) {
            m_mappingTypeCombo->setCurrentIndex(2);
        }
        return;
    }

    const int row = m_customRulesTable->rowCount();
    m_customRulesTable->insertRow(row);
    m_customRulesTable->setItem(row, 0, new QTableWidgetItem(sourceUser));
    m_customRulesTable->setItem(row, 1, new QTableWidgetItem(destinationId));
    if (m_mappingTypeCombo) {
        m_mappingTypeCombo->setCurrentIndex(2);
    }
}

QString NetworkTransferPanel::destinationIdForRow(int row) const {
    Q_ASSERT(m_orchestratorDestTable);
    Q_ASSERT(row >= 0);
    if (!m_orchestratorDestTable || row < 0) {
        return {};
    }

    auto* selectItem = m_orchestratorDestTable->item(row, 0);
    if (selectItem) {
        const QString storedId = selectItem->data(Qt::UserRole).toString();
        if (!storedId.isEmpty()) {
            return storedId;
        }
    }

    auto* hostItem = m_orchestratorDestTable->item(row, 1);
    if (hostItem) {
        return hostItem->text().trimmed();
    }

    return {};
}

QString NetworkTransferPanel::extractDraggedUserName(const QMimeData* mime) const {
    Q_ASSERT(mime);
    if (!mime || !mime->hasFormat("application/x-qabstractitemmodeldatalist")) {
        return {};
    }

    QByteArray encoded = mime->data("application/x-qabstractitemmodeldatalist");
    QDataStream stream(&encoded, QIODevice::ReadOnly);
    while (!stream.atEnd()) {
        int row = 0;
        int column = 0;
        QMap<int, QVariant> roleDataMap;
        stream >> row >> column >> roleDataMap;
        if (column == 1 && roleDataMap.contains(Qt::DisplayRole)) {
            return roleDataMap.value(Qt::DisplayRole).toString().trimmed();
        }
    }

    return {};
}

void NetworkTransferPanel::refreshAssignmentStatus() {
    Q_ASSERT(m_assignmentStatusTable);
    if (!m_assignmentStatusTable) {
        return;
    }

    m_assignmentStatusTable->setRowCount(0);
    int row = 0;

    auto addRow = [this, &row](const DeploymentAssignment& assignment,
                               const QString& status,
                               const QString& eventText) {
        m_assignmentStatusTable->insertRow(row);
        m_assignmentStatusTable->setItem(row, 0, new QTableWidgetItem(assignment.deployment_id));
        m_assignmentStatusTable->setItem(row, 1, new QTableWidgetItem(assignment.job_id));
        m_assignmentStatusTable->setItem(row, 2, new QTableWidgetItem(assignment.source_user));
        auto* statusItem = new QTableWidgetItem(status);
        applyStatusColors(statusItem, statusColor(status));
        m_assignmentStatusTable->setItem(row, 3, statusItem);
        m_assignmentStatusTable->setItem(row, 4, new QTableWidgetItem(eventText));
        row++;
    };

    if (!m_activeAssignment.deployment_id.isEmpty()) {
        const QString status = m_assignmentStatusByJob.value(m_activeAssignment.job_id,
                                                             tr("active"));
        const QString eventText = m_assignmentEventByJob.value(m_activeAssignment.job_id,
                                                               tr("Active"));
        addRow(m_activeAssignment, status, eventText);
    }

    for (const auto& assignment : m_assignmentQueue) {
        const QString status = m_assignmentStatusByJob.value(assignment.job_id, tr("queued"));
        const QString eventText = m_assignmentEventByJob.value(assignment.job_id, tr("Queued"));
        addRow(assignment, status, eventText);
    }
}

void NetworkTransferPanel::persistAssignmentQueue() const {
    if (m_assignmentQueueStore) {
        m_assignmentQueueStore->save(
            m_activeAssignment, m_assignmentQueue, m_assignmentStatusByJob, m_assignmentEventByJob);
    }
}

void NetworkTransferPanel::refreshDeploymentHistory() {
    Q_ASSERT(m_historyManager);
    Q_ASSERT(m_historyTable);
    if (!m_historyManager || !m_historyTable) {
        return;
    }

    const auto entries = m_historyManager->loadEntries();
    m_historyTable->setRowCount(0);
    int row = 0;
    for (const auto& entry : entries) {
        m_historyTable->insertRow(row);
        m_historyTable->setItem(row, 0, new QTableWidgetItem(entry.deployment_id));
        m_historyTable->setItem(
            row, 1, new QTableWidgetItem(entry.started_at.toString("yyyy-MM-dd hh:mm:ss")));
        m_historyTable->setItem(
            row, 2, new QTableWidgetItem(entry.completed_at.toString("yyyy-MM-dd hh:mm:ss")));
        m_historyTable->setItem(row, 3, new QTableWidgetItem(QString::number(entry.total_jobs)));
        m_historyTable->setItem(row,
                                4,
                                new QTableWidgetItem(QString::number(entry.completed_jobs)));
        m_historyTable->setItem(row, 5, new QTableWidgetItem(QString::number(entry.failed_jobs)));
        m_historyTable->setItem(row, 6, new QTableWidgetItem(entry.status));
        row++;
    }
}

QString NetworkTransferPanel::destinationBase() const {
    return m_destinationBaseEdit->text().trimmed();
}

}  // namespace sak
