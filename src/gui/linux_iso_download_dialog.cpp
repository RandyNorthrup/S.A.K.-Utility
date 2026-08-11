// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file linux_iso_download_dialog.cpp
 * @brief Dialog for browsing and downloading Linux distribution ISOs
 *
 * Presents a categorized list of curated Linux distributions,
 * shows details for selected distros, and manages the download
 * lifecycle with progress display and checksum verification.
 */

#include "sak/linux_iso_download_dialog.h"

#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/linux_iso_downloader.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kDistroListMinWidth = 280;
constexpr int kDistroListMinHeight = 200;
constexpr int kDistroListStretch = 3;
constexpr int kDistroDetailsStretch = 2;
constexpr double kDownloadSpeedVisibleThresholdMBps = 0.01;
constexpr int kDownloadSpeedDisplayPrecision = 1;

}  // namespace

// ============================================================================
// Construction
// ============================================================================

LinuxISODownloadDialog::LinuxISODownloadDialog(LinuxISODownloader* downloader, QWidget* parent)
    : QDialog(parent), m_downloader(downloader) {
    setWindowTitle("Download Linux ISO");
    setModal(true);
    resize(sak::kIsoDialogWidthLin, sak::kIsoDialogHeightLin);

    setupUi();
    connectSignals();
    populateDistroList();

    m_statusLabel->setText("Select a distribution to download.");
}

LinuxISODownloadDialog::~LinuxISODownloadDialog() = default;

// ============================================================================
// UI Setup
// ============================================================================

void LinuxISODownloadDialog::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* main_layout = new QVBoxLayout(this);
    setupUi_selectionGroup(main_layout);
    setupUi_progressAndButtons(main_layout);
}

