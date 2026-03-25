// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file user_migration_panel.cpp
/// @brief Implements the user profile migration panel UI for backup and restore

#include "sak/user_migration_panel.h"

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/actions/screenshot_settings_action.h"
#include "sak/config_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/quick_action_controller.h"
#include "sak/style_constants.h"
#include "sak/user_data_manager.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/widget_helpers.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QMessageBox>
#include <QProgressBar>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace sak {

UserMigrationPanel::UserMigrationPanel(QWidget* parent)
    : QWidget(parent), m_dataManager(std::make_shared<UserDataManager>()) {
    setupUi();
    setupConnections();
    createQuickActions();

    appendLog("User Migration Panel initialized");
    appendLog("Click 'Backup User Profiles...' to start the migration wizard");
}

UserMigrationPanel::~UserMigrationPanel() = default;

void UserMigrationPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium);
    mainLayout->setSpacing(sak::ui::kSpacingLarge);

    scrollArea->setWidget(contentWidget);
    rootLayout->addWidget(scrollArea);

    sak::createPanelHeader(contentWidget,
                           QStringLiteral(":/icons/icons/panel_backup_restore.svg"),
                           tr("Backup and Restore"),
                           tr("Comprehensive backup and restore wizards"
                              " for Windows user profiles."),
                           mainLayout);

    createMigrationCards(contentWidget, mainLayout);
    mainLayout->addWidget(createQuickToolsSection(contentWidget));

    mainLayout->addStretch();

    // Bottom row: Settings + Log toggle
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(sak::ui::kMarginMedium,
                                     sak::ui::kMarginTight,
                                     sak::ui::kMarginMedium,
                                     sak::ui::kMarginSmall);

    auto* settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(QStringLiteral("User Migration Settings"));
    connect(settingsBtn, &QPushButton::clicked, this, &UserMigrationPanel::onSettingsClicked);
    bottomLayout->addWidget(settingsBtn);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    bottomLayout->addWidget(m_logToggle);
    bottomLayout->addStretch();
    rootLayout->addLayout(bottomLayout);
}

void UserMigrationPanel::createMigrationCards(QWidget* parent, QVBoxLayout* layout) {
    Q_ASSERT(layout);
    const QString card_style = QString(
                                   "QFrame { background-color: %1; border: 1px solid %2;"
                                   " border-radius: 10px; padding: %3px; }"
                                   "QFrame:hover { border-color: %4; }")
                                   .arg(sak::ui::kColorBgWhite)
                                   .arg(sak::ui::kColorBorderDefault)
                                   .arg(sak::ui::kMarginMedium)
                                   .arg(sak::ui::kColorPrimary);

    auto* row = new QHBoxLayout();
    row->setSpacing(sak::ui::kSpacingLarge);

    row->addWidget(createMigrationCard(parent,
                                       {card_style,
                                        QStringLiteral(":/icons/icons/backup.svg"),
                                        tr("Backup User Profiles"),
                                        tr("Scan and select users, choose folders, configure"
                                           " filters, and create backup packages."),
                                        tr("Start Backup Wizard..."),
                                        sak::ui::kPrimaryButtonStyle,
                                        tr("Step-by-step wizard to select apps, configure"
                                           " options, and create backups"),
                                        QStringLiteral("Start Backup Wizard")},
                                       m_backupButton));

    row->addWidget(createMigrationCard(parent,
                                       {card_style,
                                        QStringLiteral(":/icons/icons/restore.svg"),
                                        tr("Restore User Profiles"),
                                        tr("Select backup, map users, configure merge"
                                           " options, and restore data with permissions."),
                                        tr("Start Restore Wizard..."),
                                        sak::ui::kSecondaryButtonStyle,
                                        tr("Step-by-step wizard to select backups, map"
                                           " users, and restore data"),
                                        QStringLiteral("Start Restore Wizard")},
                                       m_restoreButton));

    layout->addLayout(row);
}

