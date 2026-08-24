// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_linux_iso_downloader.cpp
 * @brief Comprehensive tests for Linux distro catalog and ISO downloader
 *
 * Tests cover:
 *   - Catalog population and metadata integrity
 *   - URL resolution for all source types (Direct, SourceForge, GitHub)
 *   - Version substitution in URLs and filenames
 *   - GitHub API response parsing
 *   - aria2c argument generation (standard vs SourceForge)
 *   - Checksum verification pipeline
 *   - Error handling and edge cases
 *   - Network-based version checking (skippable)
 */

#include "sak/linux_distro_catalog.h"
#include "sak/linux_iso_downloader.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

class LinuxISODownloaderTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // ========================================================================
    // Catalog Tests
    // ========================================================================

    // Population & completeness
    void testCatalogPopulated();
    void testAllDistrosHaveRequiredFields();
    void testUniqueDistroIds();
    void testAllCategoriesRepresented();
    void testCategoryNames();

    // Lookups
    void testDistroByIdFound();
    void testDistroByIdNotFound();
    void testDistrosByCategory();
    void testDistrosByCategoryCount();

    // URL resolution -- DirectURL
    void testResolveDirectUrl_Ubuntu();
    void testResolveDirectUrl_Kali();

    // URL resolution -- SourceForge
    void testResolveSourceForgeUrl_SystemRescue();
    void testResolveSourceForgeUrl_Clonezilla();
    void testResolveSourceForgeUrl_GParted();

    // URL resolution -- GitHub
    void testResolveGitHubUrl_NoCache();
    void testResolveGitHubUrl_RequiresVersionCheck();

    // Checksum URL resolution
    void testResolveChecksumUrl_Ubuntu();
    void testResolveChecksumUrl_SystemRescue();
    void testResolveChecksumUrl_GParted();
    void testResolveChecksumUrl_NoChecksum_Clonezilla();
    void testEveryPinnedChecksumUrlIsHttpsAndPerRelease();
    void testParseExpectedHash_requiresRealDigest();
    void testChecksumSectionAlgorithm_separatesSameLengthDigests();
    void testBsdRecordDigest_followsAlgorithmAndFileLabels();
    void testResolveChecksumUrl_Fedora();

    // Filename resolution
    void testResolveFileName_Direct();
    void testResolveFileName_SourceForge();
    void testResolveFileName_GitHubTemplate();

    // Version substitution
    void testVersionSubstitution();
    void testVersionSubstitutionNoPlaceholder();

    // ========================================================================
    // GitHub API Parsing Tests
    // ========================================================================
    void testGitHubVersionCheck_Success();
    void testGitHubVersionCheck_NoMatchingAsset();
    void testArchLinuxCatalogEntry();

    // ========================================================================
    // Downloader Tests
    // ========================================================================

    // Construction
    void testDownloaderConstruction();
    void testDownloaderInitialState();

    // Input validation
    void testStartDownload_UnknownDistro();
    void testStartDownload_WhileDownloading();
    void testStartDownload_RefusesReleaseWithoutPinnedChecksum();

    // Phase management
    void testPhaseTransitions();

    // Cancel
    void testCancelFromIdle();

    // Verify-result staleness guard (B10-19)
    void testShouldApplyVerifyResult_gating();

    // ========================================================================
    // Network Tests (require internet -- auto-skip if unreachable)
    // ========================================================================
    void testGitHubVersionCheck_Network();
    void testDirectUrlReachable_Ubuntu();
    void testSourceForgeUrlStructure_AllDistros();

private:
    LinuxDistroCatalog* m_catalog = nullptr;
    LinuxISODownloader* m_downloader = nullptr;
};

// ============================================================================
// Setup / Teardown
// ============================================================================

void LinuxISODownloaderTests::initTestCase() {
    qInfo() << "=== Linux ISO Downloader & Catalog Tests ===";
    m_catalog = new LinuxDistroCatalog(this);
    m_downloader = new LinuxISODownloader(this);
}

void LinuxISODownloaderTests::cleanupTestCase() {
    delete m_downloader;
    m_downloader = nullptr;
    delete m_catalog;
    m_catalog = nullptr;
    qInfo() << "=== All tests completed ===";
}

// ============================================================================
// Catalog: Population & Completeness
// ============================================================================

void LinuxISODownloaderTests::testCatalogPopulated() {
    auto all = m_catalog->allDistros();
    // Should have at least 8 distributions
    // The catalog is a fixed 12 (6 general + 1 security + 1 recovery + 3 disk tools + 1 utility);
    // `>= 8` left four slots of slack, so silently dropping entries would pass.
    QVERIFY2(all.size() == 12, qPrintable(QString("Expected 12 distros, got %1").arg(all.size())));
}

