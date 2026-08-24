// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_uninstall_types.cpp
/// @brief Unit tests for Advanced Uninstall shared data types

#include "sak/advanced_uninstall_types.h"
#include "sak/leftover_scan_provenance.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <type_traits>

class AdvancedUninstallTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- ProgramInfo --
    void programInfo_defaultConstruction();
    void programInfo_valueSemantics();
    void programInfo_moveSemantics();
    void programInfo_sourceEnum();

    // -- ScanLevel --
    void scanLevel_enumValues();

    // -- LeftoverItem --
    void leftoverItem_defaultConstruction();
    void leftoverItem_typeEnum();
    void leftoverItem_riskEnum();
    void leftoverItem_valueSemantics();

    // -- UninstallReport --
    void uninstallReport_defaultConstruction();
    void uninstallReport_resultEnum();
    void uninstallReport_valueSemantics();

    // -- UninstallQueueItem --
    void queueItem_defaultConstruction();
    void queueItem_statusEnum();
    void queueItem_valueSemantics();

    // -- ViewFilter --
    void viewFilter_enumValues();

    // -- Leftover scan provenance (proof-of-scan binding) --
    void provenanceKey_stableAcrossFormatting();
    void provenanceKey_distinctForDifferentItems();
    void provenanceStore_recordsAndMatches();
    void provenanceStore_bindsRealObjectIdentity();

    // -- Compile-Time Invariants --
    void staticAsserts_defaultConstructible();
    void staticAsserts_copyConstructible();
    void staticAsserts_movable();
};

// -- ProgramInfo -------------------------------------------------------------

void AdvancedUninstallTypesTests::programInfo_defaultConstruction() {
    sak::ProgramInfo info;

    QVERIFY(info.displayName.isEmpty());
    QVERIFY(info.publisher.isEmpty());
    QVERIFY(info.displayVersion.isEmpty());
    QVERIFY(info.installDate.isEmpty());
    QVERIFY(info.installLocation.isEmpty());
    QVERIFY(info.uninstallString.isEmpty());
    QVERIFY(info.quietUninstallString.isEmpty());
    QVERIFY(info.modifyPath.isEmpty());
    QVERIFY(info.displayIcon.isEmpty());
    QVERIFY(info.registryKeyPath.isEmpty());
    QCOMPARE(info.estimatedSizeKB, 0);
    QCOMPARE(info.actualSizeBytes, 0);
    QCOMPARE(info.source, sak::ProgramInfo::Source::RegistryHKLM);
    QVERIFY(info.packageFamilyName.isEmpty());
    QVERIFY(info.packageFullName.isEmpty());
    QVERIFY(!info.isSystemComponent);
    QVERIFY(!info.isOrphaned);
    QVERIFY(!info.isBloatware);
    QVERIFY(info.cachedImage.isNull());
}

void AdvancedUninstallTypesTests::programInfo_valueSemantics() {
    sak::ProgramInfo original;
    original.displayName = "Test App";
    original.publisher = "Test Publisher";
    original.displayVersion = "1.2.3";
    original.installDate = "20250101";
    original.installLocation = "C:\\Program Files\\TestApp";
    original.uninstallString = "C:\\uninst.exe";
    original.registryKeyPath = "HKLM\\SOFTWARE\\TestApp";
    original.estimatedSizeKB = 1024;
    original.actualSizeBytes = 1'048'576;
    original.source = sak::ProgramInfo::Source::UWP;
    original.packageFamilyName = "TestApp_abc123";
    original.isSystemComponent = true;
    original.isOrphaned = true;
    original.isBloatware = true;

    // Copy
    sak::ProgramInfo copy = original;
    QCOMPARE(copy.displayName, original.displayName);
    QCOMPARE(copy.publisher, original.publisher);
    QCOMPARE(copy.displayVersion, original.displayVersion);
    QCOMPARE(copy.installLocation, original.installLocation);
    QCOMPARE(copy.uninstallString, original.uninstallString);
    QCOMPARE(copy.registryKeyPath, original.registryKeyPath);
    QCOMPARE(copy.estimatedSizeKB, original.estimatedSizeKB);
    QCOMPARE(copy.actualSizeBytes, original.actualSizeBytes);
    QCOMPARE(copy.source, original.source);
    QCOMPARE(copy.packageFamilyName, original.packageFamilyName);
    QCOMPARE(copy.isSystemComponent, original.isSystemComponent);
    QCOMPARE(copy.isOrphaned, original.isOrphaned);
    QCOMPARE(copy.isBloatware, original.isBloatware);

    // Assignment
    sak::ProgramInfo assigned;
    assigned = original;
    QCOMPARE(assigned.displayName, original.displayName);
    QCOMPARE(assigned.source, sak::ProgramInfo::Source::UWP);
}

