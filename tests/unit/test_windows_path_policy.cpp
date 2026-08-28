// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_windows_path_policy.cpp
/// @brief Unit tests for the shared screening dialect in windows_path_policy.h.
///
/// Everything here is pure and touches no filesystem, so it runs identically anywhere. The
/// predicates it covers stand in front of two real authorities:
///   * literalLocalPathTrusted screens a registry-sourced string before the uninstaller launches
///     it with an ELEVATED token (uninstall_worker) or deletes a directory RECURSIVELY
///     (leftover_scanner). Any user can write those registry values under HKCU.
///   * isSafeChildName screens a name that is about to become a file or folder inside a directory
///     the user chose -- a rename target, a new folder, or an entry read back out of an archive.
/// Neither had a direct unit test before this file.

#include "sak/windows_path_policy.h"

#include <QtTest/QtTest>

class TestWindowsPathPolicy : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- isSafeChildName ---------------------------------------------------
    void childName_acceptsOrdinaryNames();
    void childName_rejectsTraversalAndSelf();
    void childName_rejectsSeparatorsAndAbsolutePaths();
    void childName_rejectsColonDriveAndAlternateDataStream();
    void childName_rejectsEmptyAndWhitespaceOnly();

    // -- screeningPathForm / pathSegmentsAreLiteral / driveQualifiedPath ----
    void screeningForm_normalizesSeparatorsAndTrailingSlashes();
    void segments_rejectDotAndDotDotComponents();
    void driveQualified_requiresALetteredLocalRoot();
    void literalLocalPathTrusted_refusesTheUntrustedShapes();
};

// ---------------------------------------------------------------------------
// isSafeChildName
// ---------------------------------------------------------------------------

void TestWindowsPathPolicy::childName_acceptsOrdinaryNames() {
    QVERIFY(sak::isSafeChildName(QStringLiteral("notes.txt")));
    QVERIFY(sak::isSafeChildName(QStringLiteral("New folder")));
    QVERIFY(sak::isSafeChildName(QStringLiteral("report (final).pdf")));
    // A dot INSIDE a name is ordinary; only the bare "." and ".." names are refused.
    QVERIFY(sak::isSafeChildName(QStringLiteral(".gitignore")));
    QVERIFY(sak::isSafeChildName(QStringLiteral("..leading-dots.txt")));
    QVERIFY(sak::isSafeChildName(QStringLiteral("archive.tar.gz")));
}

void TestWindowsPathPolicy::childName_rejectsTraversalAndSelf() {
    // THE rule the File Explorer panel's own copy was missing. Its guard checked only for empty,
    // '/' and '\\', so ".." passed -- and the panel feeds an accepted name to childPathFor(), which
    // resolves "<browsed dir>/.." to the PARENT directory. A rename or a create-folder that
    // accepts it targets somewhere the user did not choose.
    QVERIFY(!sak::isSafeChildName(QStringLiteral("..")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral(".")));
    // Whitespace must not launder it: the filesystem call downstream trims too, so " .. " is the
    // same name as "..".
    QVERIFY(!sak::isSafeChildName(QStringLiteral("  ..  ")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("\t.")));
}

void TestWindowsPathPolicy::childName_rejectsSeparatorsAndAbsolutePaths() {
    QVERIFY(!sak::isSafeChildName(QStringLiteral("sub/child.txt")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("sub\\child.txt")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("../escape.txt")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("..\\escape.txt")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("/etc/passwd")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("C:\\Windows\\System32\\drivers\\etc\\hosts")));
    // Every absolute form is refused by the separator or colon rule above rather than by a
    // separate absolute-path test -- see the note in the header on why no such test exists.
    QVERIFY(!sak::isSafeChildName(QStringLiteral("\\\\server\\share\\file.txt")));
}

void TestWindowsPathPolicy::childName_rejectsColonDriveAndAlternateDataStream() {
    // A bare drive letter carries no separator at all, so a separator-only rule accepts it.
    QVERIFY(!sak::isSafeChildName(QStringLiteral("C:")));
    // An NTFS ALTERNATE DATA STREAM is the sharper case: "notes.txt:hidden" writes content that
    // does not appear in any listing of the folder, and it too carries no separator.
    QVERIFY(!sak::isSafeChildName(QStringLiteral("notes.txt:hidden")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("payload:$DATA")));
    // Control: the same names WITHOUT the colon are accepted, so the refusals above are the colon
    // rule rather than some other property of these strings.
    QVERIFY(sak::isSafeChildName(QStringLiteral("C")));
    QVERIFY(sak::isSafeChildName(QStringLiteral("notes.txt-hidden")));
}

void TestWindowsPathPolicy::childName_rejectsEmptyAndWhitespaceOnly() {
    QVERIFY(!sak::isSafeChildName(QString()));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("   ")));
    QVERIFY(!sak::isSafeChildName(QStringLiteral("\t\r\n")));
}

// ---------------------------------------------------------------------------
// The registry-string screening dialect
// ---------------------------------------------------------------------------

