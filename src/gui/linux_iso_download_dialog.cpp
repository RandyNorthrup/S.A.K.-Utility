// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file linux_iso_download_dialog.cpp
 * @brief Dialog for browsing and downloading Linux distribution ISOs
 *
 * Presents a categorized list of curated Linux distributions,
 * shows details for selected distros, and manages the download
 * lifecycle with progress display and checksum verification.
 */

#include "sak/linux_iso_download_dialog.h"
#include "sak/linux_iso_downloader.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QFileDialog>
#include <QStandardPaths>
#include <QMessageBox>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>

// ============================================================================
// Construction
// ============================================================================

LinuxISODownloadDialog::LinuxISODownloadDialog(LinuxISODownloader* downloader,
                                               QWidget* parent)
    : QDialog(parent)
    , m_downloader(downloader)
{
    setWindowTitle("Download Linux ISO");
    setModal(true);
    resize(780, 680);

    setupUI();
    connectSignals();
    populateDistroList();

    m_statusLabel->setText("Select a distribution to download.");
}

LinuxISODownloadDialog::~LinuxISODownloadDialog() = default;

// ============================================================================
// UI Setup
// ============================================================================

void LinuxISODownloadDialog::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);

    // ---- Distribution Selection ----
    auto* selectionGroup = new QGroupBox("Select Distribution", this);
    auto* selectionLayout = new QVBoxLayout(selectionGroup);

    // Category filter
    auto* filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel("Category:", selectionGroup));
    m_categoryCombo = new QComboBox(selectionGroup);
    m_categoryCombo->addItem("All Distributions", -1);

    auto categoryMap = LinuxDistroCatalog::categoryNames();
    for (auto it = categoryMap.constBegin(); it != categoryMap.constEnd(); ++it) {
        m_categoryCombo->addItem(it.value(), static_cast<int>(it.key()));
    }
    filterRow->addWidget(m_categoryCombo, 1);
    selectionLayout->addLayout(filterRow);

    // Distro list + details side by side
    auto* contentLayout = new QHBoxLayout();

    // Left: List
    m_distroListWidget = new QListWidget(selectionGroup);
    m_distroListWidget->setMinimumWidth(280);
    m_distroListWidget->setMinimumHeight(200);
    contentLayout->addWidget(m_distroListWidget, 3);

    // Right: Details panel
    auto* detailsWidget = new QWidget(selectionGroup);
    auto* detailsLayout = new QVBoxLayout(detailsWidget);
    detailsLayout->setContentsMargins(8, 0, 0, 0);

    m_distroDescriptionLabel = new QLabel("", detailsWidget);
    m_distroDescriptionLabel->setWordWrap(true);
    m_distroDescriptionLabel->setStyleSheet("font-size: 10pt;");
    detailsLayout->addWidget(m_distroDescriptionLabel);

    detailsLayout->addSpacing(8);

    m_distroVersionLabel = new QLabel("", detailsWidget);
    m_distroVersionLabel->setStyleSheet("color: #64748b;");
    detailsLayout->addWidget(m_distroVersionLabel);

    m_distroSizeLabel = new QLabel("", detailsWidget);
    m_distroSizeLabel->setStyleSheet("color: #64748b;");
    detailsLayout->addWidget(m_distroSizeLabel);

    m_distroHomepageLabel = new QLabel("", detailsWidget);
    m_distroHomepageLabel->setOpenExternalLinks(true);
    m_distroHomepageLabel->setTextFormat(Qt::RichText);
    detailsLayout->addWidget(m_distroHomepageLabel);

    detailsLayout->addStretch();
    contentLayout->addWidget(detailsWidget, 2);

    selectionLayout->addLayout(contentLayout);
    mainLayout->addWidget(selectionGroup);

    // ---- Save Location ----
    auto* saveGroup = new QGroupBox("Save Location", this);
    auto* saveLayout = new QHBoxLayout(saveGroup);

    m_saveLocationEdit = new QLineEdit(saveGroup);
    m_saveLocationEdit->setPlaceholderText("Select a distribution first...");
    saveLayout->addWidget(m_saveLocationEdit);

    m_browseSaveButton = new QPushButton("Browse...", saveGroup);
    saveLayout->addWidget(m_browseSaveButton);

    mainLayout->addWidget(saveGroup);

    // ---- Progress ----
    auto* progressGroup = new QGroupBox("Progress", this);
    auto* progressLayout = new QVBoxLayout(progressGroup);

    m_statusLabel = new QLabel("Ready", progressGroup);
    progressLayout->addWidget(m_statusLabel);

    m_phaseLabel = new QLabel("", progressGroup);
    m_phaseLabel->setStyleSheet("font-weight: bold;");
    progressLayout->addWidget(m_phaseLabel);

    m_progressBar = new QProgressBar(progressGroup);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(100);
    m_progressBar->setValue(0);
    progressLayout->addWidget(m_progressBar);

    auto* detailRow = new QHBoxLayout();
    m_detailLabel = new QLabel("", progressGroup);
    detailRow->addWidget(m_detailLabel, 1);
    m_speedLabel = new QLabel("", progressGroup);
    detailRow->addWidget(m_speedLabel);
    progressLayout->addLayout(detailRow);

    mainLayout->addWidget(progressGroup);

    // ---- Action Buttons ----
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_startButton = new QPushButton("Download ISO", this);
    m_startButton->setEnabled(false);
    buttonLayout->addWidget(m_startButton);

    m_cancelButton = new QPushButton("Cancel", this);
    m_cancelButton->setEnabled(false);
    buttonLayout->addWidget(m_cancelButton);

    m_closeButton = new QPushButton("Close", this);
    buttonLayout->addWidget(m_closeButton);

    mainLayout->addLayout(buttonLayout);
}

