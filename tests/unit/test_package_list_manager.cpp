// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_package_list_manager.cpp
/// @brief Unit tests for PackageListManager

#include "sak/package_list_manager.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestPackageListManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Presets
    void presetNames_returnsAllFive();
    void preset_officePc_hasPackages();
    void preset_developer_hasPackages();
    void preset_kiosk_hasPackages();
    void preset_security_hasPackages();
    void preset_education_hasPackages();
    void preset_unknown_returnsEmpty();

    // List creation
    void createList_setsNameAndDescription();
    void createList_setsTimestamps();

    // Add/remove packages
    void addPackage_addsEntry();
    void addPackage_duplicateSkipped();
    void addPackage_withVersionAndNotes();
    void removePackage_removesEntry();
    void removePackage_notFound_returnsFalse();

    // Merge
    void mergeLists_addsNew();
    void mergeLists_skipsDuplicates();
    void mergeLists_returnsAddedCount();

    // Save/load
    void saveLoad_roundTrip();
    void loadFromFile_nonexistent_returnsEmpty();
    void saveToFile_invalidPath_returnsFalse();

    // Conversion
    void toPackagePairs_returnsCorrectPairs();
    void toPackagePairs_emptyList_returnsEmpty();
};

// ============================================================================
// Presets
// ============================================================================

void TestPackageListManager::presetNames_returnsAllFive() {
    sak::PackageListManager manager;
    auto names = manager.presetNames();
    QCOMPARE(names.size(), 5);
    QVERIFY(names.contains("Office PC"));
    QVERIFY(names.contains("Developer Workstation"));
    QVERIFY(names.contains("Kiosk / POS"));
    QVERIFY(names.contains("Security / IT Admin"));
    QVERIFY(names.contains("Education Lab"));
}

void TestPackageListManager::preset_officePc_hasPackages() {
    sak::PackageListManager manager;
    auto list = manager.preset("Office PC");
    QVERIFY(!list.entries.isEmpty());
    QVERIFY(!list.name.isEmpty());
}

void TestPackageListManager::preset_developer_hasPackages() {
    sak::PackageListManager manager;
    auto list = manager.preset("Developer Workstation");
    QVERIFY(!list.entries.isEmpty());
}

void TestPackageListManager::preset_kiosk_hasPackages() {
    sak::PackageListManager manager;
    auto list = manager.preset("Kiosk / POS");
    QVERIFY(!list.entries.isEmpty());
}

void TestPackageListManager::preset_security_hasPackages() {
    sak::PackageListManager manager;
    auto list = manager.preset("Security / IT Admin");
    QVERIFY(!list.entries.isEmpty());
}

void TestPackageListManager::preset_education_hasPackages() {
    sak::PackageListManager manager;
    auto list = manager.preset("Education Lab");
    QVERIFY(!list.entries.isEmpty());
}

void TestPackageListManager::preset_unknown_returnsEmpty() {
    sak::PackageListManager manager;
    auto list = manager.preset("Nonexistent Preset");
    QVERIFY(list.entries.isEmpty());
}

// ============================================================================
// List Creation
// ============================================================================

void TestPackageListManager::createList_setsNameAndDescription() {
    auto list = sak::PackageListManager::createList("My List", "A test package list");
    QCOMPARE(list.name, QString("My List"));
    QCOMPARE(list.description, QString("A test package list"));
}

void TestPackageListManager::createList_setsTimestamps() {
    auto list = sak::PackageListManager::createList("Test", "Test list");
    QVERIFY(!list.created_date.isEmpty());
    QVERIFY(!list.modified_date.isEmpty());
}

// ============================================================================
// Add/Remove
// ============================================================================

void TestPackageListManager::addPackage_addsEntry() {
    auto list = sak::PackageListManager::createList("Test", "");
    bool added = sak::PackageListManager::addPackage(list, "firefox");
    QVERIFY(added);
    QCOMPARE(list.entries.size(), 1);
    QCOMPARE(list.entries.first().package_id, QString("firefox"));
}

void TestPackageListManager::addPackage_duplicateSkipped() {
    auto list = sak::PackageListManager::createList("Test", "");
    sak::PackageListManager::addPackage(list, "firefox");
    bool added_again = sak::PackageListManager::addPackage(list, "firefox");
    QVERIFY(!added_again);
    QCOMPARE(list.entries.size(), 1);
}

void TestPackageListManager::addPackage_withVersionAndNotes() {
    auto list = sak::PackageListManager::createList("Test", "");
    sak::PackageListManager::addPackage(list, "nodejs", "18.20.0", "LTS version");
    QCOMPARE(list.entries.first().version, QString("18.20.0"));
    QCOMPARE(list.entries.first().notes, QString("LTS version"));
}

