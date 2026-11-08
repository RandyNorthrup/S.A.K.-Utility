// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/image_flasher_settings_dialog.h"

#include "sak/app_paths.h"
#include "sak/config_manager.h"
#include "sak/format_utils.h"
#include "sak/info_button.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

QString cachedFoldersCount(qint64 count) {
    return sak::countOf(count,
                        QStringLiteral("cached download folder"),
                        QStringLiteral("cached download folders"));
}

// A clear that removed some cache folders but not all: logged, and told to the user with the
// pronoun agreeing with how many were left behind.
void reportPartialCacheClear(QWidget* parent, int removed_count, int failed_count) {
    const QString cleared =
        sak::countOf(removed_count, QStringLiteral("folder"), QStringLiteral("folders"));
    sak::logWarning(
        "Cache clear partially failed: "
        "cleared {}, {} could not be removed",
        cleared.toStdString(),
        failed_count);
    sak::showWarningLogged(
        parent,
        "Clear Download Caches",
        QString("Cleared %1, but %2 could not be removed.\n%3")
            .arg(cleared,
                 QString::number(failed_count),
                 failed_count == 1 ? QStringLiteral("It may be in use by an active download.")
                                   : QStringLiteral("They may be in use by an active download.")));
}

constexpr int kGridNoteRowSpan = 1;
constexpr int kGridNoteColumnSpan = 2;
constexpr int kPerformanceNoteRow = 2;
constexpr int kBufferSizeMinimumMb = 1;
constexpr int kBufferSizeMaximumMb = 512;
constexpr int kBufferSizeStepMb = 16;
constexpr int kConcurrentWritesMinimum = 1;
constexpr int kConcurrentWritesMaximum = 16;
constexpr int kDefaultMaxConcurrentWrites = 1;
constexpr int kLargeDriveThresholdMinimumGb = 8;
constexpr int kLargeDriveThresholdMaximumGb = 2048;
constexpr int kLargeDriveThresholdStepGb = 8;
constexpr int kDefaultLargeDriveThresholdGb = 128;

}  // namespace

ImageFlasherSettingsDialog::ImageFlasherSettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Image Flasher Settings");
    setModal(true);
    resize(sak::kFlasherSettingsW, sak::kFlasherSettingsH);

    setupUi();
    loadSettings();
}

ImageFlasherSettingsDialog::~ImageFlasherSettingsDialog() = default;

void ImageFlasherSettingsDialog::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* main_layout = new QVBoxLayout(this);
    setupUi_generalSection(main_layout);
    setupUi_advancedSection(main_layout);
    setupUi_buttonBar(main_layout);
    alignFieldColumn();
}

// One column for every field in the dialog, the widest label's: the validation combo started at
// x=191, the performance spin boxes at x=297 and the threshold at x=210 (review 4, Y18).
void ImageFlasherSettingsDialog::alignFieldColumn() {
    int width = 0;
    for (const QWidget* label : std::as_const(m_fieldLabels)) {
        width = std::max(width, label->sizeHint().width());
    }
    for (QGridLayout* grid : std::as_const(m_fieldGrids)) {
        grid->setColumnMinimumWidth(0, width);
    }
}

void ImageFlasherSettingsDialog::setupUi_generalSection(QVBoxLayout* main_layout) {
    // Verification group
    auto* verification_group = new QGroupBox("Verification", this);
    auto* verification_layout = new QGridLayout(verification_group);

    auto* val_label_widget = sak::InfoButton::createInfoLabel(
        "Validation mode:",
        "Choose how to verify data after writing: Full reads every byte back, Quick samples random "
        "blocks, None skips verification",
        verification_group);
    verification_layout->addWidget(val_label_widget, 0, 0);
    m_validationModeCombo = new QComboBox(verification_group);
    m_validationModeCombo->setAccessibleName(tr("Verification mode"));
    m_validationModeCombo->addItem("Full Verification (Slowest, Most Reliable)", "full");
    m_validationModeCombo->addItem("Quick Check (Faster, Less Thorough)", "quick");
    m_validationModeCombo->addItem("No Verification (Fastest, No Checking)", "none");
    verification_layout->addWidget(m_validationModeCombo, 0, 1);
    verification_layout->setColumnStretch(1, 1);
    m_fieldLabels.append(val_label_widget);
    m_fieldGrids.append(verification_layout);

    auto* validation_note = new QLabel(
        "Full verification reads every byte back from the drive. "
        "Quick check samples random blocks for faster validation. "
        "No verification writes only without checking.",
        verification_group);
    validation_note->setWordWrap(true);
    validation_note->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeNote));
    verification_layout->addWidget(validation_note, 1, 0, kGridNoteRowSpan, kGridNoteColumnSpan);

    main_layout->addWidget(verification_group);
    setupUi_performanceSection(main_layout);
}

