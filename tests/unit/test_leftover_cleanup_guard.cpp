// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_leftover_cleanup_guard.cpp
/// @brief Unit tests for the fail-closed target screens behind software.clean_leftovers.
///
/// These guards are the second, independent security layer under the catastrophic human gate: they
/// refuse OS-critical / unrecoverable / injection deletion targets OUTRIGHT. An empty refusal
/// string means "allowed"; a non-empty one is the reason the target is blocked. Every assertion
/// here is pure (no filesystem/registry mutation), so the suite has no destructive side effects.

#include "sak/leftover_cleanup_guard.h"

#include <QtTest/QtTest>

using namespace sak;

namespace {
// A refusal reason is expected (blocked); an empty string means it was allowed.
[[nodiscard]] bool blocked(const QString& reason) {
    return !reason.isEmpty();
}
}  // namespace

class TestLeftoverCleanupGuard : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Registry key trees --
    void registryKeyRefusesHiveRootAndBadHive();
    void registryKeyRefusesOsCriticalSubtree();
    void registryKeyRefusesSharedRootExact();
    void registryKeyAllowsSpecificVendorKey();
    void registryKeyRefusesControlChar();
    void registryKeyRefusesSeparatorTricks();
    void registryKeyRefusesBrickCriticalDescendants();
    void registrySubkeyHiveRootDetection();

    // -- Registry values --
    void registryValueRefusesEmptyNameAndSubtree();
    void registryValueAllowsRunValue();

    // -- Services --
    void serviceRefusesCriticalAndMalformed();
    void serviceAllowsVendorService();

    // -- Scheduled tasks --
    void scheduledTaskRefusesMicrosoftTree();
    void scheduledTaskAllowsVendorTask();

    // -- Firewall rules --
    void firewallRefusesAllWildcard();
    void firewallAllowsNamedRule();

    // -- Files / folders --
    void fileRefusesSystemAndRoots();
    void fileRefusesTrailingDotSpaceEvasion();
    void fileRefusesShortNameEvasion();
    void fileRefusesUserShellFolderRoots();
    void fileRefusesBootAndCriticalTree();
    void fileAllowsSpecificLeftoverSubfolder();
    void fileRefusesUncAndRelative();

    // -- Delete-time ancestor-junction-swap re-verification --
    void handleRedirectAllowsExactMatch();
    void handleRedirectRefusesSwapAndProtected();
};

void TestLeftoverCleanupGuard::registryKeyRefusesHiveRootAndBadHive() {
    QVERIFY(blocked(registryKeyDeletionRefusal(QString())));                 // empty
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\"))));  // hive root
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKCU\\"))));
    // A hive spelling CleanupWorker does not support is refused (it would otherwise fail silently).
    QVERIFY(blocked(
        registryKeyDeletionRefusal(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Vendor"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKU\\.DEFAULT\\Foo"))));
}

