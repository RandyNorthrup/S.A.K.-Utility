// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QElapsedTimer>
#include <QMap>
#include <memory>

/**
 * @brief Windows ISO Downloader - Downloads Windows 11 ISOs from Microsoft
 * 
 * Uses Microsoft Media Creation Tool API to download official Windows 11 ISOs
 * directly without requiring the Media Creation Tool executable.
 * 
 * Workflow:
 * 1. Fetch product page to get session ID
 * 2. Request available languages
 * 3. Request download URL for selected language and architecture
 * 4. Download ISO with progress tracking
 * 5. Verify file size and integrity
 * 
 * Features:
 * - Direct download from Microsoft servers
 * - Multiple language support
 * - x64 and ARM64 architecture support
 * - Resume capability (if connection drops)
 * - Progress tracking with speed calculation
 * - Automatic retry on network errors
 * 
 * Thread-Safety: All methods are thread-safe. Network operations are async.
 * 
 * Example:
 * @code
 * WindowsISODownloader downloader;
 * connect(&downloader, &WindowsISODownloader::downloadProgress,
 *         [](qint64 bytes, qint64 total, double speed) {
 *     qDebug() << "Progress:" << (bytes * 100 / total) << "%";
 * });
 * 
 * downloader.fetchProductPage();
 * // ... wait for productPageFetched signal
 * downloader.fetchAvailableLanguages();
 * // ... wait for languagesFetched signal
 * downloader.requestDownloadUrl("English", "x64");
 * // ... wait for downloadUrlReceived signal
 * downloader.downloadISO(url, "C:/Win11.iso");
 * @endcode
 */
class WindowsISODownloader : public QObject {
    Q_OBJECT

public:
    explicit WindowsISODownloader(QObject* parent = nullptr);
    ~WindowsISODownloader() override;

    // Disable copy and move
    WindowsISODownloader(const WindowsISODownloader&) = delete;
    WindowsISODownloader& operator=(const WindowsISODownloader&) = delete;
    WindowsISODownloader(WindowsISODownloader&&) = delete;
    WindowsISODownloader& operator=(WindowsISODownloader&&) = delete;

    /**
     * @brief Step 1: Fetch product page to get session ID
     * Emits productPageFetched on success
     */
    void fetchProductPage();

    /**
     * @brief Step 2: Fetch available languages
     * Must be called after fetchProductPage succeeds
     * Emits languagesFetched on success
     */
    void fetchAvailableLanguages();

    /**
     * @brief Step 3: Request download URL
     * @param language Language name (from getAvailableLanguages())
     * @param architecture "x64" or "ARM64"
     * Emits downloadUrlReceived on success
     */
    void requestDownloadUrl(const QString& language, const QString& architecture);

    /**
     * @brief Step 4: Download ISO file
     * @param url Download URL (from downloadUrlReceived signal)
     * @param savePath Local path to save ISO
     */
    void downloadISO(const QString& url, const QString& savePath);

    /**
     * @brief Cancel ongoing operation
     */
    void cancel();

    /**
     * @brief Get available languages
     * Valid after languagesFetched signal
     * @return List of language names
     */
    QStringList getAvailableLanguages() const;

    /**
     * @brief Get available architectures
     * @return List of architectures (typically ["x64", "ARM64"])
     */
    QStringList getAvailableArchitectures() const;

    /**
     * @brief Check if download URL has expired
     * @param url URL to check
     * @return true if URL is expired or will expire soon
     */
    bool isDownloadUrlExpired(const QString& url) const;

Q_SIGNALS:
    /**
     * @brief Emitted when product page is fetched
     * @param sessionId Session ID for subsequent requests
     * @param productEditionId Product edition ID
     */
    void productPageFetched(const QString& sessionId, const QString& productEditionId);

    /**
     * @brief Emitted when languages are fetched
     * @param languages List of available languages
     */
    void languagesFetched(const QStringList& languages);

    /**
     * @brief Emitted when download URL is received
     * @param url Download URL (valid ~24 hours)
     * @param expiresAt Expiration timestamp
     */
    void downloadUrlReceived(const QString& url, const QDateTime& expiresAt);

    /**
     * @brief Emitted periodically during download
     * @param bytesReceived Bytes downloaded so far
     * @param bytesTotal Total bytes to download
     * @param speedMBps Current download speed in MB/s
     */
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal, double speedMBps);

    /**
     * @brief Emitted when download completes
     * @param filePath Path to downloaded ISO
     * @param fileSize Size of downloaded file
     */
    void downloadComplete(const QString& filePath, qint64 fileSize);

    /**
     * @brief Emitted on error
     * @param error Error message
     */
    void downloadError(const QString& error);

    /**
     * @brief Emitted with status messages
     * @param message Status message for UI display
     */
    void statusMessage(const QString& message);

private Q_SLOTS:
    void onProductPageFinished();
    void onLanguagesFinished();
    void onDownloadUrlFinished();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();
    void onNetworkError(QNetworkReply::NetworkError error);

private:
    QString parseSessionId(const QString& html);
    QString parseProductEditionId(const QString& html);
    QStringList parseLanguages(const QString& html);
    QString parseDownloadUrl(const QString& html);
    QDateTime parseExpirationTime(const QString& url);
    void calculateSpeed();

    QNetworkAccessManager* m_networkManager;
    QNetworkReply* m_currentReply;
    QFile* m_downloadFile;
    
    QString m_sessionId;
    QString m_productEditionId;
    QString m_skuId;
    QString m_orgId;
    QString m_profileId;
    QStringList m_availableLanguages;
    QMap<QString, QString> m_languageToSkuMap;
    
    QString m_downloadUrl;
    QString m_savePath;
    qint64 m_bytesReceived;
    qint64 m_bytesTotal;
    double m_speedMBps;
    
    QElapsedTimer m_downloadStartTime;
    qint64 m_lastSpeedUpdate;
    qint64 m_lastSpeedBytes;
    
    bool m_isCancelled;
    
    // Retry mechanism
    int m_retryCount;
    int m_maxRetries;
    int m_retryDelayMs;
    enum class RetryOperation { None, FetchProductPage, FetchLanguages, FetchDownloadUrl };
    RetryOperation m_pendingRetry;
    QString m_retryLanguage;
    QString m_retryArchitecture;
    
    void scheduleRetry(RetryOperation operation, const QString& language = "", const QString& architecture = "");
    void executeRetry();
    
    // API endpoints
    static const QString PRODUCT_PAGE_URL;
    static const QString API_BASE_URL;
    static const QString USER_AGENT;
};
