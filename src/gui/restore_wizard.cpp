// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/restore_wizard.h"
#include "sak/user_data_manager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QHeaderView>
#include <QDir>
#include <QTimer>

namespace sak {

// ============================================================================
// RestoreWizard
// ============================================================================

RestoreWizard::RestoreWizard(QWidget* parent)
    : QWizard(parent)
    , m_dataManager(std::make_shared<UserDataManager>())
{
    setWindowTitle("Restore Application Data");
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::HaveHelpButton, false);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setOption(QWizard::NoCancelButtonOnLastPage, true);
    
    setMinimumSize(750, 550);
    
    // Add pages
    setPage(PageWelcome, new RestoreWelcomePage(this));
    setPage(PageSelectBackup, new RestoreSelectBackupPage(m_dataManager, this));
    setPage(PageConfigure, new RestoreConfigurePage(this));
    setPage(PageProgress, new RestoreProgressPage(m_dataManager, this));
    
    setStartId(PageWelcome);
}

// ============================================================================
// RestoreWelcomePage
// ============================================================================

RestoreWelcomePage::RestoreWelcomePage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Welcome to the Restore Wizard");
    setSubTitle("This wizard will guide you through restoring your application data.");
    
    setupUI();
}

void RestoreWelcomePage::setupUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    
    m_titleLabel = new QLabel("<h2>Restore Application Data</h2>", this);
    layout->addWidget(m_titleLabel);
    
    m_descriptionLabel = new QLabel(
        "<p>This wizard will help you:</p>"
        "<ul>"
        "<li>Browse and select backup archives</li>"
        "<li>Verify backup integrity</li>"
        "<li>Choose restore location and options</li>"
        "<li>Safely restore your application data</li>"
        "</ul>"
        "<p><b>Note:</b> You can restore to the original location or choose "
        "a different directory. Existing files can be backed up before restoration.</p>"
        "<p>Click <b>Next</b> to begin.</p>",
        this
    );
    m_descriptionLabel->setWordWrap(true);
    layout->addWidget(m_descriptionLabel);
    
    layout->addStretch();
}

// ============================================================================
// RestoreSelectBackupPage
// ============================================================================

RestoreSelectBackupPage::RestoreSelectBackupPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent)
    : QWizardPage(parent)
    , m_dataManager(dataManager)
{
    setTitle("Select Backup");
    setSubTitle("Choose backup archives to restore.");
    
    setupUI();
}

void RestoreSelectBackupPage::setupUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    
    // Backup directory selection
    auto* dirLayout = new QHBoxLayout();
    dirLayout->addWidget(new QLabel("Backup Directory:", this));
    
    m_backupDirEdit = new QLineEdit(this);
    m_backupDirEdit->setPlaceholderText("Select directory containing backups");
    connect(m_backupDirEdit, &QLineEdit::textChanged,
            this, [this]() { populateBackupList(); });
    dirLayout->addWidget(m_backupDirEdit);
    
    m_browseButton = new QPushButton("Browse...", this);
    connect(m_browseButton, &QPushButton::clicked,
            this, &RestoreSelectBackupPage::onBrowseBackupDirectory);
    dirLayout->addWidget(m_browseButton);
    
    layout->addLayout(dirLayout);
    
    // Backup table
    m_backupTableWidget = new QTableWidget(this);
    m_backupTableWidget->setColumnCount(5);
    m_backupTableWidget->setHorizontalHeaderLabels(
        QStringList() << "App Name" << "Backup Date" << "Size" << "Checksum" << "Status"
    );
    m_backupTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_backupTableWidget->setSelectionMode(QAbstractItemView::MultiSelection);
    m_backupTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_backupTableWidget->horizontalHeader()->setStretchLastSection(true);
    m_backupTableWidget->verticalHeader()->setVisible(false);
    connect(m_backupTableWidget, &QTableWidget::itemSelectionChanged,
            this, &RestoreSelectBackupPage::onItemSelectionChanged);
    layout->addWidget(m_backupTableWidget);
    
    // Action buttons
    auto* buttonLayout = new QHBoxLayout();
    
    m_refreshButton = new QPushButton("Refresh", this);
    connect(m_refreshButton, &QPushButton::clicked,
            this, &RestoreSelectBackupPage::onRefreshList);
    buttonLayout->addWidget(m_refreshButton);
    
    m_verifyButton = new QPushButton("Verify Selected", this);
    m_verifyButton->setToolTip("Check SHA-256 checksums to confirm backup files aren't corrupted");
    m_verifyButton->setEnabled(false);
    connect(m_verifyButton, &QPushButton::clicked,
            this, &RestoreSelectBackupPage::onVerifyBackup);
    buttonLayout->addWidget(m_verifyButton);
    
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);
    
    // Status
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
}