void TestLeftoverCleanupGuard::registryKeyRefusesOsCriticalSubtree() {
    QVERIFY(blocked(registryKeyDeletionRefusal(
        QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services\\RpcSs"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SAM\\SAM"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SECURITY"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\BCD00000000\\Objects"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\HARDWARE\\DESCRIPTION"))));
    // Case- and separator-insensitive.
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("hklm\\system\\foo\\"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(
        QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender"))));
}

void TestLeftoverCleanupGuard::registryKeyRefusesSharedRootExact() {
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\Microsoft"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(
        QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(
        QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKCR\\CLSID"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\Classes"))));
}

void TestLeftoverCleanupGuard::registryKeyAllowsSpecificVendorKey() {
    QVERIFY(!blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\AcmeCorp\\App"))));
    QVERIFY(!blocked(registryKeyDeletionRefusal(QStringLiteral("HKCU\\SOFTWARE\\AcmeCorp"))));
    // A specific shell-extension / uninstall subkey (deeper than the blocked shared roots) is fine.
    QVERIFY(!blocked(registryKeyDeletionRefusal(
        QStringLiteral("HKLM\\SOFTWARE\\Classes\\CLSID\\{12345678-1234-1234-1234-1234567890ab}"))));
    QVERIFY(!blocked(registryKeyDeletionRefusal(
        QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AcmeApp"))));
}

void TestLeftoverCleanupGuard::registryKeyRefusesControlChar() {
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\Acme\nEvil"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\Acme*"))));
}

void TestLeftoverCleanupGuard::registryKeyRefusesSeparatorTricks() {
    // Leading, doubled, trailing, and forward-slash separators must not evade the prefix match --
    // the registry API resolves them all to the same subkey.
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\\\SYSTEM"))));
    QVERIFY(
        blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SYSTEM\\\\CurrentControlSet"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SYSTEM\\"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\/SYSTEM"))));
    // A shared root reached via a doubled separator is still the exact shared root.
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\\\"))));
}

void TestLeftoverCleanupGuard::registryKeyRefusesBrickCriticalDescendants() {
    // Launch/logon-critical DESCENDANTS that live under keys blocked only exact-at-parent: deleting
    // these bricks .exe launching or breaks login, so they are prefix-blocked (not just the
    // parent).
    QVERIFY(
        blocked(registryKeyDeletionRefusal(QStringLiteral("HKCR\\exefile\\shell\\open\\command"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(
        QStringLiteral("HKLM\\SOFTWARE\\Classes\\exefile\\shell\\open\\command"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKCR\\lnkfile\\shell\\open"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral(
        "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\S-1-5-21-1-2-3"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral(
        "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SvcHost\\netsvcs"))));
    // File-extension class registrations: the exact key unregisters the whole association.
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKCR\\.exe"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKCR\\.lnk"))));
    QVERIFY(blocked(registryKeyDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\Classes\\.exe"))));
    // A NON-launch descendant of a file class (e.g. context-menu-handler / icon subkeys) stays a
    // cleanable app leftover -- the prefix block is scoped to the shell-verb tree only.
    QVERIFY(!blocked(
        registryKeyDeletionRefusal(QStringLiteral("HKCR\\exefile\\shellex\\ContextMenuHandlers"))));
    QVERIFY(!blocked(registryKeyDeletionRefusal(QStringLiteral("HKCR\\.exe\\OpenWithProgids"))));
}

void TestLeftoverCleanupGuard::registrySubkeyHiveRootDetection() {
    // CleanupWorker's defense-in-depth screen: a post-hive subkey that collapses to nothing targets
    // the hive ROOT, so RegDeleteTreeW(hive, L"") would wipe the entire hive. It MUST be flagged
    // regardless of the separator spelling the caller supplied.
    QVERIFY(cleanupRegistrySubkeyIsHiveRoot(QString()));               // "HKLM\\" -> ""
    QVERIFY(cleanupRegistrySubkeyIsHiveRoot(QStringLiteral("\\")));    // trailing sep only
    QVERIFY(cleanupRegistrySubkeyIsHiveRoot(QStringLiteral("\\\\")));  // doubled sep
    QVERIFY(cleanupRegistrySubkeyIsHiveRoot(QStringLiteral("/")));     // forward slash
    // A real subkey (even one wrapped in stray separators) is NOT the hive root.
    QVERIFY(!cleanupRegistrySubkeyIsHiveRoot(QStringLiteral("SOFTWARE\\Acme")));
    QVERIFY(!cleanupRegistrySubkeyIsHiveRoot(QStringLiteral("\\SYSTEM\\")));
}

void TestLeftoverCleanupGuard::registryValueRefusesEmptyNameAndSubtree() {
    // Empty value name is refused (also guards CleanupWorker's Q_ASSERT(!valueName.isEmpty())).
    QVERIFY(
        blocked(registryValueDeletionRefusal(QStringLiteral("HKLM\\SOFTWARE\\Acme"), QString())));
    QVERIFY(blocked(registryValueDeletionRefusal(QString(), QStringLiteral("Val"))));
    // A value under an OS-critical subtree is still refused.
    QVERIFY(blocked(registryValueDeletionRefusal(
        QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Control"), QStringLiteral("Foo"))));
}

void TestLeftoverCleanupGuard::registryValueAllowsRunValue() {
    // Deleting a single Run value (a startup leftover) is allowed even though the KEY itself is a
    // blocked shared root for key-tree deletion.
    QVERIFY(!blocked(registryValueDeletionRefusal(
        QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QStringLiteral("AcmeUpdater"))));
    QVERIFY(!blocked(registryValueDeletionRefusal(QStringLiteral("HKCU\\SOFTWARE\\Acme"),
                                                  QStringLiteral("LastRun"))));
}

void TestLeftoverCleanupGuard::serviceRefusesCriticalAndMalformed() {
    QVERIFY(blocked(serviceDeletionRefusal(QStringLiteral("RpcSs"))));
    QVERIFY(blocked(serviceDeletionRefusal(QStringLiteral("windefend"))));  // case-insensitive
    QVERIFY(blocked(serviceDeletionRefusal(QStringLiteral("Dnscache"))));
    QVERIFY(blocked(serviceDeletionRefusal(QStringLiteral("Schedule"))));
    QVERIFY(blocked(serviceDeletionRefusal(QString())));
    QVERIFY(blocked(serviceDeletionRefusal(QStringLiteral("bad\\name"))));
    QVERIFY(blocked(serviceDeletionRefusal(QStringLiteral("wild*card"))));
    QVERIFY(blocked(serviceDeletionRefusal(QStringLiteral("has=equals"))));
}

void TestLeftoverCleanupGuard::serviceAllowsVendorService() {
    QVERIFY(!blocked(serviceDeletionRefusal(QStringLiteral("AcmeUpdateService"))));
    QVERIFY(!blocked(serviceDeletionRefusal(QStringLiteral("Acme_Helper_1"))));
}

void TestLeftoverCleanupGuard::scheduledTaskRefusesMicrosoftTree() {
    QVERIFY(blocked(scheduledTaskDeletionRefusal(
        QStringLiteral("\\Microsoft\\Windows\\Defrag\\ScheduledDefrag"))));
    QVERIFY(blocked(scheduledTaskDeletionRefusal(
        QStringLiteral("Microsoft\\Windows\\UpdateOrchestrator\\Reboot"))));  // no leading slash
    QVERIFY(blocked(scheduledTaskDeletionRefusal(QString())));
    QVERIFY(blocked(scheduledTaskDeletionRefusal(QStringLiteral("\\"))));     // root
    QVERIFY(blocked(scheduledTaskDeletionRefusal(QStringLiteral("has*wild"))));
}

void TestLeftoverCleanupGuard::scheduledTaskAllowsVendorTask() {
    QVERIFY(!blocked(scheduledTaskDeletionRefusal(QStringLiteral("AcmeUpdateTask"))));
    QVERIFY(!blocked(scheduledTaskDeletionRefusal(QStringLiteral("\\AcmeCorp\\Updater"))));
}

void TestLeftoverCleanupGuard::firewallRefusesAllWildcard() {
    QVERIFY(blocked(firewallRuleDeletionRefusal(QStringLiteral("all"))));
    QVERIFY(blocked(firewallRuleDeletionRefusal(QStringLiteral("ALL"))));
    QVERIFY(blocked(firewallRuleDeletionRefusal(QString())));
    QVERIFY(blocked(firewallRuleDeletionRefusal(QStringLiteral("bad\nrule"))));
}

void TestLeftoverCleanupGuard::firewallAllowsNamedRule() {
    // Rule names legitimately contain spaces and punctuation.
    QVERIFY(!blocked(firewallRuleDeletionRefusal(QStringLiteral("Acme App (TCP-In)"))));
    QVERIFY(!blocked(firewallRuleDeletionRefusal(QStringLiteral("AcmeUpdater"))));
}

void TestLeftoverCleanupGuard::fileRefusesSystemAndRoots() {
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Windows\\System32\\drivers"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Windows"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("D:"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Program Files"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Program Files (x86)"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\ProgramData"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Users"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Users\\Public"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Users\\Public\\AppData\\Local"))));
    QVERIFY(blocked(filePathDeletionRefusal(QString())));
}

void TestLeftoverCleanupGuard::fileRefusesTrailingDotSpaceEvasion() {
    // Win32 strips trailing dots/spaces per component, so these resolve to the protected roots and
    // must be refused despite the literal trailing character.
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Windows."))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Windows "))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Program Files."))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Windows.\\System32"))));
}

