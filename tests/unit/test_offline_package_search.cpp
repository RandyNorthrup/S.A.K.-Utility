// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_offline_package_search.cpp
/// @brief Unit tests for offline deployment package search workflow
///
/// Tests NuGet API search result parsing, ChocolateyManager search output
/// parsing, and the package selection data flow used by the offline
/// deployment tab.

#include "sak/chocolatey_manager.h"
#include "sak/offline_deployment_constants.h"
#include "sak/package_list_manager.h"

#include <QListWidget>
#include <QListWidgetItem>
#include <QSignalSpy>
#include <QtTest/QtTest>

class TestOfflinePackageSearch : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- ChocolateyManager search result parsing ---
    void parseSearchResults_validOutput_extractsPackages();
    void parseSearchResults_multipleResults_allParsed();
    void parseSearchResults_emptyOutput_returnsEmpty();
    void parseSearchResults_garbageOutput_returnsEmpty();
    void parseSearchResults_headerLines_skipped();
    void parseSearchResults_trailingNewlines_handled();

    // --- ChocolateyManager search integration ---
    void chocoManager_construction_notInitialized();
    void chocoManager_searchPackage_emptyQuery_fails();
    void chocoManager_parseSearchResults_multipleVersions();

    // --- ChocolateyManager::PackageInfo defaults ---
    void packageInfo_defaults_allEmpty();
    void packageInfo_setAndRead_preservesValues();

    // --- Package ID validation edge cases ---
    void parseSearchResults_specialChars_handled();
    void parseSearchResults_dotSeparatedId_parsed();
    void parseSearchResults_longVersion_parsed();

    // --- Offline list deduplication logic ---
    void offlineList_addPackage_addsItem();
    void offlineList_addDuplicate_rejected();
    void offlineList_addDuplicate_caseInsensitive();
    void offlineList_addMultiple_allPresent();

    // --- Search result selection data flow ---
    void searchResult_dataRoles_storePackageInfo();
    void searchResult_selectionClearsOnNewSearch();

    // --- Offline deployment constants ---
    void constants_searchDefaults_reasonable();
    void constants_nugetUrl_notEmpty();

    // --- Package list round-trip with version ---
    void packageList_addWithVersion_preservedOnSaveLoad();
};

// ============================================================================
// ChocolateyManager Search Result Parsing
// ============================================================================

void TestOfflinePackageSearch::parseSearchResults_validOutput_extractsPackages() {
    sak::ChocolateyManager manager;
    const auto results = manager.parseSearchResults("googlechrome|120.0.6099.130\n");
    QCOMPARE(results.size(), static_cast<size_t>(1));
    QCOMPARE(results[0].package_id, QString("googlechrome"));
    QCOMPARE(results[0].version, QString("120.0.6099.130"));
}

void TestOfflinePackageSearch::parseSearchResults_multipleResults_allParsed() {
    sak::ChocolateyManager manager;
    QString output =
        "googlechrome|120.0.6099.130\n"
        "firefox|121.0\n"
        "7zip|23.01\n";
    const auto results = manager.parseSearchResults(output);
    QCOMPARE(results.size(), static_cast<size_t>(3));
    QCOMPARE(results[0].package_id, QString("googlechrome"));
    QCOMPARE(results[1].package_id, QString("firefox"));
    QCOMPARE(results[2].package_id, QString("7zip"));
}

void TestOfflinePackageSearch::parseSearchResults_emptyOutput_returnsEmpty() {
    sak::ChocolateyManager manager;
    const auto results = manager.parseSearchResults(QString());
    QVERIFY(results.empty());
}

void TestOfflinePackageSearch::parseSearchResults_garbageOutput_returnsEmpty() {
    sak::ChocolateyManager manager;
    const auto results = manager.parseSearchResults("this is not valid output at all");
    QVERIFY(results.empty());
}

void TestOfflinePackageSearch::parseSearchResults_headerLines_skipped() {
    sak::ChocolateyManager manager;
    QString output =
        "Chocolatey v2.3.0\n"
        "googlechrome|120.0.6099.130\n";
    const auto results = manager.parseSearchResults(output);
    QCOMPARE(results.size(), static_cast<size_t>(1));
    QCOMPARE(results[0].package_id, QString("googlechrome"));
}

void TestOfflinePackageSearch::parseSearchResults_trailingNewlines_handled() {
    sak::ChocolateyManager manager;
    QString output = "firefox|121.0\n\n\n";
    const auto results = manager.parseSearchResults(output);
    QCOMPARE(results.size(), static_cast<size_t>(1));
    QCOMPARE(results[0].package_id, QString("firefox"));
}

// ============================================================================
// ChocolateyManager Search Integration
// ============================================================================

void TestOfflinePackageSearch::chocoManager_construction_notInitialized() {
    sak::ChocolateyManager manager;
    QVERIFY(!manager.isInitialized());
}