void RestoreSelectBackupPage::initializePage()
{
    // Set default backup directory if not already set
    if (m_backupDirEdit->text().isEmpty()) {
        QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK Backups";
        m_backupDirEdit->setText(defaultPath);
    }
    
    populateBackupList();
}

void RestoreSelectBackupPage::populateBackupList()
{
    m_backupTableWidget->setRowCount(0);
    
    QString backupDir = m_backupDirEdit->text();
    if (backupDir.isEmpty() || !QDir(backupDir).exists()) {
        m_statusLabel->setText("Please select a valid backup directory");
        return;
    }
    
    // List all backups
    auto backups = m_dataManager->listBackups(backupDir);
    
    for (const auto& backup : backups) {
        int row = m_backupTableWidget->rowCount();
        m_backupTableWidget->insertRow(row);
        
        // App Name
        auto* nameItem = new QTableWidgetItem(backup.app_name);
        nameItem->setData(Qt::UserRole, backup.backup_path);
        m_backupTableWidget->setItem(row, 0, nameItem);
        
        // Backup Date
        auto* dateItem = new QTableWidgetItem(backup.backup_date.toString("yyyy-MM-dd HH:mm:ss"));
        m_backupTableWidget->setItem(row, 1, dateItem);
        
        // Size
        double sizeMB = backup.compressed_size / (1024.0 * 1024.0);
        auto* sizeItem = new QTableWidgetItem(QString::number(sizeMB, 'f', 2) + " MB");
        m_backupTableWidget->setItem(row, 2, sizeItem);
        
        // Checksum
        QString checksumShort = backup.checksum.left(16) + "...";
        auto* checksumItem = new QTableWidgetItem(checksumShort);
        checksumItem->setToolTip(backup.checksum);
        m_backupTableWidget->setItem(row, 3, checksumItem);
        
        // Status
        auto* statusItem = new QTableWidgetItem("Not Verified");
        m_backupTableWidget->setItem(row, 4, statusItem);
    }
    
    m_backupTableWidget->resizeColumnsToContents();
    m_statusLabel->setText(QString("Found %1 backup(s)").arg(backups.size()));
}

void RestoreSelectBackupPage::onBrowseBackupDirectory()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "Select Backup Directory",
        m_backupDirEdit->text()
    );
    
    if (!dir.isEmpty()) {
        m_backupDirEdit->setText(dir);
    }
}

void RestoreSelectBackupPage::onRefreshList()
{
    populateBackupList();
}

void RestoreSelectBackupPage::onItemSelectionChanged()
{
    m_verifyButton->setEnabled(!m_backupTableWidget->selectedItems().isEmpty());
    Q_EMIT completeChanged();
}

