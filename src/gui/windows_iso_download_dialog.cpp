// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows_iso_download_dialog.h"

#include "sak/elevation_gate.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"
#include "sak/windows_iso_downloader.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kFetchBuildsGridColumn = 2;
constexpr int kFetchBuildsRowSpan = 2;
constexpr int kFetchBuildsColumnSpan = 1;
constexpr int kPrepareWeight = 5;
constexpr int kDownloadWeight = 60;
constexpr int kConvertWeight = 35;
constexpr double kDownloadSpeedVisibleThresholdMBps = 0.01;
constexpr int kTransferSpeedDisplayPrecision = 1;
constexpr int kIsoSizeDisplayPrecision = 2;

bool matchesAny(const QString& text, std::initializer_list<QLatin1String> keywords) {
    return std::ranges::any_of(keywords, [&text](const auto& kw) { return text.contains(kw); });
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

WindowsISODownloadDialog::WindowsISODownloadDialog(WindowsISODownloader* downloader,
                                                   QWidget* parent)
    : QDialog(parent), m_downloader(downloader) {
    setWindowTitle("Download Windows ISO");
    setModal(true);
    resize(sak::kIsoDialogWidthWin, sak::kIsoDialogHeightWin);

    setupUi();
    connectSignals();

    m_statusLabel->setText("Select architecture and channel, then click Fetch Builds.");
}

WindowsISODownloadDialog::~WindowsISODownloadDialog() = default;

// ============================================================================
// UI Setup
// ============================================================================

void WindowsISODownloadDialog::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* main_layout = new QVBoxLayout(this);
    setupUi_formSections(main_layout);
    setupUi_progressAndButtons(main_layout);
}

// ----------------------------------------------------------------------------
// setupUi helpers
// ----------------------------------------------------------------------------

void WindowsISODownloadDialog::setupUi_formSections(QVBoxLayout* main_layout) {
    setupUi_buildConfig(main_layout);
    setupUi_buildSelection(main_layout);
    setupUi_languageEdition(main_layout);
    setupUi_saveLocation(main_layout);
}

void WindowsISODownloadDialog::setupUi_buildConfig(QVBoxLayout* main_layout) {
    auto* config_group = new QGroupBox("Build Configuration", this);
    auto* config_layout = new QGridLayout(config_group);

    config_layout->addWidget(new QLabel("Architecture:", config_group), 0, 0);
    m_archCombo = new QComboBox(config_group);
    m_archCombo->addItem("64-bit (x64)", "amd64");
    m_archCombo->addItem("ARM64", "arm64");
    config_layout->addWidget(m_archCombo, 0, 1);

    config_layout->addWidget(new QLabel("Channel:", config_group), 1, 0);
    m_channelCombo = new QComboBox(config_group);
    for (auto ch : UupDumpApi::allChannels()) {
        m_channelCombo->addItem(UupDumpApi::channelToDisplayName(ch), static_cast<int>(ch));
    }
    config_layout->addWidget(m_channelCombo, 1, 1);

    m_fetchBuildsButton = new QPushButton("Fetch Builds", config_group);
    m_fetchBuildsButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    config_layout->addWidget(m_fetchBuildsButton,
                             0,
                             kFetchBuildsGridColumn,
                             kFetchBuildsRowSpan,
                             kFetchBuildsColumnSpan);

    main_layout->addWidget(config_group);
}

void WindowsISODownloadDialog::setupUi_buildSelection(QVBoxLayout* main_layout) {
    auto* build_group = new QGroupBox("Available Builds", this);
    auto* build_layout = new QVBoxLayout(build_group);

    m_buildListWidget = new QListWidget(build_group);
    m_buildListWidget->setMaximumHeight(sak::kListAreaMaxH);
    m_buildListWidget->setEnabled(false);
    build_layout->addWidget(m_buildListWidget);

    m_buildInfoLabel = new QLabel("", build_group);
    m_buildInfoLabel->setWordWrap(true);
    m_buildInfoLabel->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeNote));
    build_layout->addWidget(m_buildInfoLabel);

    main_layout->addWidget(build_group);
}

void WindowsISODownloadDialog::setupUi_languageEdition(QVBoxLayout* main_layout) {
    auto* selection_group = new QGroupBox("Language && Edition", this);
    auto* selection_layout = new QGridLayout(selection_group);

    selection_layout->addWidget(new QLabel("Language:", selection_group), 0, 0);
    m_languageCombo = new QComboBox(selection_group);
    m_languageCombo->setEnabled(false);
    selection_layout->addWidget(m_languageCombo, 0, 1);

    selection_layout->addWidget(new QLabel("Edition:", selection_group), 1, 0);
    m_editionCombo = new QComboBox(selection_group);
    m_editionCombo->setEnabled(false);
    selection_layout->addWidget(m_editionCombo, 1, 1);

    main_layout->addWidget(selection_group);
}