void AdvancedUninstallTypesTests::programInfo_moveSemantics() {
    sak::ProgramInfo original;
    original.displayName = "MovableApp";
    original.estimatedSizeKB = 512;

    sak::ProgramInfo moved = std::move(original);
    QCOMPARE(moved.displayName, "MovableApp");
    QCOMPARE(moved.estimatedSizeKB, 512);
}

void AdvancedUninstallTypesTests::programInfo_sourceEnum() {
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::RegistryHKLM), 0);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::RegistryHKLM_WOW64), 1);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::RegistryHKCU), 2);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::UWP), 3);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::Provisioned), 4);
}

// -- ScanLevel ---------------------------------------------------------------

void AdvancedUninstallTypesTests::scanLevel_enumValues() {
    QCOMPARE(static_cast<int>(sak::ScanLevel::Safe), 0);
    QCOMPARE(static_cast<int>(sak::ScanLevel::Moderate), 1);
    QCOMPARE(static_cast<int>(sak::ScanLevel::Advanced), 2);
}

// -- LeftoverItem ------------------------------------------------------------

void AdvancedUninstallTypesTests::leftoverItem_defaultConstruction() {
    sak::LeftoverItem item;

    QCOMPARE(item.type, sak::LeftoverItem::Type::File);
    QCOMPARE(item.risk, sak::LeftoverItem::RiskLevel::Safe);
    QVERIFY(item.path.isEmpty());
    QVERIFY(item.description.isEmpty());
    QCOMPARE(item.sizeBytes, 0);
    QVERIFY(!item.selected);
    QVERIFY(item.registryValueName.isEmpty());
    QVERIFY(item.registryValueData.isEmpty());
}

void AdvancedUninstallTypesTests::leftoverItem_typeEnum() {
    // Pin each enumerator's ordinal. The old distinct-count check (size == 9) passed under any
    // reordering, but these ordinals are stamped onto persisted leftover items, so a reorder is a
    // data-compat break the count check could not catch.
    using T = sak::LeftoverItem::Type;
    QCOMPARE(static_cast<int>(T::File), 0);
    QCOMPARE(static_cast<int>(T::Folder), 1);
    QCOMPARE(static_cast<int>(T::RegistryKey), 2);
    QCOMPARE(static_cast<int>(T::RegistryValue), 3);
    QCOMPARE(static_cast<int>(T::Service), 4);
    QCOMPARE(static_cast<int>(T::ScheduledTask), 5);
    QCOMPARE(static_cast<int>(T::FirewallRule), 6);
    QCOMPARE(static_cast<int>(T::StartupEntry), 7);
    QCOMPARE(static_cast<int>(T::ShellExtension), 8);
}

void AdvancedUninstallTypesTests::leftoverItem_riskEnum() {
    QCOMPARE(static_cast<int>(sak::LeftoverItem::RiskLevel::Safe), 0);
    QCOMPARE(static_cast<int>(sak::LeftoverItem::RiskLevel::Review), 1);
    QCOMPARE(static_cast<int>(sak::LeftoverItem::RiskLevel::Risky), 2);
}

void AdvancedUninstallTypesTests::leftoverItem_valueSemantics() {
    sak::LeftoverItem original;
    original.type = sak::LeftoverItem::Type::RegistryKey;
    original.risk = sak::LeftoverItem::RiskLevel::Risky;
    original.path = "HKLM\\SOFTWARE\\TestApp";
    original.description = "Leftover registry key";
    original.sizeBytes = 4096;
    original.selected = true;
    original.registryValueName = "ValueName";
    original.registryValueData = "ValueData";

    // Copy
    sak::LeftoverItem copy = original;
    QCOMPARE(copy.type, original.type);
    QCOMPARE(copy.risk, original.risk);
    QCOMPARE(copy.path, original.path);
    QCOMPARE(copy.description, original.description);
    QCOMPARE(copy.sizeBytes, original.sizeBytes);
    QCOMPARE(copy.selected, original.selected);
    QCOMPARE(copy.registryValueName, original.registryValueName);
    QCOMPARE(copy.registryValueData, original.registryValueData);

    // Move
    sak::LeftoverItem moved = std::move(copy);
    QCOMPARE(moved.type, sak::LeftoverItem::Type::RegistryKey);
    QCOMPARE(moved.path, original.path);
}

