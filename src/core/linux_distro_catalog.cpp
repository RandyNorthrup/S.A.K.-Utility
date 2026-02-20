// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file linux_distro_catalog.cpp
 * @brief Curated catalog of downloadable Linux distributions
 *
 * Contains metadata for IT-technician-focused Linux distributions including
 * direct download URLs, checksum URLs, and GitHub Releases API integration
 * for dynamic version discovery.
 *
 * All download URLs have been verified against official project pages.
 * SourceForge URLs use the /download suffix for automatic mirror redirection.
 * GitHub-hosted distros use the Releases API for version discovery.
 */

#include "sak/linux_distro_catalog.h"
#include "sak/logger.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>

// ============================================================================
// Construction / Destruction
// ============================================================================

LinuxDistroCatalog::LinuxDistroCatalog(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    populateCatalog();
    sak::log_info("LinuxDistroCatalog initialized with " +
                  std::to_string(m_distros.size()) + " distributions");
}

LinuxDistroCatalog::~LinuxDistroCatalog()
{
    cancelAll();
}

// ============================================================================
// Catalog Population
// ============================================================================

void LinuxDistroCatalog::populateCatalog()
{
    // ---- General Purpose ----

    addDistro({
        /*.id =*/               "ubuntu-desktop",
        /*.name =*/             "Ubuntu Desktop",
        /*.version =*/          "24.04.4",
        /*.versionLabel =*/     "Noble Numbat (LTS)",
        /*.description =*/      "The most popular Linux desktop. Full graphical environment "
                                "with office suite, web browser, and media tools. Ideal for "
                                "setting up client workstations.",
        /*.category =*/         Category::GeneralPurpose,
        /*.sourceType =*/       SourceType::DirectURL,
        /*.downloadUrl =*/      "https://releases.ubuntu.com/noble/ubuntu-{version}-desktop-amd64.iso",
        /*.checksumUrl =*/      "https://releases.ubuntu.com/noble/SHA256SUMS",
        /*.checksumType =*/     "sha256",
        /*.fileName =*/         "ubuntu-{version}-desktop-amd64.iso",
        /*.approximateSize =*/  static_cast<qint64>(6.2 * 1024 * 1024 * 1024), // ~6.2 GB
        /*.homepage =*/         "https://ubuntu.com/desktop",
        /*.githubOwner =*/      {},
        /*.githubRepo =*/       {},
        /*.githubAssetPattern =*/ {}
    });

    addDistro({
        /*.id =*/               "ubuntu-server",
        /*.name =*/             "Ubuntu Server",
        /*.version =*/          "24.04.4",
        /*.versionLabel =*/     "Noble Numbat (LTS)",
        /*.description =*/      "Minimal server installation with no GUI. Ideal for deploying "
                                "headless servers, VMs, and containers. Includes OpenSSH, LVM, "
                                "and cloud-init.",
        /*.category =*/         Category::GeneralPurpose,
        /*.sourceType =*/       SourceType::DirectURL,
        /*.downloadUrl =*/      "https://releases.ubuntu.com/noble/ubuntu-{version}-live-server-amd64.iso",
        /*.checksumUrl =*/      "https://releases.ubuntu.com/noble/SHA256SUMS",
        /*.checksumType =*/     "sha256",
        /*.fileName =*/         "ubuntu-{version}-live-server-amd64.iso",
        /*.approximateSize =*/  static_cast<qint64>(3.2 * 1024 * 1024 * 1024), // ~3.2 GB
        /*.homepage =*/         "https://ubuntu.com/server",
        /*.githubOwner =*/      {},
        /*.githubRepo =*/       {},
        /*.githubAssetPattern =*/ {}
    });

    addDistro({
        /*.id =*/               "linuxmint-cinnamon",
        /*.name =*/             "Linux Mint Cinnamon",
        /*.version =*/          "22.3",
        /*.versionLabel =*/     "Zena",
        /*.description =*/      "Sleek, modern desktop based on Ubuntu LTS. Familiar Windows-like "
                                "interface — excellent for migrating users from Windows. Includes "
                                "full multimedia codecs.",
        /*.category =*/         Category::GeneralPurpose,
        /*.sourceType =*/       SourceType::DirectURL,
        /*.downloadUrl =*/      "https://mirrors.kernel.org/linuxmint/stable/{version}/linuxmint-{version}-cinnamon-64bit.iso",
        /*.checksumUrl =*/      "https://mirrors.kernel.org/linuxmint/stable/{version}/sha256sum.txt",
        /*.checksumType =*/     "sha256",
        /*.fileName =*/         "linuxmint-{version}-cinnamon-64bit.iso",
        /*.approximateSize =*/  static_cast<qint64>(2.9 * 1024 * 1024 * 1024), // ~2.9 GB
        /*.homepage =*/         "https://linuxmint.com",
        /*.githubOwner =*/      {},
        /*.githubRepo =*/       {},
        /*.githubAssetPattern =*/ {}
    });

    // ---- Security ----

    addDistro({
        /*.id =*/               "kali-linux",
        /*.name =*/             "Kali Linux",
        /*.version =*/          "2025.4",
        /*.versionLabel =*/     "Installer",
        /*.description =*/      "The most advanced penetration testing distribution. Includes "
                                "600+ security tools for network analysis, vulnerability "
                                "assessment, and forensics. Essential for security audits.",
        /*.category =*/         Category::Security,
        /*.sourceType =*/       SourceType::DirectURL,
        /*.downloadUrl =*/      "https://cdimage.kali.org/kali-{version}/kali-linux-{version}-installer-amd64.iso",
        /*.checksumUrl =*/      "https://cdimage.kali.org/kali-{version}/SHA256SUMS",
        /*.checksumType =*/     "sha256",
        /*.fileName =*/         "kali-linux-{version}-installer-amd64.iso",
        /*.approximateSize =*/  static_cast<qint64>(4.4 * 1024 * 1024 * 1024), // ~4.4 GB
        /*.homepage =*/         "https://www.kali.org",
        /*.githubOwner =*/      {},
        /*.githubRepo =*/       {},
        /*.githubAssetPattern =*/ {}
    });

    // ---- System Recovery ----

    addDistro({
        /*.id =*/               "systemrescue",
        /*.name =*/             "SystemRescue",
        /*.version =*/          "12.03",
        /*.versionLabel =*/     {},
        /*.description =*/      "Bootable Linux rescue environment for repairing unbootable systems. "
                                "Includes filesystem tools (fsck, ntfsfix), network tools, "
                                "partition editors, and data recovery utilities.",
        /*.category =*/         Category::SystemRecovery,
        /*.sourceType =*/       SourceType::DirectURL,
        /*.downloadUrl =*/      "https://fastly-cdn.system-rescue.org/systemrescue-{version}-amd64.iso",
        /*.checksumUrl =*/      "https://fastly-cdn.system-rescue.org/systemrescue-{version}-amd64.iso.sha256",
        /*.checksumType =*/     "sha256",
        /*.fileName =*/         "systemrescue-{version}-amd64.iso",
        /*.approximateSize =*/  static_cast<qint64>(1166LL * 1024 * 1024), // ~1166 MiB
        /*.homepage =*/         "https://www.system-rescue.org",
        /*.githubOwner =*/      {},
        /*.githubRepo =*/       {},
        /*.githubAssetPattern =*/ {}
    });

    // ---- Disk Tools ----

    addDistro({
        /*.id =*/               "clonezilla",
        /*.name =*/             "Clonezilla Live",
        /*.version =*/          "3.3.0-33",
        /*.versionLabel =*/     "Stable (Debian-based)",
        /*.description =*/      "Disk imaging and cloning tool. Clone entire drives or partitions "
                                "for backup, migration, or mass deployment. Supports multicasting "
                                "for deploying to many machines simultaneously.",
        /*.category =*/         Category::DiskTools,
        /*.sourceType =*/       SourceType::SourceForge,
        /*.downloadUrl =*/      "https://sourceforge.net/projects/clonezilla/files/clonezilla_live_stable/{version}/clonezilla-live-{version}-amd64.iso/download",
        /*.checksumUrl =*/      {},  // SourceForge provides checksums on download page
        /*.checksumType =*/     {},
        /*.fileName =*/         "clonezilla-live-{version}-amd64.iso",
        /*.approximateSize =*/  static_cast<qint64>(530LL * 1024 * 1024), // ~530 MB
        /*.homepage =*/         "https://clonezilla.org",
        /*.githubOwner =*/      {},
        /*.githubRepo =*/       {},
        /*.githubAssetPattern =*/ {}
    });

    addDistro({
        /*.id =*/               "gparted-live",
        /*.name =*/             "GParted Live",
        /*.version =*/          "1.8.0-2",
        /*.versionLabel =*/     {},
        /*.description =*/      "Bootable partition editor for creating, resizing, moving, and "
                                "copying disk partitions. Supports ext2/3/4, NTFS, FAT16/32, "
                                "HFS+, XFS, and many more filesystems.",
        /*.category =*/         Category::DiskTools,
        /*.sourceType =*/       SourceType::SourceForge,
        /*.downloadUrl =*/      "https://sourceforge.net/projects/gparted/files/gparted-live-stable/{version}/gparted-live-{version}-amd64.iso/download",
        /*.checksumUrl =*/      {},  // SourceForge provides checksums on download page
        /*.checksumType =*/     {},
        /*.fileName =*/         "gparted-live-{version}-amd64.iso",
        /*.approximateSize =*/  static_cast<qint64>(635LL * 1024 * 1024), // ~635 MB
        /*.homepage =*/         "https://gparted.org",
        /*.githubOwner =*/      {},
        /*.githubRepo =*/       {},
        /*.githubAssetPattern =*/ {}
    });

    addDistro({
        /*.id =*/               "shredos",
        /*.name =*/             "ShredOS",
        /*.version =*/          "v2025.11_28_x86-64_0.40",
        /*.versionLabel =*/     "nwipe 0.40",
        /*.description =*/      "Bootable secure disk erasure tool powered by nwipe. Wipes drives "
                                "using DoD 5220.22-M, Gutmann, and other standards. Generates "
                                "PDF audit reports for compliance. Essential for ITAD.",
        /*.category =*/         Category::DiskTools,
        /*.sourceType =*/       SourceType::GitHubRelease,
        /*.downloadUrl =*/      {},  // Resolved via GitHub Releases API
        /*.checksumUrl =*/      {},  // SHA1 sidecar in release assets
        /*.checksumType =*/     "sha1",
        /*.fileName =*/         {},  // Resolved from GitHub asset name
        /*.approximateSize =*/  static_cast<qint64>(900LL * 1024 * 1024), // ~900 MB
        /*.homepage =*/         "https://github.com/PartialVolume/shredos.x86_64",
        /*.githubOwner =*/      "PartialVolume",
        /*.githubRepo =*/       "shredos.x86_64",
        /*.githubAssetPattern =*/ R"(shredos.*x86-64.*\.iso$)"
    });

    // ---- Utilities ----

    addDistro({
        /*.id =*/               "ventoy",
        /*.name =*/             "Ventoy LiveCD",
        /*.version =*/          "1.1.10",
        /*.versionLabel =*/     {},
        /*.description =*/      "Multi-boot USB solution. Install Ventoy to a USB drive, then simply "
                                "copy ISO files to the drive — no reformatting needed. Supports "
                                "110+ OS types including Windows, Linux, and WinPE.",
        /*.category =*/         Category::Utilities,
        /*.sourceType =*/       SourceType::GitHubRelease,
        /*.downloadUrl =*/      {},  // Resolved via GitHub Releases API
        /*.checksumUrl =*/      {},  // SHA256 in release body
        /*.checksumType =*/     "sha256",
        /*.fileName =*/         "ventoy-{version}-livecd.iso",
        /*.approximateSize =*/  static_cast<qint64>(196LL * 1024 * 1024), // ~196 MB
        /*.homepage =*/         "https://www.ventoy.net",
        /*.githubOwner =*/      "ventoy",
        /*.githubRepo =*/       "Ventoy",
        /*.githubAssetPattern =*/ R"(ventoy-.*-livecd\.iso$)"
    });

    addDistro({
        /*.id =*/               "memtest86plus",
        /*.name =*/             "Memtest86+",
        /*.version =*/          "7.20",
        /*.versionLabel =*/     {},
        /*.description =*/      "Comprehensive memory diagnostic tool. Tests RAM for errors using "
                                "multiple test patterns. Boots directly — no OS required. Essential "
                                "for diagnosing random crashes and blue screens.",
        /*.category =*/         Category::Utilities,
        /*.sourceType =*/       SourceType::GitHubRelease,
        /*.downloadUrl =*/      {},  // Resolved via GitHub Releases API
        /*.checksumUrl =*/      {},  // SHA256 in release assets
        /*.checksumType =*/     "sha256",
        /*.fileName =*/         {},  // Resolved from GitHub asset name
        /*.approximateSize =*/  static_cast<qint64>(25LL * 1024 * 1024), // ~25 MB
        /*.homepage =*/         "https://memtest.org",
        /*.githubOwner =*/      "memtest86plus",
        /*.githubRepo =*/       "memtest86plus",
        /*.githubAssetPattern =*/ R"(memtest86plus-.*\.iso\.gz$)"
    });
}