void LinuxISODownloaderTests::testAllDistrosHaveRequiredFields() {
    auto all = m_catalog->allDistros();
    for (const auto& d : all) {
        QVERIFY2(!d.id.isEmpty(), "Distro has empty id");
        QVERIFY2(!d.name.isEmpty(), qPrintable("Distro " + d.id + " has empty name"));
        QVERIFY2(!d.version.isEmpty(), qPrintable("Distro " + d.id + " has empty version"));
        QVERIFY2(!d.description.isEmpty(), qPrintable("Distro " + d.id + " has empty description"));
        QVERIFY2(!d.homepage.isEmpty(), qPrintable("Distro " + d.id + " has empty homepage"));
        QVERIFY2(d.approximateSize > 0, qPrintable("Distro " + d.id + " has zero approximateSize"));

        // DirectURL and SourceForge must have downloadUrl template
        if (d.sourceType == LinuxDistroCatalog::SourceType::DirectURL ||
            d.sourceType == LinuxDistroCatalog::SourceType::SourceForge) {
            QVERIFY2(!d.downloadUrl.isEmpty(),
                     qPrintable("Distro " + d.id + " (non-GitHub) has empty downloadUrl"));
            QVERIFY2(!d.fileName.isEmpty(),
                     qPrintable("Distro " + d.id + " (non-GitHub) has empty fileName"));
        }

        // GitHub distros must have owner/repo
        if (d.sourceType == LinuxDistroCatalog::SourceType::GitHubRelease) {
            QVERIFY2(!d.githubOwner.isEmpty(),
                     qPrintable("Distro " + d.id + " (GitHub) has empty githubOwner"));
            QVERIFY2(!d.githubRepo.isEmpty(),
                     qPrintable("Distro " + d.id + " (GitHub) has empty githubRepo"));
            QVERIFY2(!d.githubAssetPattern.isEmpty(),
                     qPrintable("Distro " + d.id + " (GitHub) has empty githubAssetPattern"));
        }
    }
}

void LinuxISODownloaderTests::testUniqueDistroIds() {
    auto all = m_catalog->allDistros();
    QSet<QString> ids;
    for (const auto& d : all) {
        QVERIFY2(!ids.contains(d.id), qPrintable("Duplicate distro id: " + d.id));
        ids.insert(d.id);
    }
}

void LinuxISODownloaderTests::testAllCategoriesRepresented() {
    using Cat = LinuxDistroCatalog::Category;
    QSet<int> categories;
    for (const auto& d : m_catalog->allDistros()) {
        categories.insert(static_cast<int>(d.category));
    }

    QVERIFY(categories.contains(static_cast<int>(Cat::GeneralPurpose)));
    QVERIFY(categories.contains(static_cast<int>(Cat::Security)));
    QVERIFY(categories.contains(static_cast<int>(Cat::SystemRecovery)));
    QVERIFY(categories.contains(static_cast<int>(Cat::DiskTools)));
    QVERIFY(categories.contains(static_cast<int>(Cat::Utilities)));
}

void LinuxISODownloaderTests::testCategoryNames() {
    auto names = LinuxDistroCatalog::categoryNames();
    // These strings are the rendered tab/section headings, and the Security one carries a
    // DOUBLED ampersand because Qt eats a single '&' as a mnemonic -- a detail no !isEmpty()
    // check could protect, and one that silently renders as "Security  Pen-Testing" if lost.
    QCOMPARE(names.size(), 5);
    QCOMPARE(names[LinuxDistroCatalog::Category::GeneralPurpose], QString("General Purpose"));
    QCOMPARE(names[LinuxDistroCatalog::Category::Security], QString("Security && Pen-Testing"));
    QCOMPARE(names[LinuxDistroCatalog::Category::SystemRecovery], QString("System Recovery"));
    QCOMPARE(names[LinuxDistroCatalog::Category::DiskTools], QString("Disk Tools"));
    QCOMPARE(names[LinuxDistroCatalog::Category::Utilities], QString("Utilities"));
}

// ============================================================================
// Catalog: Lookups
// ============================================================================

