// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

/**
 * @brief REST API client for the UUP dump service (uupdump.net)
 *
 * Provides typed access to the UUP dump JSON API for querying available
 * Windows builds, languages, editions, and obtaining download links for
 * UUP (Unified Update Platform) files directly from Microsoft servers.
 *
 * All network operations are asynchronous. Results are delivered via Qt signals.
 * Errors from the API or network are reported through the apiError signal.
 *
 * Thread-Safety: All methods must be called from the same thread (typically GUI thread).
 *
 * API Reference: https://git.uupdump.net/uup-dump/json-api
 *
 * Example:
 * @code
 * UupDumpApi api;
 * connect(&api, &UupDumpApi::buildsFetched, [](const auto& builds) {
 *     for (const auto& b : builds)
 *         qDebug() << b.title << b.uuid;
 * });
 * api.fetchAvailableBuilds("amd64");
 * @endcode
 */
class UupDumpApi : public QObject {
    Q_OBJECT

public:
    /// @brief Information about an available Windows build
    struct BuildInfo {
        QString uuid;       ///< Unique update identifier (used for subsequent API calls)
        QString title;      ///< Human-readable title (e.g., "Windows 11, version 24H2 (26100.3194)")
        QString build;      ///< Build number (e.g., "26100.3194")
        QString arch;       ///< Architecture (e.g., "amd64", "arm64")
        qint64  created;    ///< Unix timestamp of when the build was added to the database
    };

    /// @brief Information about a downloadable UUP file
    struct FileInfo {
        QString fileName;   ///< File name (e.g., "core_en-us.esd")
        QString sha1;       ///< SHA-1 checksum for integrity verification
        qint64  size;       ///< File size in bytes
        QString url;        ///< Direct download URL (Microsoft CDN, time-limited)
        QString uuid;       ///< File UUID
        QString expire;     ///< URL expiration timestamp
    };

    /// @brief Supported release channels
    enum class ReleaseChannel {
        Retail,             ///< Latest public release (stable)
        ReleasePreview,     ///< Release Preview channel
        Beta,               ///< Beta channel
        Dev,                ///< Dev channel
        Canary              ///< Canary channel (most experimental)
    };

    explicit UupDumpApi(QObject* parent = nullptr);
    ~UupDumpApi() override;

    // Disable copy/move
    UupDumpApi(const UupDumpApi&) = delete;
    UupDumpApi& operator=(const UupDumpApi&) = delete;
    UupDumpApi(UupDumpApi&&) = delete;
    UupDumpApi& operator=(UupDumpApi&&) = delete;

    /**
     * @brief Fetch available builds from the UUP dump database
     * @param arch Architecture filter ("amd64" or "arm64")
     * @param channel Release channel filter
     *
     * Emits buildsFetched on success, apiError on failure.
     */
    void fetchAvailableBuilds(const QString& arch = "amd64",
                              ReleaseChannel channel = ReleaseChannel::Retail);

    /**
     * @brief List available languages for a specific build
     * @param updateId Build UUID from BuildInfo::uuid
     *
     * Emits languagesFetched on success, apiError on failure.
     */
    void listLanguages(const QString& updateId);

    /**
     * @brief List available editions for a specific build and language
     * @param updateId Build UUID from BuildInfo::uuid
     * @param lang Language code in xx-xx format (e.g., "en-us")
     *
     * Emits editionsFetched on success, apiError on failure.
     */
    void listEditions(const QString& updateId, const QString& lang);

    /**
     * @brief Get download links for UUP files
     * @param updateId Build UUID from BuildInfo::uuid
     * @param lang Language code in xx-xx format
     * @param edition Edition name (e.g., "PROFESSIONAL", "CORE")
     *
     * Emits filesFetched on success, apiError on failure.
     */
    void getFiles(const QString& updateId, const QString& lang, const QString& edition);

    /**
     * @brief Cancel all pending API requests
     */
    void cancelAll();

    /**
     * @brief Convert ReleaseChannel enum to API ring parameter string
     */
    static QString channelToRing(ReleaseChannel channel);

    /**
     * @brief Convert ReleaseChannel enum to human-readable display string
     */
    static QString channelToDisplayName(ReleaseChannel channel);

    /**
     * @brief Get list of all supported release channels
     */
    static QList<ReleaseChannel> allChannels();

Q_SIGNALS:
    /**
     * @brief Emitted when builds are fetched from the database
     * @param builds List of available builds, sorted by date (newest first)
     */
    void buildsFetched(const QList<UupDumpApi::BuildInfo>& builds);

    /**
     * @brief Emitted when languages for a build are available
     * @param langCodes List of language codes (e.g., "en-us", "de-de")
     * @param langNames Map of language code to friendly name (e.g., "en-us" → "English (United States)")
     */
    void languagesFetched(const QStringList& langCodes,
                          const QMap<QString, QString>& langNames);

    /**
     * @brief Emitted when editions for a build/language are available
     * @param editions List of edition codes (e.g., "PROFESSIONAL", "CORE")
     * @param editionNames Map of edition code to friendly name
     */
    void editionsFetched(const QStringList& editions,
                         const QMap<QString, QString>& editionNames);

    /**
     * @brief Emitted when download file list is ready
     * @param updateName Human-readable build name
     * @param files List of files with download URLs and checksums
     */
    void filesFetched(const QString& updateName,
                      const QList<UupDumpApi::FileInfo>& files);

    /**
     * @brief Emitted when an API request fails
     * @param error Human-readable error description
     */
    void apiError(const QString& error);

private Q_SLOTS:
    void onBuildsFetchReply();
    void onLanguagesReply();
    void onEditionsReply();
    void onFilesReply();

private:
    QNetworkReply* sendApiRequest(const QString& endpoint,
                                 const QMap<QString, QString>& params);
    bool checkApiError(const QJsonObject& response, const QString& context);
    QString buildSearchQuery(const QString& arch, ReleaseChannel channel) const;

    QNetworkAccessManager* m_networkManager;
    QList<QNetworkReply*> m_pendingReplies;

    static constexpr const char* API_BASE_URL = "https://api.uupdump.net";
};