QFrame* UserMigrationPanel::createMigrationCard(QWidget* parent,
                                                const MigrationCardConfig& config,
                                                QPushButton*& btn) {
    const QString title_style = QString(
                                    "font-size: %1pt; font-weight: 700; color: %2;"
                                    " border: none; background: transparent;")
                                    .arg(sak::ui::kFontSizeSection)
                                    .arg(sak::ui::kColorTextHeading);

    const QString desc_style = QString(
                                   "font-size: %1pt; color: %2;"
                                   " border: none; background: transparent;")
                                   .arg(sak::ui::kFontSizeBody)
                                   .arg(sak::ui::kColorTextSecondary);

    constexpr int kLogoSize = 96;
    auto* card = new QFrame(parent);
    card->setStyleSheet(config.card_style);
    auto* lay = new QVBoxLayout(card);
    lay->setSpacing(sak::ui::kSpacingMedium);
    lay->setContentsMargins(0, 0, 0, 0);

    auto* logo = new QLabel(card);
    logo->setPixmap(QIcon(config.icon).pixmap(kLogoSize, kLogoSize));
    logo->setAlignment(Qt::AlignCenter);
    logo->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    lay->addWidget(logo);

    auto* title_label = new QLabel(config.title, card);
    title_label->setStyleSheet(title_style);
    lay->addWidget(title_label);

    auto* desc_label = new QLabel(config.desc, card);
    desc_label->setWordWrap(true);
    desc_label->setStyleSheet(desc_style);
    lay->addWidget(desc_label);

    lay->addStretch();
    btn = new QPushButton(config.btn_text, card);
    btn->setMinimumHeight(sak::kButtonHeightTall);
    btn->setStyleSheet(config.btn_style);
    btn->setToolTip(config.tip);
    btn->setAccessibleName(config.acc);
    lay->addWidget(btn);
    return card;
}

void UserMigrationPanel::setupConnections() {
    connect(m_backupButton, &QPushButton::clicked, this, &UserMigrationPanel::onBackupSelected);
    connect(m_restoreButton, &QPushButton::clicked, this, &UserMigrationPanel::onRestoreBackup);
}

