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
#include <QScrollArea>
#include <QFrame>

BackupPanel::BackupPanel(QWidget* parent)
    : QWidget(parent)
    , m_dataManager(std::make_shared<sak::UserDataManager>())
{
    setupUi();
    setupConnections();
    
    appendLog("User Migration Panel initialized");
    appendLog("Click 'Backup User Profiles...' to start the migration wizard");
}

BackupPanel::~BackupPanel() = default;

void BackupPanel::setupUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    scrollArea->setWidget(contentWidget);
    rootLayout->addWidget(scrollArea);

    // Title and description
    auto* titleLabel = new QLabel("<h2>User Profile Backup & Restore</h2>");
    mainLayout->addWidget(titleLabel);
    
    auto* descLabel = new QLabel(
        "Comprehensive backup and restore wizards for Windows user profiles."
    );
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet("color: #64748b; margin-bottom: 5px;");
    mainLayout->addWidget(descLabel);

    // Action buttons in a card-style layout
    auto* actionsGroup = new QGroupBox("Backup & Restore Wizards");
    auto* actionsLayout = new QVBoxLayout(actionsGroup);
    actionsLayout->setSpacing(12);
    actionsLayout->setContentsMargins(12, 18, 12, 12);
    
    // Backup section
    auto* backupTitle = new QLabel("<b>Backup User Profiles</b>");
    actionsLayout->addWidget(backupTitle);
    
    auto* backupDesc = new QLabel(
        "Scan and select users, choose folders, configure filters, and create backup packages."
    );
    backupDesc->setWordWrap(true);
    backupDesc->setStyleSheet("color: #475569; font-size: 9pt;");
    actionsLayout->addWidget(backupDesc);
    
    m_backupButton = new QPushButton("Start Backup Wizard...");
    m_backupButton->setMinimumHeight(40);
    m_backupButton->setToolTip("Step-by-step wizard to select apps, configure options, and create backups");
    actionsLayout->addWidget(m_backupButton);
    
    actionsLayout->addSpacing(8);
    
    // Restore section
    auto* restoreTitle = new QLabel("<b>Restore User Profiles</b>");
    actionsLayout->addWidget(restoreTitle);
    
    auto* restoreDesc = new QLabel(
        "Select backup, map users, configure merge options, and restore data with permissions."
    );
    restoreDesc->setWordWrap(true);
    restoreDesc->setStyleSheet("color: #475569; font-size: 9pt;");
    actionsLayout->addWidget(restoreDesc);
    
    m_restoreButton = new QPushButton("Start Restore Wizard...");
    m_restoreButton->setMinimumHeight(40);
    m_restoreButton->setToolTip("Step-by-step wizard to select backups, map users, and restore data");
    actionsLayout->addWidget(m_restoreButton);
    
    mainLayout->addWidget(actionsGroup);
    
    // Status
    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setStyleSheet("padding: 8px; background-color: #e2e8f0; border-radius: 10px; font-weight: 600; color: #1e293b;");
    mainLayout->addWidget(m_statusLabel);

    // Operation Log (standardized)
    auto* logGroup = new QGroupBox("Log");
    auto* logLayout = new QVBoxLayout(logGroup);
    m_logTextEdit = new QTextEdit();
    m_logTextEdit->setReadOnly(true);
    m_logTextEdit->setMinimumHeight(80);
    m_logTextEdit->setPlaceholderText("Operation log will appear here...");
    logLayout->addWidget(m_logTextEdit);
    mainLayout->addWidget(logGroup, 1);
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
            Q_EMIT statusMessage("User profile backup completed", 5000);
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
            Q_EMIT statusMessage("User profile restore completed", 5000);
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