void LinuxDistroCatalog::addDistro(const DistroInfo& distro)
{
    m_distroIndex[distro.id] = m_distros.size();
    m_distros.append(distro);
}

// ============================================================================
// Catalog Queries
// ============================================================================

QList<LinuxDistroCatalog::DistroInfo> LinuxDistroCatalog::allDistros() const
{
    return m_distros;
}

QList<LinuxDistroCatalog::DistroInfo> LinuxDistroCatalog::distrosByCategory(
    Category category) const
{
    QList<DistroInfo> result;
    for (const auto& d : m_distros) {
        if (d.category == category) {
            result.append(d);
        }
    }
    return result;
}

QMap<LinuxDistroCatalog::Category, QString> LinuxDistroCatalog::categoryNames()
{
    return {
        {Category::GeneralPurpose, "General Purpose"},
        {Category::Security,       "Security && Pen-Testing"},
        {Category::SystemRecovery, "System Recovery"},
        {Category::DiskTools,      "Disk Tools"},
        {Category::Utilities,      "Utilities"}
    };
}

LinuxDistroCatalog::DistroInfo LinuxDistroCatalog::distroById(const QString& id) const
{
    auto it = m_distroIndex.find(id);
    if (it != m_distroIndex.end() && *it >= 0 && *it < m_distros.size()) {
        return m_distros[*it];
    }
    return {};  // Return empty DistroInfo (empty id indicates not found)
}

