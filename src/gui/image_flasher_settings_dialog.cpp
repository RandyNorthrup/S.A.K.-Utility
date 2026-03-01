// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/image_flasher_settings_dialog.h"
#include "sak/config_manager.h"
#include "sak/info_button.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>

ImageFlasherSettingsDialog::ImageFlasherSettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Image Flasher Settings");
    setModal(true);
    resize(500, 450);
    
    setupUi();
    loadSettings();
}

ImageFlasherSettingsDialog::~ImageFlasherSettingsDialog() = default;

void ImageFlasherSettingsDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    setupUi_generalSection(mainLayout);
    setupUi_advancedSection(mainLayout);
    setupUi_buttonBar(mainLayout);
}

void ImageFlasherSettingsDialog::setupUi_generalSection(QVBoxLayout* mainLayout) {
    // Verification group
    auto* verificationGroup = new QGroupBox("Verification", this);
    auto* verificationLayout = new QGridLayout(verificationGroup);
    
    auto* valLabelWidget = sak::InfoButton::createInfoLabel(
        "Validation Mode:",
        "Choose how to verify data after writing: Full reads every byte back, Quick samples random blocks, None skips verification",
        verificationGroup);
    verificationLayout->addWidget(valLabelWidget, 0, 0);
    m_validationModeCombo = new QComboBox(verificationGroup);
    m_validationModeCombo->addItem("Full Verification (Slowest, Most Reliable)", "full");
    m_validationModeCombo->addItem("Quick Check (Faster, Less Thorough)", "quick");
    m_validationModeCombo->addItem("No Verification (Fastest, No Checking)", "none");
    verificationLayout->addWidget(m_validationModeCombo, 0, 1);
    
    auto* validationNote = new QLabel(
        "Full verification reads every byte back from the drive. "
        "Quick check samples random blocks for faster validation. "
        "No verification writes only without checking.",
        verificationGroup
    );
    validationNote->setWordWrap(true);
    validationNote->setStyleSheet(QString("color: %1; font-size: %2pt;").arg(sak::ui::kColorTextMuted).arg(sak::ui::kFontSizeNote));
    verificationLayout->addWidget(validationNote, 1, 0, 1, 2);
    
    mainLayout->addWidget(verificationGroup);
    
    // Performance group
    auto* performanceGroup = new QGroupBox("Performance", this);
    auto* performanceLayout = new QGridLayout(performanceGroup);
    
    auto* bufLabelWidget = sak::InfoButton::createInfoLabel(
        "Buffer Size (MB):",
        "Larger buffers improve throughput but use more RAM \u2014 64 MB is a good default for USB 3.0 drives",
        performanceGroup);
    performanceLayout->addWidget(bufLabelWidget, 0, 0);
    m_bufferSizeSpin = new QSpinBox(performanceGroup);
    m_bufferSizeSpin->setRange(1, 512);
    m_bufferSizeSpin->setSingleStep(16);
    m_bufferSizeSpin->setSuffix(" MB");
    performanceLayout->addWidget(m_bufferSizeSpin, 0, 1);
    
    auto* concLabelWidget = sak::InfoButton::createInfoLabel(
        "Max Concurrent Writes:",
        "Number of USB drives that can be flashed simultaneously \u2014 each uses one thread",
        performanceGroup);
    performanceLayout->addWidget(concLabelWidget, 1, 0);
    m_maxConcurrentWritesSpin = new QSpinBox(performanceGroup);
    m_maxConcurrentWritesSpin->setRange(1, 16);
    performanceLayout->addWidget(m_maxConcurrentWritesSpin, 1, 1);
    
    auto* performanceNote = new QLabel(
        "Larger buffer sizes may improve performance but use more memory. "
        "Concurrent writes allow flashing to multiple drives simultaneously.",
        performanceGroup
    );
    performanceNote->setWordWrap(true);
    performanceNote->setStyleSheet(QString("color: %1; font-size: %2pt;").arg(sak::ui::kColorTextMuted).arg(sak::ui::kFontSizeNote));
    performanceLayout->addWidget(performanceNote, 2, 0, 1, 2);
    
    mainLayout->addWidget(performanceGroup);
}

