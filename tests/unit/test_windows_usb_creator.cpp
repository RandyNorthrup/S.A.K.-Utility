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
 * validation that fires before formatDriveFat32/QProcess calls.
 */

#include "sak/process_runner.h"
#include "sak/windows_usb_creator.h"

#include "../support/create_file_recorder.h"
#include "../support/iso_fixture.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtEndian>
#include <QtTest>

#include <cstring>

class WindowsUSBCreatorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void installImageListing_needsTheTopLevelSourcesFolder();
    void checkInstallImage_runsTheRealToolOverARealIso();
    void finalStatus_saysFilesVerifiedNotBootable();
    void theMediaIsOneFat32Partition();
    void aSplitInstallImageIsCopiedAndReadBack();
    void theExtractionCheckWaitsForASplitImage();
    void extractionProgressIsOnTheStreamItIsReadFrom();
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

    // ---- The disk-safety check says why it could not answer ----
    void diskSafetyCheckThatCannotAnswerSaysWhyAndWritesNothing();
    void aFailureAfterTheCleanSaysTheDriveWasWritten();

    // ---- Volume label sanitization (P07-52) ----
    void sanitizeVolumeLabel_stripsPowerShellMetacharacters();
    void sanitizeVolumeLabel_keepsLegitimateLabel();
    void sanitizeVolumeLabel_capsLength();

    // ---- Bounded wait for Windows to surface the new volume (no fixed sleeps) ----
    void waitForVolume_returnsAsSoonAsTheVolumeAppears();
    void waitForVolume_readyProbeDoesNotSleep();
    void waitForVolume_timesOutFailClosed();
    void waitForVolume_honoursCancel();
    void volumeWait_showsItsTimeAndSaysWhatToDo();
    // ---- No bcdboot step, one "active", no fixed sleeps ----
    void bootSetupHasNoBcdbootStepOrDuplicateActiveOrFixedSleep();

    // ---- diskpart 0-exit-but-failed detection (CR3-9) ----
    void diskpartOutputIsError_detectsFailureMarkers();
    void diskpartOutputIsError_ignoresSuccessChatter();
    void diskpartOutputIsError_detectsSelectionAndArgumentFailures();
    void formatGivesTheVolumeALetterWhenWindowsDidNot();

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
    QCOMPARE(creator.lastError(), QStringLiteral("ISO file not found: /nonexistent/test.iso"));
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
    QCOMPARE(creator.lastError(), QStringLiteral("Invalid disk number format: ") + diskNum);
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

    // cancel() must not fabricate an error of its own.
    QCOMPARE(creator.lastError(), QString());

    // A cancel raised BEFORE a run must not survive into it: resetPerRunState() clears
    // m_cancelled at the top of createBootableUSB, so the next run must report its OWN
    // validation reason, never "Operation cancelled".
    QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
    QVERIFY(failedSpy.isValid());
    QVERIFY(!creator.createBootableUSB(QStringLiteral("/no/such.iso"), QStringLiteral("1")));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.first().at(0).toString(),
             QStringLiteral("ISO file not found: /no/such.iso"));
    QCOMPARE(creator.lastError(), QStringLiteral("ISO file not found: /no/such.iso"));
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
    QCOMPARE(errorMsg, QStringLiteral("Invalid disk number format: evil;cmd"));
}

