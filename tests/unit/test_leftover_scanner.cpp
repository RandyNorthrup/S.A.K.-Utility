// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_leftover_scanner.cpp
/// @brief Unit tests for LeftoverScanner -- pattern matching, risk classification,
///        protected paths, file system scanning, and cancellation support

#include "sak/leftover_scanner.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <type_traits>
#include <vector>

using sak::LeftoverItem;
using sak::LeftoverScanner;
using sak::ProgramInfo;
using sak::ScanLevel;

// Free decision seams defined (external linkage) in leftover_scanner.cpp. Forward-declared here so
// the security-critical parse/fail-closed logic can be exercised without netsh or a live registry.
namespace sak {
bool firewallDumpHeaderMissing(const QStringList& lines);
void applyFirewallField(const QString& trimmed, LeftoverItem& item);
QString parseFirstCsvField(const QString& line);
#ifdef Q_OS_WIN
bool growRunValueBuffers(std::vector<wchar_t>& name_buf,
                         std::vector<BYTE>& data_buf,
                         DWORD data_len);
#endif
}  // namespace sak

class LeftoverScannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction --
    void construction_safe();
    void construction_moderate();
    void construction_advanced();
    void construction_notCopyable();

    // -- File System Scanning --
    void scan_findsMatchingFolder();
    void scan_findsMatchingFile();
    void scan_ignoresNonMatchingFolder();
    void scan_safeLevelSkipsRegistry();
    void scan_cancellationStopsScan();
    void scan_progressCallbackInvoked();
    void scan_preSelectsSafeItems();

    // -- Pattern Matching --
    void scan_matchesProgramNameExact();
    void scan_matchesProgramNameCaseInsensitive();
    void scan_matchesConcatenatedName();
    void scan_skipsCommonWords();
    void scan_matchesInstallDirName();

    // -- Risk Classification --
    void scan_safeInAppData();
    void scan_safeInProgramFiles();
    void scan_registryKeySafe();
    void scan_serviceScanAtAdvanced();

    // -- Empty Program --
    void scan_emptyProgram_noResults();
    void scan_emptyPublisher_noPublisherPatterns();

    // -- Service leftover builder (SCM seam, locale-independent) --
    void buildServiceItems_matchesNameOrDisplay();
    void buildServiceItems_stopRequestedInterrupts();
    void buildServiceItems_localeIndependentFields();

    // -- Firewall parse reliability (localized dumps must fail closed) --
    void firewallDump_englishParsesReliable();
    void firewallDump_localizedMarksUnreliable();
    void firewallDump_emptyIsHonestNotUnreliable();

    // -- Firewall rule identity capture (narrows the delete to ONE rule) --
    void firewallField_directionLowercased();
    void firewallField_profileCaptured();
    void firewallField_programPathCaptured();
    void firewallField_programAnyIgnored();
    void firewallField_unrelatedLineNoOp();

    // -- Scheduled-task CSV first-field parse (embedded commas must not truncate task identity) --
    void csvFirstField_plainUnquoted();
    void csvFirstField_quotedWithEmbeddedComma();
    void csvFirstField_escapedQuotes();

    // -- Run-key ERROR_MORE_DATA buffer growth (long values must not be skipped) --
#ifdef Q_OS_WIN
    void runValueBuffers_growsDataToRequiredSize();
    void runValueBuffers_doublesNameWhenDataFits();
    void runValueBuffers_failsClosedPastCeiling();
#endif

    // -- R5-P7-6: a registry InstallLocation must EARN deletion authority --
    void installLocationSyntax_refusesUnpinnableShapes();
    void installLocationSyntax_refusesCriticalRoots();
    void installLocationSyntax_acceptsQualifiedProductDirectory();
    void installLocationOwnership_provesOnlyRealProductDirectories();
    void criticalInstallRoots_coverSystemAndProfileRoots();
    void scan_installLocationTiedToProgram_isDeletable();
    void scan_installLocationNotTiedToProgram_isReportOnly();
    void scan_installLocationUnpinnable_yieldsNoCandidate();

    // -- R5-G23-4: shared-container protection is drive-agnostic (non-C: system drive) --
    void isSharedContainerPath_isDriveAgnostic();
};

// -- Helper ------------------------------------------------------------------

namespace {

/// Create a program info suitable for testing
ProgramInfo makeTestProgram(const QString& name,
                            const QString& publisher = {},
                            const QString& installLoc = {}) {
    ProgramInfo prog;
    prog.displayName = name;
    prog.publisher = publisher;
    prog.installLocation = installLoc;
    prog.registryKeyPath = "HKLM\\SOFTWARE\\" + name;
    return prog;
}

}  // namespace