void ImageFlasherSettingsDialog::setupUi_advancedSection(QVBoxLayout* mainLayout) {
    // Safety group
    auto* safetyGroup = new QGroupBox("Safety", this);
    auto* safetyLayout = new QVBoxLayout(safetyGroup);
    
    m_showSystemDriveWarningCheck = new QCheckBox("Show system drive warning", safetyGroup);
    auto* sysRow = new QHBoxLayout();
    sysRow->addWidget(m_showSystemDriveWarningCheck);
    sysRow->addWidget(new sak::InfoButton(
        "Prevents accidentally overwriting your Windows installation drive (C:)", safetyGroup));
    sysRow->addStretch();
    safetyLayout->addLayout(sysRow);
    
    m_showLargeDriveWarningCheck = new QCheckBox("Show large drive warning", safetyGroup);
    auto* lgRow = new QHBoxLayout();
    lgRow->addWidget(m_showLargeDriveWarningCheck);
    lgRow->addWidget(new sak::InfoButton(
        "Warns when a drive exceeds the threshold below \u2014 large drives are rarely USB sticks", safetyGroup));
    lgRow->addStretch();
    safetyLayout->addLayout(lgRow);
    
    auto* thresholdLayout = new QHBoxLayout();
    thresholdLayout->addSpacing(20);
    thresholdLayout->addWidget(sak::InfoButton::createInfoLabel(
        "Large drive threshold:",
        "Drives exceeding this size trigger a warning \u2014 helps avoid accidentally flashing internal HDDs",
        safetyGroup));
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
    auto* unmRow = new QHBoxLayout();
    unmRow->addWidget(m_unmountOnCompletionCheck);
    unmRow->addWidget(new sak::InfoButton(
        "Safely ejects the drive so you can remove it immediately after flashing", behaviorGroup));
    unmRow->addStretch();
    behaviorLayout->addLayout(unmRow);
    
    m_enableNotificationsCheck = new QCheckBox("Enable desktop notifications", behaviorGroup);
    auto* notRow = new QHBoxLayout();
    notRow->addWidget(m_enableNotificationsCheck);
    notRow->addWidget(new sak::InfoButton(
        "Windows toast notification when a long-running flash finishes", behaviorGroup));
    notRow->addStretch();
    behaviorLayout->addLayout(notRow);
    
    mainLayout->addWidget(behaviorGroup);
}

void ImageFlasherSettingsDialog::setupUi_buttonBar(QVBoxLayout* mainLayout) {
    // Storage group
    auto* storageGroup = new QGroupBox("Storage", this);
    auto* storageLayout = new QVBoxLayout(storageGroup);

    m_cacheInfoLabel = new QLabel(storageGroup);
    m_cacheInfoLabel->setStyleSheet(QString("color: %1; font-size: %2pt;").arg(sak::ui::kColorTextMuted).arg(sak::ui::kFontSizeNote));
    storageLayout->addWidget(m_cacheInfoLabel);

    m_clearCacheButton = new QPushButton("Clear Download Caches", storageGroup);
    auto* cacheInfoRow = new QHBoxLayout();
    cacheInfoRow->addWidget(m_clearCacheButton);
    cacheInfoRow->addWidget(new sak::InfoButton(
        "Removes all cached Windows UUP download files from the temp directory.\n"
        "Use this to free disk space or force a fresh download.", storageGroup));
    cacheInfoRow->addStretch();
    connect(m_clearCacheButton, &QPushButton::clicked,
            this, &ImageFlasherSettingsDialog::onClearDownloadCaches);
    storageLayout->addLayout(cacheInfoRow);

    mainLayout->addWidget(storageGroup);

    updateCacheInfo();

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
    auto& config = sak::ConfigManager::instance();

    // Validation mode
    QString validationMode = config.getImageFlasherValidationMode();
    int validationIndex = m_validationModeCombo->findData(validationMode);
    if (validationIndex >= 0) {
        m_validationModeCombo->setCurrentIndex(validationIndex);
    }
    
    // Buffer size (ConfigManager stores in MB)
    m_bufferSizeSpin->setValue(config.getImageFlasherBufferSize());
    
    // Max concurrent writes
    m_maxConcurrentWritesSpin->setValue(config.getImageFlasherMaxConcurrentWrites());
    
    // Safety options
    m_showSystemDriveWarningCheck->setChecked(config.getImageFlasherShowSystemDriveWarning());
    m_showLargeDriveWarningCheck->setChecked(config.getImageFlasherShowLargeDriveWarning());
    m_largeDriveThresholdSpin->setValue(config.getImageFlasherLargeDriveThreshold());
    
    // Behavior options
    m_unmountOnCompletionCheck->setChecked(config.getImageFlasherUnmountOnCompletion());
    m_enableNotificationsCheck->setChecked(config.getImageFlasherEnableNotifications());
}

void ImageFlasherSettingsDialog::saveSettings() {
    auto& config = sak::ConfigManager::instance();

    // Validation mode
    config.setImageFlasherValidationMode(m_validationModeCombo->currentData().toString());
    
    // Buffer size
    config.setImageFlasherBufferSize(m_bufferSizeSpin->value());
    
    // Max concurrent writes
    config.setImageFlasherMaxConcurrentWrites(m_maxConcurrentWritesSpin->value());
    
    // Safety options
    config.setImageFlasherShowSystemDriveWarning(m_showSystemDriveWarningCheck->isChecked());
    config.setImageFlasherShowLargeDriveWarning(m_showLargeDriveWarningCheck->isChecked());
    config.setImageFlasherLargeDriveThreshold(m_largeDriveThresholdSpin->value());
    
    // Behavior options
    config.setImageFlasherUnmountOnCompletion(m_unmountOnCompletionCheck->isChecked());
    config.setImageFlasherEnableNotifications(m_enableNotificationsCheck->isChecked());

    config.sync();
}

