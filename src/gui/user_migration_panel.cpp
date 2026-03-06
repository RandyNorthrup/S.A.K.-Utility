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
#include <QMessageBox>
#include <QDateTime>
#include <QFont>
#include <QScrollArea>
#include <QFrame>
#include <QIcon>
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
    Q_ASSERT(!objectName().isEmpty() || true);  // widget valid
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(sak::ui::kMarginMedium, sak::ui::kMarginMedium,
        sak::ui::kMarginMedium, sak::ui::kMarginMedium);
    mainLayout->setSpacing(sak::ui::kSpacingLarge);

    scrollArea->setWidget(contentWidget);
    rootLayout->addWidget(scrollArea);

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(contentWidget, QStringLiteral(":/icons/icons/panel_backup_restore.svg"),
        tr("Backup and Restore"),
        tr("Comprehensive backup and restore wizards for Windows user profiles."), mainLayout);

    // ── Card stylesheet ─────────────────────────────────────────────────
    const QString cardStyle = QString(
        "QFrame {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "  padding: %3px;"
        "}"
        "QFrame:hover {"
        "  border-color: %4;"
        "}")
        .arg(sak::ui::kColorBgWhite)
        .arg(sak::ui::kColorBorderDefault)
        .arg(sak::ui::kMarginMedium)
        .arg(sak::ui::kColorPrimary);

    const QString cardTitleStyle = QString(
        "font-size: %1pt; font-weight: 700; color: %2; border: none; background: transparent;")
        .arg(sak::ui::kFontSizeSection)
        .arg(sak::ui::kColorTextHeading);

    const QString cardDescStyle = QString(
        "font-size: %1pt; color: %2; border: none; background: transparent;")
        .arg(sak::ui::kFontSizeBody)
        .arg(sak::ui::kColorTextSecondary);

    // ── Row: Backup | Restore ───────────────────────────────────────────
    auto* cardRow = new QHBoxLayout();
    cardRow->setSpacing(sak::ui::kSpacingLarge);

    // Backup card
    auto* backupCard = new QFrame(contentWidget);
    backupCard->setStyleSheet(cardStyle);
    auto* backupLayout = new QVBoxLayout(backupCard);
    backupLayout->setSpacing(sak::ui::kSpacingMedium);
    backupLayout->setContentsMargins(0, 0, 0, 0);

    auto* backupLogo = new QLabel(backupCard);
    backupLogo->setPixmap(QIcon(":/icons/icons/backup.svg").pixmap(96, 96));
    backupLogo->setAlignment(Qt::AlignCenter);
    backupLogo->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    backupLayout->addWidget(backupLogo);

    auto* backupTitle = new QLabel(tr("Backup User Profiles"), backupCard);
    backupTitle->setStyleSheet(cardTitleStyle);
    backupLayout->addWidget(backupTitle);

    auto* backupDesc = new QLabel(
        tr("Scan and select users, choose folders, configure filters, and create backup packages."),
        backupCard);
    backupDesc->setWordWrap(true);
    backupDesc->setStyleSheet(cardDescStyle);
    backupLayout->addWidget(backupDesc);

    backupLayout->addStretch();

    m_backupButton = new QPushButton(tr("Start Backup Wizard..."), backupCard);
    m_backupButton->setMinimumHeight(sak::kButtonHeightTall);
    m_backupButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_backupButton->setToolTip(tr("Step-by-step wizard to select apps, configure options, "
                                  "and create backups"));
    m_backupButton->setAccessibleName(QStringLiteral("Start Backup Wizard"));
    backupLayout->addWidget(m_backupButton);

    cardRow->addWidget(backupCard);

    // Restore card
    auto* restoreCard = new QFrame(contentWidget);
    restoreCard->setStyleSheet(cardStyle);
    auto* restoreLayout = new QVBoxLayout(restoreCard);
    restoreLayout->setSpacing(sak::ui::kSpacingMedium);
    restoreLayout->setContentsMargins(0, 0, 0, 0);

    auto* restoreLogo = new QLabel(restoreCard);
    restoreLogo->setPixmap(QIcon(":/icons/icons/restore.svg").pixmap(96, 96));
    restoreLogo->setAlignment(Qt::AlignCenter);
    restoreLogo->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    restoreLayout->addWidget(restoreLogo);

    auto* restoreTitle = new QLabel(tr("Restore User Profiles"), restoreCard);
    restoreTitle->setStyleSheet(cardTitleStyle);
    restoreLayout->addWidget(restoreTitle);

    auto* restoreDesc = new QLabel(
        tr("Select backup, map users, configure merge options, and restore data with permissions."),
        restoreCard);
    restoreDesc->setWordWrap(true);
    restoreDesc->setStyleSheet(cardDescStyle);
    restoreLayout->addWidget(restoreDesc);

    restoreLayout->addStretch();

    m_restoreButton = new QPushButton(tr("Start Restore Wizard..."), restoreCard);
    m_restoreButton->setMinimumHeight(sak::kButtonHeightTall);
    m_restoreButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_restoreButton->setToolTip(tr("Step-by-step wizard to select backups, map users, "
                                   "and restore data"));
    m_restoreButton->setAccessibleName(QStringLiteral("Start Restore Wizard"));
    restoreLayout->addWidget(m_restoreButton);

    cardRow->addWidget(restoreCard);

    mainLayout->addLayout(cardRow);

    mainLayout->addStretch();

    // Bottom row: Settings + Log toggle (outside scroll area, pinned to bottom)
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(sak::ui::kMarginMedium, sak::ui::kMarginTight,
        sak::ui::kMarginMedium, sak::ui::kMarginSmall);

    auto* settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(QStringLiteral("User Migration Settings"));
    connect(settingsBtn, &QPushButton::clicked, this, &UserMigrationPanel::onSettingsClicked);
    bottomLayout->addWidget(settingsBtn);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    bottomLayout->addWidget(m_logToggle);
    bottomLayout->addStretch();
    rootLayout->addLayout(bottomLayout);
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
    Q_ASSERT(m_dataManager);
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
    Q_ASSERT(m_dataManager);
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
    Q_ASSERT(!message.isEmpty());
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