void WindowsISODownloadDialog::setupUi_saveLocation(QVBoxLayout* main_layout) {
    auto* save_group = new QGroupBox("Save Location", this);
    auto* save_layout = new QHBoxLayout(save_group);

    m_saveLocationEdit = new QLineEdit(getDefaultSavePath(), save_group);
    save_layout->addWidget(m_saveLocationEdit);

    m_browseSaveButton = new QPushButton("Browse...", save_group);
    m_browseSaveButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    save_layout->addWidget(m_browseSaveButton);

    main_layout->addWidget(save_group);
}

void WindowsISODownloadDialog::setupUi_progressAndButtons(QVBoxLayout* main_layout) {
    // ---- Progress ----
    auto* progress_group = new QGroupBox("Progress", this);
    auto* progress_layout = new QVBoxLayout(progress_group);

    m_statusLabel = new QLabel("Ready", progress_group);
    progress_layout->addWidget(m_statusLabel);

    m_phaseLabel = new QLabel("", progress_group);
    m_phaseLabel->setStyleSheet(sak::ui::kFontWeightBoldStyle);
    progress_layout->addWidget(m_phaseLabel);

    auto* download_progress_label = new QLabel("Download Phase", progress_group);
    progress_layout->addWidget(download_progress_label);

    m_downloadProgressBar = new QProgressBar(progress_group);
    m_downloadProgressBar->setMinimum(0);
    m_downloadProgressBar->setMaximum(sak::kPercentMax);
    m_downloadProgressBar->setValue(0);
    progress_layout->addWidget(m_downloadProgressBar);

    auto* convert_progress_label = new QLabel("Convert & Build Phase", progress_group);
    convert_progress_label->setTextFormat(Qt::PlainText);
    progress_layout->addWidget(convert_progress_label);

    m_convertProgressBar = new QProgressBar(progress_group);
    m_convertProgressBar->setMinimum(0);
    m_convertProgressBar->setMaximum(sak::kPercentMax);
    m_convertProgressBar->setValue(0);
    progress_layout->addWidget(m_convertProgressBar);

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

    m_startButton = new QPushButton("Download && Build ISO", this);
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

void WindowsISODownloadDialog::connectSignals() {
    // UI actions
    connect(m_fetchBuildsButton,
            &QPushButton::clicked,
            this,
            &WindowsISODownloadDialog::onFetchBuildsClicked);
    connect(m_buildListWidget, &QListWidget::currentRowChanged, this, [this](int) {
        onBuildSelected();
    });
    connect(m_languageCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &WindowsISODownloadDialog::onLanguageSelected);
    connect(m_editionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        updateStartButton();
    });
    connect(m_browseSaveButton,
            &QPushButton::clicked,
            this,
            &WindowsISODownloadDialog::onBrowseSaveLocation);
    connect(m_startButton, &QPushButton::clicked, this, &WindowsISODownloadDialog::onStartDownload);
    connect(
        m_cancelButton, &QPushButton::clicked, this, &WindowsISODownloadDialog::onCancelDownload);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);

    // Downloader signals
    connect(m_downloader,
            &WindowsISODownloader::buildsFetched,
            this,
            &WindowsISODownloadDialog::onBuildsFetched);
    connect(m_downloader,
            &WindowsISODownloader::languagesFetched,
            this,
            &WindowsISODownloadDialog::onLanguagesFetched);
    connect(m_downloader,
            &WindowsISODownloader::editionsFetched,
            this,
            &WindowsISODownloadDialog::onEditionsFetched);
    connect(m_downloader,
            &WindowsISODownloader::phaseChanged,
            this,
            &WindowsISODownloadDialog::onPhaseChanged);
    connect(m_downloader,
            &WindowsISODownloader::progressUpdated,
            this,
            &WindowsISODownloadDialog::onProgressUpdated);
    connect(m_downloader,
            &WindowsISODownloader::speedUpdated,
            this,
            &WindowsISODownloadDialog::onSpeedUpdated);
    connect(m_downloader,
            &WindowsISODownloader::downloadComplete,
            this,
            &WindowsISODownloadDialog::onDownloadComplete);
    connect(m_downloader,
            &WindowsISODownloader::downloadError,
            this,
            &WindowsISODownloadDialog::onDownloadError);
    connect(m_downloader,
            &WindowsISODownloader::statusMessage,
            this,
            &WindowsISODownloadDialog::onStatusMessage);
}