void ImageFlasherSettingsDialog::onAccept() {
    saveSettings();
    accept();
}

void ImageFlasherSettingsDialog::onResetDefaults() {
    m_validationModeCombo->setCurrentIndex(0); // Full
    m_bufferSizeSpin->setValue(4096);
    m_maxConcurrentWritesSpin->setValue(1);
    m_showSystemDriveWarningCheck->setChecked(true);
    m_showLargeDriveWarningCheck->setChecked(true);
    m_largeDriveThresholdSpin->setValue(128);
    m_unmountOnCompletionCheck->setChecked(true);
    m_enableNotificationsCheck->setChecked(true);
}

// ============================================================================
// Cache Management
// ============================================================================

QStringList ImageFlasherSettingsDialog::findCacheDirectories()
{
    QStringList dirs;
    QString tempBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir tempDir(tempBase);

    for (const auto& entry : tempDir.entryList(QStringList{"sak_uup_*"}, QDir::Dirs)) {
        dirs.append(tempDir.filePath(entry));
    }
    return dirs;
}

qint64 ImageFlasherSettingsDialog::calculateCacheSize()
{
    qint64 totalBytes = 0;
    for (const auto& dirPath : findCacheDirectories()) {
        QDirIterator it(dirPath, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            totalBytes += it.fileInfo().size();
        }
    }
    return totalBytes;
}

void ImageFlasherSettingsDialog::updateCacheInfo()
{
    QStringList dirs = findCacheDirectories();
    qint64 totalBytes = 0;
    for (const auto& dirPath : dirs) {
        QDirIterator it(dirPath, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            totalBytes += it.fileInfo().size();
        }
    }

    if (dirs.isEmpty()) {
        m_cacheInfoLabel->setText("No cached downloads found.");
        m_clearCacheButton->setEnabled(false);
    } else {
        QString sizeStr;
        if (totalBytes < 1024 * 1024) {
            sizeStr = QString("%1 KB").arg(totalBytes / 1024);
        } else if (totalBytes < 1024LL * 1024 * 1024) {
            sizeStr = QString("%1 MB").arg(totalBytes / (1024.0 * 1024.0), 0, 'f', 1);
        } else {
            sizeStr = QString("%1 GB").arg(totalBytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
        }
        m_cacheInfoLabel->setText(QString("%1 cached download folder(s) using %2.")
            .arg(dirs.size()).arg(sizeStr));
        m_clearCacheButton->setEnabled(true);
    }
}

void ImageFlasherSettingsDialog::onClearDownloadCaches()
{
    QStringList dirs = findCacheDirectories();
    if (dirs.isEmpty()) {
        QMessageBox::information(this, "Clear Download Caches",
                                 "No cached downloads to clear.");
        return;
    }

    qint64 totalBytes = calculateCacheSize();
    QString sizeStr;
    if (totalBytes < 1024LL * 1024 * 1024) {
        sizeStr = QString("%1 MB").arg(totalBytes / (1024.0 * 1024.0), 0, 'f', 1);
    } else {
        sizeStr = QString("%1 GB").arg(totalBytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }

    auto reply = QMessageBox::question(this, "Clear Download Caches",
        QString("This will delete %1 cached download folder(s) (%2) "
                "from the temp directory.\n\n"
                "Any in-progress downloads should be cancelled first.\n\n"
                "Continue?")
            .arg(dirs.size()).arg(sizeStr),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    int removedCount = 0;
    int failedCount = 0;
    for (const auto& dirPath : dirs) {
        QDir dir(dirPath);
        if (dir.removeRecursively()) {
            removedCount++;
            sak::logInfo("Cleared download cache: " + dirPath.toStdString());
        } else {
            failedCount++;
            sak::logWarning("Failed to remove cache directory: " + dirPath.toStdString());
        }
    }

    updateCacheInfo();

    if (failedCount == 0) {
        QMessageBox::information(this, "Clear Download Caches",
            QString("Successfully cleared %1 cached download folder(s) (%2 freed).")
                .arg(removedCount).arg(sizeStr));
    } else {
        QMessageBox::warning(this, "Clear Download Caches",
            QString("Cleared %1 folder(s), but %2 could not be removed.\n"
                    "They may be in use by an active download.")
                .arg(removedCount).arg(failedCount));
    }
}