// ============================================================================
// Signal Connections
// ============================================================================

void LinuxISODownloadDialog::connectSignals()
{
    // UI actions
    connect(m_categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LinuxISODownloadDialog::onCategoryChanged);
    connect(m_distroListWidget, &QListWidget::currentRowChanged,
            this, [this](int) { onDistroSelected(); });
    connect(m_browseSaveButton, &QPushButton::clicked,
            this, &LinuxISODownloadDialog::onBrowseSaveLocation);
    connect(m_startButton, &QPushButton::clicked,
            this, &LinuxISODownloadDialog::onStartDownload);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &LinuxISODownloadDialog::onCancelDownload);
    connect(m_closeButton, &QPushButton::clicked,
            this, &QDialog::reject);
    connect(m_saveLocationEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { updateStartButton(); });

    // Downloader signals
    connect(m_downloader, &LinuxISODownloader::phaseChanged,
            this, &LinuxISODownloadDialog::onPhaseChanged);
    connect(m_downloader, &LinuxISODownloader::progressUpdated,
            this, &LinuxISODownloadDialog::onProgressUpdated);
    connect(m_downloader, &LinuxISODownloader::speedUpdated,
            this, &LinuxISODownloadDialog::onSpeedUpdated);
    connect(m_downloader, &LinuxISODownloader::downloadComplete,
            this, &LinuxISODownloadDialog::onDownloadComplete);
    connect(m_downloader, &LinuxISODownloader::downloadError,
            this, &LinuxISODownloadDialog::onDownloadError);
    connect(m_downloader, &LinuxISODownloader::statusMessage,
            this, &LinuxISODownloadDialog::onStatusMessage);
}

// ============================================================================
// Category & Distro Selection
// ============================================================================

void LinuxISODownloadDialog::onCategoryChanged(int /*index*/)
{
    populateDistroList();
}

