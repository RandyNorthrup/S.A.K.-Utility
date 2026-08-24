// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/linux_distro_catalog.h"

#include <QTest>

/**
 * @brief Unit tests for LinuxDistroCatalog.
 *
 * Covers catalog population, category filtering, ID lookup,
 * URL/filename resolution, and static helpers.
 */
class TestLinuxDistroCatalog : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // -- Catalog population ----------------------------------
    void testCatalogNonEmpty();
    void testAllDistrosHaveIds();
    void testAllDistrosHaveNames();
    void testAllDistrosHaveDescriptions();
    void testAllDistrosHavePositiveSize();
    void testNoDuplicateIds();

    // -- Category operations ---------------------------------
    void testCategoryNames();
    void testDistrosByCategory();
    void testAllCategoriesPresent();

    // -- ID lookup -------------------------------------------
    void testDistroByIdFound();
    void testDistroByIdNotFound();

    // -- URL resolution --------------------------------------
    void testResolveDownloadUrl();
    void testResolveChecksumUrl();
    void testResolveFileName();
    void testResolveGitHubReleaseBranch();
    void testResolveGitHubReleaseCachedAsset();

    // -- Rolling-release filename discovery (R5-G22-10) ------
    void testFilenameFromChecksumsKali();
    void testFilenameFromChecksumsDebian();
    void testFilenameFromChecksumsBinaryMarker();
    void testFilenameFromChecksumsRejectsPathAndNonRecords();
    void testFilenameFromChecksumsRejectsPathEvenWhenPatternPermits();
    void testFilenameFromChecksumsEmptyOrInvalidPattern();
    void testRollingDistrosCarryPattern();

    void testCancelAllWithNoPendingIsSafe();

    void cleanupTestCase();

private:
    std::unique_ptr<LinuxDistroCatalog> m_catalog;
};

void TestLinuxDistroCatalog::initTestCase() {
    m_catalog = std::make_unique<LinuxDistroCatalog>();
}

// ============================================================================
// Catalog population
// ============================================================================

void TestLinuxDistroCatalog::testCatalogNonEmpty() {
    const auto all = m_catalog->allDistros();
    QVERIFY2(!all.isEmpty(), "Catalog should contain distros");
    QCOMPARE(all.size(),
             12);  // fixed catalog: 6 general + 1 kali + 1 systemrescue + 3 disk + ventoy
}

void TestLinuxDistroCatalog::testAllDistrosHaveIds() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!d.id.isEmpty(), qPrintable("Distro has empty id: " + d.name));
    }
}

void TestLinuxDistroCatalog::testAllDistrosHaveNames() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!d.name.isEmpty(), qPrintable("Distro has empty name, id=" + d.id));
    }
}

void TestLinuxDistroCatalog::testAllDistrosHaveDescriptions() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!d.description.isEmpty(),
                 qPrintable("Distro '" + d.name + "' has empty description"));
    }
}

void TestLinuxDistroCatalog::testAllDistrosHavePositiveSize() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(d.approximateSize > 0,
                 qPrintable("Distro '" + d.name + "' has non-positive size"));
    }

    // '> 0' is a floor a unit-confused helper walks straight through. sizeFromMiB()
    // (linux_distro_catalog.cpp:55-57) multiplies by sak::kBytesPerMB; swapping that one token
    // for the adjacent sak::kBytesPerKB (layout_constants.h:21-22) makes every MiB-derived entry
    // 1024x too small -- still strictly positive, so this loop and the sibling file's '> 0'
    // sweep (test_linux_iso_downloader.cpp:167) both stay green, while the only exact size pin
    // anywhere (ubuntu-desktop, test_linux_iso_downloader.cpp:245) goes through sizeFromGiB and
    // is untouched. The picker would then advertise "530.0 KB" for a 530 MiB ISO
    // (linux_iso_download_dialog.cpp:343-344). Pin the four MiB-derived entries exactly --
    // integer arithmetic, no float rounding.
    QCOMPARE(m_catalog->distroById("clonezilla").approximateSize, Q_INT64_C(555'745'280));
    QCOMPARE(m_catalog->distroById("gparted-live").approximateSize, Q_INT64_C(680'525'824));
    QCOMPARE(m_catalog->distroById("shredos").approximateSize, Q_INT64_C(943'718'400));
    QCOMPARE(m_catalog->distroById("ventoy").approximateSize, Q_INT64_C(205'520'896));
}