// ============================================================================
// URL Resolution
// ============================================================================

QString LinuxDistroCatalog::resolveDownloadUrl(const DistroInfo& distro) const
{
    if (distro.sourceType == SourceType::GitHubRelease) {
        // Use cached GitHub asset URL if available
        auto it = m_githubAssetUrls.find(distro.id);
        if (it != m_githubAssetUrls.end()) {
            return *it;
        }
        // Fall back to constructing URL from known version for Ventoy
        if (!distro.fileName.isEmpty()) {
            QString base;
            if (!distro.githubOwner.isEmpty() && !distro.githubRepo.isEmpty()) {
                base = QString("https://github.com/%1/%2/releases/download/v%3/%4")
                    .arg(distro.githubOwner, distro.githubRepo, distro.version,
                         substituteVersion(distro.fileName, distro.version));
            }
            return base;
        }
        return {};
    }

    return substituteVersion(distro.downloadUrl, distro.version);
}

QString LinuxDistroCatalog::resolveChecksumUrl(const DistroInfo& distro) const
{
    if (distro.checksumUrl.isEmpty()) {
        return {};
    }
    return substituteVersion(distro.checksumUrl, distro.version);
}

QString LinuxDistroCatalog::resolveFileName(const DistroInfo& distro) const
{
    // For GitHub releases, check cached filename first
    if (distro.sourceType == SourceType::GitHubRelease) {
        auto url = m_githubAssetUrls.find(distro.id);
        if (url != m_githubAssetUrls.end()) {
            // Extract filename from URL
            QString path = QUrl(*url).path();
            int lastSlash = path.lastIndexOf('/');
            if (lastSlash >= 0) {
                return path.mid(lastSlash + 1);
            }
        }
        // Fall back to pattern-based filename
        if (!distro.fileName.isEmpty()) {
            return substituteVersion(distro.fileName, distro.version);
        }
        return distro.id + ".iso";
    }

    return substituteVersion(distro.fileName, distro.version);
}