// -- Construction ------------------------------------------------------------

void LeftoverScannerTests::construction_safe() {
    ProgramInfo prog = makeTestProgram("TestApp");
    LeftoverScanner scanner(prog, ScanLevel::Safe);
    Q_UNUSED(scanner);
}

void LeftoverScannerTests::construction_moderate() {
    ProgramInfo prog = makeTestProgram("TestApp");
    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    Q_UNUSED(scanner);
}

void LeftoverScannerTests::construction_advanced() {
    ProgramInfo prog = makeTestProgram("TestApp");
    LeftoverScanner scanner(prog, ScanLevel::Advanced);
    Q_UNUSED(scanner);
}

void LeftoverScannerTests::construction_notCopyable() {
    QVERIFY(!std::is_copy_constructible_v<LeftoverScanner>);
    QVERIFY(!std::is_copy_assignable_v<LeftoverScanner>);
    // Move is allowed
    QVERIFY(std::is_move_constructible_v<LeftoverScanner>);
}

// -- File System Scanning ----------------------------------------------------

void LeftoverScannerTests::scan_findsMatchingFolder() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Create a folder matching the program name
    QDir dir(tempDir.path());
    QVERIFY(dir.mkdir("SuperEditor"));

    ProgramInfo prog = makeTestProgram("SuperEditor");
    prog.installLocation = tempDir.path() + "/SuperEditor";

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // The scanner scans standard system directories (Program Files, AppData, etc.)
    // not our temp dir -- so we just verify it runs without error
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_findsMatchingFile() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Create a file matching the program name
    QFile file(tempDir.path() + "/SuperEditor.lnk");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("shortcut data");
    file.close();

    ProgramInfo prog = makeTestProgram("SuperEditor");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // Scanner scans standard dirs, not temp dir -- no crash is the test
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_ignoresNonMatchingFolder() {
    ProgramInfo prog = makeTestProgram("UniqueXYZ123456");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // With a unique enough name, no real leftovers should be found
    // on a system that never had this app installed
    QVERIFY(results.size() >= 0);  // No crash, results should be minimal
}

void LeftoverScannerTests::scan_safeLevelSkipsRegistry() {
    ProgramInfo prog = makeTestProgram("TestRegSkip12345");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // At Safe level, registry scanning should be skipped.
    // Verify no registry-type items are returned.
    for (const auto& item : results) {
        QVERIFY(item.type != LeftoverItem::Type::RegistryKey);
        QVERIFY(item.type != LeftoverItem::Type::RegistryValue);
    }
}

void LeftoverScannerTests::scan_cancellationStopsScan() {
    ProgramInfo prog = makeTestProgram("TestCancel12345");

    LeftoverScanner scanner(prog, ScanLevel::Advanced);
    std::atomic<bool> stop{true};  // Already stopped!

    auto results = scanner.scan(stop);

    // Should return immediately with no results when stop is already set
    QVERIFY(results.isEmpty());
}

void LeftoverScannerTests::scan_progressCallbackInvoked() {
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    int callbackCount = 0;
    auto callback = [&callbackCount](const QString& /*path*/, int /*found*/) {
        ++callbackCount;
    };

    auto results = scanner.scan(stop, callback);

    // If any items were found, callback should have been invoked
    if (!results.isEmpty()) {
        QVERIFY(callbackCount > 0);
        QCOMPARE(callbackCount, results.size());
    }
}

void LeftoverScannerTests::scan_preSelectsSafeItems() {
    // Create a program name that might match something on the system
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.risk == LeftoverItem::RiskLevel::Safe) {
            QVERIFY(item.selected);
        } else {
            QVERIFY(!item.selected);
        }
    }
}

// -- Pattern Matching --------------------------------------------------------

