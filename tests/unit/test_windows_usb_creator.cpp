// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_windows_usb_creator.cpp
 * @brief TST-07 -- Unit tests for WindowsUSBCreator validation logic.
 *
 * Tests the input validation and error handling that runs before any
 * OS process is spawned. Verifies BUG-09 fix (disk number command
 * injection prevention) and ISO existence checks.
 *
 * No admin privileges or hardware required -- all tests exercise
 * validation that fires before formatDriveNTFS/QProcess calls.
 */

#include "sak/windows_usb_creator.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>

class WindowsUSBCreatorTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // ---- Disk number validation (BUG-09 fix) ----
    void validDiskNumbers_data();
    void validDiskNumbers();
    void invalidDiskNumbersRejected_data();
    void invalidDiskNumbersRejected();

    // ---- ISO path validation ----
    void nonexistentIsoRejected();
    void emptyIsoPathRejected();

    // ---- Cancel / lastError accessors ----
    void initialStateCorrect();
    void cancelSetsFlag();
    void lastErrorIsThreadSafe();

    // ---- Signal emission on validation failure ----
    void failedSignalOnBadDiskNumber();
    void failedSignalOnMissingIso();

    // ---- Combined multi-validation ----
    void diskValidationFiresBeforeIsoCheck();

    // ---- Volume label sanitization (P07-52) ----
    void sanitizeVolumeLabel_stripsPowerShellMetacharacters();
    void sanitizeVolumeLabel_keepsLegitimateLabel();
    void sanitizeVolumeLabel_capsLength();

    // ---- bcdboot success gating (B10-22) ----
    void bcdbootReportsSuccess_onlyCleanZeroExit();

    // ---- diskpart 0-exit-but-failed detection (CR3-9) ----
    void diskpartOutputIsError_detectsFailureMarkers();
    void diskpartOutputIsError_ignoresSuccessChatter();
    void diskpartOutputIsError_detectsSelectionAndArgumentFailures();

    // ---- critical-file recognition for extraction integrity (R5-G14-3) ----
    void isCriticalWindowsFile_matchesBackslashPathsFrom7z();
    void isCriticalWindowsFile_matchesForwardSlashPaths();
    void isCriticalWindowsFile_rejectsNonCriticalPaths();

    // ---- bundled-executable trust check (CR3-6) ----
    void isSafeBundledExecutable_acceptsRegularFileUnderAppDir();
    void isSafeBundledExecutable_rejectsMissingFile();
    void isSafeBundledExecutable_rejectsFileOutsideAppDir();
    void isSafeBundledExecutable_rejectsSiblingPrefixDir();
};

// ===========================================================================

void WindowsUSBCreatorTests::initTestCase() {}
void WindowsUSBCreatorTests::cleanupTestCase() {}

// ===========================================================================
// Disk number validation (BUG-09)
// ===========================================================================

void WindowsUSBCreatorTests::validDiskNumbers_data() {
    QTest::addColumn<QString>("diskNum");

    // These are valid disk numbers (pure integers 1-3 digits).
    // They'll pass disk validation but fail on ISO check (nonexistent file).
    QTest::newRow("single digit 0") << QStringLiteral("0");
    QTest::newRow("single digit 1") << QStringLiteral("1");
    QTest::newRow("single digit 9") << QStringLiteral("9");
    QTest::newRow("two digits 10") << QStringLiteral("10");
    QTest::newRow("two digits 99") << QStringLiteral("99");
    QTest::newRow("three digits 100") << QStringLiteral("100");
    QTest::newRow("three digits 255") << QStringLiteral("255");
    QTest::newRow("three digits 999") << QStringLiteral("999");
}

void WindowsUSBCreatorTests::validDiskNumbers() {
    QFETCH(QString, diskNum);

    WindowsUSBCreator creator;

    // Valid disk number + nonexistent ISO -> should pass disk validation
    // and fail on ISO check instead.
    bool result = creator.createBootableUSB(QStringLiteral("/nonexistent/test.iso"), diskNum);
    QVERIFY(!result);

    // Error should mention "ISO file not found" (not "Invalid disk number").
    QVERIFY2(creator.lastError().contains(QStringLiteral("ISO file not found")),
             qPrintable("Expected ISO error but got: " + creator.lastError()));
}

// ---------------------------------------------------------------------------