QString LinuxDistroCatalog::substituteVersion(const QString& pattern,
                                              const QString& version) const
{
    QString result = pattern;
    result.replace("{version}", version);
    return result;
}

// ============================================================================
// GitHub Version Discovery
// ============================================================================

void LinuxDistroCatalog::checkLatestVersion(const QString& distroId)
{
    auto it = m_distroIndex.find(distroId);
    if (it == m_distroIndex.end()) {
        Q_EMIT versionCheckFailed(distroId, "Unknown distro ID: " + distroId);
        return;
    }

    const auto& distro = m_distros[*it];
    if (distro.sourceType != SourceType::GitHubRelease) {
        // Non-GitHub distros don't need version checking
        Q_EMIT versionCheckCompleted(distroId, distro, false);
        return;
    }

    QString apiUrl = QString("https://api.github.com/repos/%1/%2/releases/latest")
                         .arg(distro.githubOwner, distro.githubRepo);

    QUrl requestUrl(apiUrl);
    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "SAK-Utility/1.0");

    auto* reply = m_networkManager->get(request);
    reply->setProperty("distroId", distroId);
    m_pendingReplies.append(reply);

    connect(reply, &QNetworkReply::finished,
            this, &LinuxDistroCatalog::onGitHubReleaseReply);

    sak::log_info("Checking latest version for " + distroId.toStdString() +
                  " via GitHub API");
}