// ============================================================================
// Step 1: Fetch Builds
// ============================================================================

void WindowsISODownloadDialog::onFetchBuildsClicked() {
    Q_ASSERT(m_buildListWidget);
    Q_ASSERT(m_languageCombo);
    Q_ASSERT(m_archCombo);
    Q_ASSERT(m_channelCombo);
    const QString arch = m_archCombo->currentData().toString();
    const int channel_idx = m_channelCombo->currentData().toInt();
    auto channel = static_cast<UupDumpApi::ReleaseChannel>(channel_idx);

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

void WindowsISODownloadDialog::onBuildsFetched(const QList<UupDumpApi::BuildInfo>& builds) {
    Q_ASSERT(m_fetchBuildsButton);
    Q_ASSERT(m_buildListWidget);
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
    m_statusLabel->setText(QString("Found %1 builds. Select one to continue.").arg(builds.size()));
}

// ============================================================================
// Step 2: Build Selected -> Fetch Languages
// ============================================================================

void WindowsISODownloadDialog::onBuildSelected() {
    Q_ASSERT(m_languageCombo);
    Q_ASSERT(m_editionCombo);
    Q_ASSERT(m_buildListWidget);
    Q_ASSERT(m_buildInfoLabel);
    const int row = m_buildListWidget->currentRow();
    if (row < 0 || row >= m_builds.size()) {
        return;
    }

    const auto& build = m_builds[row];
    m_selectedUpdateId = build.uuid;

    // Show build info
    const QDateTime created = QDateTime::fromSecsSinceEpoch(build.created);
    m_buildInfoLabel->setText(QString("Build: %1 | Arch: %2 | Added: %3")
                                  .arg(build.build, build.arch, created.toString("yyyy-MM-dd")));

    // Fetch languages for this build
    m_languageCombo->clear();
    m_languageCombo->setEnabled(false);
    m_editionCombo->clear();
    m_editionCombo->setEnabled(false);
    updateStartButton();

    m_downloader->fetchLanguages(m_selectedUpdateId);
}

void WindowsISODownloadDialog::onLanguagesFetched(const QStringList& lang_codes,
                                                  const QMap<QString, QString>& lang_names) {
    m_langNames = lang_names;
    m_languageCombo->clear();

    for (const auto& code : lang_codes) {
        const QString display = lang_names.value(code, code);
        m_languageCombo->addItem(QString("%1 (%2)").arg(display, code), code);
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
// Step 3: Language Selected -> Fetch Editions
// ============================================================================

void WindowsISODownloadDialog::onLanguageSelected(int index) {
    if (index < 0) {
        return;
    }

    const QString lang_code = m_languageCombo->currentData().toString();
    if (lang_code.isEmpty() || m_selectedUpdateId.isEmpty()) {
        return;
    }

    m_editionCombo->clear();
    m_editionCombo->setEnabled(false);
    updateStartButton();

    m_downloader->fetchEditions(m_selectedUpdateId, lang_code);
}

void WindowsISODownloadDialog::onEditionsFetched(const QStringList& editions,
                                                 const QMap<QString, QString>& edition_names) {
    m_editionNames = edition_names;
    m_editionCombo->clear();

    for (const auto& code : editions) {
        const QString display = edition_names.value(code, code);
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

void WindowsISODownloadDialog::onStartDownload() {
    Q_ASSERT(m_saveLocationEdit);
    Q_ASSERT(m_startButton);
    Q_ASSERT(m_languageCombo);
    Q_ASSERT(m_editionCombo);

    // Tier 2: UUP ISO conversion uses DISM which requires admin
    auto gate = sak::showElevationGate(
        this,
        tr("Windows ISO Download"),
        tr("Creating a Windows ISO with DISM requires administrator privileges."));
    if (gate != sak::ElevationGateResult::AlreadyElevated) {
        return;
    }

    if (m_selectedUpdateId.isEmpty()) {
        sak::logWarning("No Build Selected: Please select a build first.");
        sak::showWarningLogged(this, "No Build Selected", "Please select a build first.");
        return;
    }

    const QString lang_code = m_languageCombo->currentData().toString();
    const QString edition = m_editionCombo->currentData().toString();
    QString save_path = m_saveLocationEdit->text().trimmed();

    if (lang_code.isEmpty() || edition.isEmpty()) {
        sak::logWarning("Incomplete Selection: Please select a language and edition.");
        sak::showWarningLogged(this,
                               "Incomplete Selection",
                               "Please select a language and edition.");
        return;
    }
    if (save_path.isEmpty()) {
        sak::logWarning("No Save Path: Please specify where to save the ISO.");
        sak::showWarningLogged(this, "No Save Path", "Please specify where to save the ISO.");
        return;
    }
    if (!save_path.endsWith(".iso", Qt::CaseInsensitive)) {
        save_path += ".iso";
        m_saveLocationEdit->setText(save_path);
    }

    m_isDownloading = true;
    m_currentPhase = UupIsoBuilder::Phase::Idle;
    setInputsEnabled(false);
    m_startButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_downloadProgressBar->setValue(0);
    m_convertProgressBar->setValue(0);

    m_downloader->startDownload(m_selectedUpdateId, lang_code, edition, save_path);
}

// ============================================================================
// Progress Handlers
// ============================================================================

void WindowsISODownloadDialog::onPhaseChanged(UupIsoBuilder::Phase phase,
                                              const QString& description) {
    m_currentPhase = phase;

    // A11Y: prefix phase text so status is conveyed without relying on color alone
    switch (phase) {
    case UupIsoBuilder::Phase::PreparingDownload:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kStatusColorRunning));
        m_phaseLabel->setText(QStringLiteral("\u2699 ") + description);  // [*]
        break;
    case UupIsoBuilder::Phase::DownloadingFiles:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kColorAccentEmerald));
        m_phaseLabel->setText(QStringLiteral("\u2B07 ") + description);  // v
        break;
    case UupIsoBuilder::Phase::ConvertingToISO:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kStatusColorWarning));
        m_phaseLabel->setText(QStringLiteral("\u23F3 ") + description);  // [...]
        break;
    case UupIsoBuilder::Phase::Completed:
        m_phaseLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold,
                                                                     sak::ui::kStatusColorSuccess));
        m_phaseLabel->setText(QStringLiteral("\u2714 ") + description);  // [x]
        break;
    case UupIsoBuilder::Phase::Failed:
        m_phaseLabel->setStyleSheet(
            sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold, sak::ui::kStatusColorError));
        m_phaseLabel->setText(QStringLiteral("\u2718 ") + description);  // [X]
        break;
    default:
        break;
    }
}