void TestLeftoverCleanupGuard::fileRefusesShortNameEvasion() {
    // An 8.3 short name resolves (via Win32) to a protected long-name root that the guard cannot
    // expand lexically, so any short-name component is refused fail-closed.
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\PROGRA~1"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\PROGRA~2"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\PROGRA~1\\AcmeCorp"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Vendor\\ACMEAP~1\\data"))));
    // A normal long path with a literal tilde but no 8.3 digit marker is still allowed.
    QVERIFY(
        !blocked(filePathDeletionRefusal(QStringLiteral("C:\\ProgramData\\Acme~Vendor\\data"))));
}

void TestLeftoverCleanupGuard::fileRefusesUserShellFolderRoots() {
    // A user's shell-data folder ROOT is never an app leftover: refusing it stops a poisoned batch
    // from wiping the whole Documents/Desktop/Downloads/... tree (previously only the profile root
    // and AppData roots were exact-blocked, so these passed).
    for (const auto* leaf : {"Documents",
                             "Desktop",
                             "Downloads",
                             "Pictures",
                             "Music",
                             "Videos",
                             "Favorites",
                             "Contacts",
                             "Links",
                             "Saved Games",
                             "OneDrive"}) {
        const QString path =
            QStringLiteral("C:\\Users\\Username\\%1").arg(QString::fromLatin1(leaf));
        QVERIFY2(blocked(filePathDeletionRefusal(path)), qPrintable(path));
    }
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Users\\Public\\Documents"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Users\\Public\\Pictures"))));
}