// -- UninstallReport ---------------------------------------------------------

void AdvancedUninstallTypesTests::uninstallReport_defaultConstruction() {
    sak::UninstallReport report;

    QVERIFY(report.programName.isEmpty());
    QVERIFY(report.programVersion.isEmpty());
    QVERIFY(report.programPublisher.isEmpty());
    QVERIFY(!report.restorePointCreated);
    QVERIFY(report.restorePointName.isEmpty());
    QCOMPARE(report.uninstallResult, sak::UninstallReport::UninstallResult::Success);
    QCOMPARE(report.nativeExitCode, 0);
    QCOMPARE(report.scanLevel, sak::ScanLevel::Moderate);
    QVERIFY(report.foundLeftovers.isEmpty());
    QCOMPARE(report.filesDeleted, 0);
    QCOMPARE(report.foldersDeleted, 0);
    QCOMPARE(report.registryKeysDeleted, 0);
    QCOMPARE(report.registryValuesDeleted, 0);
    QCOMPARE(report.servicesRemoved, 0);
    QCOMPARE(report.tasksRemoved, 0);
    QCOMPARE(report.firewallRulesRemoved, 0);
    QCOMPARE(report.startupEntriesRemoved, 0);
    QCOMPARE(report.failedDeletions, 0);
    QCOMPARE(report.totalSpaceRecovered, 0);
    QVERIFY(report.errorLog.isEmpty());
}

void AdvancedUninstallTypesTests::uninstallReport_resultEnum() {
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Success), 0);
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Failed), 1);
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Cancelled), 2);
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Skipped), 3);
}

