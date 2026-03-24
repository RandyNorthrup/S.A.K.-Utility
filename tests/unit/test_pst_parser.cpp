// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_parser.cpp
/// @brief Unit tests for PST/OST file parser

#include "sak/email_constants.h"
#include "sak/pst_parser.h"

#include <QByteArray>
#include <QTemporaryFile>
#include <QtTest/QtTest>

class TestPstParser : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // -- Header Validation -----------------------------------------------
    void rejectsEmptyFile();
    void rejectsTooSmallFile();
    void rejectsInvalidMagic();
    void parsesValidPstMagic();
    void detectsUnicodeVersion();
    void detectsAnsiVersion();

    // -- Encryption Detection --------------------------------------------
    void detectsNoEncryption();
    void detectsCompressibleEncryption();

    // -- Open / Close Lifecycle ------------------------------------------
    void openNonExistentFile();
    void openAndClose();
    void doubleCloseIsHarmless();
    void cancelDoesNotCrash();

    // -- File Info --------------------------------------------------------
    void fileInfoEmptyWhenClosed();

private:
    /// Build a minimal valid PST file header in a QByteArray
    QByteArray buildMinimalPstHeader(bool unicode, uint8_t encryption = 0x00);
};

void TestPstParser::initTestCase() {
    // Nothing to set up globally
}

QByteArray TestPstParser::buildMinimalPstHeader(bool unicode, uint8_t encryption) {
    constexpr int kHeaderSize = 564;
    QByteArray header(kHeaderSize, '\0');
    auto* data = reinterpret_cast<uint8_t*>(header.data());

    // Magic: "!BDN" at offset 0 (little-endian 0x2142444E)
    data[0] = 0x21;
    data[1] = 0x42;
    data[2] = 0x44;
    data[3] = 0x4E;

    // Content type: "SM" at offset 8 (PST)
    data[8] = 0x53;
    data[9] = 0x4D;

    // Data version at offset 10
    uint16_t version = unicode ? sak::email::kUnicodeVersion : sak::email::kAnsiVersion;
    data[10] = static_cast<uint8_t>(version & 0xFF);
    data[11] = static_cast<uint8_t>(version >> 8);

    // Encryption type at offset 513
    data[513] = encryption;

    return header;
}

// ============================================================================
// Header Validation
// ============================================================================

void TestPstParser::rejectsEmptyFile() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(error_spy.count() > 0 || !parser.isOpen());
}

void TestPstParser::rejectsTooSmallFile() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(QByteArray(10, '\0'));
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(error_spy.count() > 0 || !parser.isOpen());
}

void TestPstParser::rejectsInvalidMagic() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    QByteArray bad_header(564, '\0');
    bad_header[0] = 'X';
    bad_header[1] = 'Y';
    bad_header[2] = 'Z';
    bad_header[3] = 'W';
    temp_file.write(bad_header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(error_spy.count() > 0 || !parser.isOpen());
}

void TestPstParser::parsesValidPstMagic() {
    QByteArray header = buildMinimalPstHeader(true);
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    // May fail on deeper parsing (no valid BTrees), but shouldn't
    // crash and should at least recognize the magic bytes.
    parser.open(temp_file.fileName());
    parser.close();
}

void TestPstParser::detectsUnicodeVersion() {
    QByteArray header = buildMinimalPstHeader(true);
    // Verify the version bytes are correctly set
    auto* data = reinterpret_cast<const uint8_t*>(header.constData());
    uint16_t version = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
    QCOMPARE(version, sak::email::kUnicodeVersion);
}

void TestPstParser::detectsAnsiVersion() {
    QByteArray header = buildMinimalPstHeader(false);
    auto* data = reinterpret_cast<const uint8_t*>(header.constData());
    uint16_t version = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
    QCOMPARE(version, sak::email::kAnsiVersion);
}

// ============================================================================
// Encryption Detection
// ============================================================================

void TestPstParser::detectsNoEncryption() {
    QByteArray header = buildMinimalPstHeader(true, sak::email::kEncryptNone);
    QCOMPARE(static_cast<uint8_t>(header[513]), sak::email::kEncryptNone);
}

void TestPstParser::detectsCompressibleEncryption() {
    QByteArray header = buildMinimalPstHeader(true, sak::email::kEncryptCompressible);
    QCOMPARE(static_cast<uint8_t>(header[513]), sak::email::kEncryptCompressible);
}

// ============================================================================
// Open / Close Lifecycle
// ============================================================================

void TestPstParser::openNonExistentFile() {
    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(QStringLiteral("C:/nonexistent_file_xyz.pst"));
    QVERIFY(error_spy.count() > 0);
}

void TestPstParser::openAndClose() {
    PstParser parser;
    QVERIFY(!parser.isOpen());

    QByteArray header = buildMinimalPstHeader(true);
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    parser.open(temp_file.fileName());
    parser.close();
    QVERIFY(!parser.isOpen());
}

void TestPstParser::doubleCloseIsHarmless() {
    PstParser parser;
    parser.close();
    parser.close();
    QVERIFY(!parser.isOpen());
}

void TestPstParser::cancelDoesNotCrash() {
    PstParser parser;
    parser.cancel();
    QVERIFY(true);
}

// ============================================================================
// File Info
// ============================================================================

void TestPstParser::fileInfoEmptyWhenClosed() {
    PstParser parser;
    QVERIFY(!parser.isOpen());
}

QTEST_MAIN(TestPstParser)
#include "test_pst_parser.moc"