void LinuxISODownloadDialog::setupUi_selectionGroup(QVBoxLayout* main_layout) {
    // ---- Distribution Selection ----
    auto* selection_group = new QGroupBox("Select Distribution", this);
    auto* selection_layout = new QVBoxLayout(selection_group);

    // Category filter
    auto* filter_row = new QHBoxLayout();
    filter_row->addWidget(new QLabel("Category:", selection_group));
    m_categoryCombo = new QComboBox(selection_group);
    m_categoryCombo->addItem("All Distributions", -1);

    auto category_map = LinuxDistroCatalog::categoryNames();
    for (auto it = category_map.constBegin(); it != category_map.constEnd(); ++it) {
        m_categoryCombo->addItem(it.value(), static_cast<int>(it.key()));
    }
    filter_row->addWidget(m_categoryCombo, 1);
    selection_layout->addLayout(filter_row);

    // Distro list + details side by side
    auto* content_layout = new QHBoxLayout();

    // Left: List
    m_distroListWidget = new QListWidget(selection_group);
    m_distroListWidget->setMinimumWidth(kDistroListMinWidth);
    m_distroListWidget->setMinimumHeight(kDistroListMinHeight);
    content_layout->addWidget(m_distroListWidget, kDistroListStretch);

    // Right: Details panel
    auto* details_widget = new QWidget(selection_group);
    auto* details_layout = new QVBoxLayout(details_widget);
    details_layout->setContentsMargins(
        sak::ui::kMarginSmall, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    m_distroDescriptionLabel = new QLabel("", details_widget);
    m_distroDescriptionLabel->setWordWrap(true);
    m_distroDescriptionLabel->setStyleSheet(sak::ui::fontSizeStyle(sak::ui::kFontSizeBody));
    details_layout->addWidget(m_distroDescriptionLabel);

    details_layout->addSpacing(sak::ui::kSpacingMedium);

    m_distroVersionLabel = new QLabel("", details_widget);
    m_distroVersionLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    details_layout->addWidget(m_distroVersionLabel);

    m_distroSizeLabel = new QLabel("", details_widget);
    m_distroSizeLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    details_layout->addWidget(m_distroSizeLabel);

    m_distroHomepageLabel = new QLabel("", details_widget);
    m_distroHomepageLabel->setOpenExternalLinks(true);
    m_distroHomepageLabel->setTextFormat(Qt::RichText);
    details_layout->addWidget(m_distroHomepageLabel);

    details_layout->addStretch();
    content_layout->addWidget(details_widget, kDistroDetailsStretch);

    selection_layout->addLayout(content_layout);
    main_layout->addWidget(selection_group);
}

void LinuxISODownloadDialog::setupUi_progressAndButtons(QVBoxLayout* main_layout) {
    // ---- Save Location ----
    auto* save_group = new QGroupBox("Save Location", this);
    auto* save_layout = new QHBoxLayout(save_group);

    m_saveLocationEdit = new QLineEdit(save_group);
    m_saveLocationEdit->setPlaceholderText("Select a distribution first...");
    save_layout->addWidget(m_saveLocationEdit);

    m_browseSaveButton = new QPushButton("Browse...", save_group);
    m_browseSaveButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    save_layout->addWidget(m_browseSaveButton);

    main_layout->addWidget(save_group);

    // ---- Progress ----
    auto* progress_group = new QGroupBox("Progress", this);
    auto* progress_layout = new QVBoxLayout(progress_group);

    m_statusLabel = new QLabel("Ready", progress_group);
    progress_layout->addWidget(m_statusLabel);

    m_phaseLabel = new QLabel("", progress_group);
    m_phaseLabel->setStyleSheet(sak::ui::kFontWeightBoldStyle);
    progress_layout->addWidget(m_phaseLabel);

    m_progressBar = new QProgressBar(progress_group);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(sak::kPercentMax);
    m_progressBar->setValue(0);
    progress_layout->addWidget(m_progressBar);

    auto* detail_row = new QHBoxLayout();
    m_detailLabel = new QLabel("", progress_group);
    detail_row->addWidget(m_detailLabel, 1);
    m_speedLabel = new QLabel("", progress_group);
    detail_row->addWidget(m_speedLabel);
    progress_layout->addLayout(detail_row);

    main_layout->addWidget(progress_group);

    // ---- Action Buttons ----
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    m_startButton = new QPushButton("Download ISO", this);
    m_startButton->setEnabled(false);
    m_startButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_startButton);

    m_cancelButton = new QPushButton("Cancel", this);
    m_cancelButton->setEnabled(false);
    m_cancelButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    button_layout->addWidget(m_cancelButton);

    m_closeButton = new QPushButton("Close", this);
    m_closeButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    button_layout->addWidget(m_closeButton);

    main_layout->addLayout(button_layout);
}

// ============================================================================
// Signal Connections
// ============================================================================

void LinuxISODownloadDialog::connectSignals() {
    // UI actions
    connect(m_categoryCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &LinuxISODownloadDialog::onCategoryChanged);
    connect(m_distroListWidget, &QListWidget::currentRowChanged, this, [this](int) {
        onDistroSelected();
    });
    connect(m_browseSaveButton,
            &QPushButton::clicked,
            this,
            &LinuxISODownloadDialog::onBrowseSaveLocation);
    connect(m_startButton, &QPushButton::clicked, this, &LinuxISODownloadDialog::onStartDownload);
    connect(m_cancelButton, &QPushButton::clicked, this, &LinuxISODownloadDialog::onCancelDownload);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_saveLocationEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        updateStartButton();
    });

    // Downloader signals
    connect(m_downloader,
            &LinuxISODownloader::phaseChanged,
            this,
            &LinuxISODownloadDialog::onPhaseChanged);
    connect(m_downloader,
            &LinuxISODownloader::progressUpdated,
            this,
            &LinuxISODownloadDialog::onProgressUpdated);
    connect(m_downloader,
            &LinuxISODownloader::speedUpdated,
            this,
            &LinuxISODownloadDialog::onSpeedUpdated);
    connect(m_downloader,
            &LinuxISODownloader::downloadComplete,
            this,
            &LinuxISODownloadDialog::onDownloadComplete);
    connect(m_downloader,
            &LinuxISODownloader::downloadError,
            this,
            &LinuxISODownloadDialog::onDownloadError);
    connect(m_downloader,
            &LinuxISODownloader::statusMessage,
            this,
            &LinuxISODownloadDialog::onStatusMessage);
}

