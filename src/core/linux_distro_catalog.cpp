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

// Upper bound on an untrusted GitHub releases/latest JSON body. A real response is a few
// hundred KiB at most; anything past this cap is aborted/rejected rather than buffered and
// parsed unbounded (fail closed on a hostile or MITM'd response).
constexpr qint64 kMaxGitHubReleaseBytes = 8LL * 1024 * 1024;

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

// An entry with an empty checksumUrl/checksumType publishes no per-release checksum this
// catalog can pin. LinuxISODownloader::requirePinnedChecksum refuses to download such an
// entry: the SourceForge download URL redirects onto mirrors that may serve the ISO over
// plain HTTP, and without a checksum fetched over HTTPS there is nothing to verify the ISO
// against before it is written to bootable media. Adding a verified per-release checksum URL
// re-enables the entry; guessing one would defeat the check it is supposed to provide.
//
// Clonezilla Live is the one entry with no pinnable checksum. Verified against upstream:
//   - the SourceForge release directory that serves the ISO
//     (clonezilla_live_stable/{version}/) contains ONLY the .iso and .zip; CHECKSUMS.TXT and
//     SHA256SUMS both 404 there;
//   - the project's own mirror publishes CHECKSUMS.TXT/SHA256SUMS, but the path is not stable
//     across a release's lifetime: the current release sits in clonezilla-live/stable/ (whose
//     SHA256SUMS lists only that one version) and moves to clonezilla-live/old/{version}/ once
//     superseded, and older per-version directories are pruned empty.
// No single {version}-templated URL therefore addresses a given release for its whole life, so
// this entry stays refused rather than carrying a URL that resolves for a while and then rots.
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
            /*.checksumUrl =*/{},  // See the note above: no lifetime-stable per-release URL
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
            // GParted ships no .sha256 sidecar in the release directory; it publishes the
            // per-release digests inside the release note that sits in that SAME directory,
            // as standard "<sha256>  <filename>" lines. That file is {version}-templated like
            // the ISO beside it, so it addresses one specific release for good and never
            // rots. parseExpectedHash reads the digest out of it by filename.
            /*.checksumUrl =*/
            "https://downloads.sourceforge.net/project/gparted/gparted-live-stable/"
            "{version}/gparted-live-{version}-README.md",
            /*.checksumType =*/"sha256",
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
            // Resolved from the release's .sha1 sidecar asset by cacheChecksumSidecar; a
            // release that ships no sidecar has nothing to pin and the download is refused.
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
               // SystemRescue keeps a per-release .sha256 on its OWN site under
               // /releases/{version}/, retained for superseded releases as well as the current
               // one. Deliberately preferred over the sidecar in the SourceForge release
               // directory: the ISO arrives through the SourceForge mirror network, so taking
               // the digest from the project's own host makes the two legs independent instead
               // of trusting one mirror to attest to itself.
               /*.checksumUrl =*/
               "https://www.system-rescue.org/releases/{version}/"
               "systemrescue-{version}-amd64.iso.sha256",
               /*.checksumType =*/"sha256",
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
               // Resolved from the release's .sha256 sidecar asset by cacheChecksumSidecar; a
               // release that ships no sidecar has nothing to pin and the download is refused.
               /*.checksumUrl =*/{},
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
    if (distro.sourceType == SourceType::GitHubRelease) {
        // A GitHub release's checksum sidecar is cached under "<id>_checksum" as a full
        // browser_download_url; return it verbatim (no version substitution). Fall through when
        // no sidecar exists so a static checksumUrl still works and no-sidecar distros are
        // unaffected.
        auto it = m_githubAssetUrls.find(distro.id + "_checksum");
        if (it != m_githubAssetUrls.end()) {
            return *it;
        }
    }
    if (distro.checksumUrl.isEmpty()) {
        return {};
    }
    return substituteVersion(distro.checksumUrl, distro.version);
}