void ImageFlasherSettingsDialog::setupUi_performanceSection(QVBoxLayout* main_layout) {
    auto* performance_group = new QGroupBox("Performance", this);
    auto* performance_layout = new QGridLayout(performance_group);

    // The unit is the field's suffix: "Buffer Size (MB): 64 MB" gave it twice (review 4, Y18).
    auto* buf_label_widget = sak::InfoButton::createInfoLabel(
        "Buffer size:",
        "Larger buffers improve throughput but use more RAM - 64 MB is a good default for USB "
        "3.0 drives",
        performance_group);
    performance_layout->addWidget(buf_label_widget, 0, 0);
    m_bufferSizeSpin = new QSpinBox(performance_group);
    m_bufferSizeSpin->setAccessibleName(tr("Buffer size"));
    m_bufferSizeSpin->setRange(kBufferSizeMinimumMb, kBufferSizeMaximumMb);
    m_bufferSizeSpin->setSingleStep(kBufferSizeStepMb);
    m_bufferSizeSpin->setSuffix(" MB");
    performance_layout->addWidget(m_bufferSizeSpin, 0, 1);

    auto* conc_label_widget = sak::InfoButton::createInfoLabel(
        "Max concurrent writes:",
        "Number of USB drives that can be flashed simultaneously - each uses one thread",
        performance_group);
    performance_layout->addWidget(conc_label_widget, 1, 0);
    m_maxConcurrentWritesSpin = new QSpinBox(performance_group);
    m_maxConcurrentWritesSpin->setAccessibleName(tr("Concurrent writes"));
    m_maxConcurrentWritesSpin->setRange(kConcurrentWritesMinimum, kConcurrentWritesMaximum);
    performance_layout->addWidget(m_maxConcurrentWritesSpin, 1, 1);
    performance_layout->setColumnStretch(1, 1);
    m_fieldLabels.append(buf_label_widget);
    m_fieldLabels.append(conc_label_widget);
    m_fieldGrids.append(performance_layout);

    auto* performance_note = new QLabel(
        "Larger buffer sizes may improve performance but use more memory. "
        "Concurrent writes allow flashing to multiple drives simultaneously.",
        performance_group);
    performance_note->setWordWrap(true);
    performance_note->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeNote));
    performance_layout->addWidget(
        performance_note, kPerformanceNoteRow, 0, kGridNoteRowSpan, kGridNoteColumnSpan);

    main_layout->addWidget(performance_group);
}