// ============================================================================
// Category & Distro Selection
// ============================================================================

void LinuxISODownloadDialog::onCategoryChanged(int /*index*/) {
    populateDistroList();
}

void LinuxISODownloadDialog::populateDistroList() {
    Q_ASSERT(m_downloader);
    Q_ASSERT(m_distroDescriptionLabel);
    Q_ASSERT(m_distroListWidget);
    Q_ASSERT(m_categoryCombo);
    m_distroListWidget->clear();
    m_currentDistros.clear();

    const int category_value = m_categoryCombo->currentData().toInt();

    if (category_value < 0) {
        // "All" selected
        m_currentDistros = m_downloader->catalog()->allDistros();
    } else {
        auto category = static_cast<LinuxDistroCatalog::Category>(category_value);
        m_currentDistros = m_downloader->catalog()->distrosByCategory(category);
    }

    for (const auto& distro : m_currentDistros) {
        const QString label =
            QString("%1  --  %2")
                .arg(distro.name,
                     distro.versionLabel.isEmpty()
                         ? distro.version
                         : QString("%1 (%2)").arg(distro.version, distro.versionLabel));
        m_distroListWidget->addItem(label);
    }

    // Clear details
    m_distroDescriptionLabel->clear();
    m_distroVersionLabel->clear();
    m_distroSizeLabel->clear();
    m_distroHomepageLabel->clear();
    m_selectedDistroId.clear();
    m_saveLocationEdit->clear();
    m_saveLocationEdit->setPlaceholderText("Select a distribution first...");
    updateStartButton();
}

void LinuxISODownloadDialog::onDistroSelected() {
    Q_ASSERT(m_distroListWidget);
    Q_ASSERT(m_downloader);
    const int row = m_distroListWidget->currentRow();
    if (row < 0 || row >= m_currentDistros.size()) {
        m_selectedDistroId.clear();
        updateStartButton();
        return;
    }

    const auto& distro = m_currentDistros[row];
    m_selectedDistroId = distro.id;

    updateDistroDetails();

    // Set default save path using the distro's expected filename
    const QString file_name = m_downloader->catalog()->resolveFileName(distro);
    if (!file_name.isEmpty()) {
        Q_ASSERT(m_saveLocationEdit);
        m_saveLocationEdit->setText(getDefaultSavePath(file_name));
    }

    updateStartButton();
}

void LinuxISODownloadDialog::updateDistroDetails() {
    Q_ASSERT(m_downloader);
    Q_ASSERT(m_distroDescriptionLabel);
    if (m_selectedDistroId.isEmpty()) {
        return;
    }

    auto distro = m_downloader->catalog()->distroById(m_selectedDistroId);
    if (distro.id.isEmpty()) {
        return;
    }

    m_distroDescriptionLabel->setText(distro.description);

    QString version_text = QString("Version: %1").arg(distro.version);
    if (!distro.versionLabel.isEmpty()) {
        version_text += QString(" (%1)").arg(distro.versionLabel);
    }
    if (distro.sourceType == LinuxDistroCatalog::SourceType::GitHubRelease) {
        version_text += "  [latest checked at download time]";
    }
    m_distroVersionLabel->setText(version_text);

    m_distroSizeLabel->setText(
        QString("Approximate size: %1").arg(formatSize(distro.approximateSize)));

    if (!distro.homepage.isEmpty()) {
        m_distroHomepageLabel->setText(QString("<a href=\"%1\">%1</a>").arg(distro.homepage));
    } else {
        m_distroHomepageLabel->clear();
    }
}

