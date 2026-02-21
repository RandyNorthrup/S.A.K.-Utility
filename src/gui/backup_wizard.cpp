// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/backup_wizard.h"
#include "sak/user_data_manager.h"
#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/process_runner.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

namespace sak {

// ============================================================================
// BackupWizard
// ============================================================================

BackupWizard::BackupWizard(QWidget* parent)
    : QWizard(parent)
    , m_dataManager(std::make_shared<UserDataManager>())
{
    setWindowTitle("Backup Application Data");
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::HaveHelpButton, false);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setOption(QWizard::NoCancelButtonOnLastPage, true);
    
    setMinimumSize(700, 500);
    
    // Add pages
    setPage(PageWelcome, new BackupWelcomePage(this));
    setPage(PageSelectApps, new BackupSelectAppsPage(m_dataManager, this));
    setPage(PageConfigure, new BackupConfigurePage(this));
    setPage(PageProgress, new BackupProgressPage(m_dataManager, this));
    
    setStartId(PageWelcome);
}

// ============================================================================
// BackupWelcomePage
// ============================================================================

BackupWelcomePage::BackupWelcomePage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Welcome to the Backup Wizard");
    setSubTitle("This wizard will guide you through backing up your application data.");
    
    setupUI();
}

void BackupWelcomePage::setupUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    
    m_titleLabel = new QLabel("<h2>Backup Application Data</h2>", this);
    layout->addWidget(m_titleLabel);
    
    m_descriptionLabel = new QLabel(
        "<p>This wizard will help you:</p>"
        "<ul>"
        "<li>Select applications to backup</li>"
        "<li>Choose backup location and options</li>"
        "<li>Create compressed, verified backups</li>"
        "<li>Track backup progress and status</li>"
        "</ul>"
        "<p><b>Note:</b> Backups will be saved as ZIP archives with metadata "
        "for easy restoration.</p>"
        "<p>Click <b>Next</b> to begin.</p>",
        this
    );
    m_descriptionLabel->setWordWrap(true);
    layout->addWidget(m_descriptionLabel);
    
    layout->addStretch();
}

// ============================================================================
// BackupSelectAppsPage
// ============================================================================

BackupSelectAppsPage::BackupSelectAppsPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent)
    : QWizardPage(parent)
    , m_dataManager(dataManager)
{
    setTitle("Select Applications");
    setSubTitle("Choose which applications to backup.");
    
    setupUI();
}

void BackupSelectAppsPage::setupUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    
    // Instructions
    auto* instructionLabel = new QLabel(
        "Select applications from the list below, or add custom paths:",
        this
    );
    layout->addWidget(instructionLabel);
    
    // App list
    m_appListWidget = new QListWidget(this);
    m_appListWidget->setSelectionMode(QAbstractItemView::MultiSelection);
    connect(m_appListWidget, &QListWidget::itemSelectionChanged,
            this, &BackupSelectAppsPage::onItemSelectionChanged);
    layout->addWidget(m_appListWidget);
    
    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    
    m_scanButton = new QPushButton("Scan Common Apps", this);
    m_scanButton->setToolTip("Auto-detect installed apps like Chrome, Firefox, Outlook, VS Code, etc.");
    connect(m_scanButton, &QPushButton::clicked,
            this, &BackupSelectAppsPage::onScanCommonApps);
    buttonLayout->addWidget(m_scanButton);
    
    m_browseButton = new QPushButton("Add Custom Path...", this);
    connect(m_browseButton, &QPushButton::clicked,
            this, &BackupSelectAppsPage::onBrowseCustomPath);
    buttonLayout->addWidget(m_browseButton);
    
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);
    
    // Status
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
}

void BackupSelectAppsPage::initializePage()
{
    // Populate with common apps
    populateCommonApps();
}