void TestLinuxDistroCatalog::testNoDuplicateIds() {
    QSet<QString> ids;
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!ids.contains(d.id), qPrintable("Duplicate distro id: " + d.id));
        ids.insert(d.id);
    }
}

// ============================================================================
// Category operations
// ============================================================================

void TestLinuxDistroCatalog::testCategoryNames() {
    const auto names = LinuxDistroCatalog::categoryNames();
    QCOMPARE(names.size(), 5);
    QCOMPARE(names.value(LinuxDistroCatalog::Category::GeneralPurpose),
             QStringLiteral("General Purpose"));
    QCOMPARE(names.value(LinuxDistroCatalog::Category::Security),
             QStringLiteral("Security && Pen-Testing"));
    QCOMPARE(names.value(LinuxDistroCatalog::Category::SystemRecovery),
             QStringLiteral("System Recovery"));
    QCOMPARE(names.value(LinuxDistroCatalog::Category::DiskTools), QStringLiteral("Disk Tools"));
    QCOMPARE(names.value(LinuxDistroCatalog::Category::Utilities), QStringLiteral("Utilities"));
}

void TestLinuxDistroCatalog::testDistrosByCategory() {
    auto general = m_catalog->distrosByCategory(LinuxDistroCatalog::Category::GeneralPurpose);
    QCOMPARE(general.size(), 6);

    // All returned distros should be in the right category
    for (const auto& d : general) {
        QCOMPARE(d.category, LinuxDistroCatalog::Category::GeneralPurpose);
    }
}

void TestLinuxDistroCatalog::testAllCategoriesPresent() {
    Q_ASSERT(m_catalog);
    const QList<LinuxDistroCatalog::Category> cats = {
        LinuxDistroCatalog::Category::GeneralPurpose,
        LinuxDistroCatalog::Category::Security,
        LinuxDistroCatalog::Category::SystemRecovery,
        LinuxDistroCatalog::Category::DiskTools,
        LinuxDistroCatalog::Category::Utilities,
    };
    const QMap<LinuxDistroCatalog::Category, qsizetype> expectedCounts = {
        {LinuxDistroCatalog::Category::GeneralPurpose, 6},
        {LinuxDistroCatalog::Category::Security, 1},
        {LinuxDistroCatalog::Category::SystemRecovery, 1},
        {LinuxDistroCatalog::Category::DiskTools, 3},
        {LinuxDistroCatalog::Category::Utilities, 1}};
    for (auto cat : cats) {
        QCOMPARE(m_catalog->distrosByCategory(cat).size(), expectedCounts.value(cat));
    }
}

// ============================================================================
// ID lookup
// ============================================================================

void TestLinuxDistroCatalog::testDistroByIdFound() {
    // Ubuntu Desktop should always be in catalog
    auto ubuntu = m_catalog->distroById("ubuntu-desktop");
    QVERIFY2(!ubuntu.id.isEmpty(), "ubuntu-desktop not found in catalog");
    QCOMPARE(ubuntu.id, "ubuntu-desktop");
    QCOMPARE(ubuntu.name, QStringLiteral("Ubuntu Desktop"));
}

void TestLinuxDistroCatalog::testDistroByIdNotFound() {
    auto missing = m_catalog->distroById("nonexistent-distro");
    QVERIFY(missing.id.isEmpty());
}

// ============================================================================
// URL resolution
// ============================================================================

void TestLinuxDistroCatalog::testResolveDownloadUrl() {
    auto ubuntu = m_catalog->distroById("ubuntu-desktop");
    if (ubuntu.id.isEmpty()) {
        QSKIP("ubuntu-desktop not in catalog");
    }

    const QString url = m_catalog->resolveDownloadUrl(ubuntu);
    QVERIFY(!url.isEmpty());
    QCOMPARE(url,
             QStringLiteral("https://releases.ubuntu.com/resolute/ubuntu-") + ubuntu.version +
                 QStringLiteral("-desktop-amd64.iso"));
    QVERIFY(!url.contains("{version}"));
}