void LinuxISODownloaderTests::testDistroByIdFound() {
    auto d = m_catalog->distroById("ubuntu-desktop");
    QVERIFY(!d.id.isEmpty());
    QCOMPARE(d.id, QString("ubuntu-desktop"));
    QCOMPARE(d.name, QString("Ubuntu Desktop"));
    // The rest of the looked-up record. id+name matching proved the INDEX resolved, but said
    // nothing about the fields every later step consumes: the version that is substituted into
    // the download URL, the source type that selects the resolver, and the checksum type that
    // decides how the ISO is verified before it is written to removable media.
    QCOMPARE(d.version, QString("26.04"));
    QCOMPARE(d.versionLabel, QString("Resolute Raccoon (LTS)"));
    QCOMPARE(d.category, LinuxDistroCatalog::Category::GeneralPurpose);
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::DirectURL);
    QCOMPARE(d.checksumType, QString("sha256"));
    QCOMPARE(d.homepage, QString("https://ubuntu.com/desktop"));
    QCOMPARE(d.approximateSize, Q_INT64_C(6'549'825'126));
}

void LinuxISODownloaderTests::testDistroByIdNotFound() {
    auto d = m_catalog->distroById("nonexistent-distro-xyz");
    QVERIFY(d.id.isEmpty());

    auto removed = m_catalog->distroById("memtest86plus");
    QVERIFY2(removed.id.isEmpty(), "Memtest86+ should not be present in the ISO catalog");
}

void LinuxISODownloaderTests::testDistrosByCategory() {
    auto general = m_catalog->distrosByCategory(LinuxDistroCatalog::Category::GeneralPurpose);
    // Six general-purpose distros: ubuntu-desktop, ubuntu-server, linuxmint, fedora, debian, arch.
    QCOMPARE(general.size(), 6);

    // The exact membership AND order of the category, which is what the picker renders. A
    // single presence flag could not see another distro silently dropping out of the group
    // (the size check alone would still pass if one were swapped for another).
    QStringList generalIds;
    for (const auto& d : general) {
        generalIds << d.id;
    }
    QCOMPARE(generalIds,
             QStringList({"ubuntu-desktop",
                          "ubuntu-server",
                          "linuxmint-cinnamon",
                          "fedora-workstation",
                          "debian-live-gnome",
                          "arch-linux"}));
}

void LinuxISODownloaderTests::testDistrosByCategoryCount() {
    int total = 0;
    auto catNames = LinuxDistroCatalog::categoryNames();
    for (auto it = catNames.constBegin(); it != catNames.constEnd(); ++it) {
        auto distros = m_catalog->distrosByCategory(it.key());
        total += distros.size();
    }
    // Pin the per-category split, not only that the parts sum to the whole: the old compare was
    // satisfied by ANY partition of the catalog, including one where a category had gone empty
    // and its distros had all landed in another.
    QCOMPARE(m_catalog->distrosByCategory(LinuxDistroCatalog::Category::GeneralPurpose).size(), 6);
    QCOMPARE(m_catalog->distrosByCategory(LinuxDistroCatalog::Category::Security).size(), 1);
    QCOMPARE(m_catalog->distrosByCategory(LinuxDistroCatalog::Category::SystemRecovery).size(), 1);
    QCOMPARE(m_catalog->distrosByCategory(LinuxDistroCatalog::Category::DiskTools).size(), 3);
    QCOMPARE(m_catalog->distrosByCategory(LinuxDistroCatalog::Category::Utilities).size(), 1);
    QCOMPARE(total, 12);
    QCOMPARE(total, static_cast<int>(m_catalog->allDistros().size()));
}

// ============================================================================
// Catalog: URL Resolution -- DirectURL
// ============================================================================

void LinuxISODownloaderTests::testResolveDirectUrl_Ubuntu() {
    auto d = m_catalog->distroById("ubuntu-desktop");
    QString url = m_catalog->resolveDownloadUrl(d);
    // The fully-resolved URL. The four probes were jointly satisfied by a template whose
    // RELEASE PATH segment ("resolute") was wrong or unsubstituted -- the download would 404,
    // and every probe would still pass.
    QCOMPARE(url, QString("https://releases.ubuntu.com/resolute/ubuntu-26.04-desktop-amd64.iso"));
}

void LinuxISODownloaderTests::testResolveDirectUrl_Kali() {
    auto d = m_catalog->distroById("kali-linux");
    QString url = m_catalog->resolveDownloadUrl(d);
    // Kali downloads from a rolling current/ directory, so the catalog-built URL is the
    // STARTING point the rolling-filename discovery later rewrites; pin it exactly.
    QCOMPARE(url,
             QString("https://cdimage.kali.org/current/kali-linux-2026.1-installer-amd64.iso"));
}

// ============================================================================
// Catalog: URL Resolution -- SourceForge
// ============================================================================

void LinuxISODownloaderTests::testResolveSourceForgeUrl_SystemRescue() {
    auto d = m_catalog->distroById("systemrescue");
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::SourceForge);

    QString url = m_catalog->resolveDownloadUrl(d);
    // The version appears TWICE in this template (directory and filename); a contains() check
    // passes when only one of the two is substituted, which yields a 404 mirror path.
    QCOMPARE(url,
             QString("https://sourceforge.net/projects/systemrescuecd/files/sysresccd-x86/"
                     "13.00/systemrescue-13.00-amd64.iso/download"));
}

void LinuxISODownloaderTests::testResolveSourceForgeUrl_Clonezilla() {
    auto d = m_catalog->distroById("clonezilla");
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::SourceForge);

    QString url = m_catalog->resolveDownloadUrl(d);
    QCOMPARE(url,
             QString("https://sourceforge.net/projects/clonezilla/files/clonezilla_live_stable/"
                     "3.3.1-35/clonezilla-live-3.3.1-35-amd64.iso/download"));
}

void LinuxISODownloaderTests::testResolveSourceForgeUrl_GParted() {
    auto d = m_catalog->distroById("gparted-live");
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::SourceForge);

    QString url = m_catalog->resolveDownloadUrl(d);
    QCOMPARE(url,
             QString("https://sourceforge.net/projects/gparted/files/gparted-live-stable/"
                     "1.8.1-3/gparted-live-1.8.1-3-amd64.iso/download"));
}

// ============================================================================
// Catalog: URL Resolution -- GitHub
// ============================================================================

void LinuxISODownloaderTests::testResolveGitHubUrl_NoCache() {
    auto d = m_catalog->distroById("shredos");
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::GitHubRelease);

    // Without cached GitHub API data, URL should be empty for ShredOS.
    QString url = m_catalog->resolveDownloadUrl(d);
    QVERIFY(url.isEmpty());
}

void LinuxISODownloaderTests::testResolveGitHubUrl_RequiresVersionCheck() {
    auto d = m_catalog->distroById("ventoy");
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::GitHubRelease);

    QString url = m_catalog->resolveDownloadUrl(d);
    QVERIFY2(url.isEmpty(), "GitHub release URLs must come from a successful version check");
}

// ============================================================================
// Catalog: Checksum URL Resolution
// ============================================================================

void LinuxISODownloaderTests::testResolveChecksumUrl_Ubuntu() {
    auto d = m_catalog->distroById("ubuntu-desktop");
    QString url = m_catalog->resolveChecksumUrl(d);
    // The digest source is a security-load-bearing URL: it decides which host attests to the
    // ISO before it is written to removable media. contains("SHA256SUMS") passed for ANY host.
    QCOMPARE(url, QString("https://releases.ubuntu.com/resolute/SHA256SUMS"));
}

