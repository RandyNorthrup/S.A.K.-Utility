// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/image_flasher_settings_dialog.h"
#include "sak/config_manager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>

ImageFlasherSettingsDialog::ImageFlasherSettingsDialog(sak::ConfigManager* config, QWidget* parent)
    : QDialog(parent)
    , m_config(config)
{
    setWindowTitle("Image Flasher Settings");
    setModal(true);
    resize(500, 450);
    
    setupUI();
    loadSettings();
}

ImageFlasherSettingsDialog::~ImageFlasherSettingsDialog() = default;

void ImageFlasherSettingsDialog::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    
    // Verification group
    auto* verificationGroup = new QGroupBox("Verification", this);
    auto* verificationLayout = new QGridLayout(verificationGroup);
    
    verificationLayout->addWidget(new QLabel("Validation Mode:", verificationGroup), 0, 0);
    m_validationModeCombo = new QComboBox(verificationGroup);
    m_validationModeCombo->addItem("Full Verification (Slowest, Most Reliable)", "full");
    m_validationModeCombo->addItem("Sample Verification (Faster, Less Thorough)", "sample");
    m_validationModeCombo->addItem("Skip Verification (Fastest, No Checking)", "skip");
    verificationLayout->addWidget(m_validationModeCombo, 0, 1);
    
    auto* validationNote = new QLabel(
        "Full verification reads every byte back from the drive. "
        "Sample verification checks random blocks. "
        "Skip verification writes only without checking.",
        verificationGroup
    );
    validationNote->setWordWrap(true);
    validationNote->setStyleSheet("color: #64748b; font-size: 9pt;");
    verificationLayout->addWidget(validationNote, 1, 0, 1, 2);
    
    mainLayout->addWidget(verificationGroup);
    
    // Performance group
    auto* performanceGroup = new QGroupBox("Performance", this);
    auto* performanceLayout = new QGridLayout(performanceGroup);
    
    performanceLayout->addWidget(new QLabel("Buffer Size (MB):", performanceGroup), 0, 0);
    m_bufferSizeSpin = new QSpinBox(performanceGroup);
    m_bufferSizeSpin->setRange(1, 512);
    m_bufferSizeSpin->setSingleStep(16);
    m_bufferSizeSpin->setSuffix(" MB");
    performanceLayout->addWidget(m_bufferSizeSpin, 0, 1);
    
    performanceLayout->addWidget(new QLabel("Max Concurrent Writes:", performanceGroup), 1, 0);
    m_maxConcurrentWritesSpin = new QSpinBox(performanceGroup);
    m_maxConcurrentWritesSpin->setRange(1, 16);
    performanceLayout->addWidget(m_maxConcurrentWritesSpin, 1, 1);
    
    auto* performanceNote = new QLabel(
        "Larger buffer sizes may improve performance but use more memory. "
        "Concurrent writes allow flashing to multiple drives simultaneously.",
        performanceGroup
    );
    performanceNote->setWordWrap(true);
    performanceNote->setStyleSheet("color: #64748b; font-size: 9pt;");
    performanceLayout->addWidget(performanceNote, 2, 0, 1, 2);
    
    mainLayout->addWidget(performanceGroup);
    
    // Safety group
    auto* safetyGroup = new QGroupBox("Safety", this);
    auto* safetyLayout = new QVBoxLayout(safetyGroup);
    
    m_showSystemDriveWarningCheck = new QCheckBox("Show system drive warning", safetyGroup);
    m_showSystemDriveWarningCheck->setToolTip("Display warning when system drive is in the list");
    safetyLayout->addWidget(m_showSystemDriveWarningCheck);
    
    m_showLargeDriveWarningCheck = new QCheckBox("Show large drive warning", safetyGroup);
    m_showLargeDriveWarningCheck->setToolTip("Display warning for drives larger than threshold");
    safetyLayout->addWidget(m_showLargeDriveWarningCheck);
    
    auto* thresholdLayout = new QHBoxLayout();
    thresholdLayout->addSpacing(20);
    thresholdLayout->addWidget(new QLabel("Large drive threshold:", safetyGroup));
    m_largeDriveThresholdSpin = new QSpinBox(safetyGroup);
    m_largeDriveThresholdSpin->setRange(8, 2048);
    m_largeDriveThresholdSpin->setSingleStep(8);
    m_largeDriveThresholdSpin->setSuffix(" GB");
    thresholdLayout->addWidget(m_largeDriveThresholdSpin);
    thresholdLayout->addStretch();
    safetyLayout->addLayout(thresholdLayout);
    
    mainLayout->addWidget(safetyGroup);
    
    // Behavior group
    auto* behaviorGroup = new QGroupBox("Behavior", this);
    auto* behaviorLayout = new QVBoxLayout(behaviorGroup);
    
    m_unmountOnCompletionCheck = new QCheckBox("Unmount drives on completion", behaviorGroup);
    m_unmountOnCompletionCheck->setToolTip("Automatically unmount drives after successful flash");
    behaviorLayout->addWidget(m_unmountOnCompletionCheck);
    
    m_enableNotificationsCheck = new QCheckBox("Enable desktop notifications", behaviorGroup);
    m_enableNotificationsCheck->setToolTip("Show notification when flash operation completes");
    behaviorLayout->addWidget(m_enableNotificationsCheck);
    
    mainLayout->addWidget(behaviorGroup);
    
    mainLayout->addStretch();
    
    // Buttons
    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        this
    );
    
    connect(buttonBox, &QDialogButtonBox::accepted, this, &ImageFlasherSettingsDialog::onAccept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, &ImageFlasherSettingsDialog::onResetDefaults);
    
    mainLayout->addWidget(buttonBox);
}