void AdvancedUninstallTypesTests::uninstallReport_valueSemantics() {
    sak::UninstallReport original;
    original.programName = "TestProg";
    original.programVersion = "2.0";
    original.restorePointCreated = true;
    original.restorePointName = "Before TestProg";
    original.uninstallResult = sak::UninstallReport::UninstallResult::Skipped;
    original.nativeExitCode = 42;
    original.scanLevel = sak::ScanLevel::Advanced;
    original.filesDeleted = 10;
    original.foldersDeleted = 3;
    original.totalSpaceRecovered = 999'999;
    original.errorLog.append("some error");

    sak::LeftoverItem leftover;
    leftover.path = "C:\\test";
    original.foundLeftovers.append(leftover);

    // Copy
    sak::UninstallReport copy = original;
    QCOMPARE(copy.programName, "TestProg");
    QCOMPARE(copy.restorePointCreated, true);
    QCOMPARE(copy.uninstallResult, sak::UninstallReport::UninstallResult::Skipped);
    QCOMPARE(copy.nativeExitCode, 42);
    QCOMPARE(copy.filesDeleted, 10);
    QCOMPARE(copy.totalSpaceRecovered, 999'999);
    QCOMPARE(copy.foundLeftovers.size(), 1);
    QCOMPARE(copy.foundLeftovers[0].path, "C:\\test");
    QCOMPARE(copy.errorLog.size(), 1);
}

// -- UninstallQueueItem ------------------------------------------------------

void AdvancedUninstallTypesTests::queueItem_defaultConstruction() {
    sak::UninstallQueueItem item;

    QVERIFY(item.program.displayName.isEmpty());
    QCOMPARE(item.scanLevel, sak::ScanLevel::Moderate);
    QVERIFY(item.autoCleanSafeLeftovers);
    QCOMPARE(item.status, sak::UninstallQueueItem::Status::Queued);
}

void AdvancedUninstallTypesTests::queueItem_statusEnum() {
    // Pin each ordinal; the distinct-count check passed under any reordering.
    using S = sak::UninstallQueueItem::Status;
    QCOMPARE(static_cast<int>(S::Queued), 0);
    QCOMPARE(static_cast<int>(S::InProgress), 1);
    QCOMPARE(static_cast<int>(S::Completed), 2);
    QCOMPARE(static_cast<int>(S::Failed), 3);
    QCOMPARE(static_cast<int>(S::Cancelled), 4);
}

void AdvancedUninstallTypesTests::queueItem_valueSemantics() {
    sak::UninstallQueueItem original;
    original.program.displayName = "Queued App";
    original.scanLevel = sak::ScanLevel::Advanced;
    original.autoCleanSafeLeftovers = false;
    original.status = sak::UninstallQueueItem::Status::InProgress;
    original.report.programName = "Queued App";

    sak::UninstallQueueItem copy = original;
    QCOMPARE(copy.program.displayName, "Queued App");
    QCOMPARE(copy.scanLevel, sak::ScanLevel::Advanced);
    QVERIFY(!copy.autoCleanSafeLeftovers);
    QCOMPARE(copy.status, sak::UninstallQueueItem::Status::InProgress);
    QCOMPARE(copy.report.programName, "Queued App");
}

// -- ViewFilter --------------------------------------------------------------

void AdvancedUninstallTypesTests::viewFilter_enumValues() {
    QCOMPARE(static_cast<int>(sak::ViewFilter::All), 0);
    QCOMPARE(static_cast<int>(sak::ViewFilter::Win32Only), 1);
    QCOMPARE(static_cast<int>(sak::ViewFilter::UwpOnly), 2);
    QCOMPARE(static_cast<int>(sak::ViewFilter::BloatwareOnly), 3);
    QCOMPARE(static_cast<int>(sak::ViewFilter::OrphanedOnly), 4);
}

// -- Compile-Time Invariants -------------------------------------------------

void AdvancedUninstallTypesTests::staticAsserts_defaultConstructible() {
    QVERIFY(std::is_default_constructible_v<sak::ProgramInfo>);
    QVERIFY(std::is_default_constructible_v<sak::LeftoverItem>);
    QVERIFY(std::is_default_constructible_v<sak::UninstallReport>);
    QVERIFY(std::is_default_constructible_v<sak::UninstallQueueItem>);
}

void AdvancedUninstallTypesTests::staticAsserts_copyConstructible() {
    QVERIFY(std::is_copy_constructible_v<sak::ProgramInfo>);
    QVERIFY(std::is_copy_constructible_v<sak::LeftoverItem>);
    QVERIFY(std::is_copy_constructible_v<sak::UninstallReport>);
    QVERIFY(std::is_copy_constructible_v<sak::UninstallQueueItem>);
}

void AdvancedUninstallTypesTests::staticAsserts_movable() {
    QVERIFY(std::is_move_constructible_v<sak::ProgramInfo>);
    QVERIFY(std::is_move_constructible_v<sak::LeftoverItem>);
    QVERIFY(std::is_move_constructible_v<sak::UninstallReport>);
    QVERIFY(std::is_move_constructible_v<sak::UninstallQueueItem>);
}

// -- Leftover scan provenance (proof-of-scan binding) -------------------------

namespace {
sak::LeftoverItem makeItem(sak::LeftoverItem::Type type,
                           const QString& path,
                           const QString& valueName = QString()) {
    sak::LeftoverItem item;
    item.type = type;
    item.path = path;
    item.registryValueName = valueName;
    return item;
}
}  // namespace

void AdvancedUninstallTypesTests::provenanceKey_stableAcrossFormatting() {
    using T = sak::LeftoverItem::Type;
    // Filesystem: separator/case/trailing-dot differences resolve to the same key.
    QCOMPARE(sak::leftoverProvenanceKey(makeItem(T::File, "C:\\Program Files\\Acme\\a.dll")),
             sak::leftoverProvenanceKey(makeItem(T::File, "c:/program files/acme/a.dll")));
    // The trailing-dot/space half of the claim above was never exercised: Win32 discards a
    // trailing '.' or ' ' from EVERY path component, so these name the same object. Interior
    // components are used so QString::trimmed() cannot be what does the work.
    QCOMPARE(sak::leftoverProvenanceKey(makeItem(T::File, "C:\\Program Files \\Acme.\\a.dll")),
             sak::leftoverProvenanceKey(makeItem(T::File, "c:/program files/acme/a.dll")));
    // Registry key: hive case + repeated/trailing separators canonicalize.
    QCOMPARE(sak::leftoverProvenanceKey(makeItem(T::RegistryKey, "HKLM\\SOFTWARE\\Acme")),
             sak::leftoverProvenanceKey(makeItem(T::RegistryKey, "hklm\\software\\\\acme\\")));
    // Service names are case-insensitive.
    QCOMPARE(sak::leftoverProvenanceKey(makeItem(T::Service, "AcmeSvc")),
             sak::leftoverProvenanceKey(makeItem(T::Service, "acmesvc")));
    // Scheduled task: leading backslash + separator style normalize.
    QCOMPARE(sak::leftoverProvenanceKey(makeItem(T::ScheduledTask, "\\Acme\\Update")),
             sak::leftoverProvenanceKey(makeItem(T::ScheduledTask, "Acme/Update")));
}

void AdvancedUninstallTypesTests::provenanceKey_distinctForDifferentItems() {
    using T = sak::LeftoverItem::Type;
    // A registry value with a name is distinct from the bare key.
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::RegistryValue, "HKCU\\Software\\Acme", "Run")) !=
            sak::leftoverProvenanceKey(makeItem(T::RegistryKey, "HKCU\\Software\\Acme")));
    // Different value names under one key are distinct.
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::RegistryValue, "HKCU\\Software\\Acme", "Run")) !=
            sak::leftoverProvenanceKey(makeItem(T::RegistryValue, "HKCU\\Software\\Acme", "Load")));
    // SEPARATOR INJECTION: a registry path and a value name may both legally contain '|', the
    // character that joins the composite key's fields. Unescaped, (path="...\\Acme|B", value="C")
    // and (path="...\\Acme", value="B|C") collapse to the SAME key, so a scan proof for one value
    // would authorize deleting a different value under that key.
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::RegistryValue, "HKCU\\Software\\Acme|B", "C")) !=
            sak::leftoverProvenanceKey(makeItem(T::RegistryValue, "HKCU\\Software\\Acme", "B|C")));
    // Different filesystem paths are distinct.
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::File, "C:\\Acme\\a.dll")) !=
            sak::leftoverProvenanceKey(makeItem(T::File, "C:\\Acme\\b.dll")));
    // CROSS-HIVE: the SAME subkey under a different hive must NOT collide (a proof for HKCU cannot
    // authorize the equivalent HKLM deletion, and vice versa).
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::RegistryKey, "HKLM\\Software\\Acme")) !=
            sak::leftoverProvenanceKey(makeItem(T::RegistryKey, "HKCU\\Software\\Acme")));
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::RegistryValue, "HKLM\\Software\\Acme", "Run")) !=
            sak::leftoverProvenanceKey(makeItem(T::RegistryValue, "HKCU\\Software\\Acme", "Run")));
    // TYPE-BINDING: a File proof must not authorize a Folder delete at the same path (File->Folder
    // recursion escalation), nor RegistryKey vs ShellExtension at the same key.
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::File, "C:\\Acme\\data")) !=
            sak::leftoverProvenanceKey(makeItem(T::Folder, "C:\\Acme\\data")));
    // StartupEntry is a TWO-ARM key (file-backed shortcut vs registry Run value) and NEITHER arm
    // was reached: a file-backed startup proof must not authorize a plain-file delete at the same
    // path, a registry-backed one must not authorize deleting that Run VALUE directly, and the two
    // arms must not collide with each other.
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::StartupEntry, "C:\\Acme\\run.lnk")) !=
            sak::leftoverProvenanceKey(makeItem(T::File, "C:\\Acme\\run.lnk")));
    QVERIFY(sak::leftoverProvenanceKey(
                makeItem(T::StartupEntry, "HKCU\\Software\\Acme\\Run", "Acme")) !=
            sak::leftoverProvenanceKey(
                makeItem(T::RegistryValue, "HKCU\\Software\\Acme\\Run", "Acme")));
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::StartupEntry, "C:\\Acme\\run.lnk")) !=
            sak::leftoverProvenanceKey(makeItem(T::StartupEntry, "C:\\Acme\\run.lnk", "Acme")));
    QVERIFY(sak::leftoverProvenanceKey(makeItem(T::RegistryKey, "HKCR\\CLSID\\{x}")) !=
            sak::leftoverProvenanceKey(makeItem(T::ShellExtension, "HKCR\\CLSID\\{x}")));
    // FIREWALL QUALIFIERS: direction/profile/program are captured at scan time and folded into the
    // identity so a proof for one rule cannot authorize deleting a DIFFERENT rule that merely
    // shares the display name (cleanup turns them into the netsh dir=/profile=/program= narrowing).
    // Only `path` ever reached the key here, leaving all three qualifier fields silent.
    sak::LeftoverItem fwIn = makeItem(T::FirewallRule, "Acme Updater");
    fwIn.firewallDirection = "in";
    fwIn.firewallProfile = "Private";
    fwIn.firewallProgram = "C:\\Program Files\\Acme\\upd.exe";
    sak::LeftoverItem fwOtherDir = fwIn;
    fwOtherDir.firewallDirection = "out";
    sak::LeftoverItem fwOtherProfile = fwIn;
    fwOtherProfile.firewallProfile = "Public";
    sak::LeftoverItem fwOtherProgram = fwIn;
    fwOtherProgram.firewallProgram = "C:\\Program Files\\Acme\\other.exe";
    QVERIFY(sak::leftoverProvenanceKey(fwIn) != sak::leftoverProvenanceKey(fwOtherDir));
    QVERIFY(sak::leftoverProvenanceKey(fwIn) != sak::leftoverProvenanceKey(fwOtherProfile));
    QVERIFY(sak::leftoverProvenanceKey(fwIn) != sak::leftoverProvenanceKey(fwOtherProgram));
}