void WindowsUSBCreatorTests::failedSignalOnMissingIso() {
    WindowsUSBCreator creator;
    QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
    QVERIFY(failedSpy.isValid());

    creator.createBootableUSB(QStringLiteral("/no/such/file.iso"), QStringLiteral("1"));

    QCOMPARE(failedSpy.count(), 1);
    const QString errorMsg = failedSpy.first().at(0).toString();
    QCOMPARE(errorMsg, QStringLiteral("ISO file not found: /no/such/file.iso"));
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

// Review 2: in a busy VM every Windows USB job failed at once with "Could not verify target disk
// 5 is safe to erase", naming no cause, and the panel added that the drive "may be left
// unusable" although nothing had touched it. The check had 5 s for a cold PowerShell and the
// Storage module, which took 3.2 s on an idle physical PC.
void WindowsUSBCreatorTests::diskSafetyCheckThatCannotAnswerSaysWhyAndWritesNothing() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iso = dir.filePath(QStringLiteral("windows.iso"));
    QFile file(iso);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(4096, 'x'));
    file.close();

    // The timed-out message names the limit, so it pins it at 30 s.
    const struct {
        sak::ProcessResult answer;
        QString expected;
    } cases[] = {
        {[] {
             sak::ProcessResult timed_out;
             timed_out.timed_out = true;
             return timed_out;
         }(),
         QStringLiteral("Could not verify target disk 5 is safe to erase: Windows did not "
                        "describe the disk within 30 s. Nothing was written to it.")},
        {[] {
             sak::ProcessResult missing;
             missing.exit_code = 0;
             missing.std_out = QStringLiteral(
                 "ERROR|No MSFT_Disk objects found with property 'Number' equal to '5'.\r\n");
             return missing;
         }(),
         QStringLiteral("Could not verify target disk 5 is safe to erase: Windows reports \"No "
                        "MSFT_Disk objects found with property 'Number' equal to '5'.\". Nothing "
                        "was written to it. The drive may have been unplugged or renumbered: "
                        "refresh the drive list and choose it again.")},
    };
    for (const auto& probe : cases) {
        QStringList asked;
        const sak::ScopedProcessFaultInjector injector(
            [&](const QString& program,
                const QStringList& args) -> std::optional<sak::ProcessResult> {
                asked << args.join(QLatin1Char(' '));
                if (program.contains(QStringLiteral("powershell"), Qt::CaseInsensitive)) {
                    return probe.answer;
                }
                return std::nullopt;
            });
        WindowsUSBCreator creator;
        QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
        QVERIFY(!creator.createBootableUSB(iso, QStringLiteral("5")));
        QCOMPARE(failedSpy.count(), 1);
        const QString error = failedSpy.first().at(0).toString();
        QVERIFY2(error.startsWith(probe.expected), qPrintable(error));
        QVERIFY(!creator.targetWasWritten());
        QCOMPARE(asked.size(), 1);
        // Windows' own reason is what reaches the message.
        QVERIFY2(asked.first().contains(QStringLiteral("'ERROR|' + $_.Exception.Message")),
                 qPrintable(asked.first()));
    }
}

// Once the clean has been started the drive may be left with nothing on it, and the panel says so.
void WindowsUSBCreatorTests::aFailureAfterTheCleanSaysTheDriveWasWritten() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iso = dir.filePath(QStringLiteral("windows.iso"));
    QFile file(iso);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(4096, 'x'));
    file.close();
    const sak::ScopedProcessFaultInjector injector(
        [](const QString& program, const QStringList&) -> std::optional<sak::ProcessResult> {
            sak::ProcessResult answer;
            answer.exit_code = 0;
            if (program.contains(QStringLiteral("powershell"), Qt::CaseInsensitive)) {
                answer.std_out = QStringLiteral("False|False|False|31042043904|USBSTOR\\DISK\r\n");
            } else {
                answer.exit_code = 1;
                answer.std_out = QStringLiteral("DiskPart has encountered an error.");
            }
            return answer;
        });
    WindowsUSBCreator creator;
    QVERIFY(!creator.createBootableUSB(iso, QStringLiteral("5")));
    QVERIFY2(creator.targetWasWritten(), qPrintable(creator.lastError()));
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
    QCOMPARE(sak::sanitizeVolumeLabel(longLabel), QString(32, QChar('A')));
    // The 32-char cap counts ALLOWLISTED characters, not raw input positions: the stripped
    // metacharacters must not consume label budget.
    const QString mixed = QStringLiteral("!!!!!!!!ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    QCOMPARE(sak::sanitizeVolumeLabel(mixed), QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"));
}

// ===========================================================================
// Bounded wait for the new volume. The creator used fixed sleeps (3 s before the
// format, 3 s after it, 3 s of polling for the letter, 2 s after extraction):
// wasted time on fast media and a premature failure on slow media.
// ===========================================================================