void LinuxISODownloaderTests::testResolveChecksumUrl_SystemRescue() {
    // R5-P5-11: SystemRescue keeps a per-release .sha256 on its own site, so the entry is
    // pinnable and no longer refused. The digest host is deliberately NOT the SourceForge
    // mirror network that serves the ISO.
    auto d = m_catalog->distroById("systemrescue");
    const QString url = m_catalog->resolveChecksumUrl(d);
    // Both {version} substitutions (directory and filename) resolved, on the project's own
    // host rather than the SourceForge mirror that serves the ISO -- which is the entire point
    // of this entry. A contains() on the version passed with only one of the two substituted.
    QCOMPARE(url,
             QString("https://www.system-rescue.org/releases/13.00/"
                     "systemrescue-13.00-amd64.iso.sha256"));
    QCOMPARE(d.checksumType, QString("sha256"));
}

void LinuxISODownloaderTests::testResolveChecksumUrl_GParted() {
    // R5-P5-11: GParted ships no .sha256 sidecar; its per-release digests live in the release
    // note in the same {version} directory as the ISO, which is equally per-release.
    auto d = m_catalog->distroById("gparted-live");
    const QString url = m_catalog->resolveChecksumUrl(d);
    QCOMPARE(url,
             QString("https://downloads.sourceforge.net/project/gparted/gparted-live-stable/"
                     "1.8.1-3/gparted-live-1.8.1-3-README.md"));
    QCOMPARE(d.checksumType, QString("sha256"));
}

void LinuxISODownloaderTests::testResolveChecksumUrl_NoChecksum_Clonezilla() {
    // Clonezilla publishes no checksum at a URL that addresses one release for its whole life
    // (see the note in linux_distro_catalog.cpp), so it stays deliberately unpinned -- and
    // therefore refused by requirePinnedChecksum rather than downloaded unverified.
    auto d = m_catalog->distroById("clonezilla");
    QString url = m_catalog->resolveChecksumUrl(d);
    QVERIFY(url.isEmpty());
}

void LinuxISODownloaderTests::testParseExpectedHash_requiresRealDigest() {
    // R5-P5-11: GParted's per-release digests arrive inside a release note, not a bare sums
    // file, so the parser walks prose. A token is only accepted as the digest when it has the
    // shape of one -- otherwise an ordinary word sitting next to the ISO's name (or a lone
    // word on its own line) would be returned as "the hash" and verify nothing.
    const QString sha256 = QString(64, QLatin1Char('a'));
    const QString sha1 = QString(40, QLatin1Char('b'));
    QVERIFY(LinuxISODownloader::isHexDigestOfLength(sha256, 64));
    QVERIFY(LinuxISODownloader::isHexDigestOfLength(sha1, 40));
    QVERIFY(LinuxISODownloader::isHexDigestOfLength(
        QString("3F66B2E10B8BB2C573ED6CDD3A9B54FD0A8E7690634AB6B15C3C8F517992D1A1"), 64));

    // The real GParted 1.8.1-3 SHA-256 and BLAKE3 digests of the SAME ISO. Both pass the shape
    // test, which is precisely why the per-algorithm heading -- not the shape -- decides which
    // one is used.
    const QString realSha256 = "3f66b2e10b8bb2c573ed6cdd3a9b54fd0a8e7690634ab6b15c3c8f517992d1a1";
    const QString realBlake3 = "3c2c2edf2ae67b85c2b74984d2ccfcbe64045d528ac8bbc6f7d092858f66e626";
    QVERIFY(LinuxISODownloader::isHexDigestOfLength(realSha256, 64));
    QVERIFY(LinuxISODownloader::isHexDigestOfLength(realBlake3, 64));
    QVERIFY(realSha256 != realBlake3);

    // Wrong length, non-hex characters, prose words and empty input are all refused.
    QVERIFY(!LinuxISODownloader::isHexDigestOfLength(sha256, 40));
    QVERIFY(!LinuxISODownloader::isHexDigestOfLength(QString("Download"), 64));
    QVERIFY(!LinuxISODownloader::isHexDigestOfLength(QString(64, QLatin1Char('z')), 64));
    QVERIFY(!LinuxISODownloader::isHexDigestOfLength(QString(), 64));
    QVERIFY(!LinuxISODownloader::isHexDigestOfLength(sha256 + QLatin1Char('a'), 64));
}

void LinuxISODownloaderTests::testChecksumSectionAlgorithm_separatesSameLengthDigests() {
    // R5-P5-11: GParted's release note carries MD5/SHA1/SHA256/SHA512/B2/B3 blocks for the
    // SAME ISO. A BLAKE3 digest is 64 hex characters, exactly like SHA-256, so length alone
    // cannot tell them apart -- only the per-algorithm heading can. Selecting by shape would
    // silently return whichever block came first.
    QCOMPARE(LinuxISODownloader::checksumSectionAlgorithm("### SHA256SUMS:"), QString("SHA256"));
    QCOMPARE(LinuxISODownloader::checksumSectionAlgorithm("### B3SUMS:"), QString("B3"));
    QCOMPARE(LinuxISODownloader::checksumSectionAlgorithm("MD5SUMS"), QString("MD5"));
    QCOMPARE(LinuxISODownloader::checksumSectionAlgorithm("# SHA1SUMS:"), QString("SHA1"));

    // Digest records and prose are not headings.
    QVERIFY(LinuxISODownloader::checksumSectionAlgorithm(
                "3f66b2e10b8bb2c573ed6cdd3a9b54fd0a8e7690634ab6b15c3c8f517992d1a1  x.iso")
                .isEmpty());
    QVERIFY(LinuxISODownloader::checksumSectionAlgorithm("This is GParted live 1.8.1-3").isEmpty());
    QVERIFY(LinuxISODownloader::checksumSectionAlgorithm("").isEmpty());
}