void AdvancedUninstallTypesTests::provenanceStore_recordsAndMatches() {
    using T = sak::LeftoverItem::Type;
    auto& store = sak::LeftoverScanProvenance::instance();
    store.clear();
    QVERIFY(store.isEmpty());

    const sak::LeftoverItem scanned = makeItem(T::File, "C:\\Program Files\\Acme\\a.dll");
    const sak::LeftoverItem scannedReg = makeItem(T::RegistryKey, "HKLM\\SOFTWARE\\Acme");
    QVERIFY(!store.contains(scanned));  // nothing recorded yet -> proof-of-scan gate would refuse

    store.record({scanned, scannedReg, makeItem(T::Service, "AcmeSvc")});
    QVERIFY(!store.isEmpty());

    // A reformatted copy of a scanned item still matches (same normalized key).
    QVERIFY(store.contains(makeItem(T::File, "c:/program files/acme/a.dll")));
    QVERIFY(store.contains(makeItem(T::RegistryKey, "hklm\\software\\acme")));
    QVERIFY(store.contains(makeItem(T::Service, "acmesvc")));

    // An item no scan surfaced is refused (fabricated/injected path).
    QVERIFY(!store.contains(makeItem(T::File, "C:\\Program Files\\Acme\\evil.exe")));

    store.clear();
    QVERIFY(store.isEmpty());
}