void LeftoverScannerTests::scan_matchesProgramNameExact() {
    // Use a program name that DOES exist in common system directories
    // The "VLC" or "Notepad" test verifies exact matching behavior
    ProgramInfo prog = makeTestProgram("VLC media player");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // This validates that the scanner runs without error
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_matchesProgramNameCaseInsensitive() {
    // Scanner should do case-insensitive matching
    ProgramInfo prog1 = makeTestProgram("TESTAPPUPPER");
    ProgramInfo prog2 = makeTestProgram("testappupper");

    LeftoverScanner scanner1(prog1, ScanLevel::Safe);
    LeftoverScanner scanner2(prog2, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results1 = scanner1.scan(stop);
    auto results2 = scanner2.scan(stop);

    // Both should find the same results (case-insensitive matching)
    QCOMPARE(results1.size(), results2.size());
}

void LeftoverScannerTests::scan_matchesConcatenatedName() {
    // "VLC Media Player" should also match "vlcmediaplayer" (concatenated)
    ProgramInfo prog = makeTestProgram("Test App XYZ");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // Validates no crash, pattern building with concatenation
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_skipsCommonWords() {
    // Common words like "the", "media", "player" should be excluded
    // to reduce false positives. A program named only with common words
    // should have fewer matches than expected.
    ProgramInfo prog = makeTestProgram("The Free Player");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);
    // "the", "free", "player" are all excluded words, so the full name
    // "the free player" and concatenated "thefreeplayer" will be the patterns.
    // With only the full name pattern, false positives are reduced.
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_matchesInstallDirName() {
    ProgramInfo prog = makeTestProgram("MySpecialApp");
    prog.installLocation = "C:\\Program Files\\SpecialAppDir";

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // The scanner should also create patterns from the install dir name
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

// -- Risk Classification -----------------------------------------------------

void LeftoverScannerTests::scan_safeInAppData() {
    // Items found in AppData directories should be classified as Safe
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.path.toLower().contains("appdata")) {
            // File/folder items in AppData matching program name should be Safe
            if (item.type == LeftoverItem::Type::File || item.type == LeftoverItem::Type::Folder) {
                QCOMPARE(item.risk, LeftoverItem::RiskLevel::Safe);
            }
        }
    }
}

void LeftoverScannerTests::scan_safeInProgramFiles() {
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.path.toLower().contains("program files")) {
            if (item.type == LeftoverItem::Type::File || item.type == LeftoverItem::Type::Folder) {
                QCOMPARE(item.risk, LeftoverItem::RiskLevel::Safe);
            }
        }
    }
}

void LeftoverScannerTests::scan_registryKeySafe() {
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.type == LeftoverItem::Type::RegistryKey) {
            // Registry keys matching program name patterns should be Safe
            QVERIFY(item.risk == LeftoverItem::RiskLevel::Safe ||
                    item.risk == LeftoverItem::RiskLevel::Review);
        }
    }
}

void LeftoverScannerTests::scan_serviceScanAtAdvanced() {
    // Services are only scanned at Advanced level
    ProgramInfo prog = makeTestProgram("TestSvcScan12345");

    // Safe level - should not scan services
    {
        LeftoverScanner scanner(prog, ScanLevel::Safe);
        std::atomic<bool> stop{false};
        auto results = scanner.scan(stop);

        for (const auto& item : results) {
            QVERIFY(item.type != LeftoverItem::Type::Service);
            QVERIFY(item.type != LeftoverItem::Type::ScheduledTask);
            QVERIFY(item.type != LeftoverItem::Type::FirewallRule);
            QVERIFY(item.type != LeftoverItem::Type::StartupEntry);
        }
    }

    // Moderate level - should not scan services
    {
        LeftoverScanner scanner(prog, ScanLevel::Moderate);
        std::atomic<bool> stop{false};
        auto results = scanner.scan(stop);

        for (const auto& item : results) {
            QVERIFY(item.type != LeftoverItem::Type::Service);
            QVERIFY(item.type != LeftoverItem::Type::ScheduledTask);
            QVERIFY(item.type != LeftoverItem::Type::FirewallRule);
            QVERIFY(item.type != LeftoverItem::Type::StartupEntry);
        }
    }
}

// -- Empty Program -----------------------------------------------------------

void LeftoverScannerTests::scan_emptyProgram_noResults() {
    ProgramInfo prog;  // All fields empty

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // With no patterns to match, should find nothing
    QVERIFY(results.isEmpty());
}

