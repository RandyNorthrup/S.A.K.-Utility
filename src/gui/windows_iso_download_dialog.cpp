// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/windows_iso_download_dialog.h"
#include "sak/windows_iso_downloader.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QMessageBox>
#include <QDir>
#include <QDateTime>

// ============================================================================
// Construction
// ============================================================================

WindowsISODownloadDialog::WindowsISODownloadDialog(WindowsISODownloader* downloader,
                                                   QWidget* parent)
    : QDialog(parent)
    , m_downloader(downloader)
{
    setWindowTitle("Download Windows ISO");
    setModal(true);
    resize(720, 620);

    setupUI();
    connectSignals();

    m_statusLabel->setText("Select architecture and channel, then click Fetch Builds.");
}

WindowsISODownloadDialog::~WindowsISODownloadDialog() = default;

// ============================================================================
// UI Setup
// ============================================================================

void WindowsISODownloadDialog::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);

    // ---- Step 1: Architecture & Channel ----
    auto* configGroup = new QGroupBox("Build Configuration", this);
    auto* configLayout = new QGridLayout(configGroup);

    configLayout->addWidget(new QLabel("Architecture:", configGroup), 0, 0);
    m_archCombo = new QComboBox(configGroup);
    m_archCombo->addItem("64-bit (x64)", "amd64");
    m_archCombo->addItem("ARM64", "arm64");
    configLayout->addWidget(m_archCombo, 0, 1);

    configLayout->addWidget(new QLabel("Channel:", configGroup), 1, 0);
    m_channelCombo = new QComboBox(configGroup);
    for (auto ch : UupDumpApi::allChannels()) {
        m_channelCombo->addItem(UupDumpApi::channelToDisplayName(ch),
                                static_cast<int>(ch));
    }
    configLayout->addWidget(m_channelCombo, 1, 1);

    m_fetchBuildsButton = new QPushButton("Fetch Builds", configGroup);
    configLayout->addWidget(m_fetchBuildsButton, 0, 2, 2, 1);

    mainLayout->addWidget(configGroup);

    // ---- Step 2: Build Selection ----
    auto* buildGroup = new QGroupBox("Available Builds", this);
    auto* buildLayout = new QVBoxLayout(buildGroup);

    m_buildListWidget = new QListWidget(buildGroup);
    m_buildListWidget->setMaximumHeight(160);
    m_buildListWidget->setEnabled(false);
    buildLayout->addWidget(m_buildListWidget);

    m_buildInfoLabel = new QLabel("", buildGroup);
    m_buildInfoLabel->setWordWrap(true);
    m_buildInfoLabel->setStyleSheet("color: #64748b; font-size: 9pt;");
    buildLayout->addWidget(m_buildInfoLabel);

    mainLayout->addWidget(buildGroup);

    // ---- Step 3: Language & Edition ----
    auto* selectionGroup = new QGroupBox("Language && Edition", this);
    auto* selectionLayout = new QGridLayout(selectionGroup);

    selectionLayout->addWidget(new QLabel("Language:", selectionGroup), 0, 0);
    m_languageCombo = new QComboBox(selectionGroup);
    m_languageCombo->setEnabled(false);
    selectionLayout->addWidget(m_languageCombo, 0, 1);

    selectionLayout->addWidget(new QLabel("Edition:", selectionGroup), 1, 0);
    m_editionCombo = new QComboBox(selectionGroup);
    m_editionCombo->setEnabled(false);
    selectionLayout->addWidget(m_editionCombo, 1, 1);

    mainLayout->addWidget(selectionGroup);

    // ---- Step 4: Save Location ----
    auto* saveGroup = new QGroupBox("Save Location", this);
    auto* saveLayout = new QHBoxLayout(saveGroup);

    m_saveLocationEdit = new QLineEdit(getDefaultSavePath(), saveGroup);
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

    m_startButton = new QPushButton("Download && Build ISO", this);
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

