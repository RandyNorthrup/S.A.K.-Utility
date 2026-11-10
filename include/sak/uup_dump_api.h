// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace sak {
struct NetworkTransferResult;
}  // namespace sak

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
        QString title;      ///< Human-readable title
                            ///< (e.g., "Windows 11, version 24H2 (26100.3194)")
        QString build;      ///< Build number (e.g., "26100.3194")
        QString arch;       ///< Architecture (e.g., "amd64", "arm64")
        qint64 created{0};  ///< Unix timestamp of when the build was added to the database
    };

    /// @brief Information about a downloadable UUP file
    struct FileInfo {
        QString fileName;  ///< File name (e.g., "core_en-us.esd")
        QString sha1;      ///< SHA-1 checksum for integrity verification
        qint64 size{0};    ///< File size in bytes
        QString url;       ///< Direct download URL (Microsoft CDN, time-limited)
        QString uuid;      ///< File UUID
        QString expire;    ///< URL expiration timestamp
    };

    /// @brief Supported release channels
    enum class ReleaseChannel {
        Retail,          ///< Latest public release (stable)
        ReleasePreview,  ///< Release Preview channel
        Beta,            ///< Beta channel
        Dev,             ///< Dev channel
        Canary           ///< Canary channel (most experimental)
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
     * @brief Reject a file entry that could inject aria2 directives or escape/alias the download
     * dir.
     * @details fileName/url/sha1 are serialized into the aria2 input manifest. Returns false (fail
     *          closed) for any control character (NUL/CR/LF/TAB/...) in any field, and for a
     *          fileName that traverses (../), carries a path separator (/ or \), a drive/ADS ':',
     *          a reserved DOS device name (CON, NUL, COM1...), or a trailing dot/space that Windows
     *          would trim into a different on-disk name. Public+static so it is unit-testable
     *          headless.
     */
    [[nodiscard]] static bool isSafeAria2FileEntry(const FileInfo& info);

    /**
     * @brief True iff @p sha1 is a well-formed SHA-1 digest (exactly 40 hex
     *        characters). UUP files are served over an HTTP CDN, so a
     *        missing/malformed checksum means the download cannot be
     *        integrity-verified and the entry must be rejected. Pure;
     *        unit-testable.
     */
    [[nodiscard]] static bool isValidSha1(const QString& sha1);

    /**
     * @brief Convert ReleaseChannel enum to human-readable display string
     */
    static QString channelToDisplayName(ReleaseChannel channel);

    /**
     * @brief Get list of all supported release channels
     */
    static QList<ReleaseChannel> allChannels();

    /**
     * @brief Whether a listed build belongs in @p channel's list. listid.php is a text search, so
     *        "windows 11 amd64" also returns Insider Preview, preview, cumulative and .NET updates,
     *        newest first; review 3's Public Release list held only Insider builds. A public
     *        release is the build UUP dump titles "Windows 11, version 25H2 (26200.9818)". Other
     *        channels are kept as the search returns them. Pure; unit-testable.
     */
    [[nodiscard]] static bool buildBelongsToChannel(const BuildInfo& build, ReleaseChannel channel);

    /**
     * @brief @p builds with the newest feature update first, by the "version YYHn" in each title:
     *        year, then half (26H2, 26H1, 25H2, 24H2, 23H2). In listid.php's date order the VM's
     *        Public Release list read 26H1, 25H2, 26H2, 23H2: each month's update of every
     *        version is added together. Builds of one version keep the order they came in. A
     *        title with no such label (an Insider or preview build) is kept, after every
     *        labelled one, in the order it came in. Pure; unit-testable.
     */
    [[nodiscard]] static QList<BuildInfo> inReleaseOrder(QList<BuildInfo> builds);

    /**
     * @brief Whether a failed metadata answer is worth asking for once more. UUP dump answers
     *        get.php with HTTP 400 now and then under load: review 3's Download & Build failed
     *        at once with it, and the next try downloaded. Pure; unit-testable.
     */
    [[nodiscard]] static bool isPassingServiceError(int http_status);

Q_SIGNALS:
    /**
     * @brief Emitted when builds are fetched from the database
     * @param builds List of available builds, newest release first (inReleaseOrder), and newest
     *        added first within a release
     */
    void buildsFetched(const QList<UupDumpApi::BuildInfo>& builds);

    /**
     * @brief Emitted when languages for a build are available
     * @param langCodes List of language codes (e.g., "en-us", "de-de")
     * @param langNames Map of language code to friendly name (e.g.,
         "en-us" -> "English (United States)")
     */
    void languagesFetched(const QStringList& langCodes, const QMap<QString, QString>& langNames);

    /**
     * @brief Emitted when editions for a build/language are available
     * @param editions List of edition codes (e.g., "PROFESSIONAL", "CORE")
     * @param editionNames Map of edition code to friendly name
     */
    void editionsFetched(const QStringList& editions, const QMap<QString, QString>& editionNames);

    /**
     * @brief Emitted when download file list is ready
     * @param updateName Human-readable build name
     * @param files List of files with download URLs and checksums
     */
    void filesFetched(const QString& updateName, const QList<UupDumpApi::FileInfo>& files);

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
    QNetworkReply* sendApiRequest(const QString& endpoint, const QMap<QString, QString>& params);
    bool checkApiError(const QJsonObject& response, const QString& context);
    bool parseApiResponse(const QByteArray& data, const QString& context, QJsonObject& response);
    [[nodiscard]] QStringList parseEditionList(const QJsonValue& edListVal) const;
    QString buildSearchQuery(const QString& arch, ReleaseChannel channel) const;

    /// @brief Parse and validate a single build entry from the API response.
    /// @return BuildInfo if usable, std::nullopt if the entry carries no uuid (a
    ///         build we cannot address and so must not offer). An out-of-range
    ///         "created" is clamped to 0 rather than invoking double->qint64 UB.
    std::optional<BuildInfo> parseBuildEntry(const QString& key, const QJsonObject& buildObj);

    /// @brief Extract langFancyNames from API response into a display-name map
    QMap<QString, QString> parseLangFancyNames(const QJsonObject& response);

    /// @brief True iff @p info's download URL parses, is HTTPS (or plain HTTP only
    ///        for Microsoft's SHA-1-integrity-checked UUP CDN), and is non-empty.
    ///        Logs the specific rejection reason. A false trips the incomplete-set
    ///        refusal rather than shipping an unfetchable entry.
    [[nodiscard]] bool isAcceptableDownloadUrl(const FileInfo& info);

    /// @brief Parse and validate a single file entry from the API response
    /// @return FileInfo if valid, std::nullopt if rejected
    std::optional<FileInfo> parseAndValidateFileEntry(const QString& key,
                                                      const QJsonObject& fileObj);

    /// @brief Collect all valid file entries from @p filesObj into @p out. If any
    ///        entry is rejected (unsafe/bad-URL/bad-SHA-1), emits apiError and
    ///        returns false -- an incomplete set must not be built (B10-20).
    [[nodiscard]] bool collectValidFiles(const QJsonObject& filesObj, QList<FileInfo>& out);
    /// Asks for the file list once more, after a short wait, when the service answered with a
    /// passing error and this request has not been asked again yet; true when it will be.
    [[nodiscard]] bool askFilesOnceMore(const sak::NetworkTransferResult& transfer);

    QNetworkAccessManager* m_networkManager;
    QList<QNetworkReply*> m_pendingReplies;
    /// The channel the last build list was asked for; its reply is filtered to it.
    ReleaseChannel m_buildsChannel{ReleaseChannel::Retail};
    /// The last get.php request, asked once more when the service answers it with a passing error.
    struct FilesRequest {
        QString update_id;
        QString lang;
        QString edition;
        bool retried{false};
    };
    FilesRequest m_filesRequest;
    /// Counts cancelAll() calls, so a once-more request still waiting when the user cancels is
    /// not sent.
    quint64 m_cancelGeneration{0};

    static constexpr const char* API_BASE_URL = "https://api.uupdump.net";
};