QString LinuxDistroCatalog::resolveFileName(const DistroInfo& distro) const {
    if (distro.sourceType == SourceType::GitHubRelease) {
        auto url = m_githubAssetUrls.find(distro.id);
        if (url != m_githubAssetUrls.end()) {
            const QString path = QUrl(*url).path();
            const int lastSlash = path.lastIndexOf('/');
            if (lastSlash >= 0) {
                return path.mid(lastSlash + 1);
            }
        }
        if (!distro.fileName.isEmpty()) {
            return substituteVersion(distro.fileName, distro.version);
        }
        // No resolved asset and no static template: do not fabricate an "<id>.iso" guess.
        // The download is already refused (resolveDownloadUrl returns empty here), so fail
        // closed with an empty name rather than inventing one.
        return {};
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
    // The constructor creates m_networkManager and nothing reassigns it. An empty or unknown
    // distroId is rejected below by the index lookup.
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

    const QString apiUrl = QString("https://api.github.com/repos/%1/%2/releases/latest")
                               .arg(distro.githubOwner, distro.githubRepo);

    const QUrl requestUrl(apiUrl);
    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "SAK-Utility/1.0");

    auto* reply = m_networkManager->get(request);
    reply->setProperty("distroId", distroId);
    m_pendingReplies.append(reply);

    connect(reply, &QNetworkReply::finished, this, &LinuxDistroCatalog::onGitHubReleaseReply);
    // Bound the untrusted response as it arrives: abort once it runs past the cap instead of
    // letting the reply buffer an arbitrarily large body. abort() delivers finished() with an
    // error, which onGitHubReleaseReply reports as a failed version check (fail closed).
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > kMaxGitHubReleaseBytes) {
            reply->abort();
        }
    });

    sak::logInfo("Checking latest version for " + distroId.toStdString() + " via GitHub API");
}

void LinuxDistroCatalog::onGitHubReleaseReply() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    const QString distroId = reply->property("distroId").toString();

    if (reply->error() != QNetworkReply::NoError) {
        const QString error = QString("GitHub API error: %1").arg(reply->errorString());
        sak::logWarning(error.toStdString());
        Q_EMIT versionCheckFailed(distroId, error);
        return;
    }

    const QByteArray data = reply->readAll();
    if (data.size() > kMaxGitHubReleaseBytes) {
        const QString error = QStringLiteral("GitHub API response exceeds size cap");
        sak::logWarning(error.toStdString());
        Q_EMIT versionCheckFailed(distroId, error);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        const QString error = "Failed to parse GitHub API response: " + parseError.errorString();
        sak::logWarning(error.toStdString());
        Q_EMIT versionCheckFailed(distroId, error);
        return;
    }

    parseGitHubRelease(distroId, doc.object());
}

void LinuxDistroCatalog::parseGitHubRelease(const QString& distroId, const QJsonObject& release) {
    auto it = m_distroIndex.find(distroId);
    if (it == m_distroIndex.end()) {
        return;
    }

    DistroInfo& distro = m_distros[*it];
    const QString oldVersion = distro.version;

    // Extract version from tag_name
    const QString tagName = release["tag_name"].toString();
    if (tagName.isEmpty()) {
        Q_EMIT versionCheckFailed(distroId, "GitHub release has no tag_name");
        return;
    }

    // Resolve the asset BEFORE advancing the version. resolveGitHubAsset returns false without
    // mutating anything when no asset matches, so a failed resolution leaves the entry wholly
    // unchanged. Committing the version first would pair the new version with a prior release's
    // cached asset URL/size/checksum (partial-mutation / stale-asset).
    const QJsonArray assets = release["assets"].toArray();
    QString matchedName;
    if (!resolveGitHubAsset(distroId, distro, assets, matchedName)) {
        sak::logWarning("No matching asset found for " + distroId.toStdString() +
                        " with pattern: " + distro.githubAssetPattern.toStdString());
        Q_EMIT versionCheckFailed(distroId, "No matching ISO asset found in latest GitHub release");
        return;
    }

    // A matching asset is now pinned; commit the new version.
    distro.version = tagName;
    const bool changed = (oldVersion != distro.version);
    sak::logInfo("Version check for " + distroId.toStdString() + ": " + tagName.toStdString() +
                 (changed ? " (UPDATED)" : " (unchanged)") +
                 " asset: " + matchedName.toStdString());

    Q_EMIT versionCheckCompleted(distroId, distro, changed);
}

