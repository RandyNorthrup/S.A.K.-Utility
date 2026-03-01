// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/settings_dialog.h"
#include "sak/config_manager.h"
#include "sak/info_button.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QPushButton>
#include <QSettings>

namespace sak {

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUi();
    loadSettings();
    
    // Connect signals
    connect(m_okButton, &QPushButton::clicked, this, &SettingsDialog::onOkClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &SettingsDialog::onCancelClicked);
    connect(m_applyButton, &QPushButton::clicked, this, &SettingsDialog::onApplyClicked);
    connect(m_resetButton, &QPushButton::clicked, this, &SettingsDialog::onResetToDefaultsClicked);
}

void SettingsDialog::setupUi() {
    setWindowTitle(tr("Settings"));
    setMinimumSize(520, 420);
    resize(640, 540);
    setSizeGripEnabled(true);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // Create tab widget
    m_tabWidget = new QTabWidget(this);
    createBackupTab();

    mainLayout->addWidget(m_tabWidget);

    // Button layout
    auto* buttonLayout = new QHBoxLayout();
    
    m_resetButton = new QPushButton(tr("Reset to Defaults"), this);
    buttonLayout->addWidget(m_resetButton);
    
    buttonLayout->addStretch();
    
    m_okButton = new QPushButton(tr("OK"), this);
    m_okButton->setDefault(true);
    buttonLayout->addWidget(m_okButton);
    
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    buttonLayout->addWidget(m_cancelButton);
    
    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setEnabled(false);
    buttonLayout->addWidget(m_applyButton);

    mainLayout->addLayout(buttonLayout);
}

void SettingsDialog::createBackupTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    layout->addWidget(createBackupSettingsGroup(widget));
    layout->addWidget(createQuickActionsGroup(widget));

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Backup"));

    connect(m_backupThreadCount, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_backupVerifyMD5, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_quickActionsBackupLocation, &QLineEdit::textChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_quickActionsConfirm, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_quickActionsNotifications, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_quickActionsLogging, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_quickActionsCompress, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
}

QGroupBox* SettingsDialog::createBackupSettingsGroup(QWidget* parent) {
    auto* backupGroup = new QGroupBox(tr("Backup Settings"));
    auto* backupLayout = new QFormLayout();

    m_backupThreadCount = new QSpinBox();
    m_backupThreadCount->setRange(1, 16);
    backupLayout->addRow(
        InfoButton::createInfoLabel(tr("Thread Count:"),
            tr("Higher values speed up backup but use more CPU and disk I/O"), parent),
        m_backupThreadCount);

    m_backupVerifyMD5 = new QCheckBox(tr("Verify files using MD5 hash after backup"));
    backupLayout->addRow(
        InfoButton::createInfoLabel(tr("Verify MD5:"),
            tr("Re-read each copied file and verify its MD5 checksum matches the original — slower but ensures integrity"), parent),
        m_backupVerifyMD5);

    auto* locationLayout = new QHBoxLayout();
    m_lastBackupLocation = new QLineEdit();
    m_lastBackupLocation->setReadOnly(true);
    auto* browseButton = new QPushButton(tr("Browse..."));
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this,
            tr("Select Backup Location"),
            m_lastBackupLocation->text()
        );
        if (!dir.isEmpty()) {
            m_lastBackupLocation->setText(dir);
            onSettingChanged();
        }
    });
    locationLayout->addWidget(m_lastBackupLocation);
    locationLayout->addWidget(browseButton);
    backupLayout->addRow(
        InfoButton::createInfoLabel(tr("Last Location:"),
            tr("The most recently used backup destination folder"), parent),
        locationLayout);

    backupGroup->setLayout(backupLayout);
    return backupGroup;
}