void AdvancedUninstallTypesTests::provenanceStore_bindsRealObjectIdentity() {
    using T = sak::LeftoverItem::Type;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath(QStringLiteral("leftover.dat"));
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("original-leftover-bytes"));
    }

    auto& store = sak::LeftoverScanProvenance::instance();
    store.clear();
    const quint64 gen0 = store.generation();
    store.record({makeItem(T::File, filePath)});
    QCOMPARE(store.generation(), gen0 + 1);

    // Unchanged object: the scan proof still holds (a reformatted path also matches).
    QVERIFY(store.contains(makeItem(T::File, filePath)));
    // Pin the fingerprint's FIELD SET, not merely "it changed": size ALONE satisfies the swap
    // check below (23 bytes -> 46 bytes), so a size-only fingerprint stays green while the
    // creation/last-write components -- the only ones that catch a SAME-SIZE delete-and-recreate
    // -- go unproven. Recomputed from QFileInfo directly, not from the function under test.
    const QFileInfo probe(filePath);
    const QString expectedFingerprint = QStringLiteral("sz:%1|bt:%2|mt:%3")
                                            .arg(probe.size())
                                            .arg(probe.birthTime().toMSecsSinceEpoch())
                                            .arg(probe.lastModified().toMSecsSinceEpoch());
    QCOMPARE(sak::fsIdentityFingerprint(makeItem(T::File, filePath)), expectedFingerprint);
    // Both arms of the file-backed guard: a file-backed startup entry fingerprints the real
    // object, a registry-backed one (value name present) carries none however the path text reads.
    QCOMPARE(sak::fsIdentityFingerprint(makeItem(T::StartupEntry, filePath)), expectedFingerprint);
    QVERIFY(sak::fsIdentityFingerprint(makeItem(T::StartupEntry, filePath, "Acme")).isEmpty());
    QVERIFY(store.contains(makeItem(T::File, QDir::fromNativeSeparators(filePath).toUpper())));

    // Swap the object at the SAME path for different content: the on-disk identity fingerprint
    // changes, so the scan proof no longer authorizes deletion even though the path text is
    // identical (fail closed against an ancestor-junction or delete-and-recreate swap).
    QVERIFY(QFile::remove(filePath));
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("attacker-swapped-in-a-larger-different-payload"));
    }
    QVERIFY(!store.contains(makeItem(T::File, filePath)));

    store.clear();
}

QTEST_GUILESS_MAIN(AdvancedUninstallTypesTests)

#include "test_advanced_uninstall_types.moc"