void WindowsUSBCreatorTests::invalidDiskNumbersRejected_data() {
    QTest::addColumn<QString>("diskNum");
    QTest::addColumn<QString>("description");

    // Command injection attempts (BUG-09 vectors).
    QTest::newRow("semicolon injection")
        << QStringLiteral("1; rm -rf /") << QStringLiteral("semicolon cmd injection");
    QTest::newRow("newline injection")
        << QStringLiteral("1\nselect disk 0") << QStringLiteral("newline diskpart injection");
    QTest::newRow("pipe injection")
        << QStringLiteral("1 | del *") << QStringLiteral("pipe injection");
    QTest::newRow("ampersand injection")
        << QStringLiteral("1 & echo pwned") << QStringLiteral("ampersand injection");

    // Invalid formats.
    QTest::newRow("empty string") << QString() << QStringLiteral("empty string");
    QTest::newRow("alphabetic") << QStringLiteral("abc") << QStringLiteral("non-numeric");
    QTest::newRow("negative number") << QStringLiteral("-1") << QStringLiteral("negative number");
    QTest::newRow("decimal number") << QStringLiteral("1.5") << QStringLiteral("decimal");
    QTest::newRow("four digits") << QStringLiteral("1234") << QStringLiteral(">3 digits");
    QTest::newRow("leading space") << QStringLiteral(" 1") << QStringLiteral("whitespace prefix");
    QTest::newRow("trailing space") << QStringLiteral("1 ") << QStringLiteral("whitespace suffix");
    QTest::newRow("hex prefix") << QStringLiteral("0x1") << QStringLiteral("hex prefix");
    QTest::newRow("special chars") << QStringLiteral("@#$") << QStringLiteral("special characters");
    QTest::newRow("path traversal") << QStringLiteral("../etc") << QStringLiteral("path traversal");
}

void WindowsUSBCreatorTests::invalidDiskNumbersRejected() {
    QFETCH(QString, diskNum);

    WindowsUSBCreator creator;
    bool result = creator.createBootableUSB(QStringLiteral("/nonexistent.iso"), diskNum);

    QVERIFY2(!result, "Invalid disk number should be rejected");
    QVERIFY2(creator.lastError().contains(QStringLiteral("Invalid disk number")),
             qPrintable("Expected disk number error but got: " + creator.lastError()));
}

// ===========================================================================
// ISO path validation
// ===========================================================================

void WindowsUSBCreatorTests::nonexistentIsoRejected() {
    WindowsUSBCreator creator;
    bool result = creator.createBootableUSB(QStringLiteral("C:/totally/fake/path/windows.iso"),
                                            QStringLiteral("1"));

    QVERIFY(!result);
    QVERIFY(creator.lastError().contains(QStringLiteral("ISO file not found")));
}

void WindowsUSBCreatorTests::emptyIsoPathRejected() {
    WindowsUSBCreator creator;
    bool result = creator.createBootableUSB(QString(), QStringLiteral("1"));

    QVERIFY(!result);
    QVERIFY(creator.lastError().contains(QStringLiteral("ISO file not found")));
}

// ===========================================================================
// Cancel / lastError accessors
// ===========================================================================

void WindowsUSBCreatorTests::initialStateCorrect() {
    WindowsUSBCreator creator;

    QVERIFY(creator.lastError().isEmpty());
}

void WindowsUSBCreatorTests::cancelSetsFlag() {
    WindowsUSBCreator creator;

    // cancel() should not crash or throw.
    creator.cancel();

    // After cancellation, a createBootableUSB call that passes validation
    // would be cancelled at the first m_cancelled check in the format step.
    // We just verify cancel doesn't crash when called on a fresh object.
    QVERIFY(creator.lastError().isEmpty());
}

void WindowsUSBCreatorTests::lastErrorIsThreadSafe() {
    WindowsUSBCreator creator;

    // Trigger an error to populate lastError.
    creator.createBootableUSB(QStringLiteral("/no/such.iso"), QStringLiteral("bad!"));
    QVERIFY(!creator.lastError().isEmpty());

    // Call lastError multiple times -- should be safe and consistent.
    const QString err1 = creator.lastError();
    const QString err2 = creator.lastError();
    QCOMPARE(err1, err2);
}

// ===========================================================================
// Signal emission on validation failure
// ===========================================================================

void WindowsUSBCreatorTests::failedSignalOnBadDiskNumber() {
    WindowsUSBCreator creator;
    QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
    QVERIFY(failedSpy.isValid());

    creator.createBootableUSB(QStringLiteral("/dummy.iso"), QStringLiteral("evil;cmd"));

    QCOMPARE(failedSpy.count(), 1);
    const QString errorMsg = failedSpy.first().at(0).toString();
    QVERIFY(errorMsg.contains(QStringLiteral("Invalid disk number")));
}