bool LinuxDistroCatalog::resolveGitHubAsset(const QString& distroId,
                                            DistroInfo& distro,
                                            const QJsonArray& assets,
                                            QString& matchedName) {
    const QRegularExpression assetRegex(distro.githubAssetPattern,
                                        QRegularExpression::CaseInsensitiveOption);
    QString matchedUrl;
    qint64 matchedSize = 0;

    for (const auto& assetVal : assets) {
        QJsonObject asset = assetVal.toObject();
        const QString name = asset["name"].toString();
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

    // The asset is downloaded and written to bootable media, so refuse anything that is not an
    // HTTPS URL: an http:// or malformed browser_download_url from a tampered/MITM'd API
    // response must never be cached as an official asset. Fail closed on a bad scheme.
    if (QUrl(matchedUrl).scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
        sak::logWarning("Rejected non-HTTPS asset URL for " + distroId.toStdString());
        return false;
    }

    // Cache the resolved URL and size
    m_githubAssetUrls[distroId] = matchedUrl;
    m_githubAssetSizes[distroId] = matchedSize;
    if (matchedSize > 0) {
        distro.approximateSize = matchedSize;
    }

    // Look for a checksum sidecar for the declared algorithm (.sha256 / .sha1)
    cacheChecksumSidecar(distroId, matchedName, distro.checksumType, assets);

    return true;
}

void LinuxDistroCatalog::cacheChecksumSidecar(const QString& distroId,
                                              const QString& matchedName,
                                              const QString& checksumType,
                                              const QJsonArray& assets) {
    // A newly resolved release may ship no checksum sidecar. Drop any URL cached
    // for a PRIOR release first, so we never verify the new asset against an old
    // release's checksum (stale-checksum false verification / spurious mismatch).
    m_githubAssetUrls.remove(distroId + "_checksum");

    // Only accept the sidecar for the algorithm this entry declares. Accepting either
    // .sha256 or .sha1 regardless of checksumType let a differently-hashed sidecar be pinned
    // while verification computes the declared algorithm (algorithm confusion). With no
    // matching sidecar the download stays refused by requirePinnedChecksum (fail closed).
    if (checksumType.isEmpty()) {
        return;
    }
    const QString sidecarName = matchedName + "." + checksumType.toLower();
    for (const auto& assetVal : assets) {
        QJsonObject asset = assetVal.toObject();
        const QString name = asset["name"].toString();
        if (name == sidecarName) {
            const QString url = asset["browser_download_url"].toString();
            // Reject a non-HTTPS or malformed sidecar URL rather than caching it.
            if (QUrl(url).scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
                sak::logWarning("Rejected non-HTTPS checksum sidecar URL for " +
                                distroId.toStdString());
                return;
            }
            m_githubAssetUrls[distroId + "_checksum"] = url;
            break;
        }
    }
}

// ============================================================================
// Cancel
// ============================================================================

void LinuxDistroCatalog::cancelAll() {
    // abort() synchronously delivers finished(), whose slot calls
    // m_pendingReplies.removeOne(reply) -- mutating the list mid-iteration (UB).
    // Snapshot and clear FIRST, then abort from the copy.
    const QList<QNetworkReply*> pending = m_pendingReplies;
    m_pendingReplies.clear();
    for (auto* reply : pending) {
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
    }
}