void BackupSelectAppsPage::populateCommonApps()
{
    m_appListWidget->clear();
    
    // Get common data locations
    auto locations = m_dataManager->getCommonDataLocations();
    
    for (const auto& loc : locations) {
        for (const QString& path : loc.paths) {
            // BitLocker sentinel — check via WMI instead of filesystem
            if (path == "bitlocker://recovery-keys") {
                // Quick check: are any BitLocker volumes present?
                ProcessResult probe = runPowerShell(
                    "(Get-WmiObject -Namespace 'Root\\CIMv2\\Security\\MicrosoftVolumeEncryption' "
                    "-Class Win32_EncryptableVolume -ErrorAction SilentlyContinue | "
                    "Where-Object { $_.ProtectionStatus -ne $null } | Measure-Object).Count",
                    10000);
                int volume_count = probe.std_out.trimmed().toInt();
                if (volume_count > 0) {
                    auto* item = new QListWidgetItem(
                        QString("%1 (%2 encrypted volume%3)")
                            .arg(loc.description)
                            .arg(volume_count)
                            .arg(volume_count != 1 ? "s" : "")
                    );
                    item->setData(Qt::UserRole, path);
                    m_appListWidget->addItem(item);
                }
                continue;
            }

            if (QFileInfo::exists(path)) {
                auto* item = new QListWidgetItem(
                    QString("%1 (%2)").arg(loc.description, path)
                );
                item->setData(Qt::UserRole, path);
                m_appListWidget->addItem(item);
            }
        }
    }
    
    m_statusLabel->setText(QString("Found %1 common application data locations")
                                  .arg(m_appListWidget->count()));
}

void BackupSelectAppsPage::onScanCommonApps()
{
    populateCommonApps();
    m_statusLabel->setText("Rescan completed");
}

void BackupSelectAppsPage::onBrowseCustomPath()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "Select Application Data Directory",
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
    );
    
    if (!dir.isEmpty()) {
        auto* item = new QListWidgetItem(QString("Custom: %1").arg(dir));
        item->setData(Qt::UserRole, dir);
        m_appListWidget->addItem(item);
        item->setSelected(true);
        
        m_statusLabel->setText("Added custom path");
    }
}

void BackupSelectAppsPage::onItemSelectionChanged()
{
    Q_EMIT completeChanged();
}

bool BackupSelectAppsPage::isComplete() const
{
    return !m_appListWidget->selectedItems().isEmpty();
}

QStringList BackupSelectAppsPage::getSelectedApps() const
{
    QStringList apps;
    for (auto* item : m_appListWidget->selectedItems()) {
        QString text = item->text();
        // Extract app name (before the parentheses)
        int parenIndex = text.indexOf('(');
        if (parenIndex > 0) {
            apps << text.left(parenIndex).trimmed();
        } else {
            apps << text;
        }
    }
    return apps;
}

QStringList BackupSelectAppsPage::getSelectedPaths() const
{
    QStringList paths;
    for (auto* item : m_appListWidget->selectedItems()) {
        QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty()) {
            paths << path;
        }
    }
    return paths;
}

// ============================================================================
// BackupConfigurePage
// ============================================================================

BackupConfigurePage::BackupConfigurePage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Configure Backup");
    setSubTitle("Choose backup location and options.");
    
    setupUI();
}

void BackupConfigurePage::setupUI()
{
    auto* layout = new QGridLayout(this);
    layout->setSpacing(12);
    layout->setColumnStretch(1, 1);
    
    // Destination
    int row = 0;
    layout->addWidget(new QLabel("Backup Location:", this), row, 0);
    
    m_destinationEdit = new QLineEdit(this);
    m_destinationEdit->setPlaceholderText("Select backup destination directory");
    connect(m_destinationEdit, &QLineEdit::textChanged,
            this, &BackupConfigurePage::completeChanged);
    layout->addWidget(m_destinationEdit, row, 1);
    
    m_browseButton = new QPushButton("Browse...", this);
    connect(m_browseButton, &QPushButton::clicked,
            this, &BackupConfigurePage::onBrowseDestination);
    layout->addWidget(m_browseButton, row, 2);
    
    // Options
    row++;
    m_compressCheckBox = new QCheckBox("Compress backups (ZIP format)", this);
    m_compressCheckBox->setChecked(true);
    m_compressCheckBox->setToolTip("Reduces backup size by 40-70%% but takes longer to create");
    layout->addWidget(m_compressCheckBox, row, 0, 1, 3);
    
    row++;
    m_verifyCheckBox = new QCheckBox("Verify checksums after backup", this);
    m_verifyCheckBox->setChecked(true);
    m_verifyCheckBox->setToolTip("Generates SHA-256 hashes to detect corruption during restore");
    layout->addWidget(m_verifyCheckBox, row, 0, 1, 3);
    
    // Exclusions
    row++;
    layout->addWidget(new QLabel("Exclusion Patterns:", this), row, 0, Qt::AlignTop);
    
    m_exclusionEdit = new QTextEdit(this);
    m_exclusionEdit->setMaximumHeight(80);
    m_exclusionEdit->setPlainText("*.log\n*.tmp\ncache/*\ntemp/*");
    m_exclusionEdit->setToolTip("One pattern per line (e.g., *.log, cache/*)");
    layout->addWidget(m_exclusionEdit, row, 1, 1, 2);
    
    // Size estimate
    row++;
    m_sizeEstimateLabel = new QLabel(this);
    m_sizeEstimateLabel->setWordWrap(true);
    layout->addWidget(m_sizeEstimateLabel, row, 0, 1, 3);
    
    layout->setRowStretch(row + 1, 1);
}