void ImageFlasherSettingsDialog::setupUi_advancedSection(QVBoxLayout* main_layout) {
    Q_ASSERT(main_layout);
    // Safety group.
    //
    // There is deliberately no "show system drive warning" control here. The panel
    // does not warn about the system drive, it REFUSES it outright -- and so does
    // FlashCoordinator, independently, before any device is opened. A checkbox
    // offering to turn that off would describe protection the user cannot actually
    // disable, which is worse than no checkbox at all.
    auto* safety_group = new QGroupBox("Safety", this);
    auto* safety_layout = new QVBoxLayout(safety_group);

    m_showLargeDriveWarningCheck = new QCheckBox("Show large drive warning", safety_group);
    auto* lg_row = new QHBoxLayout();
    lg_row->addWidget(m_showLargeDriveWarningCheck);
    lg_row->addWidget(new sak::InfoButton(
        "Warns when a drive exceeds the threshold below - large drives are rarely USB sticks",
        safety_group));
    lg_row->addStretch();
    safety_layout->addLayout(lg_row);

    auto* threshold_layout = new QGridLayout();
    auto* threshold_label = sak::InfoButton::createInfoLabel(
        "Large drive threshold:",
        "Drives exceeding this size trigger a warning - helps avoid accidentally flashing "
        "internal HDDs",
        safety_group);
    // In under its check box, which it belongs to.
    threshold_label->setContentsMargins(
        sak::ui::kMarginXLarge, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    threshold_layout->addWidget(threshold_label, 0, 0);
    m_fieldLabels.append(threshold_label);
    m_fieldGrids.append(threshold_layout);
    m_largeDriveThresholdSpin = new QSpinBox(safety_group);
    m_largeDriveThresholdSpin->setAccessibleName(tr("Large drive threshold"));
    m_largeDriveThresholdSpin->setRange(kLargeDriveThresholdMinimumGb,
                                        kLargeDriveThresholdMaximumGb);
    m_largeDriveThresholdSpin->setSingleStep(kLargeDriveThresholdStepGb);
    m_largeDriveThresholdSpin->setSuffix(" GB");
    threshold_layout->addWidget(m_largeDriveThresholdSpin, 0, 1);
    threshold_layout->setColumnStretch(1, 1);
    safety_layout->addLayout(threshold_layout);

    main_layout->addWidget(safety_group);

    // Behavior group
    auto* behavior_group = new QGroupBox("Behavior", this);
    auto* behavior_layout = new QVBoxLayout(behavior_group);

    m_unmountOnCompletionCheck = new QCheckBox("Unmount drives on completion", behavior_group);
    auto* unm_row = new QHBoxLayout();
    unm_row->addWidget(m_unmountOnCompletionCheck);
    unm_row->addWidget(new sak::InfoButton(
        "Safely ejects the drive so you can remove it immediately after flashing", behavior_group));
    unm_row->addStretch();
    behavior_layout->addLayout(unm_row);

    m_enableNotificationsCheck = new QCheckBox("Enable desktop notifications", behavior_group);
    auto* not_row = new QHBoxLayout();
    not_row->addWidget(m_enableNotificationsCheck);
    not_row->addWidget(new sak::InfoButton(
        "Windows toast notification when a long-running flash finishes", behavior_group));
    not_row->addStretch();
    behavior_layout->addLayout(not_row);

    main_layout->addWidget(behavior_group);
}

void ImageFlasherSettingsDialog::setupUi_buttonBar(QVBoxLayout* main_layout) {
    Q_ASSERT(main_layout);
    // Storage group
    auto* storage_group = new QGroupBox("Storage", this);
    auto* storage_layout = new QVBoxLayout(storage_group);

    m_cacheInfoLabel = new QLabel(storage_group);
    m_cacheInfoLabel->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeNote));
    storage_layout->addWidget(m_cacheInfoLabel);

    m_clearCacheButton = new QPushButton("Clear Download Caches", storage_group);
    m_clearCacheButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    auto* cache_info_row = new QHBoxLayout();
    cache_info_row->addWidget(m_clearCacheButton);
    cache_info_row->addWidget(new sak::InfoButton(
        "Removes all cached Windows UUP download files from the temp directory.\n"
        "Use this to free disk space or force a fresh download.",
        storage_group));
    cache_info_row->addStretch();
    connect(m_clearCacheButton,
            &QPushButton::clicked,
            this,
            &ImageFlasherSettingsDialog::onClearDownloadCaches,
            Qt::QueuedConnection);
    storage_layout->addLayout(cache_info_row);

    main_layout->addWidget(storage_group);

    updateCacheInfo();

    main_layout->addStretch();

    // Buttons
    auto* button_box = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);

    connect(button_box, &QDialogButtonBox::accepted, this, &ImageFlasherSettingsDialog::onAccept);
    connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // One primary button, OK: Restore Defaults only resets the form (review 3, X10).
    button_box->button(QDialogButtonBox::RestoreDefaults)
        ->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(button_box->button(QDialogButtonBox::RestoreDefaults),
            &QPushButton::clicked,
            this,
            &ImageFlasherSettingsDialog::onResetDefaults);

    main_layout->addWidget(button_box);
}

void ImageFlasherSettingsDialog::loadSettings() {
    Q_ASSERT(m_validationModeCombo);
    Q_ASSERT(m_bufferSizeSpin);
    const auto& config = sak::ConfigManager::instance();

    // Validation mode
    const QString validation_mode = config.getImageFlasherValidationMode();
    const int validation_index = m_validationModeCombo->findData(validation_mode);
    if (validation_index >= 0) {
        m_validationModeCombo->setCurrentIndex(validation_index);
    }

    // Buffer size (ConfigManager stores in MB)
    m_bufferSizeSpin->setValue(config.getImageFlasherBufferSize());

    // Max concurrent writes
    m_maxConcurrentWritesSpin->setValue(config.getImageFlasherMaxConcurrentWrites());

    // Safety options
    m_showLargeDriveWarningCheck->setChecked(config.getImageFlasherShowLargeDriveWarning());
    m_largeDriveThresholdSpin->setValue(config.getImageFlasherLargeDriveThreshold());

    // Behavior options
    m_unmountOnCompletionCheck->setChecked(config.getImageFlasherUnmountOnCompletion());
    m_enableNotificationsCheck->setChecked(config.getImageFlasherEnableNotifications());
}