void ImageFlasherSettingsDialog::loadSettings() {
    // Validation mode
    QString validationMode = m_config->getValue("ImageFlasher/validation_mode", "full").toString();
    int validationIndex = m_validationModeCombo->findData(validationMode);
    if (validationIndex >= 0) {
        m_validationModeCombo->setCurrentIndex(validationIndex);
    }
    
    // Buffer size
    int bufferSize = m_config->getValue("ImageFlasher/buffer_size_mb", 64).toInt();
    m_bufferSizeSpin->setValue(bufferSize);
    
    // Max concurrent writes
    int maxConcurrent = m_config->getValue("ImageFlasher/max_concurrent_writes", 4).toInt();
    m_maxConcurrentWritesSpin->setValue(maxConcurrent);
    
    // Safety options
    bool showSystemWarning = m_config->getValue("ImageFlasher/show_system_drive_warning", true).toBool();
    m_showSystemDriveWarningCheck->setChecked(showSystemWarning);
    
    bool showLargeWarning = m_config->getValue("ImageFlasher/show_large_drive_warning", true).toBool();
    m_showLargeDriveWarningCheck->setChecked(showLargeWarning);
    
    int largeThreshold = m_config->getValue("ImageFlasher/large_drive_threshold_gb", 32).toInt();
    m_largeDriveThresholdSpin->setValue(largeThreshold);
    
    // Behavior options
    bool unmountOnCompletion = m_config->getValue("ImageFlasher/unmount_on_completion", true).toBool();
    m_unmountOnCompletionCheck->setChecked(unmountOnCompletion);
    
    bool enableNotifications = m_config->getValue("ImageFlasher/enable_notifications", true).toBool();
    m_enableNotificationsCheck->setChecked(enableNotifications);
}

void ImageFlasherSettingsDialog::saveSettings() {
    // Validation mode
    QString validationMode = m_validationModeCombo->currentData().toString();
    m_config->setValue("ImageFlasher/validation_mode", validationMode);
    
    // Buffer size
    m_config->setValue("ImageFlasher/buffer_size_mb", m_bufferSizeSpin->value());
    
    // Max concurrent writes
    m_config->setValue("ImageFlasher/max_concurrent_writes", m_maxConcurrentWritesSpin->value());
    
    // Safety options
    m_config->setValue("ImageFlasher/show_system_drive_warning", m_showSystemDriveWarningCheck->isChecked());
    m_config->setValue("ImageFlasher/show_large_drive_warning", m_showLargeDriveWarningCheck->isChecked());
    m_config->setValue("ImageFlasher/large_drive_threshold_gb", m_largeDriveThresholdSpin->value());
    
    // Behavior options
    m_config->setValue("ImageFlasher/unmount_on_completion", m_unmountOnCompletionCheck->isChecked());
    m_config->setValue("ImageFlasher/enable_notifications", m_enableNotificationsCheck->isChecked());
}

void ImageFlasherSettingsDialog::onAccept() {
    saveSettings();
    accept();
}

void ImageFlasherSettingsDialog::onResetDefaults() {
    m_validationModeCombo->setCurrentIndex(0); // Full
    m_bufferSizeSpin->setValue(64);
    m_maxConcurrentWritesSpin->setValue(4);
    m_showSystemDriveWarningCheck->setChecked(true);
    m_showLargeDriveWarningCheck->setChecked(true);
    m_largeDriveThresholdSpin->setValue(32);
    m_unmountOnCompletionCheck->setChecked(true);
    m_enableNotificationsCheck->setChecked(true);
}