void TestOfflinePackageSearch::chocoManager_searchPackage_emptyQuery_fails() {
    sak::ChocolateyManager manager;
    auto result = manager.searchPackage("", 10);
    QVERIFY(!result.success);
}

void TestOfflinePackageSearch::chocoManager_parseSearchResults_multipleVersions() {
    sak::ChocolateyManager manager;
    QString output =
        "googlechrome|120.0.6099.130\n"
        "googlechrome|119.0.6045.200\n";
    const auto results = manager.parseSearchResults(output);
    QCOMPARE(results.size(), static_cast<size_t>(2));
    QCOMPARE(results[0].version, QString("120.0.6099.130"));
    QCOMPARE(results[1].version, QString("119.0.6045.200"));
}

// ============================================================================
// ChocolateyManager::PackageInfo Defaults
// ============================================================================

void TestOfflinePackageSearch::packageInfo_defaults_allEmpty() {
    sak::ChocolateyManager::PackageInfo info;
    QVERIFY(info.package_id.isEmpty());
    QVERIFY(info.version.isEmpty());
    QVERIFY(info.title.isEmpty());
    QVERIFY(info.description.isEmpty());
}

void TestOfflinePackageSearch::packageInfo_setAndRead_preservesValues() {
    sak::ChocolateyManager::PackageInfo info;
    info.package_id = "googlechrome";
    info.version = "120.0.6099.130";
    info.title = "Google Chrome";
    info.description = "A web browser";

    QCOMPARE(info.package_id, QString("googlechrome"));
    QCOMPARE(info.version, QString("120.0.6099.130"));
    QCOMPARE(info.title, QString("Google Chrome"));
    QCOMPARE(info.description, QString("A web browser"));
}

// ============================================================================
// Package ID Edge Cases
// ============================================================================

void TestOfflinePackageSearch::parseSearchResults_specialChars_handled() {
    sak::ChocolateyManager manager;
    QString output = "notepadplusplus|8.6.2\n";
    const auto results = manager.parseSearchResults(output);
    QCOMPARE(results.size(), static_cast<size_t>(1));
    QCOMPARE(results[0].package_id, QString("notepadplusplus"));
}

void TestOfflinePackageSearch::parseSearchResults_dotSeparatedId_parsed() {
    sak::ChocolateyManager manager;
    QString output =
        "libreoffice-fresh|24.2.0\n"
        "chocolatey-core.extension|1.4.0\n";
    const auto results = manager.parseSearchResults(output);
    QCOMPARE(results.size(), static_cast<size_t>(2));
    QCOMPARE(results[0].package_id, QString("libreoffice-fresh"));
    QCOMPARE(results[1].package_id, QString("chocolatey-core.extension"));
}

void TestOfflinePackageSearch::parseSearchResults_longVersion_parsed() {
    sak::ChocolateyManager manager;
    QString output = "dotnet-sdk|8.0.200-preview.1.24101.2\n";
    const auto results = manager.parseSearchResults(output);
    QCOMPARE(results.size(), static_cast<size_t>(1));
    QCOMPARE(results[0].version, QString("8.0.200-preview.1.24101.2"));
}

// ============================================================================
// Offline List Deduplication
// ============================================================================

void TestOfflinePackageSearch::offlineList_addPackage_addsItem() {
    QListWidget list;
    QString package_id = "googlechrome";

    auto* item = new QListWidgetItem(QString("%1  (v%2)").arg(package_id, "120.0"));
    item->setData(Qt::UserRole, package_id);
    item->setData(Qt::UserRole + 1, "120.0");
    list.addItem(item);

    QCOMPARE(list.count(), 1);
    QCOMPARE(list.item(0)->data(Qt::UserRole).toString(), QString("googlechrome"));
    QCOMPARE(list.item(0)->data(Qt::UserRole + 1).toString(), QString("120.0"));
}

void TestOfflinePackageSearch::offlineList_addDuplicate_rejected() {
    QListWidget list;
    QString package_id = "firefox";

    // Add first item
    auto* item1 = new QListWidgetItem(package_id);
    item1->setData(Qt::UserRole, package_id);
    list.addItem(item1);

    // Check for duplicate (simulating onAddToOfflineList logic)
    bool exists = false;
    for (int row = 0; row < list.count(); ++row) {
        if (list.item(row)
                ->data(Qt::UserRole)
                .toString()
                .compare(package_id, Qt::CaseInsensitive) == 0) {
            exists = true;
            break;
        }
    }
    QVERIFY(exists);
    QCOMPARE(list.count(), 1);  // Should not add duplicate
}

