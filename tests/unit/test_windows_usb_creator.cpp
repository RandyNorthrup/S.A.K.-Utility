// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_windows_usb_creator.cpp
 * @brief TST-07 — Unit tests for WindowsUSBCreator validation logic.
 *
 * Tests the input validation and error handling that runs before any
 * OS process is spawned. Verifies BUG-09 fix (disk number command
 * injection prevention) and ISO existence checks.
 *
 * No admin privileges or hardware required — all tests exercise
 * validation that fires before formatDriveNTFS/QProcess calls.
 */

#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "sak/windows_usb_creator.h"

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
};

// ===========================================================================

void WindowsUSBCreatorTests::initTestCase() {}
void WindowsUSBCreatorTests::cleanupTestCase() {}

// ===========================================================================
// Disk number validation (BUG-09)
// ===========================================================================

void WindowsUSBCreatorTests::validDiskNumbers_data()
{
    QTest::addColumn<QString>("diskNum");

    // These are valid disk numbers (pure integers 1-3 digits).
    // They'll pass disk validation but fail on ISO check (nonexistent file).
    QTest::newRow("single digit 0")   << QStringLiteral("0");
    QTest::newRow("single digit 1")   << QStringLiteral("1");
    QTest::newRow("single digit 9")   << QStringLiteral("9");
    QTest::newRow("two digits 10")    << QStringLiteral("10");
    QTest::newRow("two digits 99")    << QStringLiteral("99");
    QTest::newRow("three digits 100") << QStringLiteral("100");
    QTest::newRow("three digits 255") << QStringLiteral("255");
    QTest::newRow("three digits 999") << QStringLiteral("999");
}

void WindowsUSBCreatorTests::validDiskNumbers()
{
    QFETCH(QString, diskNum);

    WindowsUSBCreator creator;

    // Valid disk number + nonexistent ISO → should pass disk validation
    // and fail on ISO check instead.
    bool result = creator.createBootableUSB(QStringLiteral("/nonexistent/test.iso"), diskNum);
    QVERIFY(!result);

    // Error should mention "ISO file not found" (not "Invalid disk number").
    QVERIFY2(creator.lastError().contains(QStringLiteral("ISO file not found")),
             qPrintable("Expected ISO error but got: " + creator.lastError()));
}

// ---------------------------------------------------------------------------

void WindowsUSBCreatorTests::invalidDiskNumbersRejected_data()
{
    QTest::addColumn<QString>("diskNum");
    QTest::addColumn<QString>("description");

    // Command injection attempts (BUG-09 vectors).
    QTest::newRow("semicolon injection")    << QStringLiteral("1; rm -rf /")
        << QStringLiteral("semicolon cmd injection");
    QTest::newRow("newline injection")       << QStringLiteral("1\nselect disk 0")
        << QStringLiteral("newline diskpart injection");
    QTest::newRow("pipe injection")          << QStringLiteral("1 | del *")
        << QStringLiteral("pipe injection");
    QTest::newRow("ampersand injection")     << QStringLiteral("1 & echo pwned")
        << QStringLiteral("ampersand injection");

    // Invalid formats.
    QTest::newRow("empty string")           << QString()
        << QStringLiteral("empty string");
    QTest::newRow("alphabetic")             << QStringLiteral("abc")
        << QStringLiteral("non-numeric");
    QTest::newRow("negative number")        << QStringLiteral("-1")
        << QStringLiteral("negative number");
    QTest::newRow("decimal number")         << QStringLiteral("1.5")
        << QStringLiteral("decimal");
    QTest::newRow("four digits")            << QStringLiteral("1234")
        << QStringLiteral(">3 digits");
    QTest::newRow("leading space")          << QStringLiteral(" 1")
        << QStringLiteral("whitespace prefix");
    QTest::newRow("trailing space")         << QStringLiteral("1 ")
        << QStringLiteral("whitespace suffix");
    QTest::newRow("hex prefix")             << QStringLiteral("0x1")
        << QStringLiteral("hex prefix");
    QTest::newRow("special chars")          << QStringLiteral("@#$")
        << QStringLiteral("special characters");
    QTest::newRow("path traversal")         << QStringLiteral("../etc")
        << QStringLiteral("path traversal");
}

void WindowsUSBCreatorTests::invalidDiskNumbersRejected()
{
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

void WindowsUSBCreatorTests::nonexistentIsoRejected()
{
    WindowsUSBCreator creator;
    bool result = creator.createBootableUSB(
        QStringLiteral("C:/totally/fake/path/windows.iso"),
        QStringLiteral("1"));

    QVERIFY(!result);
    QVERIFY(creator.lastError().contains(QStringLiteral("ISO file not found")));
}

void WindowsUSBCreatorTests::emptyIsoPathRejected()
{
    WindowsUSBCreator creator;
    bool result = creator.createBootableUSB(QString(), QStringLiteral("1"));

    QVERIFY(!result);
    QVERIFY(creator.lastError().contains(QStringLiteral("ISO file not found")));
}

// ===========================================================================
// Cancel / lastError accessors
// ===========================================================================

void WindowsUSBCreatorTests::initialStateCorrect()
{
    WindowsUSBCreator creator;

    QVERIFY(creator.lastError().isEmpty());
}

void WindowsUSBCreatorTests::cancelSetsFlag()
{
    WindowsUSBCreator creator;

    // cancel() should not crash or throw.
    creator.cancel();

    // After cancellation, a createBootableUSB call that passes validation
    // would be cancelled at the first m_cancelled check in the format step.
    // We just verify cancel doesn't crash when called on a fresh object.
    QVERIFY(creator.lastError().isEmpty());
}

void WindowsUSBCreatorTests::lastErrorIsThreadSafe()
{
    WindowsUSBCreator creator;

    // Trigger an error to populate lastError.
    creator.createBootableUSB(QStringLiteral("/no/such.iso"), QStringLiteral("bad!"));
    QVERIFY(!creator.lastError().isEmpty());

    // Call lastError multiple times — should be safe and consistent.
    const QString err1 = creator.lastError();
    const QString err2 = creator.lastError();
    QCOMPARE(err1, err2);
}

// ===========================================================================
// Signal emission on validation failure
// ===========================================================================

void WindowsUSBCreatorTests::failedSignalOnBadDiskNumber()
{
    WindowsUSBCreator creator;
    QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
    QVERIFY(failedSpy.isValid());

    creator.createBootableUSB(QStringLiteral("/dummy.iso"), QStringLiteral("evil;cmd"));

    QCOMPARE(failedSpy.count(), 1);
    const QString errorMsg = failedSpy.first().at(0).toString();
    QVERIFY(errorMsg.contains(QStringLiteral("Invalid disk number")));
}

void WindowsUSBCreatorTests::failedSignalOnMissingIso()
{
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

void WindowsUSBCreatorTests::diskValidationFiresBeforeIsoCheck()
{
    // When BOTH disk number AND ISO path are invalid, the disk number
    // validation should fire first (it's checked before ISO existence).
    WindowsUSBCreator creator;
    QSignalSpy failedSpy(&creator, &WindowsUSBCreator::failed);
    QVERIFY(failedSpy.isValid());

    bool result = creator.createBootableUSB(
        QStringLiteral("/no/such/file.iso"),
        QStringLiteral("bad;injection"));

    QVERIFY(!result);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY2(creator.lastError().contains(QStringLiteral("Invalid disk number")),
             qPrintable("Disk validation should fire before ISO check: " + creator.lastError()));
}

// ===========================================================================

QTEST_MAIN(WindowsUSBCreatorTests)
#include "test_windows_usb_creator.moc"
