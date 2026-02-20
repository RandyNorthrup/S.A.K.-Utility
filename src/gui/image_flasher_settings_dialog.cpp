// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/image_flasher_settings_dialog.h"
#include "sak/config_manager.h"
#include "sak/logger.h"
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
    m_showSystemDriveWarningCheck->setToolTip("Prevents accidentally overwriting your Windows installation drive (C:)");
    safetyLayout->addWidget(m_showSystemDriveWarningCheck);
    
    m_showLargeDriveWarningCheck = new QCheckBox("Show large drive warning", safetyGroup);
    m_showLargeDriveWarningCheck->setToolTip("Warns when a drive exceeds the threshold below — large drives are rarely USB sticks");
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
    m_unmountOnCompletionCheck->setToolTip("Safely ejects the drive so you can remove it immediately after flashing");
    behaviorLayout->addWidget(m_unmountOnCompletionCheck);
    
    m_enableNotificationsCheck = new QCheckBox("Enable desktop notifications", behaviorGroup);
    m_enableNotificationsCheck->setToolTip("Windows toast notification when a long-running flash finishes");
    behaviorLayout->addWidget(m_enableNotificationsCheck);
    
    mainLayout->addWidget(behaviorGroup);
    
    // Storage group
    auto* storageGroup = new QGroupBox("Storage", this);
    auto* storageLayout = new QVBoxLayout(storageGroup);

    m_cacheInfoLabel = new QLabel(storageGroup);
    m_cacheInfoLabel->setStyleSheet("color: #64748b; font-size: 9pt;");
    storageLayout->addWidget(m_cacheInfoLabel);

    auto* cacheButtonLayout = new QHBoxLayout();
    m_clearCacheButton = new QPushButton("Clear Download Caches", storageGroup);
    m_clearCacheButton->setToolTip(
        "Removes all cached Windows UUP download files from the temp directory.\n"
        "Use this to free disk space or force a fresh download.");
    connect(m_clearCacheButton, &QPushButton::clicked,
            this, &ImageFlasherSettingsDialog::onClearDownloadCaches);
    cacheButtonLayout->addWidget(m_clearCacheButton);
    cacheButtonLayout->addStretch();
    storageLayout->addLayout(cacheButtonLayout);

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
            sak::log_info("Cleared download cache: " + dirPath.toStdString());
        } else {
            failedCount++;
            sak::log_warning("Failed to remove cache directory: " + dirPath.toStdString());
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
