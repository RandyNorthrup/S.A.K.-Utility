// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/linux_distro_catalog.h"
#include "sak/linux_iso_downloader.h"

#include <QDialog>
#include <QComboBox>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QString>

class LinuxISODownloader;

/**
 * @brief Dialog for downloading Linux ISO images
 *
 * Single-step wizard UI:
 *   1. Select category filter (optional)
 *   2. Select distribution from the list
 *   3. View distro details (description, size, version)
 *   4. Choose save location and start download
 *
 * Displays download progress with speed and checksum verification status.
 *
 * Unlike the Windows ISO dialog which has a multi-step build/fetch wizard,
 * this dialog is simpler since Linux ISOs are direct downloads.
 */
class LinuxISODownloadDialog : public QDialog {
    Q_OBJECT

public:
    explicit LinuxISODownloadDialog(LinuxISODownloader* downloader, QWidget* parent = nullptr);
    ~LinuxISODownloadDialog() override;

    LinuxISODownloadDialog(const LinuxISODownloadDialog&) = delete;
    LinuxISODownloadDialog& operator=(const LinuxISODownloadDialog&) = delete;

    /**
     * @brief Get the downloaded ISO file path
     * @return Path to downloaded ISO, empty if not complete
     */
    QString downloadedFilePath() const { return m_downloadedFilePath; }

Q_SIGNALS:
    void downloadCompleted(const QString& filePath);

private Q_SLOTS:
    // UI action handlers
    void onCategoryChanged(int index);
    void onDistroSelected();
    void onBrowseSaveLocation();
    void onStartDownload();
    void onCancelDownload();

    // Downloader progress handlers
    void onPhaseChanged(LinuxISODownloader::Phase phase, const QString& description);
    void onProgressUpdated(int percent, const QString& detail);
    void onSpeedUpdated(double speedMBps);
    void onDownloadComplete(const QString& isoPath, qint64 fileSize);
    void onDownloadError(const QString& error);
    void onStatusMessage(const QString& message);

private:
    void setupUI();
    void connectSignals();
    void populateDistroList();
    void updateDistroDetails();
    void updateStartButton();
    void setInputsEnabled(bool enabled);
    QString getDefaultSavePath(const QString& fileName) const;
    static QString formatSize(qint64 bytes);

    LinuxISODownloader* m_downloader;

    // ---- Category & Distro Selection ----
    QComboBox* m_categoryCombo;
    QListWidget* m_distroListWidget;
    QLabel* m_distroDescriptionLabel;
    QLabel* m_distroVersionLabel;
    QLabel* m_distroSizeLabel;
    QLabel* m_distroHomepageLabel;

    // ---- Save Location ----
    QLineEdit* m_saveLocationEdit;
    QPushButton* m_browseSaveButton;

    // ---- Progress ----
    QLabel* m_statusLabel;
    QLabel* m_phaseLabel;
    QProgressBar* m_progressBar;
    QLabel* m_speedLabel;
    QLabel* m_detailLabel;

    // ---- Action Buttons ----
    QPushButton* m_startButton;
    QPushButton* m_cancelButton;
    QPushButton* m_closeButton;

    // ---- State ----
    QList<LinuxDistroCatalog::DistroInfo> m_currentDistros;
    QString m_selectedDistroId;
    QString m_downloadedFilePath;
    bool m_isDownloading = false;
};
