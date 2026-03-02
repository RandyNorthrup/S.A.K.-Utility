// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file user_migration_panel.cpp
/// @brief Implements the user profile migration panel UI for backup and restore

#include "sak/user_migration_panel.h"
#include "sak/user_data_manager.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "sak/layout_constants.h"

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

namespace sak {

UserMigrationPanel::UserMigrationPanel(QWidget* parent)
    : QWidget(parent)
    , m_dataManager(std::make_shared<UserDataManager>())
{
    setupUi();
    setupConnections();

    appendLog("User Migration Panel initialized");
    appendLog("Click 'Backup User Profiles...' to start the migration wizard");
}

UserMigrationPanel::~UserMigrationPanel() = default;

void UserMigrationPanel::setupUi()
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

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(contentWidget, tr("User Profile Backup & Restore"),
        tr("Comprehensive backup and restore wizards for Windows user profiles."), mainLayout);

    mainLayout->addWidget(createWizardButtonsGroup());

    mainLayout->addStretch();

    // Bottom row: Settings + Log toggle (outside scroll area, pinned to bottom)
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(12, 6, 12, 8);

    auto* settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(QStringLiteral("User Migration Settings"));
    connect(settingsBtn, &QPushButton::clicked, this, &UserMigrationPanel::onSettingsClicked);
    bottomLayout->addWidget(settingsBtn);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    bottomLayout->addWidget(m_logToggle);
    bottomLayout->addStretch();
    rootLayout->addLayout(bottomLayout);
}

QGroupBox* UserMigrationPanel::createWizardButtonsGroup()
{
    auto* group = new QGroupBox("Backup & Restore Wizards");
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(12);
    layout->setContentsMargins(12, 18, 12, 12);

    // Backup section
    auto* backupTitle = new QLabel("<b>Backup User Profiles</b>");
    layout->addWidget(backupTitle);

    auto* backupDesc = new QLabel(
        "Scan and select users, choose folders, configure filters, and create backup packages."
    );
    backupDesc->setWordWrap(true);
    backupDesc->setStyleSheet(QString("color: %1; font-size: %2pt;").arg(
        sak::ui::kColorTextSecondary).arg(sak::ui::kFontSizeNote));
    layout->addWidget(backupDesc);

    m_backupButton = new QPushButton("Start Backup Wizard...");
    m_backupButton->setMinimumHeight(sak::kButtonHeightStd);
    m_backupButton->setToolTip("Step-by-step wizard to select apps, configure options, and create "
                               "backups");
    m_backupButton->setAccessibleName(QStringLiteral("Start Backup Wizard"));
    layout->addWidget(m_backupButton);

    layout->addSpacing(8);

    // Restore section
    auto* restoreTitle = new QLabel("<b>Restore User Profiles</b>");
    layout->addWidget(restoreTitle);

    auto* restoreDesc = new QLabel(
        "Select backup, map users, configure merge options, and restore data with permissions."
    );
    restoreDesc->setWordWrap(true);
    restoreDesc->setStyleSheet(QString("color: %1; font-size: %2pt;").arg(
        sak::ui::kColorTextSecondary).arg(sak::ui::kFontSizeNote));
    layout->addWidget(restoreDesc);

    m_restoreButton = new QPushButton("Start Restore Wizard...");
    m_restoreButton->setMinimumHeight(sak::kButtonHeightStd);
    m_restoreButton->setToolTip(
        "Step-by-step wizard to select backups, map users, and restore data");
    m_restoreButton->setAccessibleName(QStringLiteral("Start Restore Wizard"));
    layout->addWidget(m_restoreButton);

    return group;
}

void UserMigrationPanel::setupConnections()
{
    connect(m_backupButton, &QPushButton::clicked,
            this, &UserMigrationPanel::onBackupSelected);
    connect(m_restoreButton, &QPushButton::clicked,
            this, &UserMigrationPanel::onRestoreBackup);
}