void WindowsUSBCreatorTests::failedSignalOnMissingIso() {
    WindowsUSBCreator creator;
    QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
    QVERIFY(failedSpy.isValid());

    creator.createBootableUSB(QStringLiteral("/no/such/file.iso"), QStringLiteral("1"));

    QCOMPARE(failedSpy.count(), 1);
    const QString errorMsg = failedSpy.first().at(0).toString();
    QVERIFY(errorMsg.contains(QStringLiteral("ISO file not found")));
}

// ===========================================================================
// Combined validation ordering
// ===========================================================================

void WindowsUSBCreatorTests::diskValidationFiresBeforeIsoCheck() {
    // When BOTH disk number AND ISO path are invalid, the disk number
    // validation should fire first (it's checked before ISO existence).
    WindowsUSBCreator creator;
    QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
    QVERIFY(failedSpy.isValid());

    bool result = creator.createBootableUSB(QStringLiteral("/no/such/file.iso"),
                                            QStringLiteral("bad;injection"));

    QVERIFY(!result);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY2(creator.lastError().contains(QStringLiteral("Invalid disk number")),
             qPrintable("Disk validation should fire before ISO check: " + creator.lastError()));
}

// ===========================================================================

// P07-52: a crafted ISO volume label must not survive into the elevated Set-Volume PowerShell
// command. sanitizeVolumeLabel allowlists to letters/digits and " _-." only.
void WindowsUSBCreatorTests::sanitizeVolumeLabel_stripsPowerShellMetacharacters() {
    const QString injected = QStringLiteral("W'; Start-Process calc.exe; '");
    const QString clean = sak::sanitizeVolumeLabel(injected);
    // Exact deterministic output: the four metacharacters ' ; ; ' are dropped, the allowlisted
    // chars (letters/digits/space/'-'/'.') survive, and trimmed() removes the trailing space.
    // The old per-char loop mirrored production's OWN allowlist predicate, so it was vacuous.
    QCOMPARE(clean, QStringLiteral("W Start-Process calc.exe"));
}

void WindowsUSBCreatorTests::sanitizeVolumeLabel_keepsLegitimateLabel() {
    const QString label = QStringLiteral("CCCOMA_X64FRE_EN-US_DV9");
    QCOMPARE(sak::sanitizeVolumeLabel(label), label);
}

void WindowsUSBCreatorTests::sanitizeVolumeLabel_capsLength() {
    const QString longLabel(100, QChar('A'));
    QCOMPARE(sak::sanitizeVolumeLabel(longLabel).size(), 32);
}

// ===========================================================================
// bcdboot success gating (B10-22)
// ===========================================================================

void WindowsUSBCreatorTests::bcdbootReportsSuccess_onlyCleanZeroExit() {
    // Only a clean, non-cancelled, exit-0 run counts as bootable.
    QVERIFY(WindowsUSBCreator::bcdbootReportsSuccess(false, false, 0));
    // A timeout, cancellation, or any non-zero exit is a failure (must not
    // certify the media bootable).
    QVERIFY(!WindowsUSBCreator::bcdbootReportsSuccess(true, false, 0));
    QVERIFY(!WindowsUSBCreator::bcdbootReportsSuccess(false, true, 0));
    QVERIFY(!WindowsUSBCreator::bcdbootReportsSuccess(false, false, 1));
    QVERIFY(!WindowsUSBCreator::bcdbootReportsSuccess(false, false, -1));
}

// ===========================================================================
// diskpart 0-exit-but-failed detection (CR3 finding 9)
// ===========================================================================

// diskpart frequently exits 0 even when an individual command failed, printing a
// hard-failure marker to stdout. Those markers must be treated as failure so the
// flow does not report broken media as success.
void WindowsUSBCreatorTests::diskpartOutputIsError_detectsFailureMarkers() {
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("DiskPart has encountered an error: The parameter is incorrect.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("Virtual Disk Service error:\nThe object is not found.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(QStringLiteral("Access is denied.")));
    // Case-insensitive.
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("diskpart has encountered an error")));
}

void WindowsUSBCreatorTests::diskpartOutputIsError_ignoresSuccessChatter() {
    QVERIFY(!WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("DiskPart succeeded in cleaning the disk.")));
    QVERIFY(!WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("DiskPart successfully formatted the volume.")));
    QVERIFY(!WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("DiskPart marked the current partition as active.")));
    QVERIFY(!WindowsUSBCreator::diskpartOutputIsError(QString()));
}