void UserMigrationPanel::onBackupSelected() {
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

void UserMigrationPanel::onRestoreBackup() {
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

void UserMigrationPanel::appendLog(const QString& message) {
    Q_ASSERT(!message.isEmpty());
    Q_EMIT logOutput(message);
}

// ============================================================================
// Quick Tools Section
// ============================================================================

QGroupBox* UserMigrationPanel::createQuickToolsSection(QWidget* parent) {
    auto* group = new QGroupBox(tr("Quick Tools"), parent);
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(sak::ui::kSpacingDefault);

    auto* desc = new QLabel(tr("One-click system protection and documentation tools"), group);
    desc->setStyleSheet(QString("color: %1; font-size: %2pt;")
                            .arg(sak::ui::kColorTextSecondary)
                            .arg(sak::ui::kFontSizeBody));
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto* btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(sak::ui::kSpacingDefault);

    auto make_action_button = [&](const QString& text, const QString& tooltip, const QString& key) {
        auto* btn = new QPushButton(text, group);
        btn->setMinimumHeight(sak::kButtonHeightTall);
        btn->setToolTip(tooltip);
        btn->setAccessibleName(text);
        m_action_buttons.insert(key, btn);
        btn_layout->addWidget(btn);
        return btn;
    };

    make_action_button(tr("Screenshot Settings"),
                       tr("Capture screenshots of all Windows Settings pages"),
                       QStringLiteral("Screenshot Settings"));

    make_action_button(tr("BitLocker Key Backup"),
                       tr("Backup BitLocker recovery keys (requires admin)"),
                       QStringLiteral("BitLocker Key Backup"));

    btn_layout->addStretch();
    layout->addLayout(btn_layout);

    // Status + progress
    m_action_status_label = new QLabel(tr("Ready"), group);
    m_action_status_label->setStyleSheet(QString("color: %1;").arg(sak::ui::kColorTextSecondary));
    layout->addWidget(m_action_status_label);

    m_action_progress_bar = new QProgressBar(group);
    m_action_progress_bar->setRange(0, 100);
    m_action_progress_bar->setValue(0);
    m_action_progress_bar->setVisible(false);
    layout->addWidget(m_action_progress_bar);

    return group;
}

void UserMigrationPanel::createQuickActions() {
    constexpr auto kDefaultBackupPath = "C:/SAK_Backups";
    const auto backup_path = QString::fromLatin1(kDefaultBackupPath);

    m_action_controller = new QuickActionController(this);
    m_action_controller->setBackupLocation(backup_path);

    m_action_controller->registerAction(std::make_unique<ScreenshotSettingsAction>(backup_path));
    m_action_controller->registerAction(std::make_unique<BackupBitlockerKeysAction>(backup_path));

    // Connect controller signals
    connect(m_action_controller,
            &QuickActionController::actionExecutionProgress,
            this,
            &UserMigrationPanel::onQuickActionProgress,
            Qt::QueuedConnection);
    connect(m_action_controller,
            &QuickActionController::actionExecutionComplete,
            this,
            &UserMigrationPanel::onQuickActionComplete,
            Qt::QueuedConnection);
    connect(m_action_controller,
            &QuickActionController::actionError,
            this,
            &UserMigrationPanel::onQuickActionError,
            Qt::QueuedConnection);
    connect(
        m_action_controller,
        &QuickActionController::logMessage,
        this,
        [this](const QString& msg) { appendLog(msg); },
        Qt::QueuedConnection);

    // Connect buttons to actions
    for (auto it = m_action_buttons.constBegin(); it != m_action_buttons.constEnd(); ++it) {
        const QString action_name = it.key();
        QPushButton* btn = it.value();
        QuickAction* action = m_action_controller->getAction(action_name);
        if (action) {
            connect(btn, &QPushButton::clicked, this, [this, action]() {
                onQuickActionClicked(action);
            });
        }
    }
}

void UserMigrationPanel::onQuickActionClicked(QuickAction* action) {
    Q_ASSERT(action);
    appendLog(QString("Executing: %1").arg(action->name()));
    m_action_status_label->setText(QString("Running: %1...").arg(action->name()));
    m_action_progress_bar->setValue(0);
    m_action_progress_bar->setVisible(true);
    m_action_controller->executeAction(action->name(), false);
}

void UserMigrationPanel::onQuickActionProgress(QuickAction* action,
                                               const QString& message,
                                               int progress) {
    Q_ASSERT(action);
    m_action_status_label->setText(message);
    m_action_progress_bar->setValue(progress);
}

void UserMigrationPanel::onQuickActionComplete(QuickAction* action) {
    Q_ASSERT(action);
    const auto& result = action->lastExecutionResult();
    const QString status = result.success
                               ? QString("Completed: %1").arg(action->name())
                               : QString("Failed: %1 - %2").arg(action->name(), result.message);
    m_action_status_label->setText(status);
    m_action_progress_bar->setVisible(false);
    appendLog(status);
    Q_EMIT statusMessage(status, sak::kTimerStatusDefaultMs);
}

void UserMigrationPanel::onQuickActionError(QuickAction* action, const QString& error_message) {
    Q_ASSERT(action);
    const QString status = QString("Error: %1 - %2").arg(action->name(), error_message);
    m_action_status_label->setText(status);
    m_action_progress_bar->setVisible(false);
    appendLog(status);
    sak::logError("Quick action error: {} - {}",
                  action->name().toStdString(),
                  error_message.toStdString());
}

void UserMigrationPanel::onSettingsClicked() {
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
        InfoButton::createInfoLabel(
            tr("Thread Count:"),
            tr("Number of parallel copy threads \u2014 higher values speed up backup but increase "
               "CPU and disk I/O load"),
            &dialog),
        threadSpin);

    auto* verifyCheck = new QCheckBox(tr("Verify files using MD5 hash after backup"), &dialog);
    verifyCheck->setChecked(config.getBackupVerifyMD5());
    formLayout->addRow(
        InfoButton::createInfoLabel(
            tr("Verify MD5:"),
            tr("Re-read each copied file and verify its MD5 checksum matches the original \u2014 "
               "slower but ensures integrity"),
            &dialog),
        verifyCheck);

    auto* locRow = new QHBoxLayout();
    auto* locEdit = new QLineEdit(config.getLastBackupLocation(), &dialog);
    locEdit->setReadOnly(true);
    auto* browseBtn = new QPushButton(tr("Browse..."), &dialog);
    connect(browseBtn, &QPushButton::clicked, &dialog, [&locEdit, &dialog]() {
        QString dir = QFileDialog::getExistingDirectory(&dialog,
                                                        tr("Select Backup Location"),
                                                        locEdit->text());
        if (!dir.isEmpty()) {
            locEdit->setText(dir);
        }
    });
    locRow->addWidget(locEdit);
    locRow->addWidget(browseBtn);
    formLayout->addRow(
        InfoButton::createInfoLabel(
            tr("Backup Location:"), tr("Choose the destination folder for backup files"), &dialog),
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

}  // namespace sak