void WindowsISODownloadDialog::connectSignals()
{
    // UI actions
    connect(m_fetchBuildsButton, &QPushButton::clicked,
            this, &WindowsISODownloadDialog::onFetchBuildsClicked);
    connect(m_buildListWidget, &QListWidget::currentRowChanged,
            this, [this](int) { onBuildSelected(); });
    connect(m_languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WindowsISODownloadDialog::onLanguageSelected);
    connect(m_editionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateStartButton(); });
    connect(m_browseSaveButton, &QPushButton::clicked,
            this, &WindowsISODownloadDialog::onBrowseSaveLocation);
    connect(m_startButton, &QPushButton::clicked,
            this, &WindowsISODownloadDialog::onStartDownload);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &WindowsISODownloadDialog::onCancelDownload);
    connect(m_closeButton, &QPushButton::clicked,
            this, &QDialog::reject);

    // Downloader signals
    connect(m_downloader, &WindowsISODownloader::buildsFetched,
            this, &WindowsISODownloadDialog::onBuildsFetched);
    connect(m_downloader, &WindowsISODownloader::languagesFetched,
            this, &WindowsISODownloadDialog::onLanguagesFetched);
    connect(m_downloader, &WindowsISODownloader::editionsFetched,
            this, &WindowsISODownloadDialog::onEditionsFetched);
    connect(m_downloader, &WindowsISODownloader::phaseChanged,
            this, &WindowsISODownloadDialog::onPhaseChanged);
    connect(m_downloader, &WindowsISODownloader::progressUpdated,
            this, &WindowsISODownloadDialog::onProgressUpdated);
    connect(m_downloader, &WindowsISODownloader::speedUpdated,
            this, &WindowsISODownloadDialog::onSpeedUpdated);
    connect(m_downloader, &WindowsISODownloader::downloadComplete,
            this, &WindowsISODownloadDialog::onDownloadComplete);
    connect(m_downloader, &WindowsISODownloader::downloadError,
            this, &WindowsISODownloadDialog::onDownloadError);
    connect(m_downloader, &WindowsISODownloader::statusMessage,
            this, &WindowsISODownloadDialog::onStatusMessage);
}

// ============================================================================
// Step 1: Fetch Builds
// ============================================================================

void WindowsISODownloadDialog::onFetchBuildsClicked()
{
    QString arch = m_archCombo->currentData().toString();
    int channelIdx = m_channelCombo->currentData().toInt();
    auto channel = static_cast<UupDumpApi::ReleaseChannel>(channelIdx);

    m_buildListWidget->clear();
    m_buildListWidget->setEnabled(false);
    m_languageCombo->clear();
    m_languageCombo->setEnabled(false);
    m_editionCombo->clear();
    m_editionCombo->setEnabled(false);
    m_builds.clear();
    m_selectedUpdateId.clear();
    updateStartButton();

    m_statusLabel->setText("Fetching available builds...");
    m_fetchBuildsButton->setEnabled(false);

    m_downloader->fetchBuilds(arch, channel);
}

void WindowsISODownloadDialog::onBuildsFetched(
    const QList<UupDumpApi::BuildInfo>& builds)
{
    m_fetchBuildsButton->setEnabled(true);
    m_builds = builds;

    m_buildListWidget->clear();
    if (builds.isEmpty()) {
        m_statusLabel->setText("No builds found for selected options.");
        return;
    }

    for (const auto& build : builds) {
        QString label = build.title;
        if (label.isEmpty()) {
            label = QString("Build %1 (%2)").arg(build.build, build.arch);
        }
        m_buildListWidget->addItem(label);
    }

    m_buildListWidget->setEnabled(true);
    m_statusLabel->setText(
        QString("Found %1 builds. Select one to continue.").arg(builds.size()));
}

// ============================================================================
// Step 2: Build Selected → Fetch Languages
// ============================================================================

void WindowsISODownloadDialog::onBuildSelected()
{
    int row = m_buildListWidget->currentRow();
    if (row < 0 || row >= m_builds.size()) return;

    const auto& build = m_builds[row];
    m_selectedUpdateId = build.uuid;

    // Show build info
    QDateTime created = QDateTime::fromSecsSinceEpoch(build.created);
    m_buildInfoLabel->setText(
        QString("Build: %1 | Arch: %2 | Added: %3")
            .arg(build.build, build.arch,
                 created.toString("yyyy-MM-dd")));

    // Fetch languages for this build
    m_languageCombo->clear();
    m_languageCombo->setEnabled(false);
    m_editionCombo->clear();
    m_editionCombo->setEnabled(false);
    updateStartButton();

    m_downloader->fetchLanguages(m_selectedUpdateId);
}