void LinuxISODownloaderTests::testBsdRecordDigest_followsAlgorithmAndFileLabels() {
    // R5-P5-11 follow-up: Fedora publishes BSD-style records, which the parser never handled,
    // so Fedora could never verify. The algorithm is named on the line, so it is followed the
    // same way a section heading is.
    using D = LinuxISODownloader;
    const QString iso = "Fedora-Workstation-Live-44-1.7.x86_64.iso";
    const QString other = "Fedora-Workstation-Live-44-1.7.aarch64.iso";
    const QString digest = "1620295f6a00c27c3208f0c00b8ece4eab1ec69b9002152d97488bf26a426ddf";
    const QString record = "SHA256 (" + iso + ") = " + digest;
    QCOMPARE(D::bsdRecordDigest(record, iso, "sha256", 64), digest);

    // "SHA-256" and "SHA256" name the same algorithm.
    const QString dashed = "SHA-256 (" + iso + ") = " + digest;
    QCOMPARE(D::bsdRecordDigest(dashed, iso, "sha256", 64), digest);

    // NEGATIVE: a record naming a DIFFERENT algorithm must be rejected, never accepted just
    // because its digest happens to be the right length.
    const QString blake = "BLAKE3 (" + iso + ") = " + digest;
    QVERIFY(D::bsdRecordDigest(record, iso, "sha1", 40).isEmpty());
    QVERIFY(D::bsdRecordDigest(blake, iso, "sha256", 64).isEmpty());

    // NEGATIVE: a record naming a DIFFERENT file must be rejected, so a CHECKSUM covering
    // several artifacts cannot yield another file's digest.
    QVERIFY(D::bsdRecordDigest(record, other, "sha256", 64).isEmpty());

    // NEGATIVE: the shape check still applies -- a non-digest token is not a digest.
    const QString notHash = "SHA256 (" + iso + ") = notahash";
    const QString shortHash = "SHA256 (" + iso + ") = " + digest.left(63);
    QVERIFY(D::bsdRecordDigest(notHash, iso, "sha256", 64).isEmpty());
    QVERIFY(D::bsdRecordDigest(shortHash, iso, "sha256", 64).isEmpty());

    // A plain sums record and the PGP armor Fedora wraps its CHECKSUM in are not BSD records.
    const QString plain = digest + "  " + iso;
    QVERIFY(D::bsdRecordDigest(plain, iso, "sha256", 64).isEmpty());
    QVERIFY(D::bsdRecordDigest("-----BEGIN PGP SIGNATURE-----", iso, "sha256", 64).isEmpty());
}

void LinuxISODownloaderTests::testResolveChecksumUrl_Fedora() {
    // Fedora's CHECKSUM was pinned and HTTPS all along; what was missing was a parser that
    // could read its BSD-style records. Assert the pin still satisfies the catalog invariant.
    auto d = m_catalog->distroById("fedora-workstation");
    const QString url = m_catalog->resolveChecksumUrl(d);
    // startsWith("https://") is the weakest possible form of the HTTPS claim this test is
    // named for; the whole URL pins the host as well as the scheme.
    QCOMPARE(url,
             QString("https://download.fedoraproject.org/pub/fedora/linux/releases/44/"
                     "Workstation/x86_64/iso/Fedora-Workstation-44-1.7-x86_64-CHECKSUM"));
    QCOMPARE(d.checksumType, QString("sha256"));
}

void LinuxISODownloaderTests::testEveryPinnedChecksumUrlIsHttpsAndPerRelease() {
    // Every catalog entry that carries a pinned checksum must carry it over TLS and must
    // resolve to a concrete release: an unresolved {version} placeholder would fetch a
    // non-existent file and a plain-HTTP digest could be tampered with on the same leg as
    // the ISO. An entry with no pinned checksum is allowed -- it is refused at download time.
    for (const auto& d : m_catalog->allDistros()) {
        const QString url = m_catalog->resolveChecksumUrl(d);
        if (url.isEmpty()) {
            QVERIFY2(d.checksumUrl.isEmpty() ||
                         d.sourceType == LinuxDistroCatalog::SourceType::GitHubRelease,
                     qPrintable("Entry " + d.id + " has a checksum template that resolved away"));
            continue;
        }
        QVERIFY2(url.startsWith("https://"),
                 qPrintable(d.id + " checksum URL is not HTTPS: " + url));
        QVERIFY2(!url.contains("{version}"),
                 qPrintable(d.id + " checksum URL has an unresolved placeholder: " + url));
        QVERIFY2(!d.checksumType.isEmpty(), qPrintable(d.id + " has a checksum URL but no type"));
    }
}

// ============================================================================
// Catalog: Filename Resolution
// ============================================================================