namespace {
constexpr int kFastPollMs = 1;
constexpr int kLongPollMs = 60'000;
constexpr int kShortTimeoutMs = 50;
constexpr int kNeverTimeoutMs = 600'000;
constexpr int kSlowBoundMs = 5000;
constexpr int kProbesUntilReady = 3;
}  // namespace

// Review 3: on the capture VM the build gave up after 60 s at "Waiting for Windows to recognize the
// new partition...", a status that never moved, with an error that said nothing about what to do.
void WindowsUSBCreatorTests::volumeWait_showsItsTimeAndSaysWhatToDo() {
    QCOMPARE(WindowsUSBCreator::volumeAppearTimeoutMs(), 180'000);
    QCOMPARE(WindowsUSBCreator::volumeWaitStatusText(25'400, 180'000),
             QStringLiteral("Waiting for Windows to recognize the new partition... 25 s of 180 s"));
    const QString error = WindowsUSBCreator::volumeNeverAppearedText(QStringLiteral("5"), 180'000);
    QVERIFY2(error.startsWith(QStringLiteral("Windows did not surface the new partition's volume "
                                             "on disk 5 within 180 seconds; nothing was "
                                             "formatted.")),
             qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("Unplug the drive, plug it in again, and run the flash "
                                           "again.")),
             qPrintable(error));
}

void WindowsUSBCreatorTests::waitForVolume_returnsAsSoonAsTheVolumeAppears() {
    int probes = 0;
    const auto outcome =
        WindowsUSBCreator::waitForVolume([&probes]() { return ++probes >= kProbesUntilReady; },
                                         []() { return false; },
                                         kNeverTimeoutMs,
                                         kFastPollMs);
    QCOMPARE(outcome, WindowsUSBCreator::VolumeWaitOutcome::Ready);
    QCOMPARE(probes, kProbesUntilReady);
}

void WindowsUSBCreatorTests::waitForVolume_readyProbeDoesNotSleep() {
    // The probe runs before any sleep: a volume that is already there costs no wait at all,
    // even with a one-minute poll interval.
    QElapsedTimer timer;
    timer.start();
    const auto outcome = WindowsUSBCreator::waitForVolume(
        []() { return true; }, []() { return false; }, kNeverTimeoutMs, kLongPollMs);
    QCOMPARE(outcome, WindowsUSBCreator::VolumeWaitOutcome::Ready);
    QVERIFY2(timer.elapsed() < kSlowBoundMs, qPrintable(QString::number(timer.elapsed())));
}

void WindowsUSBCreatorTests::waitForVolume_timesOutFailClosed() {
    int probes = 0;
    QElapsedTimer timer;
    timer.start();
    const auto outcome = WindowsUSBCreator::waitForVolume(
        [&probes]() {
            ++probes;
            return false;
        },
        []() { return false; },
        kShortTimeoutMs,
        kFastPollMs);
    QCOMPARE(outcome, WindowsUSBCreator::VolumeWaitOutcome::TimedOut);
    QVERIFY(timer.elapsed() >= kShortTimeoutMs);
    QVERIFY2(timer.elapsed() < kSlowBoundMs, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(probes > 1);
}

void WindowsUSBCreatorTests::waitForVolume_honoursCancel() {
    int probes = 0;
    const auto outcome = WindowsUSBCreator::waitForVolume(
        [&probes]() {
            ++probes;
            return true;
        },
        []() { return true; },
        kNeverTimeoutMs,
        kFastPollMs);
    QCOMPARE(outcome, WindowsUSBCreator::VolumeWaitOutcome::Cancelled);
    QCOMPARE(probes, 0);
}

// The USB maker ran bcdboot against the install media root -- its own comment recorded that
// bcdboot needs a Windows directory and install media has none -- then set the partition
// "active" a second time after extraction, padded with fixed sleeps. The media boots from the
// ISO's own boot files and the flag is set once at partition creation and verified at the end.
void WindowsUSBCreatorTests::bootSetupHasNoBcdbootStepOrDuplicateActiveOrFixedSleep() {
    const QDir repo(QStringLiteral(SAK_REPO_ROOT));
    QString source;
    for (const QString& relative : {QStringLiteral("src/core/windows_usb_creator.cpp"),
                                    QStringLiteral("src/core/windows_usb_creator_extract.cpp")}) {
        QFile file(repo.filePath(relative));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(relative));
        source += QString::fromUtf8(file.readAll());
    }
    QVERIFY2(source.size() > 0, "creator sources were not read");
    QVERIFY(!source.contains(QStringLiteral("QStringLiteral(\"bcdboot.exe\")")));
    QVERIFY(!source.contains(QStringLiteral("runBcdboot")));
    // Once in the sources, in the partition script, and once in that script.
    QCOMPARE(source.count(QStringLiteral("active\\n")), 1);
    QCOMPARE(WindowsUSBCreator::partitionScript(QStringLiteral("3"), 0)
                 .count(QStringLiteral("\nactive\n")),
             1);
    QVERIFY(!source.contains(QStringLiteral("QThread::msleep(sak::kTimer")));
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
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("DiskPart failed to clear disk attributes.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("The specified disk is not convertible.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("There is not enough usable space for this operation.")));
    QVERIFY(WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("The operation is not supported by the object.")));
    // Normal progress chatter still reads as success.
    QVERIFY(!WindowsUSBCreator::diskpartOutputIsError(
        QStringLiteral("Volume 3 is the selected volume.\nDiskPart successfully assigned the "
                       "drive letter or mount point.")));
}