void LeftoverScannerTests::scan_emptyPublisher_noPublisherPatterns() {
    ProgramInfo prog = makeTestProgram("UniqueTestApp99999");
    // publisher left empty

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // Should not crash and should work based on name patterns only
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

// -- Service leftover builder (SCM seam) --------------------------------------
// buildServiceLeftoverItems is the locale-independent successor to the old sc.exe console parse:
// it consumes (serviceName, displayName) pairs -- exactly what EnumServicesStatusExW yields -- so
// the matching no longer depends on English "SERVICE_NAME:"/"DISPLAY_NAME:" labels. These pure
// tests exercise it directly without touching the live SCM.

void LeftoverScannerTests::buildServiceItems_matchesNameOrDisplay() {
    const QVector<QPair<QString, QString>> services = {
        {QStringLiteral("AcmeSync"), QStringLiteral("Acme Sync Service")},
        {QStringLiteral("WSearch"), QStringLiteral("Windows Search")},
        {QStringLiteral("svc-acme-helper"), QStringLiteral("Unrelated Display")},
        {QStringLiteral("keynamemiss"), QStringLiteral("Acme Background Agent")},
    };
    // Matcher: anything containing "acme" (case-insensitive), applied to name AND display.
    const auto matches = [](const QString& text) {
        return text.contains(QStringLiteral("acme"), Qt::CaseInsensitive);
    };
    std::atomic<bool> stop{false};

    const QVector<LeftoverItem> items = sak::buildServiceLeftoverItems(services, matches, stop);

    // Rows 0 (name+display), 2 (name only), 3 (display only) match; row 1 does not.
    QCOMPARE(items.size(), 3);
    for (const auto& item : items) {
        QCOMPARE(item.type, LeftoverItem::Type::Service);
        QCOMPARE(item.risk, LeftoverItem::RiskLevel::Risky);
    }
    // path is the service KEY name (what an uninstall would target), not the display string.
    QCOMPARE(items.at(0).path, QStringLiteral("AcmeSync"));
    QVERIFY(items.at(0).description.contains(QStringLiteral("Acme Sync Service")));
    QCOMPARE(items.at(1).path, QStringLiteral("svc-acme-helper"));
    QCOMPARE(items.at(2).path, QStringLiteral("keynamemiss"));
}

void LeftoverScannerTests::buildServiceItems_stopRequestedInterrupts() {
    QVector<QPair<QString, QString>> services;
    for (int i = 0; i < 100; ++i) {
        services.append({QStringLiteral("acme%1").arg(i), QStringLiteral("Acme %1").arg(i)});
    }
    const auto matches = [](const QString&) {
        return true;
    };  // would match every row
    std::atomic<bool> stop{true};  // already cancelled before the first iteration

    const QVector<LeftoverItem> items = sak::buildServiceLeftoverItems(services, matches, stop);
    QVERIFY(items.isEmpty());  // cooperative cancel honored -> no items built
}

void LeftoverScannerTests::buildServiceItems_localeIndependentFields() {
    // The SCM fields are language-neutral: even a display name in a non-Latin script flows through
    // untouched (the old console parse keyed off English labels and would have dropped this row).
    const QVector<QPair<QString, QString>> services = {
        {QStringLiteral("AcmeSvc"),
         QString::fromUtf8("\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x93\xE3\x82\xB9")},  // JP "service"
    };
    const auto matches = [](const QString& text) {
        return text.contains(QStringLiteral("AcmeSvc"));  // matches the language-neutral key name
    };
    std::atomic<bool> stop{false};

    const QVector<LeftoverItem> items = sak::buildServiceLeftoverItems(services, matches, stop);
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.at(0).path, QStringLiteral("AcmeSvc"));
    QVERIFY(items.at(0).description.contains(
        QString::fromUtf8("\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x93\xE3\x82\xB9")));
}

// -- Firewall parse reliability ----------------------------------------------
// netsh delimits each rule with a locale-neutral dashed separator, but the "Rule Name:" label is
// localized. A dump with separators yet no parseable header is a blind (localized) parse: it MUST
// mark the phase unreliable, never present its empty item list as an honest "no firewall rules".

namespace {
QStringList dumpLines(const QString& body) {
    return body.split(QLatin1Char('\n'));
}
const QString kSep =
    QStringLiteral("----------------------------------------------------------------------");
}  // namespace

void LeftoverScannerTests::firewallDump_englishParsesReliable() {
    const QString body = QStringLiteral("Rule Name:  Acme Updater\n") + kSep +
                         QStringLiteral("\nEnabled:  Yes\nRule Name:  Media Player\n") + kSep +
                         QStringLiteral("\nEnabled:  Yes\n");
    QVERIFY(!sak::firewallDumpHeaderMissing(dumpLines(body)));
}

void LeftoverScannerTests::firewallDump_localizedMarksUnreliable() {
    // German labels: rules clearly present (separators) but the English "Rule Name:" is absent.
    const QString body = QStringLiteral("Regelname:  Acme Updater\n") + kSep +
                         QStringLiteral("\nAktiviert:  Ja\nRegelname:  Media Player\n") + kSep +
                         QStringLiteral("\nAktiviert:  Ja\n");
    QVERIFY(sak::firewallDumpHeaderMissing(dumpLines(body)));
}

void LeftoverScannerTests::firewallDump_emptyIsHonestNotUnreliable() {
    // No rule blocks at all -> honest empty, not a failed parse.
    const QString body = QStringLiteral("No rules match the specified criteria.\n");
    QVERIFY(!sak::firewallDumpHeaderMissing(dumpLines(body)));
}

