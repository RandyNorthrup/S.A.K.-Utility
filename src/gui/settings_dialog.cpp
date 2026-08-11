// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/settings_dialog.h"

#include "sak/config_manager.h"
#include "sak/info_button.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace sak {

namespace {
constexpr int kBackupThreadCountMin = 1;
constexpr int kBackupThreadCountMax = 16;
}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setupUi();
    loadSettings();

    // Connect signals
    connect(m_okButton, &QPushButton::clicked, this, &SettingsDialog::onOkClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &SettingsDialog::onCancelClicked);
    connect(m_applyButton, &QPushButton::clicked, this, &SettingsDialog::onApplyClicked);
    connect(m_resetButton, &QPushButton::clicked, this, &SettingsDialog::onResetToDefaultsClicked);
}

void SettingsDialog::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    setWindowTitle(tr("Settings"));
    setMinimumSize(sak::kDialogWidthLarge, sak::kDialogHeightMedium);
    resize(sak::kDialogWidthXLarge, sak::kDialogHeightLarge);
    setSizeGripEnabled(true);

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium);

    // Create tab widget
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setAccessibleName(tr("Settings tabs"));
    createBackupTab();

    main_layout->addWidget(m_tabWidget);

    // Button layout
    auto* button_layout = new QHBoxLayout();

    m_resetButton = new QPushButton(tr("Reset to Defaults"), this);
    m_resetButton->setAccessibleName(tr("Reset settings to defaults"));
    m_resetButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    button_layout->addWidget(m_resetButton);

    button_layout->addStretch();

    m_okButton = new QPushButton(tr("OK"), this);
    m_okButton->setAccessibleName(tr("Save settings"));
    m_okButton->setDefault(true);
    m_okButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_okButton);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setAccessibleName(tr("Cancel settings changes"));
    m_cancelButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    button_layout->addWidget(m_cancelButton);

    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setAccessibleName(tr("Apply settings changes"));
    m_applyButton->setEnabled(false);
    m_applyButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_applyButton);

    main_layout->addLayout(button_layout);

    Q_ASSERT(m_okButton);
}

void SettingsDialog::createBackupTab() {
    Q_ASSERT(m_tabWidget);
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    layout->addWidget(createBackupSettingsGroup(widget));
    layout->addWidget(createQuickActionsGroup(widget));

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Backup"));

    connect(m_backupThreadCount,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &SettingsDialog::onSettingChanged);
    constexpr auto kCheckBoxChangedSignal = &QCheckBox::toggled;
    connect(m_backupVerifyMD5, kCheckBoxChangedSignal, this, &SettingsDialog::onSettingChanged);
    connect(m_quickActionsBackupLocation,
            &QLineEdit::textChanged,
            this,
            &SettingsDialog::onSettingChanged);
    connect(m_quickActionsConfirm, kCheckBoxChangedSignal, this, &SettingsDialog::onSettingChanged);
    connect(m_quickActionsNotifications,
            kCheckBoxChangedSignal,
            this,
            &SettingsDialog::onSettingChanged);
    connect(m_quickActionsLogging, kCheckBoxChangedSignal, this, &SettingsDialog::onSettingChanged);
    connect(
        m_quickActionsCompress, kCheckBoxChangedSignal, this, &SettingsDialog::onSettingChanged);
}