void TestLinuxDistroCatalog::testResolveChecksumUrl() {
    Q_ASSERT(m_catalog);
    auto ubuntu = m_catalog->distroById("ubuntu-desktop");
    if (ubuntu.id.isEmpty()) {
        QSKIP("ubuntu-desktop not in catalog");
    }

    const QString url = m_catalog->resolveChecksumUrl(ubuntu);
    // ubuntu-desktop publishes a checksum endpoint, so the resolver must return it.
    QVERIFY2(!ubuntu.checksumUrl.isEmpty(), "ubuntu-desktop should carry a checksum URL");
    QVERIFY(!url.isEmpty());

    // Kills 'checksum-url-source-swapped-to-download': the resolved CHECKSUM url must be the
    // SHA256SUMS endpoint itself (ubuntu-desktop's checksumUrl carries no {version}, so the
    // resolver returns it verbatim) AND must be DISTINCT from the ISO download URL. If
    // resolveChecksumUrl returned distro.downloadUrl instead, every assert below turns red.
    QCOMPARE(url, ubuntu.checksumUrl);
    QCOMPARE(url, QStringLiteral("https://releases.ubuntu.com/resolute/SHA256SUMS"));
    QVERIFY2(url != m_catalog->resolveDownloadUrl(ubuntu),
             "checksum URL must not be the ISO download URL (would verify the wrong file)");
}

void TestLinuxDistroCatalog::testResolveFileName() {
    auto ubuntu = m_catalog->distroById("ubuntu-desktop");
    if (ubuntu.id.isEmpty()) {
        QSKIP("ubuntu-desktop not in catalog");
    }

    const QString name = m_catalog->resolveFileName(ubuntu);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.endsWith(".iso"));
    QVERIFY(!name.contains("{version}"));

    // Kills 'filename-middle-segment-wrong': assert the WHOLE expected filename, every segment
    // included -- not just the .iso suffix. A checksum-by-filename lookup matches the exact
    // name, so a wrong middle segment ('...-server-...' instead of '...-desktop-...') must turn
    // this red even though it still ends in ".iso" and carries no "{version}".
    const QString expected = "ubuntu-" + ubuntu.version + "-desktop-amd64.iso";
    QCOMPARE(name, expected);
}

void TestLinuxDistroCatalog::testResolveGitHubReleaseBranch() {
    // Drives the GitHubRelease branch of the resolvers with REAL catalog distros: before this,
    // only DirectURL ubuntu-desktop was resolved, leaving the GitHubRelease path uncovered.
    // shredos and ventoy are the shipped GitHubRelease entries.
    const auto shredos = m_catalog->distroById("shredos");
    const auto ventoy = m_catalog->distroById("ventoy");
    QVERIFY2(!shredos.id.isEmpty(), "shredos (GitHubRelease) missing from catalog");
    QVERIFY2(!ventoy.id.isEmpty(), "ventoy (GitHubRelease) missing from catalog");
    QCOMPARE(shredos.sourceType, LinuxDistroCatalog::SourceType::GitHubRelease);
    QCOMPARE(ventoy.sourceType, LinuxDistroCatalog::SourceType::GitHubRelease);

    // resolveDownloadUrl's GitHubRelease branch ('github-download-resolve-uncovered'): with no
    // cached asset (no network version check has run in this hermetic test) the download URL
    // MUST fail closed to empty rather than fall through to a substituted template. These
    // asserts EXECUTE that branch. The find-key mutation (distro.id -> distro.id + "_x") is only
    // OBSERVABLE once m_githubAssetUrls holds an entry for distro.id, which is populated solely
    // by a networked GitHub version check -- unreachable from a hermetic test without a src
    // seam -- so the empty-cache path returns {} for both original and mutant. The exact,
    // no-network kill-proof for this branch is carried by resolveFileName below.
    QVERIFY2(m_catalog->resolveDownloadUrl(shredos).isEmpty(),
             "GitHubRelease download URL must be empty until a version check caches an asset");
    QVERIFY2(m_catalog->resolveDownloadUrl(ventoy).isEmpty(),
             "GitHubRelease download URL must be empty until a version check caches an asset");

    // resolveFileName's GitHubRelease branch: with no cached asset, ventoy falls back to its
    // static filename template. Assert the EXACT full filename (host/tag/asset are only known
    // post-version-check, but the filename template is deterministic offline). Kills
    // 'github-filename-empty-guard-inverted': inverting `if (!distro.fileName.isEmpty())` drops
    // this fallback and returns empty.
    const QString expectedVentoyName = "ventoy-" + ventoy.version + "-livecd.iso";
    QCOMPARE(m_catalog->resolveFileName(ventoy), expectedVentoyName);
    // Two DISTINCT arms of resolveFileName's GitHubRelease branch both yield empty for shredos --
    // the cached-asset MISS and the empty static template -- so a bare isEmpty() here is equally
    // satisfied when the cached-asset arm is deleted outright. That arm is otherwise dead in the
    // whole suite. Deleting it would save a freshly resolved asset under the STALE template name,
    // so the on-disk name and the checksum-record lookup diverge. Pin the precondition that makes
    // the template arm operative here, then prove the cached-asset arm is LIVE on a locally
    // seeded catalog: shredos ships no fileName template, so the basename can ONLY come from the
    // cached URL's path.
    QVERIFY2(shredos.fileName.isEmpty(), "shredos ships no static filename template");
    QVERIFY(m_catalog->resolveFileName(shredos).isEmpty());

    LinuxDistroCatalog seeded;
    const QString shredosAsset = QStringLiteral(
        "https://github.com/PartialVolume/shredos.x86_64/releases/download/"
        "v2025.11_30_x86-64_0.41/shredos-2025.11_30_x86-64_0.41.iso");
    seeded.m_githubAssetUrls[shredos.id] = shredosAsset;  // friend access
    QCOMPARE(seeded.resolveFileName(shredos), QStringLiteral("shredos-2025.11_30_x86-64_0.41.iso"));
}