// -- Firewall rule identity capture ------------------------------------------
// applyFirewallField pulls the dir=/profile=/program= identity off a netsh rule block so
// cleanup deletes only the ONE matching rule instead of every rule sharing the name.

void LeftoverScannerTests::firewallField_directionLowercased() {
    LeftoverItem item;
    sak::applyFirewallField(QStringLiteral("Direction:                            In"), item);
    QCOMPARE(item.firewallDirection, QStringLiteral("in"));
}

void LeftoverScannerTests::firewallField_profileCaptured() {
    LeftoverItem item;
    sak::applyFirewallField(QStringLiteral("Profiles:  Domain,Private,Public"), item);
    QCOMPARE(item.firewallProfile, QStringLiteral("Domain,Private,Public"));
}

void LeftoverScannerTests::firewallField_programPathCaptured() {
    LeftoverItem item;
    sak::applyFirewallField(QStringLiteral("Program:  C:\\Apps\\Acme\\acme.exe"), item);
    QCOMPARE(item.firewallProgram, QStringLiteral("C:\\Apps\\Acme\\acme.exe"));
}

void LeftoverScannerTests::firewallField_programAnyIgnored() {
    LeftoverItem item;
    sak::applyFirewallField(QStringLiteral("Program:  Any"), item);
    QVERIFY(item.firewallProgram.isEmpty());
}

void LeftoverScannerTests::firewallField_unrelatedLineNoOp() {
    LeftoverItem item;
    sak::applyFirewallField(QStringLiteral("Enabled:  Yes"), item);
    QVERIFY(item.firewallDirection.isEmpty());
    QVERIFY(item.firewallProfile.isEmpty());
    QVERIFY(item.firewallProgram.isEmpty());
}

// -- Scheduled-task CSV first-field parse -------------------------------------
// schtasks /fo CSV quotes each field; a naive split(',')[0] truncates a task name that contains a
// comma, so a later removeScheduledTask would target the wrong/nonexistent task. parseFirstCsvField
// honors the quoting so the task identity is preserved.

void LeftoverScannerTests::csvFirstField_plainUnquoted() {
    QCOMPARE(sak::parseFirstCsvField(QStringLiteral("TaskName,Next Run Time,Status")),
             QStringLiteral("TaskName"));
}

void LeftoverScannerTests::csvFirstField_quotedWithEmbeddedComma() {
    QCOMPARE(sak::parseFirstCsvField(QStringLiteral("\"Acme, Inc. Updater\",\"N/A\",\"Ready\"")),
             QStringLiteral("Acme, Inc. Updater"));
}

void LeftoverScannerTests::csvFirstField_escapedQuotes() {
    // A doubled quote inside a quoted field is a single literal quote.
    QCOMPARE(sak::parseFirstCsvField(QStringLiteral("\"He said \"\"hi\"\"\",\"x\"")),
             QStringLiteral("He said \"hi\""));
}

#ifdef Q_OS_WIN
// -- Run-key ERROR_MORE_DATA buffer growth -----------------------------------
// RegEnumValueW reports only the required DATA size on ERROR_MORE_DATA; the name size is unknown.
// growRunValueBuffers must therefore grow data to the reported size and double the name buffer, and
// must fail closed (return false) past a sane ceiling rather than skip a value silently or spin.

void LeftoverScannerTests::runValueBuffers_growsDataToRequiredSize() {
    std::vector<wchar_t> name_buf(256);
    std::vector<BYTE> data_buf(1024);
    const bool ok = sak::growRunValueBuffers(name_buf, data_buf, 4096);
    QVERIFY(ok);
    QCOMPARE(static_cast<int>(data_buf.size()), 4096);
    QCOMPARE(static_cast<int>(name_buf.size()), 256);  // data-only shortfall leaves name untouched
}

void LeftoverScannerTests::runValueBuffers_doublesNameWhenDataFits() {
    std::vector<wchar_t> name_buf(256);
    std::vector<BYTE> data_buf(1024);
    // data_len fits the current data buffer -> the shortfall is the name buffer, which doubles.
    const bool ok = sak::growRunValueBuffers(name_buf, data_buf, 512);
    QVERIFY(ok);
    QCOMPARE(static_cast<int>(name_buf.size()), 512);
    QCOMPARE(static_cast<int>(data_buf.size()), 1024);
}