void RestoreSelectBackupPage::onVerifyBackup()
{
    auto selectedRows = m_backupTableWidget->selectionModel()->selectedRows();
    
    for (const auto& index : selectedRows) {
        int row = index.row();
        QString backupPath = m_backupTableWidget->item(row, 0)->data(Qt::UserRole).toString();
        
        bool valid = m_dataManager->verifyBackup(backupPath);
        
        auto* statusItem = m_backupTableWidget->item(row, 4);
        if (valid) {
            statusItem->setText("✓ Valid");
            statusItem->setForeground(Qt::darkGreen);
        } else {
            statusItem->setText("✗ Invalid");
            statusItem->setForeground(Qt::red);
        }
    }
    
    m_statusLabel->setText("Verification complete");
}

bool RestoreSelectBackupPage::isComplete() const
{
    return !m_backupTableWidget->selectedItems().isEmpty();
}

QStringList RestoreSelectBackupPage::getSelectedBackups() const
{
    QStringList backups;
    auto selectedRows = m_backupTableWidget->selectionModel()->selectedRows();
    
    for (const auto& index : selectedRows) {
        QString backupPath = m_backupTableWidget->item(index.row(), 0)->data(Qt::UserRole).toString();
        backups << backupPath;
    }
    
    return backups;
}

// ============================================================================
// RestoreConfigurePage
// ============================================================================

RestoreConfigurePage::RestoreConfigurePage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Configure Restore");
    setSubTitle("Choose restore location and options.");
    
    setupUI();
}

void RestoreConfigurePage::setupUI()
{
    auto* layout = new QGridLayout(this);
    layout->setSpacing(12);
    layout->setColumnStretch(1, 1);
    
    // Destination
    int row = 0;
    layout->addWidget(new QLabel("Restore Location:", this), row, 0);
    
    m_destinationEdit = new QLineEdit(this);
    m_destinationEdit->setPlaceholderText("Select restore destination directory");
    connect(m_destinationEdit, &QLineEdit::textChanged,
            this, &RestoreConfigurePage::completeChanged);
    layout->addWidget(m_destinationEdit, row, 1);
    
    auto* buttonLayout = new QHBoxLayout();
    m_browseButton = new QPushButton("Browse...", this);
    connect(m_browseButton, &QPushButton::clicked,
            this, &RestoreConfigurePage::onBrowseDestination);
    buttonLayout->addWidget(m_browseButton);
    
    m_originalButton = new QPushButton("Use Original", this);
    m_originalButton->setToolTip("Set destination to the same path the backup was originally created from");
    connect(m_originalButton, &QPushButton::clicked,
            this, &RestoreConfigurePage::onUseOriginalLocation);
    buttonLayout->addWidget(m_originalButton);
    
    layout->addLayout(buttonLayout, row, 2);
    
    // Options
    row++;
    m_verifyCheckBox = new QCheckBox("Verify checksums before restore", this);
    m_verifyCheckBox->setChecked(true);
    m_verifyCheckBox->setToolTip("Checks backup integrity first — aborts if corruption is detected");
    layout->addWidget(m_verifyCheckBox, row, 0, 1, 3);
    
    row++;
    m_createBackupCheckBox = new QCheckBox("Backup existing data before overwriting", this);
    m_createBackupCheckBox->setChecked(true);
    m_createBackupCheckBox->setToolTip("Saves a snapshot of current files so you can undo the restore if needed");
    layout->addWidget(m_createBackupCheckBox, row, 0, 1, 3);
    
    row++;
    m_overwriteCheckBox = new QCheckBox("Overwrite existing files", this);
    m_overwriteCheckBox->setChecked(false);
    m_overwriteCheckBox->setToolTip("When unchecked, existing files are skipped and only missing files are restored");
    layout->addWidget(m_overwriteCheckBox, row, 0, 1, 3);
    
    row++;
    m_timestampsCheckBox = new QCheckBox("Restore original timestamps", this);
    m_timestampsCheckBox->setChecked(true);
    m_timestampsCheckBox->setToolTip("Uses the original file dates instead of today's date");
    layout->addWidget(m_timestampsCheckBox, row, 0, 1, 3);
    
    // Warning
    row++;
    m_warningLabel = new QLabel(
        "<b>Warning:</b> Restoring data may overwrite existing files. "
        "It is recommended to keep the \"Backup existing data\" option enabled.",
        this
    );
    m_warningLabel->setWordWrap(true);
    m_warningLabel->setStyleSheet("QLabel { color: #b45309; padding: 10px; background-color: #fef3c7; border-radius: 10px; }");
    layout->addWidget(m_warningLabel, row, 0, 1, 3);
    
    layout->setRowStretch(row + 1, 1);
}