QGroupBox* SettingsDialog::createQuickActionsGroup(QWidget* parent) {
    auto* quickActionsGroup = new QGroupBox(tr("Quick Actions"));
    auto* quickActionsLayout = new QFormLayout();

    auto* qaLocationLayout = new QHBoxLayout();
    m_quickActionsBackupLocation = new QLineEdit();
    m_quickActionsBackupLocation->setPlaceholderText(tr("C:\\SAK_Backups"));
    auto* qaBrowseButton = new QPushButton(tr("Browse..."));
    connect(qaBrowseButton, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this,
            tr("Select Quick Actions Backup Location"),
            m_quickActionsBackupLocation->text()
        );
        if (!dir.isEmpty()) {
            m_quickActionsBackupLocation->setText(dir);
            onSettingChanged();
        }
    });
    qaLocationLayout->addWidget(m_quickActionsBackupLocation);
    qaLocationLayout->addWidget(qaBrowseButton);
    quickActionsLayout->addRow(
        InfoButton::createInfoLabel(tr("Backup Location:"),
            tr("Default location for Quick Actions backup operations"), parent),
        qaLocationLayout);

    m_quickActionsConfirm = new QCheckBox(tr("Confirm before executing actions"));
    quickActionsLayout->addRow(
        InfoButton::createInfoLabel(QString(),
            tr("Show a confirmation dialog before each action runs to prevent accidental execution"), parent),
        m_quickActionsConfirm);

    m_quickActionsNotifications = new QCheckBox(tr("Show completion notifications"));
    quickActionsLayout->addRow(
        InfoButton::createInfoLabel(QString(),
            tr("Display a status bar notification when an action finishes or fails"), parent),
        m_quickActionsNotifications);

    m_quickActionsLogging = new QCheckBox(tr("Enable detailed logging"));
    quickActionsLayout->addRow(
        InfoButton::createInfoLabel(QString(),
            tr("Write detailed progress and scan information to the log window"), parent),
        m_quickActionsLogging);

    m_quickActionsCompress = new QCheckBox(tr("Compress backups (saves space)"));
    quickActionsLayout->addRow(
        InfoButton::createInfoLabel(QString(),
            tr("Use ZIP compression for backup output files — slower but uses less disk space"), parent),
        m_quickActionsCompress);

    quickActionsGroup->setLayout(quickActionsLayout);
    return quickActionsGroup;
}

void SettingsDialog::loadSettings() {
    auto& config = ConfigManager::instance();

    // Backup
    m_backupThreadCount->setValue(config.getBackupThreadCount());
    m_backupVerifyMD5->setChecked(config.getBackupVerifyMD5());
    m_lastBackupLocation->setText(config.getLastBackupLocation());

    // Quick Actions
    {
        QSettings qaSettings("SAK", "QuickActions");
        m_quickActionsBackupLocation->setText(qaSettings.value("backup_location", "C:\\SAK_Backups").toString());
        m_quickActionsConfirm->setChecked(qaSettings.value("confirm_before_execute", true).toBool());
        m_quickActionsNotifications->setChecked(qaSettings.value("show_notifications", true).toBool());
        m_quickActionsLogging->setChecked(qaSettings.value("enable_logging", true).toBool());
        m_quickActionsCompress->setChecked(qaSettings.value("compress_backups", true).toBool());
    }

    m_settingsModified = false;
    m_applyButton->setEnabled(false);
}

void SettingsDialog::saveSettings() {
    auto& config = ConfigManager::instance();

    // Backup
    config.setBackupThreadCount(m_backupThreadCount->value());
    config.setBackupVerifyMD5(m_backupVerifyMD5->isChecked());
    config.setLastBackupLocation(m_lastBackupLocation->text());

    // Quick Actions
    {
        QSettings qaSettings("SAK", "QuickActions");
        qaSettings.setValue("backup_location", m_quickActionsBackupLocation->text());
        qaSettings.setValue("confirm_before_execute", m_quickActionsConfirm->isChecked());
        qaSettings.setValue("show_notifications", m_quickActionsNotifications->isChecked());
        qaSettings.setValue("enable_logging", m_quickActionsLogging->isChecked());
        qaSettings.setValue("compress_backups", m_quickActionsCompress->isChecked());
    }

    // Sync to disk
    config.sync();

    m_settingsModified = false;
    m_applyButton->setEnabled(false);
}

void SettingsDialog::applySettings() {
    if (!validateSettings()) {
        return;
    }
    saveSettings();
}

bool SettingsDialog::validateSettings() {
    // Validate thread count
    if (m_backupThreadCount->value() < 1) {
        QMessageBox::warning(this, tr("Invalid Setting"), 
                           tr("Thread count must be at least 1."));
        m_tabWidget->setCurrentIndex(0); // Switch to Backup tab
        m_backupThreadCount->setFocus();
        return false;
    }

    return true;
}

void SettingsDialog::onApplyClicked() {
    applySettings();
}

void SettingsDialog::onOkClicked() {
    if (m_settingsModified) {
        applySettings();
    }
    accept();
}

void SettingsDialog::onCancelClicked() {
    if (m_settingsModified) {
        auto reply = QMessageBox::question(
            this,
            tr("Unsaved Changes"),
            tr("You have unsaved changes. Are you sure you want to discard them?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        
        if (reply != QMessageBox::Yes) {
            return;
        }
    }
    reject();
}

void SettingsDialog::onResetToDefaultsClicked() {
    auto reply = QMessageBox::question(
        this,
        tr("Reset to Defaults"),
        tr("Are you sure you want to reset all settings to their default values?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        ConfigManager::instance().resetToDefaults();
        loadSettings();
        QMessageBox::information(this, tr("Reset Complete"), 
                               tr("All settings have been reset to defaults."));
    }
}

void SettingsDialog::onSettingChanged() {
    m_settingsModified = true;
    m_applyButton->setEnabled(true);
}

} // namespace sak