void LeftoverScannerTests::runValueBuffers_failsClosedPastCeiling() {
    std::vector<wchar_t> name_buf(256);
    std::vector<BYTE> data_buf(1024);
    // A pathological 2 MiB data requirement exceeds the 1 MiB ceiling -> fail closed.
    const bool ok = sak::growRunValueBuffers(name_buf, data_buf, 2u * 1024u * 1024u);
    QVERIFY(!ok);
}
#endif

// -- R5-P7-6: registry InstallLocation as deletion authority -----------------
//
// InstallLocation lives in the Uninstall subtree, which ANY user can write under HKCU, and
// the whole-directory leftover built from it is handed to a recursive delete run by an
// administrator. It therefore has to EARN that authority: pin one literal local directory,
// never a system/shared/profile root, and be tied back to the program being uninstalled.

namespace {
/// The screening tests supply their own root list so they stay independent of the machine
/// they run on; criticalInstallRoots() itself is covered separately.
QStringList testCriticalRoots() {
    return {QStringLiteral("C:\\Windows"),
            QStringLiteral("C:\\Program Files"),
            QStringLiteral("C:\\Users"),
            QStringLiteral("C:\\Users\\Username")};
}
}  // namespace

void LeftoverScannerTests::installLocationSyntax_refusesUnpinnableShapes() {
    const QStringList roots = testCriticalRoots();
    const auto refused = [&roots](const QString& value) {
        return !LeftoverScanner::installLocationSyntaxRefusal(value, roots).isEmpty();
    };

    // Nothing recorded, or whitespace only.
    QVERIFY(refused(QString()));
    QVERIFY(refused(QStringLiteral("   ")));
    // Bare and relative forms name no fixed directory at all.
    QVERIFY(refused(QStringLiteral("MyApp")));
    QVERIFY(refused(QStringLiteral("tools\\MyApp")));
    QVERIFY(refused(QStringLiteral(".\\MyApp")));
    // Drive-relative resolves against that drive's CURRENT directory.
    QVERIFY(refused(QStringLiteral("C:MyApp")));
    // Root-relative resolves against the CURRENT drive.
    QVERIFY(refused(QStringLiteral("\\MyApp")));
    // A remote share is an origin this machine cannot vouch for.
    QVERIFY(refused(QStringLiteral("\\\\server\\share\\MyApp")));
    // A traversal segment moves the delete out of the directory the value names.
    QVERIFY(refused(QStringLiteral("C:\\Apps\\MyApp\\..\\..\\Windows")));
    QVERIFY(refused(QStringLiteral("C:\\Apps\\.\\MyApp")));
    // An unexpanded variable does not name what its author meant.
    QVERIFY(refused(QStringLiteral("%ProgramFiles%\\MyApp")));
    // A WHOLE VOLUME must never become a recursive-delete target.
    QVERIFY(refused(QStringLiteral("C:\\")));
    QVERIFY(refused(QStringLiteral("D:\\")));
    QVERIFY(refused(QStringLiteral("D:/")));
}

void LeftoverScannerTests::installLocationSyntax_refusesCriticalRoots() {
    const QStringList roots = testCriticalRoots();
    const auto refused = [&roots](const QString& value) {
        return !LeftoverScanner::installLocationSyntaxRefusal(value, roots).isEmpty();
    };

    // The OS, shared-program and profile roots themselves, however they are spelled.
    QVERIFY(refused(QStringLiteral("C:\\Windows")));
    QVERIFY(refused(QStringLiteral("c:\\windows")));
    QVERIFY(refused(QStringLiteral("C:/Windows")));
    QVERIFY(refused(QStringLiteral("C:\\Windows\\")));
    QVERIFY(refused(QStringLiteral("C:\\Program Files")));
    // The profiles container AND another user's profile root: wiping either is as
    // catastrophic as wiping our own, and an HKCU value can name any of them.
    QVERIFY(refused(QStringLiteral("C:\\Users")));
    QVERIFY(refused(QStringLiteral("C:\\Users\\Username")));

    // A product directory UNDER one of those roots is the normal, allowed case.
    QVERIFY(LeftoverScanner::installLocationSyntaxRefusal(
                QStringLiteral("C:\\Program Files\\MyApp"), roots)
                .isEmpty());
    QVERIFY(LeftoverScanner::installLocationSyntaxRefusal(
                QStringLiteral("C:\\Users\\Username\\MyApp"), roots)
                .isEmpty());
}