// ============================================================================
// Save Location
// ============================================================================

void LinuxISODownloadDialog::onBrowseSaveLocation() {
    Q_ASSERT(m_saveLocationEdit);
    QString current = m_saveLocationEdit->text();
    if (current.isEmpty()) {
        current = getDefaultSavePath("linux.iso");
    }

    // Determine file filter based on selected distro
    const QString filter = "ISO Files (*.iso);;Compressed Images (*.iso.gz *.img);;All Files (*.*)";

    const QString file_path = QFileDialog::getSaveFileName(this, "Save Linux ISO", current, filter);

    if (!file_path.isEmpty()) {
        Q_ASSERT(m_saveLocationEdit);
        m_saveLocationEdit->setText(file_path);
    }
}

// ============================================================================
// Start Download
// ============================================================================

void LinuxISODownloadDialog::onStartDownload() {
    Q_ASSERT(m_saveLocationEdit);
    Q_ASSERT(m_startButton);
    if (m_selectedDistroId.isEmpty()) {
        sak::logWarning("ISO download attempted with no distribution selected");
        sak::showWarningLogged(this,
                               "No Distribution Selected",
                               "Please select a distribution to download.");
        return;
    }

    const QString save_path = m_saveLocationEdit->text().trimmed();
    if (save_path.isEmpty()) {
        sak::logWarning("ISO download attempted with no save path specified");
        sak::showWarningLogged(this, "No Save Path", "Please specify where to save the ISO.");
        return;
    }

    // Ensure parent directory exists
    const QString save_dir = QFileInfo(save_path).absolutePath();
    if (!QDir().mkpath(save_dir)) {
        sak::logWarning("Failed to create download directory: {}", save_dir.toStdString());
    }

    // Check for existing file
    if (QFile::exists(save_path)) {
        auto reply = sak::showQuestionLogged(this,
                                             "File Exists",
                                             QString("The file already exists:\n\n%1\n\n"
                                                     "Do you want to overwrite it?")
                                                 .arg(save_path),
                                             QMessageBox::Yes | QMessageBox::No);

        if (reply != QMessageBox::Yes) {
            return;
        }
    }

    m_isDownloading = true;
    setInputsEnabled(false);
    m_startButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_progressBar->setValue(0);
    m_speedLabel->clear();
    m_detailLabel->clear();

    m_downloader->startDownload(m_selectedDistroId, save_path);
}

// ============================================================================
// Progress Handlers
// ============================================================================

void LinuxISODownloadDialog::onPhaseChanged(LinuxISODownloader::Phase phase,
                                            const QString& description) {
    // A11Y: prefix phase text so status is conveyed without relying on color alone
    switch (phase) {
    case LinuxISODownloader::Phase::ResolvingVersion:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kColorAccentPurple));
        m_phaseLabel->setText(QStringLiteral("\u2699 ") + description);  // [*]
        break;
    case LinuxISODownloader::Phase::Downloading:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kColorAccentEmerald));
        m_phaseLabel->setText(QStringLiteral("\u2B07 ") + description);  // v
        break;
    case LinuxISODownloader::Phase::VerifyingChecksum:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kStatusColorWarning));
        m_phaseLabel->setText(QStringLiteral("\u23F3 ") + description);  // [...]
        break;
    case LinuxISODownloader::Phase::Completed:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kStatusColorSuccess));
        m_phaseLabel->setText(QStringLiteral("\u2714 ") + description);  // [x]
        break;
    case LinuxISODownloader::Phase::Failed:
        m_phaseLabel->setStyleSheet(
            sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold, sak::ui::kStatusColorError));
        m_phaseLabel->setText(QStringLiteral("\u2718 ") + description);  // [X]
        break;
    default:
        m_phaseLabel->setStyleSheet(sak::ui::kFontWeightBoldStyle);
        break;
    }
}

void LinuxISODownloadDialog::onProgressUpdated(int percent, const QString& detail) {
    m_progressBar->setValue(percent);
    m_detailLabel->setText(detail);
}