void LinuxISODownloaderTests::testResolveFileName_Direct() {
    auto d = m_catalog->distroById("ubuntu-desktop");
    QString filename = m_catalog->resolveFileName(d);
    // This is the name the ISO is SAVED under, and the same name the checksum record is matched
    // against; a shape check could not see the flavour ("desktop" vs "live-server") drifting.
    QCOMPARE(filename, QString("ubuntu-26.04-desktop-amd64.iso"));
}

void LinuxISODownloaderTests::testResolveFileName_SourceForge() {
    auto d = m_catalog->distroById("gparted-live");
    QString filename = m_catalog->resolveFileName(d);
    QCOMPARE(filename, QString("gparted-live-1.8.1-3-amd64.iso"));
}

void LinuxISODownloaderTests::testResolveFileName_GitHubTemplate() {
    auto d = m_catalog->distroById("ventoy");
    QString filename = m_catalog->resolveFileName(d);
    // GitHub-backed distros keep a deterministic filename template before live asset
    // resolution. The old guarded check was VACUOUS whenever fileName was empty -- which is
    // precisely the regression (a dropped template) it was meant to catch.
    QCOMPARE(filename, QString("ventoy-1.1.12-livecd.iso"));
}

// ============================================================================
// Catalog: Version Substitution
// ============================================================================

void LinuxISODownloaderTests::testVersionSubstitution() {
    auto d = m_catalog->distroById("ubuntu-server");
    QString url = m_catalog->resolveDownloadUrl(d);
    // Substitution proven by the whole result: "contains the version" also held if the version
    // were spliced into the wrong segment, and the server flavour is what distinguishes this
    // entry from ubuntu-desktop, which shares every other part of the URL.
    QCOMPARE(url,
             QString("https://releases.ubuntu.com/resolute/ubuntu-26.04-live-server-amd64.iso"));
}

void LinuxISODownloaderTests::testVersionSubstitutionNoPlaceholder() {
    // Distros without {version} in URL should still resolve fine
    auto all = m_catalog->allDistros();
    for (const auto& d : all) {
        if (d.sourceType != LinuxDistroCatalog::SourceType::GitHubRelease) {
            QString url = m_catalog->resolveDownloadUrl(d);
            QVERIFY2(!url.contains("{version}"),
                     qPrintable("Unresolved {version} in URL for " + d.id));
        }
    }
}

// ============================================================================
// GitHub API Parsing
// ============================================================================

void LinuxISODownloaderTests::testGitHubVersionCheck_Success() {
    // Create a separate catalog for isolated testing
    LinuxDistroCatalog catalog;
    QSignalSpy completedSpy(&catalog, &LinuxDistroCatalog::versionCheckCompleted);
    QSignalSpy failedSpy(&catalog, &LinuxDistroCatalog::versionCheckFailed);

    // Simulate a GitHub release JSON for Ventoy
    QJsonObject release;
    release["tag_name"] = "v1.2.0";
    QJsonArray assets;
    QJsonObject isoAsset;
    isoAsset["name"] = "ventoy-1.2.0-livecd.iso";
    isoAsset["browser_download_url"] =
        "https://github.com/ventoy/Ventoy/releases/download/v1.2.0/ventoy-1.2.0-livecd.iso";
    isoAsset["size"] = 200'000'000;
    assets.append(isoAsset);
    release["assets"] = assets;

    // Use the parseGitHubRelease method indirectly via checkLatestVersion signal
    // For unit testing, we verify the catalog can look up ventoy
    auto d = catalog.distroById("ventoy");
    QCOMPARE(d.id, QString("ventoy"));
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::GitHubRelease);
    QVERIFY(!d.githubAssetPattern.isEmpty());

    // Verify the asset pattern regex compiles and matches
    QRegularExpression assetRegex(d.githubAssetPattern, QRegularExpression::CaseInsensitiveOption);
    QVERIFY(assetRegex.isValid());
    QVERIFY(assetRegex.match("ventoy-1.2.0-livecd.iso").hasMatch());
    QVERIFY(!assetRegex.match("ventoy-1.2.0-windows.zip").hasMatch());
}

void LinuxISODownloaderTests::testGitHubVersionCheck_NoMatchingAsset() {
    // Verify ShredOS pattern doesn't match non-ISO files
    auto d = m_catalog->distroById("shredos");
    QRegularExpression regex(d.githubAssetPattern, QRegularExpression::CaseInsensitiveOption);
    QVERIFY(regex.isValid());
    QVERIFY(!regex.match("shredos-source.tar.gz").hasMatch());
    QVERIFY(!regex.match("README.md").hasMatch());

    // Should match ISO files
    QVERIFY(regex.match("shredos-v2025.11_30_x86-64_0.41.iso").hasMatch());
}

void LinuxISODownloaderTests::testArchLinuxCatalogEntry() {
    auto d = m_catalog->distroById("arch-linux");
    QVERIFY(!d.id.isEmpty());
    QCOMPARE(d.name, QString("Arch Linux"));
    QCOMPARE(d.sourceType, LinuxDistroCatalog::SourceType::DirectURL);

    const QString url = m_catalog->resolveDownloadUrl(d);
    // Arch serves a rolling "latest" path with no version in it, so the two probes could not
    // detect the directory changing underneath; pin both URLs whole.
    QCOMPARE(url, QString("https://geo.mirror.pkgbuild.com/iso/latest/archlinux-x86_64.iso"));

    const QString checksumUrl = m_catalog->resolveChecksumUrl(d);
    QCOMPARE(checksumUrl, QString("https://geo.mirror.pkgbuild.com/iso/latest/sha256sums.txt"));
}