void TestPackageListManager::removePackage_removesEntry() {
    auto list = sak::PackageListManager::createList("Test", "");
    sak::PackageListManager::addPackage(list, "firefox");
    sak::PackageListManager::addPackage(list, "chrome");
    QCOMPARE(list.entries.size(), 2);

    bool removed = sak::PackageListManager::removePackage(list, "firefox");
    QVERIFY(removed);
    QCOMPARE(list.entries.size(), 1);
    QCOMPARE(list.entries.first().package_id, QString("chrome"));
}

void TestPackageListManager::removePackage_notFound_returnsFalse() {
    auto list = sak::PackageListManager::createList("Test", "");
    bool removed = sak::PackageListManager::removePackage(list, "nonexistent");
    QVERIFY(!removed);
}

// ============================================================================
// Merge
// ============================================================================

void TestPackageListManager::mergeLists_addsNew() {
    auto target = sak::PackageListManager::createList("Target", "");
    sak::PackageListManager::addPackage(target, "firefox");

    auto source = sak::PackageListManager::createList("Source", "");
    sak::PackageListManager::addPackage(source, "chrome");
    sak::PackageListManager::addPackage(source, "vlc");

    sak::PackageListManager::mergeLists(target, source);
    QCOMPARE(target.entries.size(), 3);
}

void TestPackageListManager::mergeLists_skipsDuplicates() {
    auto target = sak::PackageListManager::createList("Target", "");
    sak::PackageListManager::addPackage(target, "firefox");

    auto source = sak::PackageListManager::createList("Source", "");
    sak::PackageListManager::addPackage(source, "firefox");
    sak::PackageListManager::addPackage(source, "vlc");

    sak::PackageListManager::mergeLists(target, source);
    QCOMPARE(target.entries.size(), 2);
}

void TestPackageListManager::mergeLists_returnsAddedCount() {
    auto target = sak::PackageListManager::createList("Target", "");
    sak::PackageListManager::addPackage(target, "firefox");

    auto source = sak::PackageListManager::createList("Source", "");
    sak::PackageListManager::addPackage(source, "firefox");
    sak::PackageListManager::addPackage(source, "vlc");
    sak::PackageListManager::addPackage(source, "chrome");

    int added = sak::PackageListManager::mergeLists(target, source);
    QCOMPARE(added, 2);
}

// ============================================================================
// Save/Load
// ============================================================================

void TestPackageListManager::saveLoad_roundTrip() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    auto original = sak::PackageListManager::createList("RoundTrip Test", "Testing save/load");
    sak::PackageListManager::addPackage(original, "firefox", "130.0", "Main browser");
    sak::PackageListManager::addPackage(original, "vlc", "", "Media player");

    QString file_path = temp_dir.path() + "/test_list.json";

    bool saved = sak::PackageListManager::saveToFile(original, file_path);
    QVERIFY(saved);
    QVERIFY(QFile::exists(file_path));

    auto loaded = sak::PackageListManager::loadFromFile(file_path);
    QCOMPARE(loaded.name, original.name);
    QCOMPARE(loaded.description, original.description);
    QCOMPARE(loaded.entries.size(), original.entries.size());

    for (int index = 0; index < loaded.entries.size(); ++index) {
        QCOMPARE(loaded.entries[index].package_id, original.entries[index].package_id);
        QCOMPARE(loaded.entries[index].version, original.entries[index].version);
        QCOMPARE(loaded.entries[index].notes, original.entries[index].notes);
    }
}

void TestPackageListManager::loadFromFile_nonexistent_returnsEmpty() {
    auto list = sak::PackageListManager::loadFromFile("C:/nonexistent/path/file.json");
    QVERIFY(list.entries.isEmpty());
    QVERIFY(list.name.isEmpty());
}

void TestPackageListManager::saveToFile_invalidPath_returnsFalse() {
    auto list = sak::PackageListManager::createList("Test", "");
    bool saved = sak::PackageListManager::saveToFile(list, "Z:\\nonexistent\\dir\\file.json");
    QVERIFY(!saved);
}

// ============================================================================
// Conversion
// ============================================================================

void TestPackageListManager::toPackagePairs_returnsCorrectPairs() {
    auto list = sak::PackageListManager::createList("Test", "");
    sak::PackageListManager::addPackage(list, "firefox", "130.0");
    sak::PackageListManager::addPackage(list, "vlc");

    auto pairs = sak::PackageListManager::toPackagePairs(list);
    QCOMPARE(pairs.size(), 2);
    QCOMPARE(pairs[0].first, QString("firefox"));
    QCOMPARE(pairs[0].second, QString("130.0"));
    QCOMPARE(pairs[1].first, QString("vlc"));
    QVERIFY(pairs[1].second.isEmpty());
}

void TestPackageListManager::toPackagePairs_emptyList_returnsEmpty() {
    auto list = sak::PackageListManager::createList("Empty", "");
    auto pairs = sak::PackageListManager::toPackagePairs(list);
    QVERIFY(pairs.isEmpty());
}

QTEST_GUILESS_MAIN(TestPackageListManager)
#include "test_package_list_manager.moc"