void BackupConfigurePage::initializePage()
{
    // Set default backup location if not already set
    if (m_destinationEdit->text().isEmpty()) {
        QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK Backups";
        m_destinationEdit->setText(defaultPath);
    }
}

void BackupConfigurePage::onBrowseDestination()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "Select Backup Destination",
        m_destinationEdit->text()
    );
    
    if (!dir.isEmpty()) {
        m_destinationEdit->setText(dir);
    }
}

bool BackupConfigurePage::isComplete() const
{
    return !m_destinationEdit->text().isEmpty();
}

QString BackupConfigurePage::getBackupLocation() const
{
    return m_destinationEdit->text();
}

bool BackupConfigurePage::getCompressEnabled() const
{
    return m_compressCheckBox->isChecked();
}

bool BackupConfigurePage::getVerifyChecksum() const
{
    return m_verifyCheckBox->isChecked();
}

QStringList BackupConfigurePage::getExclusionPatterns() const
{
    QString text = m_exclusionEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        return QStringList();
    }
    return text.split('\n', Qt::SkipEmptyParts);
}

// ============================================================================
// BackupProgressPage
// ============================================================================

BackupProgressPage::BackupProgressPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent)
    : QWizardPage(parent)
    , m_dataManager(dataManager)
    , m_backupComplete(false)
    , m_backupSuccess(false)
    , m_completedBackups(0)
    , m_totalBackups(0)
{
    setTitle("Backup Progress");
    setSubTitle("Creating backups...");
    setFinalPage(true);
    
    setupUI();
    
    // Connect signals
    connect(m_dataManager.get(), &UserDataManager::operationStarted,
            this, &BackupProgressPage::onOperationStarted);
    connect(m_dataManager.get(), &UserDataManager::progressUpdate,
            this, &BackupProgressPage::onProgressUpdate);
    connect(m_dataManager.get(), &UserDataManager::operationCompleted,
            this, &BackupProgressPage::onOperationCompleted);
    connect(m_dataManager.get(), &UserDataManager::operationError,
            this, &BackupProgressPage::onOperationError);
}

void BackupProgressPage::setupUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    
    m_statusLabel = new QLabel("Initializing backup...", this);
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

void BackupProgressPage::initializePage()
{
    m_backupComplete = false;
    m_backupSuccess = false;
    m_completedBackups = 0;
    m_logTextEdit->clear();
    m_progressBar->setValue(0);
    
    // Start backup after short delay to allow UI to update
    QTimer::singleShot(500, this, &BackupProgressPage::startBackup);
}