void TestLinuxDistroCatalog::testResolveGitHubReleaseCachedAsset() {
    // Kills 'github-download-resolve-uncovered' OFFLINE. resolveDownloadUrl's GitHubRelease
    // branch looks up m_githubAssetUrls.find(distro.id); the mutant changes that key to
    // distro.id + "_x". The edit is only OBSERVABLE once the cache holds an entry under the real
    // id, which production fills solely via a networked GitHub version check. Using the
    // friend-class test seam, seed that private cache directly (no network) with the exact asset
    // URL a successful resolveGitHubAsset would cache, then assert the resolver returns it
    // verbatim: the real code finds distro.id and returns the cached URL; the mutant looks up
    // distro.id + "_x" -> cache miss -> {}, so the QCOMPARE turns red.
    LinuxDistroCatalog cat;
    const auto ventoy = cat.distroById("ventoy");
    QVERIFY2(!ventoy.id.isEmpty(), "ventoy (GitHubRelease) missing from catalog");
    QCOMPARE(ventoy.sourceType, LinuxDistroCatalog::SourceType::GitHubRelease);

    const QString assetUrl = QStringLiteral(
        "https://github.com/ventoy/Ventoy/releases/download/v1.1.12/"
        "ventoy-1.1.12-livecd.iso");
    cat.m_githubAssetUrls[ventoy.id] = assetUrl;  // friend access; mirrors resolveGitHubAsset

    QCOMPARE(cat.resolveDownloadUrl(ventoy), assetUrl);
}

// ============================================================================
// Rolling-release filename discovery (R5-G22-10)
// ============================================================================

namespace {
// A valid 64-hex SHA-256 digest (sha256 of the empty input); the parser only
// checks digest SHAPE, so the same value stands in for every record here.
const QString kHash =
    QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

QString kaliPattern() {
    return QStringLiteral(R"(^kali-linux-[0-9][0-9.]*-installer-amd64\.iso$)");
}
QString debianPattern() {
    return QStringLiteral(R"(^debian-live-[0-9][0-9.]*-amd64-gnome\.iso$)");
}
}  // namespace

