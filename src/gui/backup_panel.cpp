// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/backup_panel.h"
#include "sak/user_data_manager.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"

#include "sak/config_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QDateTime>
#include <QFont>
#include <QScrollArea>
#include <QFrame>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QSpinBox>

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

    mainLayout->addStretch();

    // Bottom row: Settings + Log toggle (outside scroll area, pinned to bottom)
    auto* bottomLayout = new QHBoxLayout();

    auto* settingsBtn = new QPushButton(tr("Settings"), this);
    connect(settingsBtn, &QPushButton::clicked, this, &BackupPanel::onSettingsClicked);
    bottomLayout->addWidget(settingsBtn);

    m_logToggle = new sak::LogToggleSwitch(tr("Log"), this);
    bottomLayout->addWidget(m_logToggle);
    bottomLayout->addStretch();
    rootLayout->addLayout(bottomLayout);
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
            Q_EMIT statusMessage("User profile backup completed", 5000);
        } else {
            appendLog("User profile backup wizard cancelled");
            Q_EMIT statusMessage("Backup wizard cancelled", 3000);
        }
        wizard->deleteLater();
    });
    
    // Show wizard
    appendLog("Launching backup wizard...");
    Q_EMIT statusMessage("Backup wizard launched", 3000);
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
            Q_EMIT statusMessage("User profile restore completed", 5000);
        } else {
            appendLog("User profile restore wizard cancelled");
            Q_EMIT statusMessage("Restore wizard cancelled", 3000);
        }
        wizard->deleteLater();
    });
    
    // Show wizard
    appendLog("Launching restore wizard...");
    Q_EMIT statusMessage("Restore wizard launched", 3000);
    wizard->show();
    wizard->raise();
    wizard->activateWindow();
}

void BackupPanel::appendLog(const QString& message)
{
    Q_EMIT logOutput(message);
}

void BackupPanel::onSettingsClicked()
{
    auto& config = sak::ConfigManager::instance();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("User Migration Settings"));
    dialog.setMinimumWidth(440);
    auto* layout = new QVBoxLayout(&dialog);

    auto* formLayout = new QFormLayout();

    auto* threadSpin = new QSpinBox(&dialog);
    threadSpin->setRange(1, 16);
    threadSpin->setValue(config.getBackupThreadCount());
    formLayout->addRow(
        sak::InfoButton::createInfoLabel(tr("Thread Count:"),
            tr("Number of parallel copy threads \u2014 higher values speed up backup but increase CPU and disk I/O load"), &dialog),
        threadSpin);

    auto* verifyCheck = new QCheckBox(tr("Verify files using MD5 hash after backup"), &dialog);
    verifyCheck->setChecked(config.getBackupVerifyMD5());
    formLayout->addRow(
        sak::InfoButton::createInfoLabel(tr("Verify MD5:"),
            tr("Re-read each copied file and verify its MD5 checksum matches the original \u2014 slower but ensures integrity"), &dialog),
        verifyCheck);

    auto* locRow = new QHBoxLayout();
    auto* locEdit = new QLineEdit(config.getLastBackupLocation(), &dialog);
    locEdit->setReadOnly(true);
    auto* browseBtn = new QPushButton(tr("Browse..."), &dialog);
    connect(browseBtn, &QPushButton::clicked, &dialog, [&locEdit, &dialog]() {
        QString dir = QFileDialog::getExistingDirectory(&dialog, tr("Select Backup Location"), locEdit->text());
        if (!dir.isEmpty()) locEdit->setText(dir);
    });
    locRow->addWidget(locEdit);
    locRow->addWidget(browseBtn);
    formLayout->addRow(
        sak::InfoButton::createInfoLabel(tr("Backup Location:"),
            tr("Choose the destination folder for backup files"), &dialog),
        locRow);

    layout->addLayout(formLayout);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        config.setBackupThreadCount(threadSpin->value());
        config.setBackupVerifyMD5(verifyCheck->isChecked());
        config.setLastBackupLocation(locEdit->text());
        config.sync();
        appendLog(QString("Settings updated: threads=%1, verify=%2, location=%3")
                      .arg(threadSpin->value())
                      .arg(verifyCheck->isChecked() ? "yes" : "no")
                      .arg(locEdit->text()));
    }
}