// ============================================================================
// Downloader: Construction & State
// ============================================================================

void LinuxISODownloaderTests::testDownloaderConstruction() {
    LinuxISODownloader downloader;
    QVERIFY(downloader.catalog() != nullptr);
    QVERIFY(!downloader.isDownloading());
}

void LinuxISODownloaderTests::testDownloaderInitialState() {
    LinuxISODownloader downloader;
    QCOMPARE(downloader.currentPhase(), LinuxISODownloader::Phase::Idle);
    QVERIFY(!downloader.isDownloading());
}

// ============================================================================
// Downloader: Input Validation
// ============================================================================

void LinuxISODownloaderTests::testStartDownload_UnknownDistro() {
    LinuxISODownloader downloader;
    QSignalSpy errorSpy(&downloader, &LinuxISODownloader::downloadError);

    downloader.startDownload("nonexistent-distro", "C:/tmp/test.iso");

    QCOMPARE(errorSpy.count(), 1);
    // The message ECHOES the rejected id, which is what tells a caller the lookup failed rather
    // than the download; "Unknown" alone also matched an unrelated unknown-error path.
    QCOMPARE(errorSpy.at(0).at(0).toString(), QString("Unknown distribution: nonexistent-distro"));
}

void LinuxISODownloaderTests::testStartDownload_WhileDownloading() {
    // We can't easily simulate a download in progress without aria2c,
    // but we can verify the guard logic exists by checking the phase
    LinuxISODownloader downloader;
    // Phase is Idle, so starting should not produce "already in progress"
    QSignalSpy errorSpy(&downloader, &LinuxISODownloader::downloadError);

    // First attempt with unknown distro to trigger error
    downloader.startDownload("nonexistent", "C:/tmp/test.iso");
    QCOMPARE(errorSpy.count(), 1);
    // Explicitly NOT the "already in progress" refusal this test exists to rule out.
    QCOMPARE(errorSpy.at(0).at(0).toString(), QString("Unknown distribution: nonexistent"));
}

void LinuxISODownloaderTests::testStartDownload_RefusesReleaseWithoutPinnedChecksum() {
    // R5-P5-11: a downloaded ISO is written to bootable media. A catalog entry with no pinned
    // checksum could not be verified at all, and the SourceForge download URL redirects onto
    // mirrors that may serve the file over plain HTTP -- so the download previously completed
    // as an explicitly-unverified artifact. It is now refused up front instead.
    auto clonezilla = m_catalog->distroById("clonezilla");
    QVERIFY(m_catalog->resolveChecksumUrl(clonezilla).isEmpty());

    LinuxISODownloader downloader;
    QSignalSpy errorSpy(&downloader, &LinuxISODownloader::downloadError);
    downloader.startDownload("clonezilla", QDir::tempPath() + "/sak-test-clonezilla.iso");

    QCOMPARE(errorSpy.count(), 1);
    // The whole refusal, naming the distro and the reason. "mentions checksum" was equally
    // satisfied by a checksum DOWNLOAD failure or a mismatch, neither of which proves the
    // download was refused BEFORE any bytes were fetched.
    QCOMPARE(errorSpy.at(0).at(0).toString(),
             QString("No pinned checksum is published for Clonezilla Live; refusing to download "
                     "an ISO whose integrity cannot be verified before it is written to "
                     "removable media."));
    QCOMPARE(downloader.currentPhase(), LinuxISODownloader::Phase::Failed);
    QVERIFY(!downloader.isDownloading());

    // An entry that DOES publish a pinned HTTPS checksum is not blocked by this guard.
    auto ubuntu = m_catalog->distroById("ubuntu-desktop");
    const QString ubuntuChecksum = m_catalog->resolveChecksumUrl(ubuntu);
    QVERIFY(!ubuntuChecksum.isEmpty());
    QVERIFY(ubuntuChecksum.startsWith("https://"));
}

// ============================================================================
// Downloader: Phase Management
// ============================================================================

void LinuxISODownloaderTests::testPhaseTransitions() {
    LinuxISODownloader downloader;

    // Initial state
    QCOMPARE(downloader.currentPhase(), LinuxISODownloader::Phase::Idle);
    QVERIFY(!downloader.isDownloading());

    // After failed start attempt, should not be in downloading state
    downloader.startDownload("nonexistent", "C:/tmp/test.iso");
    QVERIFY(!downloader.isDownloading());
    // isDownloading() is false for Idle AND Failed alike, so pin the phase the failed start
    // actually left behind -- an unknown id must not latch the machine into Failed.
    QCOMPARE(downloader.currentPhase(), LinuxISODownloader::Phase::Idle);
}

// ============================================================================
// Downloader: Cancel
// ============================================================================

void LinuxISODownloaderTests::testCancelFromIdle() {
    LinuxISODownloader downloader;
    QSignalSpy statusSpy(&downloader, &LinuxISODownloader::statusMessage);

    // Cancel from idle should not crash
    downloader.cancel();
    QCOMPARE(downloader.currentPhase(), LinuxISODownloader::Phase::Idle);
    // The spy was declared and never asserted. A cancel from idle still reports itself once,
    // so the user is not left wondering whether the click registered.
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(), QString("Download cancelled"));
}