void RestoreConfigurePage::initializePage()
{
    // Clear destination - user should choose explicitly
    if (m_destinationEdit->text().isEmpty()) {
        m_destinationEdit->setPlaceholderText("Browse or use original location");
    }
}

void RestoreConfigurePage::onBrowseDestination()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "Select Restore Destination",
        m_destinationEdit->text()
    );
    
    if (!dir.isEmpty()) {
        m_destinationEdit->setText(dir);
    }
}

void RestoreConfigurePage::onUseOriginalLocation()
{
    QMessageBox::information(this, "Original Location",
        "To restore to the original location:\n\n"
        "1. Check the backup metadata in the previous page\n"
        "2. Note the original source path\n"
        "3. Browse to that directory\n\n"
        "Or leave the restore location empty to use the backup's embedded path.");
}

bool RestoreConfigurePage::isComplete() const
{
    return !m_destinationEdit->text().isEmpty();
}

QString RestoreConfigurePage::getRestoreLocation() const
{
    return m_destinationEdit->text();
}

bool RestoreConfigurePage::getVerifyChecksum() const
{
    return m_verifyCheckBox->isChecked();
}

bool RestoreConfigurePage::getCreateBackup() const
{
    return m_createBackupCheckBox->isChecked();
}

bool RestoreConfigurePage::getOverwriteExisting() const
{
    return m_overwriteCheckBox->isChecked();
}

bool RestoreConfigurePage::getRestoreTimestamps() const
{
    return m_timestampsCheckBox->isChecked();
}

// ============================================================================
// RestoreProgressPage
// ============================================================================

RestoreProgressPage::RestoreProgressPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent)
    : QWizardPage(parent)
    , m_dataManager(dataManager)
    , m_restoreComplete(false)
    , m_restoreSuccess(false)
    , m_completedRestores(0)
    , m_totalRestores(0)
{
    setTitle("Restore Progress");
    setSubTitle("Restoring backups...");
    setFinalPage(true);
    
    setupUI();
    
    // Connect signals
    connect(m_dataManager.get(), &UserDataManager::operationStarted,
            this, &RestoreProgressPage::onOperationStarted);
    connect(m_dataManager.get(), &UserDataManager::progressUpdate,
            this, &RestoreProgressPage::onProgressUpdate);
    connect(m_dataManager.get(), &UserDataManager::operationCompleted,
            this, &RestoreProgressPage::onOperationCompleted);
    connect(m_dataManager.get(), &UserDataManager::operationError,
            this, &RestoreProgressPage::onOperationError);
}

void RestoreProgressPage::setupUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    
    m_statusLabel = new QLabel("Initializing restore...", this);
    layout->addWidget(m_statusLabel);
    
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    layout->addWidget(m_progressBar);
    
    layout->addWidget(new QLabel("Operation Log:", this));
    
    m_logTextEdit = new QTextEdit(this);
    m_logTextEdit->setReadOnly(true);
    layout->addWidget(m_logTextEdit);
}

void RestoreProgressPage::initializePage()
{
    m_restoreComplete = false;
    m_restoreSuccess = false;
    m_completedRestores = 0;
    m_logTextEdit->clear();
    m_progressBar->setValue(0);
    
    // Start restore after short delay
    QTimer::singleShot(500, this, &RestoreProgressPage::startRestore);
}