void LinuxDistroCatalog::onGitHubReleaseReply()
{
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    QString distroId = reply->property("distroId").toString();

    if (reply->error() != QNetworkReply::NoError) {
        QString error = QString("GitHub API error: %1").arg(reply->errorString());
        sak::log_warning(error.toStdString());
        Q_EMIT versionCheckFailed(distroId, error);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        QString error = "Failed to parse GitHub API response: " + parseError.errorString();
        sak::log_warning(error.toStdString());
        Q_EMIT versionCheckFailed(distroId, error);
        return;
    }

    parseGitHubRelease(distroId, doc.object());
}

void LinuxDistroCatalog::parseGitHubRelease(const QString& distroId,
                                            const QJsonObject& release)
{
    auto it = m_distroIndex.find(distroId);
    if (it == m_distroIndex.end()) return;

    DistroInfo& distro = m_distros[*it];
    QString oldVersion = distro.version;

    // Extract version from tag_name
    QString tagName = release["tag_name"].toString();
    if (tagName.isEmpty()) {
        Q_EMIT versionCheckFailed(distroId, "GitHub release has no tag_name");
        return;
    }

    // Update version (strip leading 'v' if present for display)
    distro.version = tagName;

    // Find matching asset by pattern
    QJsonArray assets = release["assets"].toArray();
    QRegularExpression assetRegex(distro.githubAssetPattern,
                                 QRegularExpression::CaseInsensitiveOption);

    QString matchedUrl;
    qint64 matchedSize = 0;
    QString matchedName;

    for (const auto& assetVal : assets) {
        QJsonObject asset = assetVal.toObject();
        QString name = asset["name"].toString();

        if (assetRegex.match(name).hasMatch()) {
            matchedUrl = asset["browser_download_url"].toString();
            matchedSize = asset["size"].toInteger();
            matchedName = name;
            break;
        }
    }

    if (matchedUrl.isEmpty()) {
        sak::log_warning("No matching asset found for " + distroId.toStdString() +
                        " with pattern: " + distro.githubAssetPattern.toStdString());
        Q_EMIT versionCheckFailed(distroId,
            "No matching ISO asset found in latest GitHub release");
        return;
    }

    // Cache the resolved URL and size
    m_githubAssetUrls[distroId] = matchedUrl;
    m_githubAssetSizes[distroId] = matchedSize;

    // Update approximate size if GitHub reports it
    if (matchedSize > 0) {
        distro.approximateSize = matchedSize;
    }

    // Look for checksum sidecar (e.g., .sha256, .sha1)
    for (const auto& assetVal : assets) {
        QJsonObject asset = assetVal.toObject();
        QString name = asset["name"].toString();

        if (name == matchedName + ".sha256" || name == matchedName + ".sha1") {
            m_githubAssetUrls[distroId + "_checksum"] =
                asset["browser_download_url"].toString();
            break;
        }
    }

    bool changed = (oldVersion != distro.version);
    sak::log_info("Version check for " + distroId.toStdString() + ": " +
                  tagName.toStdString() +
                  (changed ? " (UPDATED)" : " (unchanged)") +
                  " asset: " + matchedName.toStdString());

    Q_EMIT versionCheckCompleted(distroId, distro, changed);
}

// ============================================================================
// Cancel
// ============================================================================

void LinuxDistroCatalog::cancelAll()
{
    for (auto* reply : m_pendingReplies) {
        reply->abort();
        reply->deleteLater();
    }
    m_pendingReplies.clear();
}