void TestLeftoverCleanupGuard::fileRefusesBootAndCriticalTree() {
    // Boot / system-critical roots are refused for the whole subtree (no subfolder is ever a
    // legitimate leftover), unlike shared roots which block only the exact path.
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\bootmgr"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Boot"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Boot\\BCD"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\Recovery"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("D:\\EFI\\Microsoft\\Boot"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\$Recycle.Bin\\S-1-5-21"))));
    QVERIFY(blocked(
        filePathDeletionRefusal(QStringLiteral("D:\\System Volume Information\\tracking.log"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("C:\\pagefile.sys"))));
}

void TestLeftoverCleanupGuard::fileAllowsSpecificLeftoverSubfolder() {
    QVERIFY(!blocked(filePathDeletionRefusal(QStringLiteral("C:\\Program Files\\AcmeCorp\\App"))));
    QVERIFY(!blocked(filePathDeletionRefusal(
        QStringLiteral("C:\\Users\\Public\\AppData\\Local\\AcmeCorp\\cache.db"))));
    QVERIFY(!blocked(filePathDeletionRefusal(QStringLiteral("C:\\ProgramData\\AcmeCorp"))));
    // A per-app subfolder UNDER a shell folder stays cleanable (the block is the folder root only).
    QVERIFY(!blocked(filePathDeletionRefusal(
        QStringLiteral("C:\\Users\\Username\\Documents\\AcmeApp\\save.dat"))));
    QVERIFY(!blocked(
        filePathDeletionRefusal(QStringLiteral("C:\\Users\\Username\\Downloads\\AcmeSetup"))));
}

void TestLeftoverCleanupGuard::fileRefusesUncAndRelative() {
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("\\\\host\\share\\file"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("relative\\path"))));
    QVERIFY(blocked(filePathDeletionRefusal(QStringLiteral("\\\\.\\PhysicalDrive0"))));
}

void TestLeftoverCleanupGuard::handleRedirectAllowsExactMatch() {
    // The object's real resolved path equals the validated path and is not protected -> safe.
    QVERIFY(!blocked(cleanupHandleRedirectRefusal(
        QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale.dll"),
        QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale.dll"))));
    // Case- and separator-insensitive: GetFinalPathNameByHandleW returns the on-disk case / native
    // separators, which must still match the validated path.
    QVERIFY(
        !blocked(cleanupHandleRedirectRefusal(QStringLiteral("C:\\ProgramData\\AcmeCorp\\cache"),
                                              QStringLiteral("c:\\programdata\\acmecorp\\cache"))));
    QVERIFY(
        !blocked(cleanupHandleRedirectRefusal(QStringLiteral("C:/ProgramData/AcmeCorp/cache"),
                                              QStringLiteral("C:\\ProgramData\\AcmeCorp\\cache"))));
}

void TestLeftoverCleanupGuard::handleRedirectRefusesSwapAndProtected() {
    // Ancestor junction swapped the leftover subfolder to a junction pointing at System32: the real
    // resolved path lands in the Windows tree -> refused as protected.
    QVERIFY(blocked(
        cleanupHandleRedirectRefusal(QStringLiteral("C:\\Program Files\\AcmeCorp\\cache\\data.bin"),
                                     QStringLiteral("C:\\Windows\\System32\\data.bin"))));
    // Ancestor junction redirects to a DIFFERENT but benign location the human never confirmed:
    // still refused (the real target must equal the validated target).
    QVERIFY(blocked(cleanupHandleRedirectRefusal(
        QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale.dll"),
        QStringLiteral("C:\\Users\\Username\\Documents\\important\\stale.dll"))));
    // Redirected onto a drive root or a UNC target -> refused.
    QVERIFY(blocked(cleanupHandleRedirectRefusal(QStringLiteral("C:\\Program Files\\AcmeCorp\\App"),
                                                 QStringLiteral("C:\\"))));
    QVERIFY(
        blocked(cleanupHandleRedirectRefusal(QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\x"),
                                             QStringLiteral("\\\\attacker\\share\\x"))));
    // Handle could not be resolved (GetFinalPathNameByHandleW failed) -> refused, never delete
    // blind.
    QVERIFY(blocked(cleanupHandleRedirectRefusal(
        QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\x"), QString())));
}

QTEST_MAIN(TestLeftoverCleanupGuard)
#include "test_leftover_cleanup_guard.moc"