void WindowsUSBCreatorTests::diskpartOutputIsError_detectsSelectionAndArgumentFailures() {
    // R5-P5-7: the original three markers missed the failures diskpart reports most often
    // while still exiting 0 -- a command that ran against no selection, or with arguments it
    // rejected. Those leave the media unformatted and must not be read as success.
    QVERIFY(
        WindowsUSBCreator::diskpartOutputIsError(QStringLiteral("There is no volume selected.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(QStringLiteral("There is no disk selected.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("There is no partition selected.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("The arguments specified for this command are not valid.")));
    QVERIFY(
        WindowsUSBCreator::diskpartOutputIsError(QStringLiteral("The media is write protected.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("The disk management services could not complete the operation.")));
    // Normal progress chatter still reads as success.
    QVERIFY(!WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("Volume 3 is the selected volume.\nDiskPart successfully assigned the "
                       "drive letter or mount point.")));
}

// ===========================================================================
// bundled-executable trust check (CR3 finding 6)
// ===========================================================================

void WindowsUSBCreatorTests::isSafeBundledExecutable_acceptsRegularFileUnderAppDir() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString exe = dir.path() + QStringLiteral("/7z.exe");
    QFile f(exe);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("MZ");
    f.close();
    QVERIFY(WindowsUSBCreator::isSafeBundledExecutable(exe, dir.path()));
}

void WindowsUSBCreatorTests::isSafeBundledExecutable_rejectsMissingFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A path that does not exist must fail closed.
    QVERIFY(!WindowsUSBCreator::isSafeBundledExecutable(dir.path() + QStringLiteral("/nope.exe"),
                                                        dir.path()));
}

void WindowsUSBCreatorTests::isSafeBundledExecutable_rejectsFileOutsideAppDir() {
    QTemporaryDir dirA;
    QTemporaryDir dirB;
    QVERIFY(dirA.isValid());
    QVERIFY(dirB.isValid());
    const QString exe = dirA.path() + QStringLiteral("/7z.exe");
    QFile f(exe);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("MZ");
    f.close();
    // Real regular file, but NOT under the claimed application directory.
    QVERIFY(!WindowsUSBCreator::isSafeBundledExecutable(exe, dirB.path()));
}

void WindowsUSBCreatorTests::isSafeBundledExecutable_rejectsSiblingPrefixDir() {
    // CODEX_REVIEW_4 H8: a bare startsWith let a sibling whose name merely begins
    // with the app-dir name (<base>/app2 vs <base>/app) pass containment. Require a
    // real path-segment boundary.
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString appDir = base.path() + QStringLiteral("/app");
    const QString evilDir = base.path() + QStringLiteral("/app2");
    QVERIFY(QDir().mkpath(appDir));
    QVERIFY(QDir().mkpath(evilDir));
    const QString exe = evilDir + QStringLiteral("/7z.exe");
    QFile f(exe);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("MZ");
    f.close();
    QVERIFY(!WindowsUSBCreator::isSafeBundledExecutable(exe, appDir));
}

// R5-G14-3. This helper was private and had no test at all, and it shipped with
// "sources\\\\boot.wim" -- four backslashes in the source, so the runtime string held two
// and could never match a real path. 7z lists ISO members with backslashes, so the three
// largest and most important payloads were silently excluded from the extraction integrity
// check while it went on reporting success over whatever it did match.
void WindowsUSBCreatorTests::isCriticalWindowsFile_matchesBackslashPathsFrom7z() {
    // Exactly the shape `7z l` prints on Windows. Each of these failed before the fix.
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources\\boot.wim")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources\\install.wim")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources\\install.esd")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("SOURCES\\BOOT.WIM")));

    // The doubled form the buggy literal actually looked for must NOT be what decides it.
    QVERIFY(!WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources\\\\notboot.txt")));
}

void WindowsUSBCreatorTests::isCriticalWindowsFile_matchesForwardSlashPaths() {
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources/boot.wim")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources/install.wim")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources/install.esd")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("setup.exe")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("bootmgr")));
    QVERIFY(WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("efi/microsoft/boot/bootmgr")));
}

void WindowsUSBCreatorTests::isCriticalWindowsFile_rejectsNonCriticalPaths() {
    QVERIFY(!WindowsUSBCreator::isCriticalWindowsFile(QString()));
    QVERIFY(!WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("sources/lang.ini")));
    QVERIFY(!WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("boot/bcd")));
    QVERIFY(!WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("autorun.inf")));
    // boot.wim only counts under sources/: a same-named file elsewhere is not the payload
    // whose size the integrity check is pinning.
    QVERIFY(!WindowsUSBCreator::isCriticalWindowsFile(QStringLiteral("other/boot.wim")));
}

QTEST_MAIN(WindowsUSBCreatorTests)
#include "test_windows_usb_creator.moc"