void LinuxISODownloadDialog::populateDistroList()
{
    m_distroListWidget->clear();
    m_currentDistros.clear();

    int categoryValue = m_categoryCombo->currentData().toInt();

    if (categoryValue < 0) {
        // "All" selected
        m_currentDistros = m_downloader->catalog()->allDistros();
    } else {
        auto category = static_cast<LinuxDistroCatalog::Category>(categoryValue);
        m_currentDistros = m_downloader->catalog()->distrosByCategory(category);
    }

    for (const auto& distro : m_currentDistros) {
        QString label = QString("%1  —  %2").arg(distro.name, distro.versionLabel.isEmpty()
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

void LinuxISODownloadDialog::onDistroSelected()
{
    int row = m_distroListWidget->currentRow();
    if (row < 0 || row >= m_currentDistros.size()) {
        m_selectedDistroId.clear();
        updateStartButton();
        return;
    }

    const auto& distro = m_currentDistros[row];
    m_selectedDistroId = distro.id;

    updateDistroDetails();

    // Set default save path using the distro's expected filename
    QString fileName = m_downloader->catalog()->resolveFileName(distro);
    if (!fileName.isEmpty()) {
        m_saveLocationEdit->setText(getDefaultSavePath(fileName));
    }

    updateStartButton();
}

void LinuxISODownloadDialog::updateDistroDetails()
{
    if (m_selectedDistroId.isEmpty()) return;

    auto distro = m_downloader->catalog()->distroById(m_selectedDistroId);
    if (distro.id.isEmpty()) return;

    m_distroDescriptionLabel->setText(distro.description);

    QString versionText = QString("Version: %1").arg(distro.version);
    if (!distro.versionLabel.isEmpty()) {
        versionText += QString(" (%1)").arg(distro.versionLabel);
    }
    if (distro.sourceType == LinuxDistroCatalog::SourceType::GitHubRelease) {
        versionText += "  [latest checked at download time]";
    }
    m_distroVersionLabel->setText(versionText);

    m_distroSizeLabel->setText(QString("Approximate size: %1").arg(formatSize(distro.approximateSize)));

    if (!distro.homepage.isEmpty()) {
        m_distroHomepageLabel->setText(
            QString("<a href=\"%1\">%1</a>").arg(distro.homepage));
    } else {
        m_distroHomepageLabel->clear();
    }
}

// ============================================================================
// Save Location
// ============================================================================

void LinuxISODownloadDialog::onBrowseSaveLocation()
{
    QString current = m_saveLocationEdit->text();
    if (current.isEmpty()) {
        current = getDefaultSavePath("linux.iso");
    }

    // Determine file filter based on selected distro
    QString filter = "ISO Files (*.iso);;Compressed Images (*.iso.gz *.img);;All Files (*.*)";

    QString filePath = QFileDialog::getSaveFileName(
        this, "Save Linux ISO", current, filter);

    if (!filePath.isEmpty()) {
        m_saveLocationEdit->setText(filePath);
    }
}

// ============================================================================
// Start Download
// ============================================================================

void LinuxISODownloadDialog::onStartDownload()
{
    if (m_selectedDistroId.isEmpty()) {
        QMessageBox::warning(this, "No Distribution Selected",
                             "Please select a distribution to download.");
        return;
    }

    QString savePath = m_saveLocationEdit->text().trimmed();
    if (savePath.isEmpty()) {
        QMessageBox::warning(this, "No Save Path",
                             "Please specify where to save the ISO.");
        return;
    }

    // Ensure parent directory exists
    QDir().mkpath(QFileInfo(savePath).absolutePath());

    // Check for existing file
    if (QFile::exists(savePath)) {
        auto reply = QMessageBox::question(this, "File Exists",
            QString("The file already exists:\n\n%1\n\n"
                    "Do you want to overwrite it?").arg(savePath),
            QMessageBox::Yes | QMessageBox::No);

        if (reply != QMessageBox::Yes) return;
    }

    m_isDownloading = true;
    setInputsEnabled(false);
    m_startButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_progressBar->setValue(0);
    m_speedLabel->clear();
    m_detailLabel->clear();

    m_downloader->startDownload(m_selectedDistroId, savePath);
}

// ============================================================================
// Progress Handlers
// ============================================================================

void LinuxISODownloadDialog::onPhaseChanged(LinuxISODownloader::Phase phase,
                                            const QString& description)
{
    m_phaseLabel->setText(description);

    switch (phase) {
    case LinuxISODownloader::Phase::ResolvingVersion:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #7c3aed;");
        break;
    case LinuxISODownloader::Phase::Downloading:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #059669;");
        break;
    case LinuxISODownloader::Phase::VerifyingChecksum:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #d97706;");
        break;
    case LinuxISODownloader::Phase::Completed:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #16a34a;");
        break;
    case LinuxISODownloader::Phase::Failed:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #dc2626;");
        break;
    default:
        m_phaseLabel->setStyleSheet("font-weight: bold;");
        break;
    }
}

void LinuxISODownloadDialog::onProgressUpdated(int percent, const QString& detail)
{
    m_progressBar->setValue(percent);
    m_detailLabel->setText(detail);
}

void LinuxISODownloadDialog::onSpeedUpdated(double speedMBps)
{
    if (speedMBps > 0.01) {
        m_speedLabel->setText(QString("%1 MB/s").arg(speedMBps, 0, 'f', 1));
    }
}

void LinuxISODownloadDialog::onDownloadComplete(const QString& isoPath,
                                                qint64 fileSize)
{
    m_downloadedFilePath = isoPath;
    m_isDownloading = false;

    m_progressBar->setValue(100);
    m_speedLabel->clear();
    m_detailLabel->clear();
    m_cancelButton->setEnabled(false);

    QString sizeStr = formatSize(fileSize);
    m_statusLabel->setText(QString("Download complete! (%1)").arg(sizeStr));
    m_phaseLabel->setText("Complete!");
    m_phaseLabel->setStyleSheet("font-weight: bold; color: #16a34a;");

    QMessageBox::information(this, "Download Complete",
        QString("Linux ISO downloaded successfully!\n\n"
                "Saved to: %1\nSize: %2\n\n"
                "Click OK to use this image.")
            .arg(isoPath, sizeStr));

    Q_EMIT downloadCompleted(isoPath);
    accept();
}

void LinuxISODownloadDialog::onDownloadError(const QString& error)
{
    m_isDownloading = false;

    m_statusLabel->setText(QString("Error: %1").arg(error));
    m_cancelButton->setEnabled(false);
    setInputsEnabled(true);
    updateStartButton();

    QMessageBox::critical(this, "Download Error",
        QString("Failed to download Linux ISO:\n\n%1\n\n"
                "Please check your internet connection and try again.")
            .arg(error));
}

void LinuxISODownloadDialog::onStatusMessage(const QString& message)
{
    m_statusLabel->setText(message);
}

// ============================================================================
// Cancel
// ============================================================================

void LinuxISODownloadDialog::onCancelDownload()
{
    auto reply = QMessageBox::question(this, "Cancel Download",
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

void LinuxISODownloadDialog::updateStartButton()
{
    bool ready = !m_isDownloading
                 && !m_selectedDistroId.isEmpty()
                 && !m_saveLocationEdit->text().trimmed().isEmpty();
    m_startButton->setEnabled(ready);
}

void LinuxISODownloadDialog::setInputsEnabled(bool enabled)
{
    m_categoryCombo->setEnabled(enabled);
    m_distroListWidget->setEnabled(enabled);
    m_saveLocationEdit->setEnabled(enabled);
    m_browseSaveButton->setEnabled(enabled);
}

QString LinuxISODownloadDialog::getDefaultSavePath(const QString& fileName) const
{
    QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return QDir(downloads).filePath(fileName);
}

QString LinuxISODownloadDialog::formatSize(qint64 bytes)
{
    if (bytes <= 0) return "Unknown";
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}