void UserMigrationPanel::onBackupSelected()
{
    // Launch the comprehensive user profile backup wizard
    auto* wizard = new UserProfileBackupWizard(this);

    // Connect wizard completion to log updates
    connect(wizard, &QDialog::finished, this, [this, wizard](int result) {
        if (result == QDialog::Accepted) {
            appendLog("=== User Profile Backup Wizard Completed ===");
            Q_EMIT statusMessage("User profile backup completed", sak::kTimerStatusDefaultMs);
        } else {
            appendLog("User profile backup wizard cancelled");
            Q_EMIT statusMessage("Backup wizard cancelled", sak::kTimerStatusMessageMs);
        }
        wizard->deleteLater();
    });

    // Show wizard
    appendLog("Launching backup wizard...");
    Q_EMIT statusMessage("Backup wizard launched", sak::kTimerStatusMessageMs);
    wizard->show();
    wizard->raise();
    wizard->activateWindow();
}

void UserMigrationPanel::onRestoreBackup()
{
    // Launch the comprehensive user profile restore wizard
    auto* wizard = new UserProfileRestoreWizard(this);

    // Connect wizard completion to log updates
    connect(wizard, &QDialog::finished, this, [this, wizard](int result) {
        if (result == QDialog::Accepted) {
            appendLog("=== User Profile Restore Wizard Completed ===");
            appendLog(QString("Restored from: %1").arg(wizard->backupPath()));
            Q_EMIT statusMessage("User profile restore completed", sak::kTimerStatusDefaultMs);
        } else {
            appendLog("User profile restore wizard cancelled");
            Q_EMIT statusMessage("Restore wizard cancelled", sak::kTimerStatusMessageMs);
        }
        wizard->deleteLater();
    });

    // Show wizard
    appendLog("Launching restore wizard...");
    Q_EMIT statusMessage("Restore wizard launched", sak::kTimerStatusMessageMs);
    wizard->show();
    wizard->raise();
    wizard->activateWindow();
}

void UserMigrationPanel::appendLog(const QString& message)
{
    Q_EMIT logOutput(message);
}

void UserMigrationPanel::onSettingsClicked()
{
    auto& config = ConfigManager::instance();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("User Migration Settings"));
    dialog.setMinimumWidth(440);
    auto* layout = new QVBoxLayout(&dialog);

    auto* formLayout = new QFormLayout();

    auto* threadSpin = new QSpinBox(&dialog);
    threadSpin->setRange(1, 16);
    threadSpin->setValue(config.getBackupThreadCount());
    formLayout->addRow(
        InfoButton::createInfoLabel(tr("Thread Count:"),
            tr("Number of parallel copy threads \u2014 higher values speed up backup but increase "
               "CPU and disk I/O load"), &dialog),
        threadSpin);

    auto* verifyCheck = new QCheckBox(tr("Verify files using MD5 hash after backup"), &dialog);
    verifyCheck->setChecked(config.getBackupVerifyMD5());
    formLayout->addRow(
        InfoButton::createInfoLabel(tr("Verify MD5:"),
            tr("Re-read each copied file and verify its MD5 checksum matches the original \u2014 "
               "slower but ensures integrity"), &dialog),
        verifyCheck);

    auto* locRow = new QHBoxLayout();
    auto* locEdit = new QLineEdit(config.getLastBackupLocation(), &dialog);
    locEdit->setReadOnly(true);
    auto* browseBtn = new QPushButton(tr("Browse..."), &dialog);
    connect(browseBtn, &QPushButton::clicked, &dialog, [&locEdit, &dialog]() {
        QString dir = QFileDialog::getExistingDirectory(&dialog, tr("Select Backup Location"),
            locEdit->text());
        if (!dir.isEmpty()) locEdit->setText(dir);
    });
    locRow->addWidget(locEdit);
    locRow->addWidget(browseBtn);
    formLayout->addRow(
        InfoButton::createInfoLabel(tr("Backup Location:"),
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

} // namespace sak