void TestLinuxDistroCatalog::testFilenameFromChecksumsKali() {
    // A realistic Kali current/SHA256SUMS listing several images. The anchored
    // pattern must pick ONLY the installer amd64 image, not the netinst, purple,
    // live, or arm64 variants whose names share the prefix.
    const QString sums = kHash + "  kali-linux-2026.2-installer-netinst-amd64.iso\n" + kHash +
                         "  kali-linux-2026.2-installer-purple-amd64.iso\n" + kHash +
                         "  kali-linux-2026.2-live-amd64.iso\n" + kHash +
                         "  kali-linux-2026.2-installer-arm64.iso\n" + kHash +
                         "  kali-linux-2026.2-installer-amd64.iso\n";
    QCOMPARE(LinuxDistroCatalog::filenameFromChecksums(sums, kaliPattern()),
             QStringLiteral("kali-linux-2026.2-installer-amd64.iso"));

    // Every pattern this file feeds the parser is ^...$-anchored, so production's OWN full-match
    // guard (linux_distro_catalog.cpp:608-609) never decides anything: weakening it to a bare
    // `if (match.hasMatch())` leaves the whole suite green. (The catalogued mutants flip == to
    // != , i.e. always-FALSE; the always-TRUE direction that DELETES the defense is the one
    // nothing covers.) Drive it with an UNANCHORED pattern -- the case the guard exists for --
    // and prove a substring hit is refused at either end, with a positive control showing the
    // pattern itself DOES match, so only the full-match guard can be doing the refusing.
    const QString unanchored = QStringLiteral(R"(kali-linux-[0-9][0-9.]*-installer-amd64\.iso)");
    QCOMPARE(LinuxDistroCatalog::filenameFromChecksums(
                 kHash + "  kali-linux-2026.2-installer-amd64.iso\n", unanchored),
             QStringLiteral("kali-linux-2026.2-installer-amd64.iso"));
    QVERIFY2(LinuxDistroCatalog::filenameFromChecksums(
                 kHash + "  mirror-kali-linux-2026.2-installer-amd64.iso\n", unanchored)
                 .isEmpty(),
             "a prefixed name must be refused: the match does not span the whole filename");
    QVERIFY2(LinuxDistroCatalog::filenameFromChecksums(
                 kHash + "  kali-linux-2026.2-installer-amd64.iso.bin\n", unanchored)
                 .isEmpty(),
             "a suffixed name must be refused: the match does not span the whole filename");

    // WHICH record wins is a documented contract (linux_distro_catalog.h:151 "returns the first
    // remaining filename that FULLY matches"): with exactly one matching record the fixture above
    // cannot tell first-wins from last-wins, so moving the in-loop `return filename;` (:610) to a
    // last-wins accumulate would silently pick a different image with no test noticing.
    QCOMPARE(LinuxDistroCatalog::filenameFromChecksums(
                 kHash + "  kali-linux-2026.2-installer-amd64.iso\n" + kHash +
                     "  kali-linux-2026.3-installer-amd64.iso\n",
                 kaliPattern()),
             QStringLiteral("kali-linux-2026.2-installer-amd64.iso"));
}

void TestLinuxDistroCatalog::testFilenameFromChecksumsDebian() {
    const QString sums = kHash + "  debian-live-13.6.0-amd64-kde.iso\n" + kHash +
                         "  debian-live-13.6.0-amd64-xfce.iso\n" + kHash +
                         "  debian-live-13.6.0-amd64-standard.iso\n" + kHash +
                         "  debian-live-13.6.0-amd64-gnome.iso\n";
    QCOMPARE(LinuxDistroCatalog::filenameFromChecksums(sums, debianPattern()),
             QStringLiteral("debian-live-13.6.0-amd64-gnome.iso"));
}

void TestLinuxDistroCatalog::testFilenameFromChecksumsBinaryMarker() {
    // GNU coreutils' binary-mode record uses "hash *file"; the '*' must be stripped.
    const QString sums = kHash + " *kali-linux-2026.3-installer-amd64.iso\n";
    QCOMPARE(LinuxDistroCatalog::filenameFromChecksums(sums, kaliPattern()),
             QStringLiteral("kali-linux-2026.3-installer-amd64.iso"));
}

void TestLinuxDistroCatalog::testFilenameFromChecksumsRejectsPathAndNonRecords() {
    // None of these is an acceptable record for the Kali installer image:
    //  - a comment line,
    //  - a matching name but under a subdirectory (path separator -> rejected so a
    //    checksums line can never redirect the download outside its directory),
    //  - a non-digest first token (prose, not a checksum record),
    //  - an unrelated file.
    const QString sums = "# Kali SHA256SUMS\n" + kHash +
                         "  subdir/kali-linux-2026.2-installer-amd64.iso\n"
                         "notahash  kali-linux-2026.2-installer-amd64.iso\n" +
                         kHash + "  README.txt\n";
    QVERIFY(LinuxDistroCatalog::filenameFromChecksums(sums, kaliPattern()).isEmpty());
}

