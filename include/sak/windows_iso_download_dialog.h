// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/uup_dump_api.h"
#include "sak/uup_iso_builder.h"

#include <QDialog>
#include <QComboBox>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QString>
#include <QMap>

class WindowsISODownloader;

/**
 * @brief Dialog for downloading Windows ISOs via UUP dump
 *
 * Multi-step wizard UI:
 *   Step 1: Select architecture and release channel, fetch builds
 *   Step 2: Select a build from the list
 *   Step 3: Select language and edition
 *   Step 4: Choose save location and start download + conversion
 *
 * Displays phased progress (download → conversion) with speed and ETA.
 */
class WindowsISODownloadDialog : public QDialog {
    Q_OBJECT

public:
    explicit WindowsISODownloadDialog(WindowsISODownloader* downloader, QWidget* parent = nullptr);
    ~WindowsISODownloadDialog() override;

    WindowsISODownloadDialog(const WindowsISODownloadDialog&) = delete;
    WindowsISODownloadDialog& operator=(const WindowsISODownloadDialog&) = delete;

    /**
     * @brief Get the downloaded ISO file path
     * @return Path to downloaded ISO, empty if not complete
     */
    QString downloadedFilePath() const { return m_downloadedFilePath; }

Q_SIGNALS:
    void downloadCompleted(const QString& filePath);

private Q_SLOTS:
    // API result handlers
    void onBuildsFetched(const QList<UupDumpApi::BuildInfo>& builds);
    void onLanguagesFetched(const QStringList& langCodes,
                            const QMap<QString, QString>& langNames);
    void onEditionsFetched(const QStringList& editions,
                           const QMap<QString, QString>& editionNames);

    // Build progress handlers
    void onPhaseChanged(UupIsoBuilder::Phase phase, const QString& description);
    void onProgressUpdated(int overallPercent, const QString& detail);
    void onSpeedUpdated(double downloadSpeedMBps);
    void onDownloadComplete(const QString& isoPath, qint64 fileSize);
    void onDownloadError(const QString& error);
    void onStatusMessage(const QString& message);

    // UI action handlers
    void onFetchBuildsClicked();
    void onBuildSelected();
    void onLanguageSelected(int index);
    void onBrowseSaveLocation();
    void onStartDownload();
    void onCancelDownload();

private:
    void setupUI();
    void connectSignals();
    void updateStartButton();
    QString getDefaultSavePath();
    void setInputsEnabled(bool enabled);

    WindowsISODownloader* m_downloader;

    // ---- Step 1: Architecture & Channel ----
    QComboBox* m_archCombo;
    QComboBox* m_channelCombo;
    QPushButton* m_fetchBuildsButton;

    // ---- Step 2: Build selection ----
    QListWidget* m_buildListWidget;
    QLabel* m_buildInfoLabel;

    // ---- Step 3: Language & Edition ----
    QComboBox* m_languageCombo;
    QComboBox* m_editionCombo;

    // ---- Step 4: Save location ----
    QLineEdit* m_saveLocationEdit;
    QPushButton* m_browseSaveButton;

    // ---- Progress ----
    QLabel* m_statusLabel;
    QLabel* m_phaseLabel;
    QProgressBar* m_progressBar;
    QLabel* m_speedLabel;
    QLabel* m_detailLabel;

    // ---- Action buttons ----
    QPushButton* m_startButton;
    QPushButton* m_cancelButton;
    QPushButton* m_closeButton;

    // ---- State ----
    QList<UupDumpApi::BuildInfo> m_builds;
    QMap<QString, QString> m_langNames;
    QMap<QString, QString> m_editionNames;
    QString m_selectedUpdateId;
    QString m_downloadedFilePath;
    bool m_isDownloading = false;
};