void LinuxISODownloaderTests::testShouldApplyVerifyResult_gating() {
    // Same generation, not cancelled -> apply.
    QVERIFY(LinuxISODownloader::shouldApplyVerifyResult(false, 5, 5));
    // Cancelled -> never apply, even if generation still matches.
    QVERIFY(!LinuxISODownloader::shouldApplyVerifyResult(true, 5, 5));
    // Superseded by a newer operation -> discard the stale result.
    QVERIFY(!LinuxISODownloader::shouldApplyVerifyResult(false, 5, 6));
    QVERIFY(!LinuxISODownloader::shouldApplyVerifyResult(true, 5, 6));
}

// ============================================================================
// Network Tests
// ============================================================================

void LinuxISODownloaderTests::testGitHubVersionCheck_Network() {
    LinuxDistroCatalog catalog;
    QSignalSpy completedSpy(&catalog, &LinuxDistroCatalog::versionCheckCompleted);
    QSignalSpy failedSpy(&catalog, &LinuxDistroCatalog::versionCheckFailed);

    catalog.checkLatestVersion("ventoy");

    // Wait up to 15 seconds for API response
    bool gotResponse = false;
    for (int i = 0; i < 150 && !gotResponse; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (completedSpy.count() > 0 || failedSpy.count() > 0) {
            gotResponse = true;
        }
    }

    if (!gotResponse) {
        QSKIP("GitHub API unreachable -- skipping network test");
    }

    if (failedSpy.count() > 0) {
        qWarning() << "GitHub API error:" << failedSpy.at(0).at(1).toString();
        QSKIP("GitHub API returned error -- skipping");
    }

    QCOMPARE(completedSpy.count(), 1);
    QString distroId = completedSpy.at(0).at(0).toString();
    QCOMPARE(distroId, QString("ventoy"));

    // After version check, the resolved URL should be populated
    auto d = catalog.distroById("ventoy");
    QString url = catalog.resolveDownloadUrl(d);
    QVERIFY2(!url.isEmpty(), "Download URL should be resolved after version check");
    QVERIFY2(url.startsWith("https://"), qPrintable("Download URL should be HTTPS: " + url));
}

void LinuxISODownloaderTests::testDirectUrlReachable_Ubuntu() {
    Q_ASSERT(m_catalog);
    // Quick HEAD request to verify Ubuntu URL is reachable
    auto d = m_catalog->distroById("ubuntu-server");
    QString url = m_catalog->resolveDownloadUrl(d);

    QNetworkAccessManager nam;
    QUrl requestUrl(url);
    QNetworkRequest request(requestUrl);
    request.setRawHeader("User-Agent", "SAK-Utility-Test/1.0");

    auto* reply = nam.head(request);
    QSignalSpy finishedSpy(reply, &QNetworkReply::finished);

    // qWaitFor re-checks the spy instead of waiting for a further emission, so a reply that
    // finished before the wait begins is not misread as a timeout. A genuine timeout still
    // skips: this test is only meaningful with network access.
    bool ok = QTest::qWaitFor([&finishedSpy]() { return finishedSpy.count() > 0; }, 15'000);
    if (!ok) {
        reply->deleteLater();
        QSKIP("Network timeout -- skipping URL reachability test");
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    // Accept 200 (OK) or 302/301 (redirect) as valid
    QVERIFY2(statusCode == 200 || statusCode == 301 || statusCode == 302,
             qPrintable(QString("Unexpected HTTP status %1 for %2").arg(statusCode).arg(url)));
}

void LinuxISODownloaderTests::testSourceForgeUrlStructure_AllDistros() {
    // Verify all SourceForge distros have properly formed URLs
    auto all = m_catalog->allDistros();
    // The whole loop body is skipped for every non-SourceForge entry, so it would pass
    // VACUOUSLY if the catalog stopped carrying SourceForge distros at all (or if the
    // sourceType of all three drifted). Count what was actually checked.
    int sourceForgeChecked = 0;
    for (const auto& d : all) {
        if (d.sourceType != LinuxDistroCatalog::SourceType::SourceForge) {
            continue;
        }
        ++sourceForgeChecked;

        QString url = m_catalog->resolveDownloadUrl(d);

        QVERIFY2(url.startsWith("https://sourceforge.net/projects/"),
                 qPrintable("SF URL must start with https://sourceforge.net/projects/: " + d.id +
                            " -> " + url));
        QVERIFY2(url.endsWith("/download"),
                 qPrintable("SF URL must end with /download: " + d.id + " -> " + url));
        QVERIFY2(url.contains(d.version),
                 qPrintable("SF URL must contain version: " + d.id + " -> " + url));
        QVERIFY2(!url.contains("{version}"),
                 qPrintable("Unresolved {version} placeholder: " + d.id + " -> " + url));

        // Verify filename resolution
        QString filename = m_catalog->resolveFileName(d);
        QVERIFY2(!filename.isEmpty(), qPrintable("SF distro " + d.id + " has empty filename"));
        QVERIFY2(filename.contains(d.version),
                 qPrintable("SF filename missing version: " + d.id + " -> " + filename));
    }
    // systemrescue, clonezilla, gparted-live.
    QCOMPARE(sourceForgeChecked, 3);
}

QTEST_GUILESS_MAIN(LinuxISODownloaderTests)
#include "test_linux_iso_downloader.moc"