void TestLinuxDistroCatalog::testFilenameFromChecksumsRejectsPathEvenWhenPatternPermits() {
    // Defense in depth that is independent of the anchored per-distro pattern: the
    // '/' and '\\' rejection must refuse a path-bearing name even when the pattern
    // itself would fully match it, so a checksums line can never splice a path
    // separator into the download URL. The permissive ".*" pattern below fully
    // matches both names (proving the pattern is not what rejects them); only the
    // path guard refuses them. Without that guard the discovery would return a name
    // carrying a subdirectory / traversal component.
    const QString permissive = QStringLiteral(R"(^kali-linux-.*-amd64\.iso$)");
    const QString slash = kHash + "  kali-linux-2026.2/installer-amd64.iso\n";
    QVERIFY(LinuxDistroCatalog::filenameFromChecksums(slash, permissive).isEmpty());
    const QString backslash = kHash + "  kali-linux-2026.2\\installer-amd64.iso\n";
    QVERIFY(LinuxDistroCatalog::filenameFromChecksums(backslash, permissive).isEmpty());
}

void TestLinuxDistroCatalog::testFilenameFromChecksumsEmptyOrInvalidPattern() {
    const QString sums = kHash + "  kali-linux-2026.2-installer-amd64.iso\n";
    // Empty pattern -> empty (feature disabled for this distro).
    QVERIFY(LinuxDistroCatalog::filenameFromChecksums(sums, QString()).isEmpty());
    // Invalid regex -> empty (fails closed, does not throw or match).
    QVERIFY(LinuxDistroCatalog::filenameFromChecksums(sums, QStringLiteral("[")).isEmpty());
    // Empty checksum body -> empty.
    QVERIFY(LinuxDistroCatalog::filenameFromChecksums(QString(), kaliPattern()).isEmpty());
}

void TestLinuxDistroCatalog::testRollingDistrosCarryPattern() {
    // The two rolling distros must carry the discovery pattern; a pinned-version
    // distro must not (so it keeps its static filename path).
    const auto kali = m_catalog->distroById("kali-linux");
    const auto debian = m_catalog->distroById("debian-live-gnome");
    QVERIFY(!kali.id.isEmpty());
    QVERIFY(!debian.id.isEmpty());
    QCOMPARE(kali.rollingFilenamePattern, kaliPattern());
    QCOMPARE(debian.rollingFilenamePattern, debianPattern());

    const auto ubuntu = m_catalog->distroById("ubuntu-desktop");
    if (!ubuntu.id.isEmpty()) {
        QVERIFY(ubuntu.rollingFilenamePattern.isEmpty());
    }
}

// ============================================================================
// Cancel (B10-08)
// ============================================================================

void TestLinuxDistroCatalog::testCancelAllWithNoPendingIsSafe() {
    // B10-08: cancelAll() snapshots+clears m_pendingReplies before aborting so a
    // finished()-driven removeOne() cannot invalidate the iteration. With no pending
    // replies it is a safe no-op; the catalog stays usable afterward.
    m_catalog->cancelAll();
    // !isEmpty() is a floor that survives cancelAll() wiping state it has no business touching:
    // it proves only that m_distros is non-empty. LinuxISODownloader::cancel() calls this on
    // EVERY user cancel, and cancelAll deliberately touches ONLY m_pendingReplies. A "cancel
    // everything" edit that also cleared m_distroIndex would leave allDistros() non-empty while
    // every distroById() returned the not-found sentinel; one that cleared m_githubAssetUrls
    // would leave a user who cancels one download and restarts a GitHub-backed one with an empty
    // download URL until another network version check. Pin the catalog, the index, and the
    // resolved-asset cache.
    QCOMPARE(m_catalog->allDistros().size(), 12);
    QCOMPARE(m_catalog->distroById("ubuntu-desktop").id, QStringLiteral("ubuntu-desktop"));

    LinuxDistroCatalog seeded;
    const QString asset = QStringLiteral(
        "https://github.com/ventoy/Ventoy/releases/download/v1.1.12/ventoy-1.1.12-livecd.iso");
    seeded.m_githubAssetUrls[QStringLiteral("ventoy")] = asset;  // friend access
    seeded.cancelAll();
    QCOMPARE(seeded.resolveDownloadUrl(seeded.distroById("ventoy")), asset);
    m_catalog->cancelAll();  // idempotent
}

// ============================================================================
// Cleanup
// ============================================================================

void TestLinuxDistroCatalog::cleanupTestCase() {
    m_catalog.reset();
}

QTEST_MAIN(TestLinuxDistroCatalog)
#include "test_linux_distro_catalog.moc"