void RestoreProgressPage::startRestore()
{
    // Get configuration from previous pages
    auto* selectPage = qobject_cast<RestoreSelectBackupPage*>(wizard()->page(RestoreWizard::PageSelectBackup));
    auto* configPage = qobject_cast<RestoreConfigurePage*>(wizard()->page(RestoreWizard::PageConfigure));
    
    if (!selectPage || !configPage) {
        m_logTextEdit->append("ERROR: Could not retrieve wizard pages");
        m_restoreComplete = true;
        m_restoreSuccess = false;
        Q_EMIT completeChanged();
        return;
    }
    
    QStringList backups = selectPage->getSelectedBackups();
    QString restoreDir = configPage->getRestoreLocation();
    
    m_totalRestores = backups.size();
    m_statusLabel->setText(QString("Restoring %1 backup(s)...").arg(m_totalRestores));
    
    // Configure restore
    UserDataManager::RestoreConfig config;
    config.verify_checksum = configPage->getVerifyChecksum();
    config.create_backup = configPage->getCreateBackup();
    config.overwrite_existing = configPage->getOverwriteExisting();
    config.restore_timestamps = configPage->getRestoreTimestamps();
    
    // Create restore directory
    QDir dir(restoreDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    m_logTextEdit->append(QString("Starting restore to: %1").arg(restoreDir));
    m_logTextEdit->append(QString("Backups: %1").arg(m_totalRestores));
    m_logTextEdit->append("");
    
    // Restore each selected backup
    for (const QString& backupPath : backups) {
        auto backupInfo = m_dataManager->getBackupInfo(backupPath);
        
        if (backupInfo.app_name.isEmpty()) {
            m_logTextEdit->append(QString("ERROR: Could not read backup metadata: %1").arg(backupPath));
            m_completedRestores++;
            continue;
        }
        
        m_logTextEdit->append(QString("[%1] Restoring from %2...")
                                     .arg(backupInfo.app_name)
                                     .arg(QFileInfo(backupPath).fileName()));
        
        bool success = m_dataManager->restoreAppData(backupPath, restoreDir, config);
        
        if (!success) {
            m_logTextEdit->append(QString("  FAILED: %1").arg(backupInfo.app_name));
        }
    }
}

void RestoreProgressPage::onOperationStarted(const QString& appName, const QString& /*operation*/)
{
    m_logTextEdit->append(QString("[%1] Starting restore...").arg(appName));
}

void RestoreProgressPage::onProgressUpdate(int current, int total, const QString& message)
{
    if (total > 0) {
        int percent = (current * 100) / total;
        m_progressBar->setValue(percent);
    }
    
    if (!message.isEmpty()) {
        m_logTextEdit->append(QString("  %1").arg(message));
    }
}

void RestoreProgressPage::onOperationCompleted(const QString& appName, bool success, const QString& message)
{
    m_completedRestores++;
    
    if (success) {
        m_logTextEdit->append(QString("[%1] SUCCESS: %2").arg(appName, message));
    } else {
        m_logTextEdit->append(QString("[%1] FAILED: %2").arg(appName, message));
    }
    
    // Update overall progress
    int overallPercent = (m_completedRestores * 100) / m_totalRestores;
    m_progressBar->setValue(overallPercent);
    m_statusLabel->setText(QString("Completed %1 of %2 restores")
                                  .arg(m_completedRestores)
                                  .arg(m_totalRestores));
    
    // Check if all complete
    if (m_completedRestores >= m_totalRestores) {
        m_restoreComplete = true;
        m_restoreSuccess = true;
        m_statusLabel->setText("Restore completed successfully!");
        m_logTextEdit->append("");
        m_logTextEdit->append("=== Restore Complete ===");
        Q_EMIT completeChanged();
    }
}

void RestoreProgressPage::onOperationError(const QString& appName, const QString& error)
{
    m_logTextEdit->append(QString("[%1] ERROR: %2").arg(appName, error));
}

bool RestoreProgressPage::isComplete() const
{
    return m_restoreComplete;
}

} // namespace sak