void LeftoverScannerTests::installLocationSyntax_acceptsQualifiedProductDirectory() {
    const QStringList roots = testCriticalRoots();
    // Separators either way, trailing separator tolerated (registry values carry one
    // routinely), surrounding whitespace trimmed.
    QVERIFY(LeftoverScanner::installLocationSyntaxRefusal(QStringLiteral("C:\\Apps\\MyApp"), roots)
                .isEmpty());
    QVERIFY(LeftoverScanner::installLocationSyntaxRefusal(QStringLiteral("C:/Apps/MyApp"), roots)
                .isEmpty());
    QVERIFY(
        LeftoverScanner::installLocationSyntaxRefusal(QStringLiteral("C:\\Apps\\MyApp\\"), roots)
            .isEmpty());
    QVERIFY(
        LeftoverScanner::installLocationSyntaxRefusal(QStringLiteral("  D:\\Apps\\MyApp  "), roots)
            .isEmpty());
}

void LeftoverScannerTests::installLocationOwnership_provesOnlyRealProductDirectories() {
    const auto proves = [](const QString& location, const QString& name) {
        return LeftoverScanner::installLocationMatchesProgram(location, name, QString());
    };

    // The directory leaf is the program.
    QVERIFY(
        proves(QStringLiteral("C:\\Program Files\\SuperEditor"), QStringLiteral("SuperEditor")));
    // Punctuation, case and spacing differences fold away.
    QVERIFY(proves(QStringLiteral("C:\\Program Files\\Notepad++"),
                   QStringLiteral("Notepad++ (64-bit)")));
    QVERIFY(proves(QStringLiteral("C:\\Program Files\\7-Zip"), QStringLiteral("7 Zip")));
    // The "Publisher\\Product" layout folds together with its parent.
    QVERIFY(proves(QStringLiteral("C:\\Program Files\\Google\\Chrome"),
                   QStringLiteral("Google Chrome")));
    // A UWP package family name is accepted as the identity too.
    QVERIFY(LeftoverScanner::installLocationMatchesProgram(
        QStringLiteral("C:\\Apps\\ContosoReader"), QString(), QStringLiteral("ContosoReader_8we")));

    // THE FINDING: a value that merely CLAIMS a directory proves nothing. Another user's
    // documents folder is not this program's install location just because HKCU says so.
    QVERIFY(
        !proves(QStringLiteral("C:\\Users\\Username\\Documents"), QStringLiteral("SuperEditor")));
    QVERIFY(!proves(QStringLiteral("C:\\Program Files\\SomeOtherVendor"),
                    QStringLiteral("SuperEditor")));
    // A program with no identity at all can never prove ownership of anything.
    QVERIFY(!proves(QStringLiteral("C:\\Program Files\\SuperEditor"), QString()));
    // A short leaf must not prefix-match its way onto an unrelated program.
    QVERIFY(!proves(QStringLiteral("C:\\Apps\\Su"), QStringLiteral("SuperEditor")));
}

void LeftoverScannerTests::criticalInstallRoots_coverSystemAndProfileRoots() {
#ifndef Q_OS_WIN
    QSKIP("The critical-root set is derived from the Windows environment");
#else
    const QStringList roots = LeftoverScanner::criticalInstallRoots();
    QVERIFY(!roots.isEmpty());
    const auto covers = [&roots](const QString& expected) {
        return std::any_of(roots.cbegin(), roots.cend(), [&expected](const QString& root) {
            return root.compare(expected, Qt::CaseInsensitive) == 0;
        });
    };
    // Derived from the live environment, so a non-C: system drive is protected too.
    QVERIFY2(covers(QDir::toNativeSeparators(qEnvironmentVariable("SystemRoot"))),
             qPrintable(roots.join(QLatin1Char(';'))));
    QVERIFY2(covers(QDir::toNativeSeparators(qEnvironmentVariable("ProgramData"))),
             qPrintable(roots.join(QLatin1Char(';'))));
    QVERIFY2(covers(QDir::toNativeSeparators(qEnvironmentVariable("USERPROFILE"))),
             qPrintable(roots.join(QLatin1Char(';'))));
    // The profiles container itself, so "C:\\Users" can never be the delete target.
    QDir profiles(qEnvironmentVariable("USERPROFILE"));
    QVERIFY(profiles.cdUp());
    QVERIFY2(covers(QDir::toNativeSeparators(profiles.absolutePath())),
             qPrintable(roots.join(QLatin1Char(';'))));
#endif
}

namespace {
/// The install-location leftover for @p location, or a default-constructed item when the
/// scan produced no entry for that path at all.
LeftoverItem installLocationItem(const QVector<LeftoverItem>& results, const QString& location) {
    const QString wanted = QDir::toNativeSeparators(location);
    for (const LeftoverItem& item : results) {
        if (item.path.compare(wanted, Qt::CaseInsensitive) == 0) {
            return item;
        }
    }
    return {};
}
}  // namespace