void WindowsISODownloadDialog::onLanguagesFetched(
    const QStringList& langCodes,
    const QMap<QString, QString>& langNames)
{
    m_langNames = langNames;
    m_languageCombo->clear();

    for (const auto& code : langCodes) {
        QString display = langNames.value(code, code);
        m_languageCombo->addItem(
            QString("%1 (%2)").arg(display, code), code);
    }

    // Default to English (United States)
    for (int i = 0; i < m_languageCombo->count(); ++i) {
        if (m_languageCombo->itemData(i).toString() == "en-us") {
            m_languageCombo->setCurrentIndex(i);
            break;
        }
    }

    m_languageCombo->setEnabled(true);
    m_statusLabel->setText("Select language to see available editions.");
}

// ============================================================================
// Step 3: Language Selected → Fetch Editions
// ============================================================================

void WindowsISODownloadDialog::onLanguageSelected(int index)
{
    if (index < 0) return;

    QString langCode = m_languageCombo->currentData().toString();
    if (langCode.isEmpty() || m_selectedUpdateId.isEmpty()) return;

    m_editionCombo->clear();
    m_editionCombo->setEnabled(false);
    updateStartButton();

    m_downloader->fetchEditions(m_selectedUpdateId, langCode);
}

void WindowsISODownloadDialog::onEditionsFetched(
    const QStringList& editions,
    const QMap<QString, QString>& editionNames)
{
    m_editionNames = editionNames;
    m_editionCombo->clear();

    for (const auto& code : editions) {
        QString display = editionNames.value(code, code);
        m_editionCombo->addItem(display, code);
    }

    // Default to Professional if available
    for (int i = 0; i < m_editionCombo->count(); ++i) {
        if (m_editionCombo->itemData(i).toString() == "PROFESSIONAL") {
            m_editionCombo->setCurrentIndex(i);
            break;
        }
    }

    m_editionCombo->setEnabled(true);
    m_statusLabel->setText("Ready to download. Choose edition and save location.");
    updateStartButton();
}

// ============================================================================
// Step 4: Start Download & Build
// ============================================================================

void WindowsISODownloadDialog::onStartDownload()
{
    if (m_selectedUpdateId.isEmpty()) {
        QMessageBox::warning(this, "No Build Selected",
                             "Please select a build first.");
        return;
    }

    QString langCode = m_languageCombo->currentData().toString();
    QString edition = m_editionCombo->currentData().toString();
    QString savePath = m_saveLocationEdit->text().trimmed();

    if (langCode.isEmpty() || edition.isEmpty()) {
        QMessageBox::warning(this, "Incomplete Selection",
                             "Please select a language and edition.");
        return;
    }
    if (savePath.isEmpty()) {
        QMessageBox::warning(this, "No Save Path",
                             "Please specify where to save the ISO.");
        return;
    }
    if (!savePath.endsWith(".iso", Qt::CaseInsensitive)) {
        savePath += ".iso";
        m_saveLocationEdit->setText(savePath);
    }

    m_isDownloading = true;
    setInputsEnabled(false);
    m_startButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_progressBar->setValue(0);

    m_downloader->startDownload(m_selectedUpdateId, langCode, edition, savePath);
}

// ============================================================================
// Progress Handlers
// ============================================================================

void WindowsISODownloadDialog::onPhaseChanged(UupIsoBuilder::Phase phase,
                                              const QString& description)
{
    m_phaseLabel->setText(description);

    switch (phase) {
    case UupIsoBuilder::Phase::PreparingDownload:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #2563eb;");
        break;
    case UupIsoBuilder::Phase::DownloadingFiles:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #059669;");
        break;
    case UupIsoBuilder::Phase::ConvertingToISO:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #d97706;");
        break;
    case UupIsoBuilder::Phase::Completed:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #16a34a;");
        break;
    case UupIsoBuilder::Phase::Failed:
        m_phaseLabel->setStyleSheet("font-weight: bold; color: #dc2626;");
        break;
    default:
        break;
    }
}

void WindowsISODownloadDialog::onProgressUpdated(int overallPercent,
                                                 const QString& detail)
{
    m_progressBar->setValue(overallPercent);
    m_detailLabel->setText(detail);
}

