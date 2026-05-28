// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

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

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace {

using DistroInfo = LinuxDistroCatalog::DistroInfo;
using DistroCategory = LinuxDistroCatalog::Category;
using DistroSourceType = LinuxDistroCatalog::SourceType;

constexpr double kUbuntuDesktopSizeGiB = 6.1;
constexpr double kUbuntuServerSizeGiB = 2.7;
constexpr double kLinuxMintSizeGiB = 2.9;
constexpr double kFedoraWorkstationSizeGiB = 2.7;
constexpr double kDebianLiveSizeGiB = 3.6;
constexpr double kArchLinuxSizeGiB = 1.4;
constexpr qint64 kClonezillaSizeMiB = 530;
constexpr qint64 kGpartedSizeMiB = 649;
constexpr qint64 kShredOsSizeMiB = 900;
constexpr double kKaliSizeGiB = 4.4;
constexpr double kSystemRescueSizeGiB = 1.3;
constexpr qint64 kVentoySizeMiB = 196;

qint64 sizeFromGiB(double gib) {
    return static_cast<qint64>(gib * sak::kBytesPerGBf);
}

qint64 sizeFromMiB(qint64 mib) {
    return mib * sak::kBytesPerMB;
}

DistroInfo ubuntuDesktopDistro() {
    return {/*.id =*/"ubuntu-desktop",
            /*.name =*/"Ubuntu Desktop",
            /*.version =*/"26.04",
            /*.versionLabel =*/"Resolute Raccoon (LTS)",
            /*.description =*/
            "The most popular Linux desktop. Full graphical environment "
            "with office suite, web browser, and media tools. Ideal for "
            "setting up client workstations.",
            /*.category =*/DistroCategory::GeneralPurpose,
            /*.sourceType =*/DistroSourceType::DirectURL,
            /*.downloadUrl =*/
            "https://releases.ubuntu.com/resolute/"
            "ubuntu-{version}-desktop-amd64.iso",
            /*.checksumUrl =*/"https://releases.ubuntu.com/resolute/SHA256SUMS",
            /*.checksumType =*/"sha256",
            /*.fileName =*/"ubuntu-{version}-desktop-amd64.iso",
            /*.approximateSize =*/sizeFromGiB(kUbuntuDesktopSizeGiB),
            /*.homepage =*/"https://ubuntu.com/desktop",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo ubuntuServerDistro() {
    return {/*.id =*/"ubuntu-server",
            /*.name =*/"Ubuntu Server",
            /*.version =*/"26.04",
            /*.versionLabel =*/"Resolute Raccoon (LTS)",
            /*.description =*/
            "Minimal server installation with no GUI. Ideal for deploying "
            "headless servers, VMs, and containers. Includes OpenSSH, LVM, "
            "and cloud-init.",
            /*.category =*/DistroCategory::GeneralPurpose,
            /*.sourceType =*/DistroSourceType::DirectURL,
            /*.downloadUrl =*/
            "https://releases.ubuntu.com/resolute/"
            "ubuntu-{version}-live-server-amd64.iso",
            /*.checksumUrl =*/"https://releases.ubuntu.com/resolute/SHA256SUMS",
            /*.checksumType =*/"sha256",
            /*.fileName =*/"ubuntu-{version}-live-server-amd64.iso",
            /*.approximateSize =*/sizeFromGiB(kUbuntuServerSizeGiB),
            /*.homepage =*/"https://ubuntu.com/server",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo linuxMintDistro() {
    return {/*.id =*/"linuxmint-cinnamon",
            /*.name =*/"Linux Mint Cinnamon",
            /*.version =*/"22.3",
            /*.versionLabel =*/"Zena",
            /*.description =*/
            "Sleek, modern desktop based on Ubuntu LTS. Familiar Windows-like "
            "interface -- excellent for migrating users from Windows. Includes "
            "full multimedia codecs.",
            /*.category =*/DistroCategory::GeneralPurpose,
            /*.sourceType =*/DistroSourceType::DirectURL,
            /*.downloadUrl =*/
            "https://mirrors.kernel.org/linuxmint/stable/{version}/"
            "linuxmint-{version}-cinnamon-64bit.iso",
            /*.checksumUrl =*/"https://mirrors.kernel.org/linuxmint/stable/{version}/sha256sum.txt",
            /*.checksumType =*/"sha256",
            /*.fileName =*/"linuxmint-{version}-cinnamon-64bit.iso",
            /*.approximateSize =*/sizeFromGiB(kLinuxMintSizeGiB),
            /*.homepage =*/"https://linuxmint.com",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo fedoraWorkstationDistro() {
    return {/*.id =*/"fedora-workstation",
            /*.name =*/"Fedora Workstation",
            /*.version =*/"44",
            /*.versionLabel =*/"Workstation Live 1.7",
            /*.description =*/
            "Modern GNOME desktop from the Fedora Project. Good for testing "
            "current Linux desktop workflows and hardware support.",
            /*.category =*/DistroCategory::GeneralPurpose,
            /*.sourceType =*/DistroSourceType::DirectURL,
            /*.downloadUrl =*/
            "https://download.fedoraproject.org/pub/fedora/linux/releases/{version}/"
            "Workstation/x86_64/iso/Fedora-Workstation-Live-{version}-1.7.x86_64.iso",
            /*.checksumUrl =*/
            "https://download.fedoraproject.org/pub/fedora/linux/releases/{version}/"
            "Workstation/x86_64/iso/Fedora-Workstation-{version}-1.7-x86_64-CHECKSUM",
            /*.checksumType =*/"sha256",
            /*.fileName =*/"Fedora-Workstation-Live-{version}-1.7.x86_64.iso",
            /*.approximateSize =*/sizeFromGiB(kFedoraWorkstationSizeGiB),
            /*.homepage =*/"https://fedoraproject.org/workstation/",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo debianLiveDistro() {
    return {/*.id =*/"debian-live-gnome",
            /*.name =*/"Debian Live GNOME",
            /*.version =*/"13.5.0",
            /*.versionLabel =*/"Trixie live",
            /*.description =*/
            "Official Debian live desktop image with GNOME and Calamares "
            "installer. Useful for Linux troubleshooting and installs from USB.",
            /*.category =*/DistroCategory::GeneralPurpose,
            /*.sourceType =*/DistroSourceType::DirectURL,
            /*.downloadUrl =*/
            "https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/"
            "debian-live-{version}-amd64-gnome.iso",
            /*.checksumUrl =*/
            "https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/SHA256SUMS",
            /*.checksumType =*/"sha256",
            /*.fileName =*/"debian-live-{version}-amd64-gnome.iso",
            /*.approximateSize =*/sizeFromGiB(kDebianLiveSizeGiB),
            /*.homepage =*/"https://www.debian.org/distrib/",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo archLinuxDistro() {
    return {/*.id =*/"arch-linux",
            /*.name =*/"Arch Linux",
            /*.version =*/"2026.05.01",
            /*.versionLabel =*/"Rolling release install ISO",
            /*.description =*/
            "Rolling-release Linux install image for advanced users and "
            "technicians who need a minimal, current base system.",
            /*.category =*/DistroCategory::GeneralPurpose,
            /*.sourceType =*/DistroSourceType::DirectURL,
            /*.downloadUrl =*/"https://geo.mirror.pkgbuild.com/iso/latest/archlinux-x86_64.iso",
            /*.checksumUrl =*/"https://geo.mirror.pkgbuild.com/iso/latest/sha256sums.txt",
            /*.checksumType =*/"sha256",
            /*.fileName =*/"archlinux-x86_64.iso",
            /*.approximateSize =*/sizeFromGiB(kArchLinuxSizeGiB),
            /*.homepage =*/"https://archlinux.org/download/",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo clonezillaDistro() {
    return {/*.id =*/"clonezilla",
            /*.name =*/"Clonezilla Live",
            /*.version =*/"3.3.1-35",
            /*.versionLabel =*/"Stable (Debian-based)",
            /*.description =*/
            "Disk imaging and cloning tool. Clone entire drives or partitions "
            "for backup, migration, or mass deployment. Supports multicasting "
            "for deploying to many machines simultaneously.",
            /*.category =*/DistroCategory::DiskTools,
            /*.sourceType =*/DistroSourceType::SourceForge,
            /*.downloadUrl =*/
            "https://sourceforge.net/projects/clonezilla/files/"
            "clonezilla_live_stable/{version}/"
            "clonezilla-live-{version}-amd64.iso/download",
            /*.checksumUrl =*/{},
            /*.checksumType =*/{},
            /*.fileName =*/"clonezilla-live-{version}-amd64.iso",
            /*.approximateSize =*/sizeFromMiB(kClonezillaSizeMiB),
            /*.homepage =*/"https://clonezilla.org",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo gpartedDistro() {
    return {/*.id =*/"gparted-live",
            /*.name =*/"GParted Live",
            /*.version =*/"1.8.1-3",
            /*.versionLabel =*/{},
            /*.description =*/
            "Bootable partition editor for creating, resizing, moving, and "
            "copying disk partitions. Supports ext2/3/4, NTFS, FAT16/32, "
            "HFS+, XFS, and many more filesystems.",
            /*.category =*/DistroCategory::DiskTools,
            /*.sourceType =*/DistroSourceType::SourceForge,
            /*.downloadUrl =*/
            "https://sourceforge.net/projects/gparted/files/"
            "gparted-live-stable/{version}/"
            "gparted-live-{version}-amd64.iso/download",
            /*.checksumUrl =*/{},
            /*.checksumType =*/{},
            /*.fileName =*/"gparted-live-{version}-amd64.iso",
            /*.approximateSize =*/sizeFromMiB(kGpartedSizeMiB),
            /*.homepage =*/"https://gparted.org",
            /*.githubOwner =*/{},
            /*.githubRepo =*/{},
            /*.githubAssetPattern =*/{}};
}

DistroInfo shredOsDistro() {
    return {/*.id =*/"shredos",
            /*.name =*/"ShredOS",
            /*.version =*/"v2025.11_30_x86-64_0.41",
            /*.versionLabel =*/"nwipe 0.41",
            /*.description =*/
            "Bootable secure disk erasure tool powered by nwipe. Wipes drives "
            "using DoD 5220.22-M, Gutmann, and other standards. Generates "
            "PDF audit reports for compliance. Essential for ITAD.",
            /*.category =*/DistroCategory::DiskTools,
            /*.sourceType =*/DistroSourceType::GitHubRelease,
            /*.downloadUrl =*/{},
            /*.checksumUrl =*/{},
            /*.checksumType =*/"sha1",
            /*.fileName =*/{},
            /*.approximateSize =*/sizeFromMiB(kShredOsSizeMiB),
            /*.homepage =*/"https://github.com/PartialVolume/shredos.x86_64",
            /*.githubOwner =*/"PartialVolume",
            /*.githubRepo =*/"shredos.x86_64",
            /*.githubAssetPattern =*/R"(shredos.*x86-64.*\.iso$)"};
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

LinuxDistroCatalog::LinuxDistroCatalog(QObject* parent)
    : QObject(parent), m_networkManager(new QNetworkAccessManager(this)) {
    populateCatalog();
    sak::logInfo("LinuxDistroCatalog initialized with " + std::to_string(m_distros.size()) +
                 " distributions");
}

LinuxDistroCatalog::~LinuxDistroCatalog() {
    cancelAll();
}

// ============================================================================
// Catalog Population
// ============================================================================

void LinuxDistroCatalog::populateCatalog() {
    addGeneralPurposeDistros();
    addSecurityDistros();
    addSystemRecoveryDistros();
    addDiskToolDistros();
    addUtilityDistros();
}

void LinuxDistroCatalog::addGeneralPurposeDistros() {
    addDistro(ubuntuDesktopDistro());
    addDistro(ubuntuServerDistro());
    addDistro(linuxMintDistro());
    addDistro(fedoraWorkstationDistro());
    addDistro(debianLiveDistro());
    addDistro(archLinuxDistro());
}

void LinuxDistroCatalog::addSecurityDistros() {
    addDistro({/*.id =*/"kali-linux",
               /*.name =*/"Kali Linux",
               /*.version =*/"2026.1",
               /*.versionLabel =*/"Installer",
               /*.description =*/
               "The most advanced penetration testing distribution. Includes "
               "600+ security tools for network analysis, vulnerability "
               "assessment, and forensics. Essential for security audits.",
               /*.category =*/Category::Security,
               /*.sourceType =*/SourceType::DirectURL,
               /*.downloadUrl =
                */
               "https://cdimage.kali.org/current/"
               "kali-linux-{version}-installer-amd64.iso",
               /*.checksumUrl =*/"https://cdimage.kali.org/current/SHA256SUMS",
               /*.checksumType =*/"sha256",
               /*.fileName =*/"kali-linux-{version}-installer-amd64.iso",
               /*.approximateSize =*/sizeFromGiB(kKaliSizeGiB),  // ~4.4 GB
               /*.homepage =*/"https://www.kali.org",
               /*.githubOwner =*/{},
               /*.githubRepo =*/{},
               /*.githubAssetPattern =*/{}});
}

void LinuxDistroCatalog::addSystemRecoveryDistros() {
    addDistro({/*.id =*/"systemrescue",
               /*.name =*/"SystemRescue",
               /*.version =*/"13.00",
               /*.versionLabel =*/{},
               /*.description =*/
               "Bootable Linux rescue environment for repairing unbootable "
               "systems. "
               "Includes filesystem tools (fsck, ntfsfix), network tools, "
               "partition editors, and data recovery utilities.",
               /*.category =*/Category::SystemRecovery,
               /*.sourceType =*/SourceType::SourceForge,
               /*.downloadUrl =
                */
               "https://sourceforge.net/projects/"
               "systemrescuecd/files/sysresccd-x86/"
               "{version}/systemrescue-{version}-"
               "amd64.iso/download",
               /*.checksumUrl =*/{},  // SourceForge provides checksums on download page
               /*.checksumType =*/{},
               /*.fileName =*/"systemrescue-{version}-amd64.iso",
               /*.approximateSize =*/sizeFromGiB(kSystemRescueSizeGiB),
               /*.homepage =*/"https://www.system-rescue.org",
               /*.githubOwner =*/{},
               /*.githubRepo =*/{},
               /*.githubAssetPattern =*/{}});
}

void LinuxDistroCatalog::addDiskToolDistros() {
    addDistro(clonezillaDistro());
    addDistro(gpartedDistro());
    addDistro(shredOsDistro());
}

void LinuxDistroCatalog::addUtilityDistros() {
    addDistro({/*.id =*/"ventoy",
               /*.name =*/"Ventoy LiveCD",
               /*.version =*/"1.1.12",
               /*.versionLabel =*/{},
               /*.description =*/
               "Multi-boot USB solution. Install Ventoy to a USB drive, then "
               "simply "
               "copy ISO files to the drive -- no reformatting needed. Supports "
               "110+ OS types including Windows, Linux, and WinPE.",
               /*.category =*/Category::Utilities,
               /*.sourceType =*/SourceType::GitHubRelease,
               /*.downloadUrl =*/{},  // Resolved via GitHub Releases API
               /*.checksumUrl =*/{},  // SHA256 in release body
               /*.checksumType =*/"sha256",
               /*.fileName =*/"ventoy-{version}-livecd.iso",
               /*.approximateSize =*/sizeFromMiB(kVentoySizeMiB),  // ~196 MB
               /*.homepage =*/"https://www.ventoy.net",
               /*.githubOwner =*/"ventoy",
               /*.githubRepo =*/"Ventoy",
               /*.githubAssetPattern =*/R"(ventoy-.*-livecd\.iso$)"});
}

void LinuxDistroCatalog::addDistro(const DistroInfo& distro) {
    m_distroIndex[distro.id] = m_distros.size();
    m_distros.append(distro);
}

// ============================================================================
// Catalog Queries
// ============================================================================

QList<LinuxDistroCatalog::DistroInfo> LinuxDistroCatalog::allDistros() const {
    return m_distros;
}

QList<LinuxDistroCatalog::DistroInfo> LinuxDistroCatalog::distrosByCategory(
    Category category) const {
    QList<DistroInfo> result;
    for (const auto& distro : m_distros) {
        if (distro.category == category) {
            result.append(distro);
        }
    }
    return result;
}

QMap<LinuxDistroCatalog::Category, QString> LinuxDistroCatalog::categoryNames() {
    return {{Category::GeneralPurpose, "General Purpose"},
            {Category::Security, "Security && Pen-Testing"},
            {Category::SystemRecovery, "System Recovery"},
            {Category::DiskTools, "Disk Tools"},
            {Category::Utilities, "Utilities"}};
}

LinuxDistroCatalog::DistroInfo LinuxDistroCatalog::distroById(const QString& id) const {
    auto it = m_distroIndex.find(id);
    if (it != m_distroIndex.end() && *it >= 0 && *it < m_distros.size()) {
        return m_distros[*it];
    }
    return {};  // Return empty DistroInfo (empty id indicates not found)
}

// ============================================================================
// URL Resolution
// ============================================================================

QString LinuxDistroCatalog::resolveDownloadUrl(const DistroInfo& distro) const {
    if (distro.sourceType == SourceType::GitHubRelease) {
        auto it = m_githubAssetUrls.find(distro.id);
        if (it != m_githubAssetUrls.end()) {
            return *it;
        }
        return {};
    }

    return substituteVersion(distro.downloadUrl, distro.version);
}

QString LinuxDistroCatalog::resolveChecksumUrl(const DistroInfo& distro) const {
    if (distro.checksumUrl.isEmpty()) {
        return {};
    }
    return substituteVersion(distro.checksumUrl, distro.version);
}

QString LinuxDistroCatalog::resolveFileName(const DistroInfo& distro) const {
    if (distro.sourceType == SourceType::GitHubRelease) {
        auto url = m_githubAssetUrls.find(distro.id);
        if (url != m_githubAssetUrls.end()) {
            QString path = QUrl(*url).path();
            int lastSlash = path.lastIndexOf('/');
            if (lastSlash >= 0) {
                return path.mid(lastSlash + 1);
            }
        }
        if (!distro.fileName.isEmpty()) {
            return substituteVersion(distro.fileName, distro.version);
        }
        return distro.id + ".iso";
    }

    return substituteVersion(distro.fileName, distro.version);
}

QString LinuxDistroCatalog::substituteVersion(const QString& pattern,
                                              const QString& version) const {
    QString result = pattern;
    result.replace("{version}", version);
    return result;
}

// ============================================================================
// GitHub Version Discovery
// ============================================================================

void LinuxDistroCatalog::checkLatestVersion(const QString& distroId) {
    Q_ASSERT(!distroId.isEmpty());
    Q_ASSERT(m_networkManager);
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

    connect(reply, &QNetworkReply::finished, this, &LinuxDistroCatalog::onGitHubReleaseReply);

    sak::logInfo("Checking latest version for " + distroId.toStdString() + " via GitHub API");
}

void LinuxDistroCatalog::onGitHubReleaseReply() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    QString distroId = reply->property("distroId").toString();

    if (reply->error() != QNetworkReply::NoError) {
        QString error = QString("GitHub API error: %1").arg(reply->errorString());
        sak::logWarning(error.toStdString());
        Q_EMIT versionCheckFailed(distroId, error);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        QString error = "Failed to parse GitHub API response: " + parseError.errorString();
        sak::logWarning(error.toStdString());
        Q_EMIT versionCheckFailed(distroId, error);
        return;
    }

    parseGitHubRelease(distroId, doc.object());
}

void LinuxDistroCatalog::parseGitHubRelease(const QString& distroId, const QJsonObject& release) {
    Q_ASSERT(!distroId.isEmpty());
    Q_ASSERT(!release.isEmpty());
    auto it = m_distroIndex.find(distroId);
    if (it == m_distroIndex.end()) {
        return;
    }

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

    QJsonArray assets = release["assets"].toArray();
    QString matchedName;
    if (!resolveGitHubAsset(distroId, distro, assets, matchedName)) {
        sak::logWarning("No matching asset found for " + distroId.toStdString() +
                        " with pattern: " + distro.githubAssetPattern.toStdString());
        Q_EMIT versionCheckFailed(distroId, "No matching ISO asset found in latest GitHub release");
        return;
    }

    bool changed = (oldVersion != distro.version);
    sak::logInfo("Version check for " + distroId.toStdString() + ": " + tagName.toStdString() +
                 (changed ? " (UPDATED)" : " (unchanged)") +
                 " asset: " + matchedName.toStdString());

    Q_EMIT versionCheckCompleted(distroId, distro, changed);
}

bool LinuxDistroCatalog::resolveGitHubAsset(const QString& distroId,
                                            DistroInfo& distro,
                                            const QJsonArray& assets,
                                            QString& matchedName) {
    QRegularExpression assetRegex(distro.githubAssetPattern,
                                  QRegularExpression::CaseInsensitiveOption);
    QString matchedUrl;
    qint64 matchedSize = 0;

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
        return false;
    }

    // Cache the resolved URL and size
    m_githubAssetUrls[distroId] = matchedUrl;
    m_githubAssetSizes[distroId] = matchedSize;
    if (matchedSize > 0) {
        distro.approximateSize = matchedSize;
    }

    // Look for checksum sidecar (e.g., .sha256, .sha1)
    cacheChecksumSidecar(distroId, matchedName, assets);

    return true;
}

void LinuxDistroCatalog::cacheChecksumSidecar(const QString& distroId,
                                              const QString& matchedName,
                                              const QJsonArray& assets) {
    for (const auto& assetVal : assets) {
        QJsonObject asset = assetVal.toObject();
        QString name = asset["name"].toString();
        if (name == matchedName + ".sha256" || name == matchedName + ".sha1") {
            m_githubAssetUrls[distroId + "_checksum"] = asset["browser_download_url"].toString();
            break;
        }
    }
}

// ============================================================================
// Cancel
// ============================================================================

void LinuxDistroCatalog::cancelAll() {
    for (auto* reply : m_pendingReplies) {
        reply->abort();
        reply->deleteLater();
    }
    m_pendingReplies.clear();
}
