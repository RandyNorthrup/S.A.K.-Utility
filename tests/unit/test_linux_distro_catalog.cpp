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
    QVERIFY2(all.size() >= 5, "Catalog should have at least 5 distros");
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
    QVERIFY(names.contains(LinuxDistroCatalog::Category::GeneralPurpose));
    QVERIFY(names.contains(LinuxDistroCatalog::Category::Security));
    QVERIFY(names.contains(LinuxDistroCatalog::Category::SystemRecovery));
    QVERIFY(names.contains(LinuxDistroCatalog::Category::DiskTools));
    QVERIFY(names.contains(LinuxDistroCatalog::Category::Utilities));
}

void TestLinuxDistroCatalog::testDistrosByCategory() {
    auto general = m_catalog->distrosByCategory(LinuxDistroCatalog::Category::GeneralPurpose);
    QVERIFY(!general.isEmpty());

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
    for (auto cat : cats) {
        auto distros = m_catalog->distrosByCategory(cat);
        QVERIFY2(!distros.isEmpty(),
                 qPrintable("Category " + QString::number(static_cast<int>(cat)) +
                            " has no distros"));
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
    QVERIFY(ubuntu.name.contains("Ubuntu"));
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
    QVERIFY(url.contains(ubuntu.version));
    QVERIFY(!url.contains("{version}"));
}

void TestLinuxDistroCatalog::testResolveChecksumUrl() {
    Q_ASSERT(m_catalog);
    auto ubuntu = m_catalog->distroById("ubuntu-desktop");
    if (ubuntu.id.isEmpty()) {
        QSKIP("ubuntu-desktop not in catalog");
    }

    const QString url = m_catalog->resolveChecksumUrl(ubuntu);
    // Some distros may not have checksum URLs
    if (!ubuntu.checksumUrl.isEmpty()) {
        QVERIFY(!url.isEmpty());
    }
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
    QVERIFY(!kali.rollingFilenamePattern.isEmpty());
    QVERIFY(!debian.rollingFilenamePattern.isEmpty());

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
    QVERIFY(!m_catalog->allDistros().isEmpty());
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