void LinuxISODownloadDialog::onSpeedUpdated(double speed_m_bps) {
    if (speed_m_bps > kDownloadSpeedVisibleThresholdMBps) {
        m_speedLabel->setText(
            QString("%1 MB/s").arg(speed_m_bps, 0, 'f', kDownloadSpeedDisplayPrecision));
    }
}

void LinuxISODownloadDialog::onDownloadComplete(const QString& iso_path, qint64 file_size) {
    Q_ASSERT(m_progressBar);
    Q_ASSERT(m_speedLabel);
    m_downloadedFilePath = iso_path;
    m_isDownloading = false;

    m_progressBar->setValue(sak::kPercentMax);
    m_speedLabel->clear();
    m_detailLabel->clear();
    m_cancelButton->setEnabled(false);

    const QString size_str = formatSize(file_size);
    m_statusLabel->setText(QString("Download complete! (%1)").arg(size_str));
    m_phaseLabel->setText("Complete!");
    m_phaseLabel->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold, sak::ui::kStatusColorSuccess));

    sak::showInformationLogged(this,
                               "Download Complete",
                               QString("Linux ISO downloaded successfully!\n\n"
                                       "Saved to: %1\nSize: %2\n\n"
                                       "Click OK to use this image.")
                                   .arg(iso_path, size_str));

    Q_EMIT downloadCompleted(iso_path);
    accept();
}

void LinuxISODownloadDialog::onDownloadError(const QString& error) {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_cancelButton);
    m_isDownloading = false;

    m_statusLabel->setText(QString("Error: %1").arg(error));
    m_cancelButton->setEnabled(false);
    setInputsEnabled(true);
    updateStartButton();

    sak::logError("Linux ISO download failed: {}", error.toStdString());
    sak::showCriticalLogged(this,
                            "Download Error",
                            QString("Failed to download Linux ISO:\n\n%1\n\n"
                                    "Please check your internet connection and try again.")
                                .arg(error));
}

void LinuxISODownloadDialog::onStatusMessage(const QString& message) {
    Q_ASSERT(m_statusLabel);
    m_statusLabel->setText(message);
}

// ============================================================================
// Cancel
// ============================================================================

void LinuxISODownloadDialog::onCancelDownload() {
    Q_ASSERT(m_downloader);
    Q_ASSERT(m_statusLabel);
    auto reply = sak::showQuestionLogged(this,
                                         "Cancel Download",
                                         "Are you sure you want to cancel the download? "
                                         "Partial files will be removed.",
                                         QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        m_downloader->cancel();
        m_isDownloading = false;
        m_statusLabel->setText("Download cancelled");
        m_phaseLabel->clear();
        m_speedLabel->clear();
        m_detailLabel->clear();
        m_progressBar->setValue(0);
        m_cancelButton->setEnabled(false);
        setInputsEnabled(true);
        updateStartButton();
    }
}

// ============================================================================
// Helpers
// ============================================================================

void LinuxISODownloadDialog::updateStartButton() {
    const bool ready = !m_isDownloading && !m_selectedDistroId.isEmpty() &&
                       !m_saveLocationEdit->text().trimmed().isEmpty();
    m_startButton->setEnabled(ready);
}

void LinuxISODownloadDialog::setInputsEnabled(bool enabled) {
    m_categoryCombo->setEnabled(enabled);
    m_distroListWidget->setEnabled(enabled);
    m_saveLocationEdit->setEnabled(enabled);
    m_browseSaveButton->setEnabled(enabled);
}

QString LinuxISODownloadDialog::getDefaultSavePath(const QString& file_name) const {
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return QDir(downloads).filePath(file_name);
}

QString LinuxISODownloadDialog::formatSize(qint64 bytes) {
    if (bytes <= 0) {
        return QStringLiteral("Unknown");
    }
    return sak::formatBytes(bytes);
}