void WindowsISODownloadDialog::onProgressUpdated(int overall_percent, const QString& detail) {
    Q_ASSERT(m_downloadProgressBar);
    Q_ASSERT(m_convertProgressBar);
    int download_percent = 0;
    int convert_percent = 0;

    if (overall_percent > kPrepareWeight) {
        download_percent = static_cast<int>(
            ((overall_percent - kPrepareWeight) * sak::kPercentMaxF) / kDownloadWeight);
    }
    download_percent = std::clamp(download_percent, 0, sak::kPercentMax);

    if (overall_percent > (kPrepareWeight + kDownloadWeight)) {
        convert_percent = static_cast<int>(
            ((overall_percent - (kPrepareWeight + kDownloadWeight)) * sak::kPercentMaxF) /
            kConvertWeight);
    }
    convert_percent = std::clamp(convert_percent, 0, sak::kPercentMax);

    if (m_currentPhase == UupIsoBuilder::Phase::PreparingDownload) {
        download_percent = 0;
        convert_percent = 0;
    } else if (m_currentPhase == UupIsoBuilder::Phase::DownloadingFiles) {
        convert_percent = 0;
    } else if (m_currentPhase == UupIsoBuilder::Phase::ConvertingToISO) {
        download_percent = sak::kPercentMax;
    } else if (m_currentPhase == UupIsoBuilder::Phase::Completed) {
        download_percent = sak::kPercentMax;
        convert_percent = sak::kPercentMax;
    }

    m_downloadProgressBar->setValue(download_percent);
    m_convertProgressBar->setValue(convert_percent);
    m_detailLabel->setText(detail);
}

void WindowsISODownloadDialog::onSpeedUpdated(double download_speed_m_bps) {
    if (download_speed_m_bps > kDownloadSpeedVisibleThresholdMBps) {
        m_speedLabel->setText(
            QString("%1 MB/s").arg(download_speed_m_bps, 0, 'f', kTransferSpeedDisplayPrecision));
    }
}