// Step 1 waited a minute for a drive letter Windows never gave: it assigns one only while automount
// is on, and in the VM it gave none to a freshly formatted USB disk it treats as fixed.
void WindowsUSBCreatorTests::formatGivesTheVolumeALetterWhenWindowsDidNot() {
    QCOMPARE(WindowsUSBCreator::assignDriveLetterScript(QStringLiteral("5")),
             QStringLiteral("select disk 5\nselect partition 1\nassign\nexit\n"));

    // The format step hands the volume a letter before the wait that needs one, not after.
    QFile source(QStringLiteral(SAK_REPO_ROOT "/src/core/windows_usb_creator.cpp"));
    QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(source.readAll());
    const qsizetype format_start =
        text.indexOf(QStringLiteral("bool WindowsUSBCreator::formatPartitionFat32("));
    const qsizetype format_end =
        text.indexOf(QStringLiteral("QString WindowsUSBCreator::assignDriveLetterScript("));
    QVERIFY(format_start >= 0 && format_end > format_start);
    const QString body = text.mid(format_start, format_end - format_start);
    const qsizetype formatted = body.indexOf(QStringLiteral("runDiskpartScript(formatScript"));
    const qsizetype assigned =
        body.indexOf(QStringLiteral("assignDriveLetterIfMissing(diskNumber)"));
    QVERIFY2(formatted >= 0 && assigned > formatted, "assign after format, inside the format step");
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
    // A DIRECTORY is not a bundled executable. Without the isFile() guard the
    // canonical==canonicalDir branch would certify the app directory itself as
    // runnable, and a subdirectory would pass the containment branch.
    QVERIFY(!WindowsUSBCreator::isSafeBundledExecutable(dir.path(), dir.path()));
    QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/tools")));
    QVERIFY(!WindowsUSBCreator::isSafeBundledExecutable(dir.path() + QStringLiteral("/tools"),
                                                        dir.path()));
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

// ----- The real tool over real images. The listing below is 7z's own output for the Windows 11
// 25H2 ISO (UDF), filtered the same way; the Arch Linux ISO gives an empty listing with exit 0.
// -----
void WindowsUSBCreatorTests::installImageListing_needsTheTopLevelSourcesFolder() {
    const QString real_windows = QStringLiteral(
        "2025-09-15 12:53:56 .....   6868632137   6868633600  sources\\install.wim\r\n");
    QVERIFY(WindowsUSBCreator::listingHasInstallImage(real_windows));
    // ISO 9660 names come out in capitals.
    QVERIFY(WindowsUSBCreator::listingHasInstallImage(
        QStringLiteral("2025-09-15 12:53:56 .....  10  10  SOURCES\\INSTALL.ESD")));
    QVERIFY(WindowsUSBCreator::listingHasInstallImage(
        QStringLiteral("2025-09-15 12:53:56 .....  10  10  sources/install.swm")));

    QVERIFY(!WindowsUSBCreator::listingHasInstallImage(QString()));
    // A rescue disc carries boot.wim, and a tool may keep its own image elsewhere.
    QVERIFY(!WindowsUSBCreator::listingHasInstallImage(
        QStringLiteral("2025-09-15 12:53:44 .....  10  10  sources\\boot.wim")));
    QVERIFY(!WindowsUSBCreator::listingHasInstallImage(
        QStringLiteral("2025-09-15 12:53:44 .....  10  10  rescue\\sources\\install.wim")));
}

namespace {
QString bundledSevenZip() {
    return QDir(QFileInfo(QStringLiteral(__FILE__)).absolutePath())
        .filePath(QStringLiteral("../../tools/chocolatey/tools/7z.exe"));
}

QString writeTestFile(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    QFile file(dir.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return file.fileName();
}
}  // namespace

void WindowsUSBCreatorTests::checkInstallImage_runsTheRealToolOverARealIso() {
    const QString seven_zip = bundledSevenZip();
    QVERIFY2(QFile::exists(seven_zip), qPrintable(seven_zip));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    using Check = WindowsUSBCreator::InstallImageCheck;

    const QString install =
        writeTestFile(dir, QStringLiteral("win.iso"), sak_test::isoWithSourcesFile("INSTALL.WIM"));
    QCOMPARE(WindowsUSBCreator::checkInstallImageWith(install, seven_zip), Check::Found);

    // The rescue-disc shape: Windows-looking, boot.wim, no install image.
    const QString rescue =
        writeTestFile(dir, QStringLiteral("rescue.iso"), sak_test::isoWithSourcesFile("BOOT.WIM"));
    QCOMPARE(WindowsUSBCreator::checkInstallImageWith(rescue, seven_zip), Check::NotFound);

    // A raw disk image 7z cannot open is an ordinary thing to flash, not a failure.
    const QString raw = writeTestFile(dir, QStringLiteral("raw.img"), QByteArray(64 * 1024, 'r'));
    QCOMPARE(WindowsUSBCreator::checkInstallImageWith(raw, seven_zip), Check::NotFound);

    // No tool, no answer. The caller refuses rather than guessing a writer.
    QCOMPARE(WindowsUSBCreator::checkInstallImageWith(install,
                                                      dir.filePath(QStringLiteral("no.exe"))),
             Check::Failed);
}

// "Verified bootable" was a claim nothing here tested. It says what was checked: FAT32, the UEFI
// boot file and the install image.
void WindowsUSBCreatorTests::finalStatus_saysFilesVerifiedNotBootable() {
    const QString status = WindowsUSBCreator::finalVerificationStatus();
    QVERIFY2(!status.contains(QStringLiteral("BOOTABLE"), Qt::CaseInsensitive), qPrintable(status));
    QVERIFY2(status.contains(QStringLiteral("files verified")), qPrintable(status));
    QVERIFY2(status.contains(QStringLiteral("FAT32")), qPrintable(status));
}

// testpc1: the media was one NTFS partition, which a UEFI PC with Secure Boot, as Windows 11
// needs, does not start. It is one FAT32 partition, capped at 32 GB on a larger stick, made
// active for BIOS PCs as well.
void WindowsUSBCreatorTests::theMediaIsOneFat32Partition() {
    QCOMPARE(WindowsUSBCreator::fat32PartitionMb(31'042'043'904LL), 0);
    QCOMPARE(WindowsUSBCreator::fat32PartitionMb(32'768LL * 1024 * 1024), 0);
    QCOMPARE(WindowsUSBCreator::fat32PartitionMb(64'023'257'088LL), 32'768);
    QCOMPARE(WindowsUSBCreator::partitionScript(QStringLiteral("3"), 0),
             QStringLiteral("select disk 3\nclean\nconvert mbr\ncreate partition primary\n"
                            "select partition 1\nactive\nexit\n"));
    QVERIFY(WindowsUSBCreator::partitionScript(QStringLiteral("3"), 32'768)
                .contains(QStringLiteral("create partition primary size=32768\n")));
    QVERIFY(WindowsUSBCreator::formatFat32Script(QStringLiteral("3"), QStringLiteral("WINDOWS"))
                .contains(QStringLiteral("format FS=FAT32 QUICK label=\"WINDOWS\"")));
    // FAT32 holds a file of 4 GiB less one byte, and no more: a larger install.wim is split.
    QVERIFY(!WindowsUSBCreator::installImageNeedsSplit(4'294'967'295LL));
    QVERIFY(WindowsUSBCreator::installImageNeedsSplit(4'294'967'296LL));
    QVERIFY(WindowsUSBCreator::installImageNeedsSplit(6'868'632'137LL));  // Win11 25H2's
    const QStringList args = WindowsUSBCreator::splitImageArguments(
        QStringLiteral("C:/work/install.wim"), QStringLiteral("C:/work/parts/install.swm"));
    QCOMPARE(args,
             QStringList({QStringLiteral("/Split-Image"),
                          QStringLiteral("/ImageFile:C:\\work\\install.wim"),
                          QStringLiteral("/SWMFile:C:\\work\\parts\\install.swm"),
                          QStringLiteral("/FileSize:3800"),
                          QStringLiteral("/CheckIntegrity")}));
}

namespace {
// A stand-in for DISM /Split-Image: three parts beside the /SWMFile it is given.
std::optional<sak::ProcessResult> fakeDismSplit(QStringList* asked,
                                                const QString& program,
                                                const QStringList& args) {
    *asked << program + QLatin1Char(' ') + args.join(QLatin1Char(' '));
    QString swm;
    for (const QString& arg : args) {
        if (arg.startsWith(QStringLiteral("/SWMFile:"))) {
            swm = arg.mid(9);
        }
    }
    const QFileInfo first(swm);
    for (const QString& name : {QStringLiteral("install.swm"),
                                QStringLiteral("install2.swm"),
                                QStringLiteral("install10.swm")}) {
        QFile part(first.absoluteDir().filePath(name));
        if (!part.open(QIODevice::WriteOnly)) {
            return std::nullopt;
        }
        part.write(name.toUtf8().repeated(100));
    }
    sak::ProcessResult done;
    done.exit_code = 0;
    return done;
}
}  // namespace

// The split is DISM's; here a stand-in writes three parts. Each is copied to the drive with its
// hash, read back at the end, and a part that changed on the drive fails the verification.
void WindowsUSBCreatorTests::aSplitInstallImageIsCopiedAndReadBack() {
    QTemporaryDir work;
    QTemporaryDir drive;
    QVERIFY(work.isValid() && drive.isValid());
    const QString wim = work.filePath(QStringLiteral("install.wim"));
    QFile image(wim);
    QVERIFY(image.open(QIODevice::WriteOnly));
    image.write(QByteArray(1024, 'w'));
    image.close();
    QVERIFY(QDir(drive.path()).mkpath(QStringLiteral("sources")));
    const QString sources = drive.filePath(QStringLiteral("sources"));
    QStringList asked;
    const sak::ScopedProcessFaultInjector dism(
        [&asked](const QString& program, const QStringList& args) {
            return fakeDismSplit(&asked, program, args);
        });
    WindowsUSBCreator creator;
    QSignalSpy progress(&creator, &WindowsUSBCreator::progressUpdated);
    QVERIFY2(creator.splitInstallImageInto(QStringLiteral("C:/fake/dism.exe"), wim, sources),
             qPrintable(creator.lastError()));
    // testpc1: the bar waited at 13% through 12 minutes of split and copy. It moves with each
    // part, and stays under the 60% of a verified extraction.
    QList<int> seen;
    for (const QList<QVariant>& emitted : progress) {
        seen << emitted.first().toInt();
    }
    QCOMPARE(seen, QList<int>({52, 54, 56, 59}));
    QCOMPARE(asked.size(), 1);
    QVERIFY(asked.first().contains(QStringLiteral("/Split-Image")));
    QVERIFY(!QFileInfo::exists(wim));
    QStringList names;
    for (const auto& part : creator.splitParts()) {
        names << part.name;
        QCOMPARE(part.sha256.size(), 32);
    }
    QCOMPARE(names,
             QStringList({QStringLiteral("install.swm"),
                          QStringLiteral("install2.swm"),
                          QStringLiteral("install10.swm")}));
    {
        // testpc1: the second part read back in 17.5 s against 101 s for the first, from
        // Windows' file cache. Each part is read from a handle that bypasses it.
        const sak::test::CreateFileRecorder recorder;
        QVERIFY2(creator.verifySplitParts(sources), qPrintable(creator.lastError()));
        for (const QString& name : names) {
            QVERIFY2(sak::test::CreateFileRecorder::readOnlyUnbuffered(
                         QDir(sources).filePath(name)),
                     qPrintable(name + QStringLiteral(" was not read back unbuffered")));
        }
    }

    // One byte changed on the drive, same size.
    QFile changed(QDir(sources).filePath(QStringLiteral("install2.swm")));
    QVERIFY(changed.open(QIODevice::ReadWrite));
    changed.seek(10);
    changed.write("X");
    changed.close();
    QVERIFY(!creator.verifySplitParts(sources));
    QVERIFY2(creator.lastError().contains(QStringLiteral("install2.swm")),
             qPrintable(creator.lastError()));
}

// testpc1: the check after the extraction asked for install.wim, which a split leaves out of it on
// purpose, so every FAT32 build of a Windows 11 ISO failed there. Unsplit, the image is required.
void WindowsUSBCreatorTests::theExtractionCheckWaitsForASplitImage() {
    QTemporaryDir drive;
    QVERIFY(drive.isValid());
    const QString root = QDir::toNativeSeparators(drive.path()) + QLatin1Char('\\');
    QVERIFY(QDir(drive.path()).mkpath(QStringLiteral("sources")));
    for (const QString& name : {QStringLiteral("bootmgr"), QStringLiteral("sources/boot.wim")}) {
        QFile file(drive.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QCOMPARE(WindowsUSBCreator::extractedBootFilesProblem(root, true), QString());
    QVERIFY(WindowsUSBCreator::extractedBootFilesProblem(root, false)
                .contains(QStringLiteral("No Windows install image")));
    QVERIFY(QFile::remove(drive.filePath(QStringLiteral("sources/boot.wim"))));
    QVERIFY(WindowsUSBCreator::extractedBootFilesProblem(root, true)
                .contains(QStringLiteral("sources/boot.wim")));
}

// testpc1: the build read 7-Zip's progress from stdout, and -bsp2 sends it to stderr, so the bar
// sat at 13% for the whole extraction. Measured there with the bundled 7-Zip 26.05: with -bsp2 the
// line " 20% - sources\boot.wim" came on stderr and none on stdout; with -bsp1, on stdout.
void WindowsUSBCreatorTests::extractionProgressIsOnTheStreamItIsReadFrom() {
    const QString iso = QStringLiteral("C:\\SAK-Samples\\windows.iso");
    const QStringList split =
        WindowsUSBCreator::extractionArguments(iso, QStringLiteral("E:\\"), true);
    QVERIFY2(split.contains(QStringLiteral("-bsp1")), qPrintable(split.join(QLatin1Char(' '))));
    QVERIFY(!split.contains(QStringLiteral("-bsp2")));
    QVERIFY(split.contains(QStringLiteral("-x!sources\\install.wim")));
    QVERIFY(!WindowsUSBCreator::extractionArguments(iso, QStringLiteral("E:\\"), false)
                 .contains(QStringLiteral("-x!sources\\install.wim")));
    // Those bytes, as 7-Zip rewrites its line in place; the newest percentage counts.
    const QString chunk = QStringLiteral(
        "  0% 100 Open\r             \r--\r\n 20% - sources\\boot.wim\r                       \r"
        " 46% - sources\\boot.wim\r");
    QCOMPARE(WindowsUSBCreator::extractionPercentIn(chunk), 46);
    QCOMPARE(WindowsUSBCreator::extractionPercentIn(
                 QStringLiteral("Path = C:\\w.iso\r\nType = Udf\r\n")),
             -1);
}

QTEST_MAIN(WindowsUSBCreatorTests)
#include "test_windows_usb_creator.moc"