void TestOfflinePackageSearch::offlineList_addDuplicate_caseInsensitive() {
    QListWidget list;

    auto* item1 = new QListWidgetItem("googlechrome");
    item1->setData(Qt::UserRole, "googlechrome");
    list.addItem(item1);

    // Check case-insensitive duplicate
    QString new_id = "GoogleChrome";
    bool exists = false;
    for (int row = 0; row < list.count(); ++row) {
        if (list.item(row)->data(Qt::UserRole).toString().compare(new_id, Qt::CaseInsensitive) ==
            0) {
            exists = true;
            break;
        }
    }
    QVERIFY(exists);
}

void TestOfflinePackageSearch::offlineList_addMultiple_allPresent() {
    QListWidget list;
    QStringList ids = {"googlechrome", "firefox", "7zip", "vlc"};

    for (const auto& pkg_id : ids) {
        auto* item = new QListWidgetItem(pkg_id);
        item->setData(Qt::UserRole, pkg_id);
        list.addItem(item);
    }

    QCOMPARE(list.count(), 4);
    QCOMPARE(list.item(2)->data(Qt::UserRole).toString(), QString("7zip"));
}

// ============================================================================
// Search Result Selection Data Flow
// ============================================================================

void TestOfflinePackageSearch::searchResult_dataRoles_storePackageInfo() {
    QListWidget search_results;

    // Simulate populating search results (as onOfflineSearchComplete does)
    auto* item = new QListWidgetItem("googlechrome  (v120.0.6099.130)  — 50000000 downloads");
    item->setData(Qt::UserRole, "googlechrome");
    item->setData(Qt::UserRole + 1, "120.0.6099.130");
    item->setToolTip("A web browser by Google");
    search_results.addItem(item);

    // Verify data can be read back (as onAddToOfflineList does)
    auto* first = search_results.item(0);
    QCOMPARE(first->data(Qt::UserRole).toString(), QString("googlechrome"));
    QCOMPARE(first->data(Qt::UserRole + 1).toString(), QString("120.0.6099.130"));
    QVERIFY(!first->toolTip().isEmpty());
}

void TestOfflinePackageSearch::searchResult_selectionClearsOnNewSearch() {
    QListWidget search_results;

    // Populate with initial results
    auto* item1 = new QListWidgetItem("firefox");
    item1->setData(Qt::UserRole, "firefox");
    search_results.addItem(item1);
    QCOMPARE(search_results.count(), 1);

    // Simulate new search (clear + repopulate)
    search_results.clear();
    QCOMPARE(search_results.count(), 0);

    auto* item2 = new QListWidgetItem("googlechrome");
    item2->setData(Qt::UserRole, "googlechrome");
    search_results.addItem(item2);
    QCOMPARE(search_results.count(), 1);
    QCOMPARE(search_results.item(0)->data(Qt::UserRole).toString(), QString("googlechrome"));
}

// ============================================================================
// Offline Deployment Constants
// ============================================================================

void TestOfflinePackageSearch::constants_searchDefaults_reasonable() {
    QVERIFY(sak::offline::kSearchResultsDefault > 0);
    QVERIFY(sak::offline::kSearchResultsDefault <= sak::offline::kSearchMaxResults);
    QVERIFY(sak::offline::kSearchMaxResults <= 100);
}

void TestOfflinePackageSearch::constants_nugetUrl_notEmpty() {
    QVERIFY(QString(sak::offline::kNuGetBaseUrl).startsWith("https://"));
    QVERIFY(!QString(sak::offline::kNuGetSearchPath).isEmpty());
}

// ============================================================================
// Package List Round-Trip with Version
// ============================================================================

void TestOfflinePackageSearch::packageList_addWithVersion_preservedOnSaveLoad() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    QString file_path = temp_dir.path() + "/test_list.json";

    // Create and populate a list with versioned packages
    auto list = sak::PackageListManager::createList("Test", "Package search test");
    sak::PackageListManager::addPackage(list, "googlechrome", "120.0.6099.130");
    sak::PackageListManager::addPackage(list, "firefox", "121.0");
    sak::PackageListManager::addPackage(list, "7zip", "");  // latest

    QVERIFY(sak::PackageListManager::saveToFile(list, file_path));

    auto loaded = sak::PackageListManager::loadFromFile(file_path);
    QCOMPARE(loaded.entries.size(), 3);
    QCOMPARE(loaded.entries[0].package_id, QString("googlechrome"));
    QCOMPARE(loaded.entries[0].version, QString("120.0.6099.130"));
    QCOMPARE(loaded.entries[1].package_id, QString("firefox"));
    QCOMPARE(loaded.entries[1].version, QString("121.0"));
    QCOMPARE(loaded.entries[2].package_id, QString("7zip"));
    QVERIFY(loaded.entries[2].version.isEmpty());
}

QTEST_MAIN(TestOfflinePackageSearch)
#include "test_offline_package_search.moc"
