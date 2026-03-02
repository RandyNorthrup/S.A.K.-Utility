// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QTest>
#include "sak/linux_distro_catalog.h"

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

    // ── Catalog population ──────────────────────────────────
    void testCatalogNonEmpty();
    void testAllDistrosHaveIds();
    void testAllDistrosHaveNames();
    void testAllDistrosHaveDescriptions();
    void testAllDistrosHavePositiveSize();
    void testNoDuplicateIds();

    // ── Category operations ─────────────────────────────────
    void testCategoryNames();
    void testDistrosByCategory();
    void testAllCategoriesPresent();

    // ── ID lookup ───────────────────────────────────────────
    void testDistroByIdFound();
    void testDistroByIdNotFound();

    // ── URL resolution ──────────────────────────────────────
    void testResolveDownloadUrl();
    void testResolveChecksumUrl();
    void testResolveFileName();

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
    QVERIFY2(!all.isEmpty(),
             "Catalog should contain distros");
    QVERIFY2(all.size() >= 5,
             "Catalog should have at least 5 distros");
}

void TestLinuxDistroCatalog::testAllDistrosHaveIds() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!d.id.isEmpty(),
                 qPrintable(
                     "Distro has empty id: " + d.name));
    }
}

void TestLinuxDistroCatalog::testAllDistrosHaveNames() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!d.name.isEmpty(),
                 qPrintable(
                     "Distro has empty name, id=" + d.id));
    }
}

void TestLinuxDistroCatalog::testAllDistrosHaveDescriptions() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!d.description.isEmpty(),
                 qPrintable(
                     "Distro '" + d.name
                     + "' has empty description"));
    }
}

void TestLinuxDistroCatalog::testAllDistrosHavePositiveSize() {
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(d.approximateSize > 0,
                 qPrintable(
                     "Distro '" + d.name
                     + "' has non-positive size"));
    }
}

void TestLinuxDistroCatalog::testNoDuplicateIds() {
    QSet<QString> ids;
    for (const auto& d : m_catalog->allDistros()) {
        QVERIFY2(!ids.contains(d.id),
                 qPrintable(
                     "Duplicate distro id: " + d.id));
        ids.insert(d.id);
    }
}

// ============================================================================
// Category operations
// ============================================================================

void TestLinuxDistroCatalog::testCategoryNames() {
    const auto names =
        LinuxDistroCatalog::categoryNames();
    QCOMPARE(names.size(), 5);
    QVERIFY(names.contains(
        LinuxDistroCatalog::Category::GeneralPurpose));
    QVERIFY(names.contains(
        LinuxDistroCatalog::Category::Security));
    QVERIFY(names.contains(
        LinuxDistroCatalog::Category::SystemRecovery));
    QVERIFY(names.contains(
        LinuxDistroCatalog::Category::DiskTools));
    QVERIFY(names.contains(
        LinuxDistroCatalog::Category::Utilities));
}

void TestLinuxDistroCatalog::testDistrosByCategory() {
    auto general = m_catalog->distrosByCategory(
        LinuxDistroCatalog::Category::GeneralPurpose);
    QVERIFY(!general.isEmpty());

    // All returned distros should be in the right category
    for (const auto& d : general) {
        QCOMPARE(d.category,
                 LinuxDistroCatalog::Category::GeneralPurpose);
    }
}

void TestLinuxDistroCatalog::testAllCategoriesPresent() {
    const QList<LinuxDistroCatalog::Category> cats = {
        LinuxDistroCatalog::Category::GeneralPurpose,
        LinuxDistroCatalog::Category::Security,
        LinuxDistroCatalog::Category::SystemRecovery,
        LinuxDistroCatalog::Category::DiskTools,
        LinuxDistroCatalog::Category::Utilities,
    };
    for (auto cat : cats) {
        auto distros =
            m_catalog->distrosByCategory(cat);
        QVERIFY2(!distros.isEmpty(),
                 qPrintable(
                     "Category "
                     + QString::number(
                         static_cast<int>(cat))
                     + " has no distros"));
    }
}

// ============================================================================
// ID lookup
// ============================================================================

void TestLinuxDistroCatalog::testDistroByIdFound() {
    // Ubuntu Desktop should always be in catalog
    auto ubuntu =
        m_catalog->distroById("ubuntu-desktop");
    QVERIFY2(!ubuntu.id.isEmpty(),
             "ubuntu-desktop not found in catalog");
    QCOMPARE(ubuntu.id, "ubuntu-desktop");
    QVERIFY(ubuntu.name.contains("Ubuntu"));
}

void TestLinuxDistroCatalog::testDistroByIdNotFound() {
    auto missing =
        m_catalog->distroById("nonexistent-distro");
    QVERIFY(missing.id.isEmpty());
}

// ============================================================================
// URL resolution
// ============================================================================

void TestLinuxDistroCatalog::testResolveDownloadUrl() {
    auto ubuntu =
        m_catalog->distroById("ubuntu-desktop");
    if (ubuntu.id.isEmpty()) {
        QSKIP("ubuntu-desktop not in catalog");
    }

    const QString url =
        m_catalog->resolveDownloadUrl(ubuntu);
    QVERIFY(!url.isEmpty());
    QVERIFY(url.contains(ubuntu.version));
    QVERIFY(!url.contains("{version}"));
}

void TestLinuxDistroCatalog::testResolveChecksumUrl() {
    auto ubuntu =
        m_catalog->distroById("ubuntu-desktop");
    if (ubuntu.id.isEmpty()) {
        QSKIP("ubuntu-desktop not in catalog");
    }

    const QString url =
        m_catalog->resolveChecksumUrl(ubuntu);
    // Some distros may not have checksum URLs
    if (!ubuntu.checksumUrl.isEmpty()) {
        QVERIFY(!url.isEmpty());
    }
}

void TestLinuxDistroCatalog::testResolveFileName() {
    auto ubuntu =
        m_catalog->distroById("ubuntu-desktop");
    if (ubuntu.id.isEmpty()) {
        QSKIP("ubuntu-desktop not in catalog");
    }

    const QString name =
        m_catalog->resolveFileName(ubuntu);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.endsWith(".iso"));
    QVERIFY(!name.contains("{version}"));
}

// ============================================================================
// Cleanup
// ============================================================================

void TestLinuxDistroCatalog::cleanupTestCase() {
    m_catalog.reset();
}

QTEST_MAIN(TestLinuxDistroCatalog)
#include "test_linux_distro_catalog.moc"