void TestWindowsPathPolicy::screeningForm_normalizesSeparatorsAndTrailingSlashes() {
    QCOMPARE(sak::screeningPathForm(QStringLiteral("  C:\\Program Files\\App  ")),
             QStringLiteral("C:/Program Files/App"));
    QCOMPARE(sak::screeningPathForm(QStringLiteral("C:/App/")), QStringLiteral("C:/App"));
    QCOMPARE(sak::screeningPathForm(QStringLiteral("C:/App///")), QStringLiteral("C:/App"));
    // A bare volume root keeps its trailing slash, so driveQualifiedPath can refuse it rather
    // than seeing a bare "C:" and reading it as a drive-RELATIVE path.
    QCOMPARE(sak::screeningPathForm(QStringLiteral("C:/")), QStringLiteral("C:/"));
    // Normalizing must NOT resolve traversal: collapsing it here would hide the manipulation
    // pathSegmentsAreLiteral exists to reject.
    QCOMPARE(sak::screeningPathForm(QStringLiteral("C:\\App\\..\\Windows")),
             QStringLiteral("C:/App/../Windows"));
}

void TestWindowsPathPolicy::segments_rejectDotAndDotDotComponents() {
    QVERIFY(sak::pathSegmentsAreLiteral(QStringLiteral("C:/Program Files/App")));
    QVERIFY(!sak::pathSegmentsAreLiteral(QStringLiteral("C:/App/../Windows/System32")));
    QVERIFY(!sak::pathSegmentsAreLiteral(QStringLiteral("C:/App/./bin")));
    QVERIFY(!sak::pathSegmentsAreLiteral(QStringLiteral("C:/../Windows")));
    // A segment that merely BEGINS with dots is a real directory name, not a traversal.
    QVERIFY(sak::pathSegmentsAreLiteral(QStringLiteral("C:/App/..data/bin")));
}

void TestWindowsPathPolicy::driveQualified_requiresALetteredLocalRoot() {
    QVERIFY(sak::driveQualifiedPath(QStringLiteral("C:/Program Files/App")));
    QVERIFY(sak::driveQualifiedPath(QStringLiteral("d:/tools/x.exe")));
    // Drive-RELATIVE: Windows resolves "C:App" against the current directory ON C:, which is not
    // the literal location the string appears to name.
    QVERIFY(!sak::driveQualifiedPath(QStringLiteral("C:App")));
    // A bare volume root names no program and no leftover directory worth deleting recursively.
    QVERIFY(!sak::driveQualifiedPath(QStringLiteral("C:/")));
    // An ALTERNATE DATA STREAM suffix: drive-qualified and literal, so only the "no further
    // colon" rule refuses it. Without that rule this satisfies an extension check while the
    // content actually launched lives in a stream no directory listing shows.
    QVERIFY(!sak::driveQualifiedPath(QStringLiteral("C:/dir/readme.txt:payload.exe")));
    // UNC points at a remote origin nobody here can vouch for.
    QVERIFY(!sak::driveQualifiedPath(QStringLiteral("//server/share/app.exe")));
    // Rooted-but-driveless is resolved against the CURRENT drive.
    QVERIFY(!sak::driveQualifiedPath(QStringLiteral("/Program Files/App")));
    // Bare and relative forms are resolved by the CreateProcess search order.
    QVERIFY(!sak::driveQualifiedPath(QStringLiteral("app.exe")));
    QVERIFY(!sak::driveQualifiedPath(QStringLiteral("bin/app.exe")));
}

void TestWindowsPathPolicy::literalLocalPathTrusted_refusesTheUntrustedShapes() {
    // The composed predicate that actually gates an ELEVATED launch (uninstall_worker) and a
    // RECURSIVE delete (leftover_scanner) on a string any user can write under HKCU.
    QVERIFY(sak::literalLocalPathTrusted(QStringLiteral("C:\\Program Files\\App\\uninstall.exe")));

    QVERIFY(!sak::literalLocalPathTrusted(QString()));
    QVERIFY(!sak::literalLocalPathTrusted(QStringLiteral("   ")));
    QVERIFY(!sak::literalLocalPathTrusted(QStringLiteral("uninstall.exe")));
    QVERIFY(!sak::literalLocalPathTrusted(QStringLiteral("C:App\\uninstall.exe")));
    QVERIFY(!sak::literalLocalPathTrusted(QStringLiteral("\\\\server\\share\\uninstall.exe")));
    QVERIFY(
        !sak::literalLocalPathTrusted(QStringLiteral("C:\\App\\..\\Windows\\System32\\cmd.exe")));
    // An unexpanded environment reference does not name what its author meant, and what it
    // expands to is decided by the environment at launch time rather than by this string.
    QVERIFY(!sak::literalLocalPathTrusted(QStringLiteral("%ProgramFiles%\\App\\uninstall.exe")));
    // The case that ISOLATES the '%' rule: drive-qualified, literal segments, and a leading drive
    // letter, so every other check passes and only the percent refusal stands between an HKCU
    // string and an elevated launch whose target the environment decides.
    QVERIFY(!sak::literalLocalPathTrusted(
        QStringLiteral("C:\\Program Files\\%APPDATA%\\uninstall.exe")));
}

QTEST_MAIN(TestWindowsPathPolicy)
#include "test_windows_path_policy.moc"