void BackupProgressPage::startBackup()
{
    // Get configuration from previous pages
    auto* selectPage = qobject_cast<BackupSelectAppsPage*>(wizard()->page(BackupWizard::PageSelectApps));
    auto* configPage = qobject_cast<BackupConfigurePage*>(wizard()->page(BackupWizard::PageConfigure));
    
    if (!selectPage || !configPage) {
        m_logTextEdit->append("ERROR: Could not retrieve wizard pages");
        m_backupComplete = true;
        m_backupSuccess = false;
        Q_EMIT completeChanged();
        return;
    }
    
    QStringList apps = selectPage->getSelectedApps();
    QStringList paths = selectPage->getSelectedPaths();
    QString backupDir = configPage->getBackupLocation();
    
    m_totalBackups = paths.size();
    m_statusLabel->setText(QString("Backing up %1 application(s)...").arg(m_totalBackups));
    
    // Configure backup
    UserDataManager::BackupConfig config;
    config.compress = configPage->getCompressEnabled();
    config.verify_checksum = configPage->getVerifyChecksum();
    config.exclude_patterns = configPage->getExclusionPatterns();
    
    // Create backup directory
    QDir dir(backupDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    m_logTextEdit->append(QString("Starting backup to: %1").arg(backupDir));
    m_logTextEdit->append(QString("Applications: %1").arg(apps.join(", ")));
    m_logTextEdit->append("");
    
    // Backup each selected app
    for (int i = 0; i < paths.size(); ++i) {
        QString appName = (i < apps.size()) ? apps[i] : QString("App%1").arg(i + 1);

        // Handle BitLocker key backup via dedicated action
        if (paths[i] == "bitlocker://recovery-keys") {
            m_logTextEdit->append("[BitLocker Recovery Keys] Starting backup...");
            auto bitlocker_action = std::make_unique<BackupBitlockerKeysAction>(backupDir);

            connect(bitlocker_action.get(), &QuickAction::executionProgress,
                    this, [this](const QString& msg, int progress) {
                m_logTextEdit->append(QString("  %1").arg(msg));
                if (m_totalBackups > 0) {
                    int overall = (m_completedBackups * 100 + progress) / m_totalBackups;
                    m_progressBar->setValue(qMin(overall, 100));
                }
            });

            connect(bitlocker_action.get(), &QuickAction::logMessage,
                    this, [this](const QString& msg) {
                m_logTextEdit->append(QString("  %1").arg(msg));
            });

            // Run scan + execute synchronously (we're already on the UI thread
            // via QTimer, and the action uses process_runner internally)
            bitlocker_action->scan();
            bitlocker_action->execute();

            auto exec_result = bitlocker_action->lastExecutionResult();
            m_completedBackups++;

            if (exec_result.success) {
                m_logTextEdit->append(QString("[BitLocker Recovery Keys] SUCCESS: %1").arg(exec_result.message));
                if (!exec_result.output_path.isEmpty()) {
                    m_logTextEdit->append(QString("  Saved to: %1").arg(exec_result.output_path));
                }
            } else {
                m_logTextEdit->append(QString("[BitLocker Recovery Keys] FAILED: %1").arg(exec_result.message));
            }

            int overallPercent = (m_completedBackups * 100) / m_totalBackups;
            m_progressBar->setValue(overallPercent);
            m_statusLabel->setText(QString("Completed %1 of %2 backups")
                                          .arg(m_completedBackups).arg(m_totalBackups));
            continue;
        }

        QStringList sourcePaths;
        sourcePaths << paths[i];
        
        auto result = m_dataManager->backupAppData(appName, sourcePaths, backupDir, config);
        
        if (!result.has_value()) {
            m_logTextEdit->append(QString("FAILED: %1").arg(appName));
        }
    }
}

void BackupProgressPage::onOperationStarted(const QString& appName, const QString& /*operation*/)
{
    m_logTextEdit->append(QString("[%1] Starting backup...").arg(appName));
}

void BackupProgressPage::onProgressUpdate(int current, int total, const QString& message)
{
    if (total > 0) {
        int percent = (current * 100) / total;
        m_progressBar->setValue(percent);
    }
    
    if (!message.isEmpty()) {
        m_logTextEdit->append(QString("  %1").arg(message));
    }
}

void BackupProgressPage::onOperationCompleted(const QString& appName, bool success, const QString& message)
{
    m_completedBackups++;
    
    if (success) {
        m_logTextEdit->append(QString("[%1] SUCCESS: %2").arg(appName, message));
    } else {
        m_logTextEdit->append(QString("[%1] FAILED: %2").arg(appName, message));
    }
    
    // Update overall progress
    int overallPercent = (m_completedBackups * 100) / m_totalBackups;
    m_progressBar->setValue(overallPercent);
    m_statusLabel->setText(QString("Completed %1 of %2 backups")
                                  .arg(m_completedBackups)
                                  .arg(m_totalBackups));
    
    // Check if all complete
    if (m_completedBackups >= m_totalBackups) {
        m_backupComplete = true;
        m_backupSuccess = true;
        m_statusLabel->setText("Backup completed successfully!");
        m_logTextEdit->append("");
        m_logTextEdit->append("=== Backup Complete ===");
        Q_EMIT completeChanged();
    }
}

void BackupProgressPage::onOperationError(const QString& appName, const QString& error)
{
    m_logTextEdit->append(QString("[%1] ERROR: %2").arg(appName, error));
}

bool BackupProgressPage::isComplete() const
{
    return m_backupComplete;
}

} // namespace sak
