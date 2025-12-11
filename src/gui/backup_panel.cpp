#include "sak/backup_panel.h"
#include "sak/user_data_manager.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QDateTime>
#include <QFont>

BackupPanel::BackupPanel(QWidget* parent)
    : QWidget(parent)
    , m_dataManager(std::make_shared<sak::UserDataManager>())
{
    setupUi();
    setupConnections();
    
    appendLog("User Profile Backup Panel initialized");
    appendLog("Click 'Backup User Profiles...' to start the backup wizard");
}

BackupPanel::~BackupPanel() = default;

void BackupPanel::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    // Title and description
    auto* titleLabel = new QLabel("<h2>User Profile Backup & Restore</h2>");
    mainLayout->addWidget(titleLabel);
    
    auto* descLabel = new QLabel(
        "Comprehensive backup and restore wizards for Windows user profiles."
    );
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet("color: #666; margin-bottom: 5px;");
    mainLayout->addWidget(descLabel);

    // Action buttons in a card-style layout
    auto* actionsGroup = new QGroupBox("Backup & Restore Wizards");
    auto* actionsLayout = new QVBoxLayout(actionsGroup);
    actionsLayout->setSpacing(10);
    actionsLayout->setContentsMargins(10, 15, 10, 10);
    
    // Backup section
    auto* backupCard = new QWidget();
    backupCard->setStyleSheet("QWidget { background-color: #f8f9fa; border: 1px solid #dee2e6; border-radius: 4px; padding: 10px; }");
    auto* backupLayout = new QVBoxLayout(backupCard);
    backupLayout->setSpacing(8);
    backupLayout->setContentsMargins(10, 10, 10, 10);
    
    auto* backupTitle = new QLabel("<b>Backup User Profiles</b>");
    backupLayout->addWidget(backupTitle);
    
    auto* backupDesc = new QLabel(
        "Scan and select users, choose folders, configure filters, and create backup packages."
    );
    backupDesc->setWordWrap(true);
    backupDesc->setStyleSheet("color: #555; font-size: 9pt;");
    backupLayout->addWidget(backupDesc);
    
    m_backupButton = new QPushButton("Start Backup Wizard...");
    m_backupButton->setMinimumHeight(32);
    m_backupButton->setToolTip("Launch comprehensive backup wizard");
    backupLayout->addWidget(m_backupButton);
    
    actionsLayout->addWidget(backupCard);
    
    // Restore section
    auto* restoreCard = new QWidget();
    restoreCard->setStyleSheet("QWidget { background-color: #f8f9fa; border: 1px solid #dee2e6; border-radius: 4px; padding: 10px; }");
    auto* restoreLayout = new QVBoxLayout(restoreCard);
    restoreLayout->setSpacing(8);
    restoreLayout->setContentsMargins(10, 10, 10, 10);
    
    auto* restoreTitle = new QLabel("<b>Restore User Profiles</b>");
    restoreLayout->addWidget(restoreTitle);
    
    auto* restoreDesc = new QLabel(
        "Select backup, map users, configure merge options, and restore data with permissions."
    );
    restoreDesc->setWordWrap(true);
    restoreDesc->setStyleSheet("color: #555; font-size: 9pt;");
    restoreLayout->addWidget(restoreDesc);
    
    m_restoreButton = new QPushButton("Start Restore Wizard...");
    m_restoreButton->setMinimumHeight(32);
    m_restoreButton->setToolTip("Launch comprehensive restore wizard");
    restoreLayout->addWidget(m_restoreButton);
    
    actionsLayout->addWidget(restoreCard);
    
    mainLayout->addWidget(actionsGroup);
    
    // Status
    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setStyleSheet("padding: 6px; background-color: #e9ecef; border-radius: 4px; font-weight: bold;");
    mainLayout->addWidget(m_statusLabel);

    // Log viewer
    auto* logGroup = new QGroupBox("Operation Log");
    auto* logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(5, 10, 5, 5);
    
    m_logTextEdit = new QTextEdit();
    m_logTextEdit->setReadOnly(true);
    m_logTextEdit->setMinimumHeight(120);
    m_logTextEdit->setFont(QFont("Consolas", 9));
    logLayout->addWidget(m_logTextEdit);
    
    mainLayout->addWidget(logGroup, 1); // Give log viewer stretch factor
}

void BackupPanel::setupConnections()
{
    connect(m_backupButton, &QPushButton::clicked,
            this, &BackupPanel::onBackupSelected);
    connect(m_restoreButton, &QPushButton::clicked,
            this, &BackupPanel::onRestoreBackup);
}

void BackupPanel::onBackupSelected()
{
    // Launch the comprehensive user profile backup wizard
    auto* wizard = new sak::UserProfileBackupWizard(this);
    
    // Connect wizard completion to log updates
    connect(wizard, &QDialog::finished, this, [this, wizard](int result) {
        if (result == QDialog::Accepted) {
            appendLog("=== User Profile Backup Wizard Completed ===");
            m_statusLabel->setText("Backup completed via wizard");
            Q_EMIT status_message("User profile backup completed", 5000);
        } else {
            appendLog("User profile backup wizard cancelled");
            m_statusLabel->setText("Ready");
        }
        wizard->deleteLater();
    });
    
    // Show wizard
    appendLog("Launching backup wizard...");
    m_statusLabel->setText("Backup wizard launched");
    wizard->show();
    wizard->raise();
    wizard->activateWindow();
}

void BackupPanel::onRestoreBackup()
{
    // Launch the comprehensive user profile restore wizard
    auto* wizard = new sak::UserProfileRestoreWizard(this);
    
    // Connect wizard completion to log updates
    connect(wizard, &QDialog::finished, this, [this, wizard](int result) {
        if (result == QDialog::Accepted) {
            appendLog("=== User Profile Restore Wizard Completed ===");
            appendLog(QString("Restored from: %1").arg(wizard->backupPath()));
            m_statusLabel->setText("Restore completed via wizard");
            Q_EMIT status_message("User profile restore completed", 5000);
        } else {
            appendLog("User profile restore wizard cancelled");
            m_statusLabel->setText("Ready");
        }
        wizard->deleteLater();
    });
    
    // Show wizard
    appendLog("Launching restore wizard...");
    m_statusLabel->setText("Restore wizard launched");
    wizard->show();
    wizard->raise();
    wizard->activateWindow();
}

void BackupPanel::appendLog(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logTextEdit->append(QString("[%1] %2").arg(timestamp, message));
}