void WindowsISODownloadDialog::onDownloadComplete(const QString& iso_path, qint64 file_size) {
    Q_ASSERT(m_downloadProgressBar);
    Q_ASSERT(m_convertProgressBar);
    m_downloadedFilePath = iso_path;
    m_isDownloading = false;

    m_downloadProgressBar->setValue(sak::kPercentMax);
    m_convertProgressBar->setValue(sak::kPercentMax);
    const double size_gb = static_cast<double>(file_size) / sak::kBytesPerGBf;
    m_statusLabel->setText(QString("ISO created successfully! (%1 GB)")
                               .arg(size_gb, 0, 'f', kIsoSizeDisplayPrecision));
    m_phaseLabel->setText("Complete!");
    m_phaseLabel->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightBold, sak::ui::kStatusColorSuccess));
    m_speedLabel->clear();
    m_detailLabel->clear();
    m_cancelButton->setEnabled(false);

    sak::showInformationLogged(this,
                               "ISO Build Complete",
                               QString("Windows ISO has been created successfully!\n\n"
                                       "Saved to: %1\nSize: %2 GB\n\nClick OK to use this image.")
                                   .arg(iso_path)
                                   .arg(size_gb, 0, 'f', kIsoSizeDisplayPrecision));

    Q_EMIT downloadCompleted(iso_path);
    accept();
}

void WindowsISODownloadDialog::onDownloadError(const QString& error) {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_cancelButton);
    m_isDownloading = false;

    m_statusLabel->setText(QString("Error: %1").arg(error));
    m_cancelButton->setEnabled(false);
    setInputsEnabled(true);
    updateStartButton();

    QString guidance = "Please check the detailed converter output and try again.";
    const QString lower = error.toLower();
    if (matchesAny(lower,
                   {QLatin1String("download"),
                    QLatin1String("network"),
                    QLatin1String("aria2"),
                    QLatin1String("internet")})) {
        guidance = "Please check your internet connection and try again.";
    } else if (matchesAny(lower, {QLatin1String("appx"), QLatin1String("msixbundle")})) {
        guidance =
            "This is a known issue with AppX provisioning. "
            "See the suggestions above for possible workarounds.";
    } else if (matchesAny(lower, {QLatin1String("administrator"), QLatin1String("elevated")})) {
        guidance = "Please restart S.A.K. Utility as Administrator and try again.";
    } else if (matchesAny(lower, {QLatin1String("disk space"), QLatin1String("not enough")})) {
        guidance = "Free disk space on the system and output drives, then retry.";
    }

    sak::logError(("Build Error: Failed to create Windows ISO: " + error).toStdString());
    sak::showCriticalLogged(
        this,
        "Build Error",
        QString("Failed to create Windows ISO:\n\n%1\n\n%2").arg(error, guidance));
}

void WindowsISODownloadDialog::onStatusMessage(const QString& message) {
    Q_ASSERT(m_statusLabel);
    m_statusLabel->setText(message);
}

// ============================================================================
// Cancel
// ============================================================================

void WindowsISODownloadDialog::onCancelDownload() {
    Q_ASSERT(m_downloader);
    Q_ASSERT(m_statusLabel);
    auto reply = sak::showQuestionLogged(this,
                                         "Cancel Build",
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
        m_downloadProgressBar->setValue(0);
        m_convertProgressBar->setValue(0);
        m_cancelButton->setEnabled(false);
        setInputsEnabled(true);
        updateStartButton();
    }
}

// ============================================================================
// Helpers
// ============================================================================

void WindowsISODownloadDialog::onBrowseSaveLocation() {
    Q_ASSERT(m_saveLocationEdit);
    QString current = m_saveLocationEdit->text();
    if (current.isEmpty()) {
        current = getDefaultSavePath();
    }

    QString file_path = QFileDialog::getSaveFileName(
        this, "Save Windows ISO", current, "ISO Files (*.iso);;All Files (*.*)");

    if (!file_path.isEmpty()) {
        if (!file_path.endsWith(".iso", Qt::CaseInsensitive)) {
            file_path += ".iso";
        }
        m_saveLocationEdit->setText(file_path);
    }
}

void WindowsISODownloadDialog::updateStartButton() {
    const bool ready = !m_isDownloading && !m_selectedUpdateId.isEmpty() &&
                       m_languageCombo->currentIndex() >= 0 &&
                       m_editionCombo->currentIndex() >= 0 &&
                       !m_saveLocationEdit->text().trimmed().isEmpty();
    m_startButton->setEnabled(ready);
}

void WindowsISODownloadDialog::setInputsEnabled(bool enabled) {
    m_archCombo->setEnabled(enabled);
    m_channelCombo->setEnabled(enabled);
    m_fetchBuildsButton->setEnabled(enabled);
    m_buildListWidget->setEnabled(enabled);
    m_languageCombo->setEnabled(enabled);
    m_editionCombo->setEnabled(enabled);
    m_saveLocationEdit->setEnabled(enabled);
    m_browseSaveButton->setEnabled(enabled);
}

QString WindowsISODownloadDialog::getDefaultSavePath() {
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return QDir(downloads).filePath("Windows.iso");
}
