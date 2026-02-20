// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

/**
 * @brief Catalog of downloadable Linux distributions with version discovery
 *
 * Maintains a curated list of IT-technician-focused Linux distributions with
 * direct download URLs, checksum verification URLs, and optional dynamic
 * version discovery via GitHub Releases API.
 *
 * Each distro entry contains all metadata needed to construct a download URL,
 * verify integrity post-download, and present the distro to users in the UI.
 *
 * Thread-Safety: All public methods must be called from the GUI thread.
 * Network operations (version checking) are asynchronous via Qt signals.
 *
 * @see LinuxISODownloader for the download orchestrator
 * @see LinuxISODownloadDialog for the user-facing dialog
 */
class LinuxDistroCatalog : public QObject {
    Q_OBJECT

public:
    /// @brief Distro use-case category for UI filtering
    enum class Category {
        GeneralPurpose,   ///< Desktop/server Linux distributions
        Security,         ///< Security auditing and penetration testing
        SystemRecovery,   ///< System rescue and recovery environments
        DiskTools,        ///< Disk cloning, partitioning, secure erasure
        Utilities         ///< Multi-boot tools, memory testing, etc.
    };

    /// @brief How the download URL is resolved
    enum class SourceType {
        DirectURL,        ///< Static URL with version substitution
        GitHubRelease,    ///< Resolved via GitHub Releases API
        SourceForge       ///< SourceForge mirror redirect
    };

    /// @brief Complete metadata for a downloadable distribution
    struct DistroInfo {
        QString id;                ///< Unique identifier (e.g., "ubuntu-desktop")
        QString name;              ///< Display name (e.g., "Ubuntu Desktop")
        QString version;           ///< Current known version (e.g., "24.04.4")
        QString versionLabel;      ///< Optional label (e.g., "Noble Numbat", "LTS")
        QString description;       ///< Short description for UI
        Category category;         ///< Use-case category
        SourceType sourceType;     ///< How the URL is resolved
        QString downloadUrl;       ///< Direct URL or URL template ({version} placeholder)
        QString checksumUrl;       ///< URL to checksum file (SHA256SUMS, .sha256, etc.)
        QString checksumType;      ///< "sha256" or "sha1"
        QString fileName;          ///< Expected filename (with {version} placeholder)
        qint64 approximateSize;    ///< Approximate size in bytes (for UI display)
        QString homepage;          ///< Project homepage URL

        // GitHub-specific fields (only used when sourceType == GitHubRelease)
        QString githubOwner;       ///< GitHub repository owner
        QString githubRepo;        ///< GitHub repository name
        QString githubAssetPattern;///< Regex pattern to match the ISO asset filename
    };

    explicit LinuxDistroCatalog(QObject* parent = nullptr);
    ~LinuxDistroCatalog() override;

    // Disable copy/move
    LinuxDistroCatalog(const LinuxDistroCatalog&) = delete;
    LinuxDistroCatalog& operator=(const LinuxDistroCatalog&) = delete;
    LinuxDistroCatalog(LinuxDistroCatalog&&) = delete;
    LinuxDistroCatalog& operator=(LinuxDistroCatalog&&) = delete;

    /**
     * @brief Get all available distributions
     * @return List of all distro entries in the catalog
     */
    QList<DistroInfo> allDistros() const;

    /**
     * @brief Get distributions filtered by category
     * @param category Category to filter by
     * @return Filtered list of distro entries
     */
    QList<DistroInfo> distrosByCategory(Category category) const;

    /**
     * @brief Get all available category names for UI display
     * @return Map of Category enum to display name
     */
    static QMap<Category, QString> categoryNames();

    /**
     * @brief Get a distro by its unique ID
     * @param id Distro identifier
     * @return DistroInfo if found, nullopt-equivalent (empty id) if not
     */
    DistroInfo distroById(const QString& id) const;

    /**
     * @brief Resolve the final download URL for a distro
     *
     * For DirectURL/SourceForge: substitutes {version} in the URL template.
     * For GitHubRelease: returns the cached asset URL from the last version check.
     *
     * @param distro The distro to resolve the URL for
     * @return Resolved download URL, empty if not available
     */
    QString resolveDownloadUrl(const DistroInfo& distro) const;

    /**
     * @brief Resolve the final checksum URL for a distro
     * @param distro The distro to resolve the checksum URL for
     * @return Resolved checksum URL, empty if not available
     */
    QString resolveChecksumUrl(const DistroInfo& distro) const;

    /**
     * @brief Resolve the expected filename for a distro download
     * @param distro The distro to resolve the filename for
     * @return Resolved filename
     */
    QString resolveFileName(const DistroInfo& distro) const;

    /**
     * @brief Check for latest version of a GitHub-hosted distro
     *
     * Queries the GitHub Releases API for the latest release, updates the
     * cached version and download URL. Emits versionCheckCompleted when done.
     *
     * @param distroId ID of the distro to check
     */
    void checkLatestVersion(const QString& distroId);

    /**
     * @brief Cancel any pending version check requests
     */
    void cancelAll();

Q_SIGNALS:
    /**
     * @brief Emitted when a GitHub version check completes
     * @param distroId The distro that was checked
     * @param updatedDistro Updated distro info with latest version
     * @param changed True if the version differs from the catalog default
     */
    void versionCheckCompleted(const QString& distroId,
                               const LinuxDistroCatalog::DistroInfo& updatedDistro,
                               bool changed);

    /**
     * @brief Emitted when a version check fails
     * @param distroId The distro that was checked
     * @param error Human-readable error message
     */
    void versionCheckFailed(const QString& distroId, const QString& error);

private Q_SLOTS:
    void onGitHubReleaseReply();

private:
    void populateCatalog();
    void addDistro(const DistroInfo& distro);
    QString substituteVersion(const QString& pattern, const QString& version) const;
    void parseGitHubRelease(const QString& distroId, const QJsonObject& release);

    QList<DistroInfo> m_distros;
    QMap<QString, int> m_distroIndex; ///< Maps distro ID to index in m_distros
    QMap<QString, QString> m_githubAssetUrls; ///< Cached GitHub asset URLs
    QMap<QString, qint64> m_githubAssetSizes; ///< Cached GitHub asset sizes
    QNetworkAccessManager* m_networkManager;
    QList<QNetworkReply*> m_pendingReplies;
};
