// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>
#include <QComboBox>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QString>
#include <QDateTime>

class WindowsISODownloader;

/**
 * @brief Dialog for downloading Windows 11 ISOs
 * 
 * Provides UI for selecting language, architecture, and download location
 * for Windows 11 ISO downloads via WindowsISODownloader.
 */
class WindowsISODownloadDialog : public QDialog {
    Q_OBJECT

public:
    explicit WindowsISODownloadDialog(WindowsISODownloader* downloader, QWidget* parent = nullptr);
    ~WindowsISODownloadDialog() override;

    /**
     * @brief Get the downloaded ISO file path
     * @return Path to downloaded ISO, empty if download not complete
     */
    QString downloadedFilePath() const { return m_downloadedFilePath; }

Q_SIGNALS:
    void downloadCompleted(const QString& filePath);

private Q_SLOTS:
    void onProductPageFetched(const QString& sessionId, const QString& productEditionId);
    void onLanguagesFetched(const QStringList& languages);
    void onDownloadUrlReceived(const QString& url, const QDateTime& expiresAt);
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal, double speedMBps);
    void onDownloadComplete(const QString& filePath, qint64 fileSize);
    void onDownloadError(const QString& error);
    void onStatusMessage(const QString& message);
    
    void onBrowseSaveLocation();
    void onStartDownload();
    void onCancelDownload();

private:
    void setupUI();
    void connectSignals();
    void updateDownloadButton();
    QString getDefaultSavePath();

    WindowsISODownloader* m_downloader;
    
    // UI Components
    QComboBox* m_languageCombo;
    QComboBox* m_architectureCombo;
    QLineEdit* m_saveLocationEdit;
    QPushButton* m_browseSaveButton;
    QLabel* m_statusLabel;
    QProgressBar* m_progressBar;
    QLabel* m_speedLabel;
    QLabel* m_sizeLabel;
    QPushButton* m_downloadButton;
    QPushButton* m_cancelButton;
    QPushButton* m_closeButton;
    
    // State
    QString m_downloadUrl;
    QDateTime m_urlExpiresAt;
    QString m_downloadedFilePath;
    bool m_isDownloading;
    bool m_sessionInitialized;
    bool m_languagesLoaded;
};