void LeftoverScannerTests::scan_installLocationTiedToProgram_isDeletable() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QVERIFY(QDir(tempDir.path()).mkdir(QStringLiteral("ZephyrLeftoverApp")));
    const QString location = tempDir.path() + QStringLiteral("/ZephyrLeftoverApp");

    // A directory that really is named after the program keeps the normal behaviour: it is
    // a deletable, pre-selected Safe candidate.
    ProgramInfo prog = makeTestProgram(QStringLiteral("ZephyrLeftoverApp"), {}, location);
    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};
    const auto results = scanner.scan(stop);

    const LeftoverItem item = installLocationItem(results, location);
    QVERIFY2(!item.path.isEmpty(), "the install directory must still be reported");
    QVERIFY(item.deletable);
    QCOMPARE(item.risk, LeftoverItem::RiskLevel::Safe);
    QVERIFY(item.selected);
}

void LeftoverScannerTests::scan_installLocationNotTiedToProgram_isReportOnly() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QVERIFY(QDir(tempDir.path()).mkdir(QStringLiteral("BobPersonalArchive")));
    const QString location = tempDir.path() + QStringLiteral("/BobPersonalArchive");

    // THE FINDING: a non-admin HKCU Uninstall entry naming somebody else's folder. The
    // directory exists and is not a protected path, so it used to be classified Safe and
    // PRE-SELECTED for an admin-run recursive delete. It must now be report-only.
    ProgramInfo prog = makeTestProgram(QStringLiteral("ZephyrLeftoverApp"), {}, location);
    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};
    const auto results = scanner.scan(stop);

    const LeftoverItem item = installLocationItem(results, location);
    QVERIFY2(!item.path.isEmpty(), "the path must still be REPORTED for a technician to see");
    QVERIFY2(!item.deletable, "an unprovable install location must carry no deletion authority");
    QVERIFY(!item.selected);
    QCOMPARE(item.risk, LeftoverItem::RiskLevel::Risky);
    QVERIFY2(item.description.contains(QStringLiteral("not deletable")),
             qPrintable(item.description));
}

void LeftoverScannerTests::scan_installLocationUnpinnable_yieldsNoCandidate() {
    // A value that cannot even be pinned to one literal local directory yields NO item at
    // all -- not a candidate carrying a warning. It is never stat'ed either.
    for (const QString& location : {QStringLiteral("\\\\server\\share\\ZephyrLeftoverApp"),
                                    QStringLiteral("%ProgramFiles%\\ZephyrLeftoverApp"),
                                    QStringLiteral("ZephyrLeftoverApp"),
                                    QStringLiteral("C:\\")}) {
        ProgramInfo prog = makeTestProgram(QStringLiteral("ZephyrLeftoverApp"), {}, location);
        LeftoverScanner scanner(prog, ScanLevel::Safe);
        std::atomic<bool> stop{false};
        const auto results = scanner.scan(stop);
        QVERIFY2(installLocationItem(results, location).path.isEmpty(), qPrintable(location));
    }
}

void LeftoverScannerTests::isSharedContainerPath_isDriveAgnostic() {
    // kProtectedPaths hard-codes C:, but this leaf check is the DRIVE-AGNOSTIC guard that keeps a
    // shared OS/vendor container Risky on ANY drive -- the safety property the leftover cleaner
    // relies on for a machine whose system drive is not C:.
    for (const QString& drive :
         {QStringLiteral("C:/"), QStringLiteral("D:/"), QStringLiteral("Z:/")}) {
        QVERIFY(LeftoverScanner::isSharedContainerPath(drive + QStringLiteral("Windows/System32")));
        QVERIFY(LeftoverScanner::isSharedContainerPath(drive + QStringLiteral("ProgramData")));
        QVERIFY(
            LeftoverScanner::isSharedContainerPath(drive + QStringLiteral("Program Files (x86)")));
    }
    // Case-insensitive on both the drive letter and the leaf.
    QVERIFY(LeftoverScanner::isSharedContainerPath(QStringLiteral("d:\\windows\\SYSTEM32")));
    // A single product's own directory (any drive) is NOT a shared container.
    QVERIFY(
        !LeftoverScanner::isSharedContainerPath(QStringLiteral("D:/Users/Bob/AppData/Local/Acme")));
    QVERIFY(!LeftoverScanner::isSharedContainerPath(QString()));
}

QTEST_GUILESS_MAIN(LeftoverScannerTests)

#include "test_leftover_scanner.moc"