QGroupBox* SettingsDialog::createBackupSettingsGroup(QWidget* parent) {
    auto* backup_group = new QGroupBox(tr("Backup Settings"));
    auto* backup_layout = new QFormLayout();

    m_backupThreadCount = new QSpinBox();
    m_backupThreadCount->setRange(kBackupThreadCountMin, kBackupThreadCountMax);
    backup_layout->addRow(InfoButton::createInfoLabel(
                              tr("Thread Count:"),
                              tr("Higher values speed up backup but use more CPU and disk I/O"),
                              parent),
                          m_backupThreadCount);

    m_backupVerifyMD5 = new QCheckBox(tr("Verify files using MD5 hash after backup"));
    backup_layout->addRow(
        InfoButton::createInfoLabel(tr("Verify MD5:"),
                                    tr("Re-read each copied file and verify its MD5 checksum "
                                       "matches the original -- slower "
                                       "but ensures integrity"),
                                    parent),
        m_backupVerifyMD5);

    auto* location_layout = new QHBoxLayout();
    m_lastBackupLocation = new QLineEdit();
    m_lastBackupLocation->setReadOnly(true);
    auto* browse_button = new QPushButton(tr("Browse..."));
    browse_button->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(browse_button, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this,
                                                              tr("Select Backup Location"),
                                                              m_lastBackupLocation->text());
        if (!dir.isEmpty()) {
            m_lastBackupLocation->setText(dir);
            onSettingChanged();
        }
    });
    location_layout->addWidget(m_lastBackupLocation);
    location_layout->addWidget(browse_button);
    backup_layout->addRow(
        InfoButton::createInfoLabel(
            tr("Last Location:"), tr("The most recently used backup destination folder"), parent),
        location_layout);

    backup_group->setLayout(backup_layout);
    return backup_group;
}

QGroupBox* SettingsDialog::createQuickActionsGroup(QWidget* parent) {
    auto* quick_actions_group = new QGroupBox(tr("Quick Actions"));
    auto* quick_actions_layout = new QFormLayout();

    auto* qa_location_layout = new QHBoxLayout();
    m_quickActionsBackupLocation = new QLineEdit();
    m_quickActionsBackupLocation->setPlaceholderText(tr("C:\\SAK_Backups"));
    auto* qa_browse_button = new QPushButton(tr("Browse..."));
    qa_browse_button->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(qa_browse_button, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Select Quick Actions Backup Location"), m_quickActionsBackupLocation->text());
        if (!dir.isEmpty()) {
            m_quickActionsBackupLocation->setText(dir);
            onSettingChanged();
        }
    });
    qa_location_layout->addWidget(m_quickActionsBackupLocation);
    qa_location_layout->addWidget(qa_browse_button);
    quick_actions_layout->addRow(
        InfoButton::createInfoLabel(tr("Backup Location:"),
                                    tr("Default location for Quick Actions backup operations"),
                                    parent),
        qa_location_layout);

    m_quickActionsConfirm = new QCheckBox(tr("Confirm before executing actions"));
    quick_actions_layout->addRow(
        InfoButton::createInfoLabel(QString(),
                                    tr("Show a confirmation dialog before each "
                                       "action runs to prevent accidental execution"),
                                    parent),
        m_quickActionsConfirm);

    m_quickActionsNotifications = new QCheckBox(tr("Show completion notifications"));
    quick_actions_layout->addRow(
        InfoButton::createInfoLabel(
            QString(),
            tr("Display a status bar notification when an action finishes or fails"),
            parent),
        m_quickActionsNotifications);

    m_quickActionsLogging = new QCheckBox(tr("Enable detailed logging"));
    quick_actions_layout->addRow(
        InfoButton::createInfoLabel(
            QString(),
            tr("Write detailed progress and scan information to the log window"),
            parent),
        m_quickActionsLogging);

    m_quickActionsCompress = new QCheckBox(tr("Compress backups (saves space)"));
    quick_actions_layout->addRow(
        InfoButton::createInfoLabel(
            QString(),
            tr("Use ZIP compression for backup output files -- slower but uses less disk space"),
            parent),
        m_quickActionsCompress);

    quick_actions_group->setLayout(quick_actions_layout);
    return quick_actions_group;
}