void ImageFlasherSettingsDialog::saveSettings() {
    Q_ASSERT(m_validationModeCombo);
    Q_ASSERT(m_bufferSizeSpin);
    auto& config = sak::ConfigManager::instance();

    // Validation mode
    config.setImageFlasherValidationMode(m_validationModeCombo->currentData().toString());

    // Buffer size
    config.setImageFlasherBufferSize(m_bufferSizeSpin->value());

    // Max concurrent writes
    config.setImageFlasherMaxConcurrentWrites(m_maxConcurrentWritesSpin->value());

    // Safety options
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
    m_validationModeCombo->setCurrentIndex(0);  // Full
    m_bufferSizeSpin->setValue(sak::kImageFlasherDefaultBufferMb);
    m_maxConcurrentWritesSpin->setValue(kDefaultMaxConcurrentWrites);
    m_showLargeDriveWarningCheck->setChecked(true);
    m_largeDriveThresholdSpin->setValue(kDefaultLargeDriveThresholdGb);
    m_unmountOnCompletionCheck->setChecked(true);
    m_enableNotificationsCheck->setChecked(true);
}

// ============================================================================
// Cache Management
// ============================================================================

QStringList ImageFlasherSettingsDialog::findCacheDirectories() {
    QStringList dirs;
    const QString temp_base = sak::app_paths::tempDirectory();
    const QDir temp_dir(temp_base);

    for (const auto& entry : temp_dir.entryList(QStringList{"sak_uup_*"}, QDir::Dirs)) {
        dirs.append(temp_dir.filePath(entry));
    }
    return dirs;
}

qint64 ImageFlasherSettingsDialog::calculateCacheSize() {
    qint64 total_bytes = 0;
    for (const auto& dir_path : findCacheDirectories()) {
        QDirIterator it(dir_path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            total_bytes += it.fileInfo().size();
        }
    }
    return total_bytes;
}

void ImageFlasherSettingsDialog::updateCacheInfo() {
    Q_ASSERT(m_cacheInfoLabel);
    Q_ASSERT(m_clearCacheButton);
    const QStringList dirs = findCacheDirectories();
    qint64 total_bytes = 0;
    for (const auto& dir_path : dirs) {
        QDirIterator it(dir_path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            total_bytes += it.fileInfo().size();
        }
    }

    if (dirs.isEmpty()) {
        m_cacheInfoLabel->setText("No cached downloads found.");
        m_clearCacheButton->setEnabled(false);
    } else {
        QString size_str;
        size_str = sak::formatBytes(total_bytes);
        m_cacheInfoLabel->setText(
            QString("%1 using %2.").arg(cachedFoldersCount(dirs.size()), size_str));
        m_clearCacheButton->setEnabled(true);
    }
}

void ImageFlasherSettingsDialog::onClearDownloadCaches() {
    const QStringList dirs = findCacheDirectories();
    if (dirs.isEmpty()) {
        sak::showInformationLogged(this, "Clear Download Caches", "No cached downloads to clear.");
        return;
    }

    const qint64 total_bytes = calculateCacheSize();
    const QString size_str = sak::formatBytes(total_bytes);

    auto reply =
        sak::showQuestionLogged(this,
                                "Clear Download Caches",
                                QString("This will delete %1 (%2) "
                                        "from the temp directory.\n\n"
                                        "Any in-progress downloads should be cancelled first.\n\n"
                                        "Continue?")
                                    .arg(cachedFoldersCount(dirs.size()), size_str),
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    int removed_count = 0;
    int failed_count = 0;
    for (const auto& dir_path : dirs) {
        QDir dir(dir_path);
        if (dir.removeRecursively()) {
            removed_count++;
            sak::logInfo("Cleared download cache: " + dir_path.toStdString());
        } else {
            failed_count++;
            sak::logWarning("Failed to remove cache directory: " + dir_path.toStdString());
        }
    }

    updateCacheInfo();

    if (failed_count == 0) {
        sak::showInformationLogged(this,
                                   "Clear Download Caches",
                                   QString("Successfully cleared %1 (%2 freed).")
                                       .arg(cachedFoldersCount(removed_count), size_str));
    } else {
        reportPartialCacheClear(this, removed_count, failed_count);
    }
}