void WindowsISODownloadDialog::onSpeedUpdated(double downloadSpeedMBps)
{
    if (downloadSpeedMBps > 0.01) {
        m_speedLabel->setText(
            QString("%1 MB/s").arg(downloadSpeedMBps, 0, 'f', 1));
    }
}

void WindowsISODownloadDialog::onDownloadComplete(const QString& isoPath,
                                                  qint64 fileSize)
{
    m_downloadedFilePath = isoPath;
    m_isDownloading = false;

    m_progressBar->setValue(100);
    double sizeGB = fileSize / (1024.0 * 1024.0 * 1024.0);
    m_statusLabel->setText(
        QString("ISO created successfully! (%1 GB)").arg(sizeGB, 0, 'f', 2));
    m_phaseLabel->setText("Complete!");
    m_phaseLabel->setStyleSheet("font-weight: bold; color: #16a34a;");
    m_speedLabel->clear();
    m_detailLabel->clear();
    m_cancelButton->setEnabled(false);

    QMessageBox::information(this, "ISO Build Complete",
        QString("Windows ISO has been created successfully!\n\n"
                "Saved to: %1\nSize: %2 GB\n\nClick OK to use this image.")
            .arg(isoPath)
            .arg(sizeGB, 0, 'f', 2));

    Q_EMIT downloadCompleted(isoPath);
    accept();
}

void WindowsISODownloadDialog::onDownloadError(const QString& error)
{
    m_isDownloading = false;

    m_statusLabel->setText(QString("Error: %1").arg(error));
    m_cancelButton->setEnabled(false);
    setInputsEnabled(true);
    updateStartButton();

    QMessageBox::critical(this, "Build Error",
        QString("Failed to create Windows ISO:\n\n%1\n\n"
                "Please check your internet connection and try again.")
            .arg(error));
}

void WindowsISODownloadDialog::onStatusMessage(const QString& message)
{
    m_statusLabel->setText(message);
}

// ============================================================================
// Cancel
// ============================================================================

void WindowsISODownloadDialog::onCancelDownload()
{
    auto reply = QMessageBox::question(this, "Cancel Build",
        "Are you sure you want to cancel?\n\n"
        "Downloaded files will be preserved so the download "
        "can be resumed if you retry the same build.",
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        m_downloader->cancel();
        m_isDownloading = false;
        m_statusLabel->setText("Build cancelled");
        m_phaseLabel->clear();
        m_speedLabel->clear();
        m_detailLabel->clear();
        m_cancelButton->setEnabled(false);
        setInputsEnabled(true);
        updateStartButton();
    }
}

// ============================================================================
// Helpers
// ============================================================================

void WindowsISODownloadDialog::onBrowseSaveLocation()
{
    QString current = m_saveLocationEdit->text();
    if (current.isEmpty()) current = getDefaultSavePath();

    QString filePath = QFileDialog::getSaveFileName(
        this, "Save Windows ISO", current,
        "ISO Files (*.iso);;All Files (*.*)");

    if (!filePath.isEmpty()) {
        if (!filePath.endsWith(".iso", Qt::CaseInsensitive))
            filePath += ".iso";
        m_saveLocationEdit->setText(filePath);
    }
}

void WindowsISODownloadDialog::updateStartButton()
{
    bool ready = !m_isDownloading
                 && !m_selectedUpdateId.isEmpty()
                 && m_languageCombo->currentIndex() >= 0
                 && m_editionCombo->currentIndex() >= 0
                 && !m_saveLocationEdit->text().trimmed().isEmpty();
    m_startButton->setEnabled(ready);
}

void WindowsISODownloadDialog::setInputsEnabled(bool enabled)
{
    m_archCombo->setEnabled(enabled);
    m_channelCombo->setEnabled(enabled);
    m_fetchBuildsButton->setEnabled(enabled);
    m_buildListWidget->setEnabled(enabled);
    m_languageCombo->setEnabled(enabled);
    m_editionCombo->setEnabled(enabled);
    m_saveLocationEdit->setEnabled(enabled);
    m_browseSaveButton->setEnabled(enabled);
}

QString WindowsISODownloadDialog::getDefaultSavePath()
{
    QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return QDir(downloads).filePath("Windows.iso");
}