void SettingsDialog::loadSettings() {
    Q_ASSERT(m_backupThreadCount);
    Q_ASSERT(m_backupVerifyMD5);
    const auto& config = ConfigManager::instance();

    // Backup
    m_backupThreadCount->setValue(config.getBackupThreadCount());
    m_backupVerifyMD5->setChecked(config.getBackupVerifyMD5());
    m_lastBackupLocation->setText(config.getLastBackupLocation());

    // Quick Actions
    {
        const QSettings qa_settings("SAK", "QuickActions");
        m_quickActionsBackupLocation->setText(
            qa_settings.value("backup_location", "C:\\SAK_Backups").toString());
        m_quickActionsConfirm->setChecked(
            qa_settings.value("confirm_before_execute", true).toBool());
        m_quickActionsNotifications->setChecked(
            qa_settings.value("show_notifications", true).toBool());
        m_quickActionsLogging->setChecked(qa_settings.value("enable_logging", true).toBool());
        m_quickActionsCompress->setChecked(qa_settings.value("compress_backups", true).toBool());
    }

    m_settingsModified = false;
    m_applyButton->setEnabled(false);
}

void SettingsDialog::saveSettings() {
    Q_ASSERT(m_backupThreadCount);
    Q_ASSERT(m_backupVerifyMD5);
    auto& config = ConfigManager::instance();

    // Backup
    config.setBackupThreadCount(m_backupThreadCount->value());
    config.setBackupVerifyMD5(m_backupVerifyMD5->isChecked());
    config.setLastBackupLocation(m_lastBackupLocation->text());

    // Quick Actions
    {
        QSettings qa_settings("SAK", "QuickActions");
        qa_settings.setValue("backup_location", m_quickActionsBackupLocation->text());
        qa_settings.setValue("confirm_before_execute", m_quickActionsConfirm->isChecked());
        qa_settings.setValue("show_notifications", m_quickActionsNotifications->isChecked());
        qa_settings.setValue("enable_logging", m_quickActionsLogging->isChecked());
        qa_settings.setValue("compress_backups", m_quickActionsCompress->isChecked());
    }

    // Sync to disk
    config.sync();

    m_settingsModified = false;
    m_applyButton->setEnabled(false);
}

bool SettingsDialog::applySettings() {
    if (!validateSettings()) {
        return false;
    }
    saveSettings();
    return true;
}

bool SettingsDialog::validateSettings() {
    Q_ASSERT(m_backupThreadCount);
    Q_ASSERT(m_tabWidget);
    // Validate thread count
    if (m_backupThreadCount->value() < 1) {
        sak::logWarning("Invalid setting: backup thread count less than 1 (value: {})",
                        m_backupThreadCount->value());
        sak::showWarningLogged(this, tr("Invalid Setting"), tr("Thread count must be at least 1."));
        m_tabWidget->setCurrentIndex(0);  // Switch to Backup tab
        m_backupThreadCount->setFocus();
        return false;
    }

    return true;
}

void SettingsDialog::onApplyClicked() {
    applySettings();
}

void SettingsDialog::onOkClicked() {
    // Only close on a fully successful apply: if validation fails, keep the dialog
    // open so the invalid setting cannot be committed behind an "OK".
    if (m_settingsModified && !applySettings()) {
        return;
    }
    accept();
}

void SettingsDialog::onCancelClicked() {
    if (m_settingsModified) {
        auto reply = sak::showQuestionLogged(
            this,
            tr("Unsaved Changes"),
            tr("You have unsaved changes. Are you sure you want to discard them?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);

        if (reply != QMessageBox::Yes) {
            return;
        }
    }
    reject();
}

void SettingsDialog::onResetToDefaultsClicked() {
    auto reply = sak::showQuestionLogged(
        this,
        tr("Reset to Defaults"),
        tr("Are you sure you want to reset all settings to their default values?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        ConfigManager::instance().resetToDefaults();
        loadSettings();
        sak::showInformationLogged(this,
                                   tr("Reset Complete"),
                                   tr("All settings have been reset to defaults."));
    }
}

void SettingsDialog::onSettingChanged() {
    m_settingsModified = true;
    m_applyButton->setEnabled(true);
}

}  // namespace sak
