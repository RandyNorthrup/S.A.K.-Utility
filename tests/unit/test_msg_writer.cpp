// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_msg_writer.cpp
/// @brief Unit tests for MsgWriter OLE2 compound file output

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/msg_writer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestMsgWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Plain text message
    // ====================================================================

    void testWritePlainTextMsg() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MsgWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("MSG Test");
        item.sender_name = QStringLiteral("Alice");
        item.sender_email = QStringLiteral("alice@example.com");
        item.display_to = QStringLiteral("bob@example.com");
        item.body_plain = QStringLiteral("Hello from MSG writer.");
        item.date = QDateTime(QDate(2025, 3, 15), QTime(14, 30, 0), QTimeZone::utc());

        QVector<sak::MapiProperty> props;
        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, props, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.exists());
        QVERIFY(file.size() > 0);

        // Verify OLE2 magic bytes
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray header = file.read(8);
        // CFB magic: D0 CF 11 E0 A1 B1 1A E1
        QCOMPARE(static_cast<uint8_t>(header.at(0)), 0xD0u);
        QCOMPARE(static_cast<uint8_t>(header.at(1)), 0xCFu);
        QCOMPARE(static_cast<uint8_t>(header.at(2)), 0x11u);
        QCOMPARE(static_cast<uint8_t>(header.at(3)), 0xE0u);
    }

    // ====================================================================
    // Preserves Folder Structure
    // ====================================================================

    void testPreserveFolderStructure() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MsgWriter writer(temp_dir.path(), false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Folder Test");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), QTimeZone::utc());

        QVector<sak::MapiProperty> props;
        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result =
            writer.writeMessage(item, props, no_attachments, QStringLiteral("Inbox/Projects"));
        QVERIFY(result.has_value());

        QString expected = temp_dir.path() + "/Inbox/Projects";
        QVERIFY(QDir(expected).exists());
    }

    // ====================================================================
    // Date Prefix
    // ====================================================================

    void testDatePrefix() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MsgWriter writer(temp_dir.path(), true, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Dated");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 3, 15), QTime(0, 0, 0), QTimeZone::utc());

        QVector<sak::MapiProperty> props;
        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, props, no_attachments, QString());
        QVERIFY(result.has_value());

        QFileInfo fi(result.value());
        QVERIFY(fi.fileName().startsWith("2025-03-15_"));
    }

    // ====================================================================
    // Total Bytes Written
    // ====================================================================

    void testTotalBytesWritten() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MsgWriter writer(temp_dir.path(), false, false);
        QCOMPARE(writer.totalBytesWritten(), 0);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Size Test");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Some content.");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<sak::MapiProperty> props;
        QVector<QPair<QString, QByteArray>> no_attachments;
        std::ignore = writer.writeMessage(item, props, no_attachments, QString());

        QVERIFY(writer.totalBytesWritten() > 0);
    }
};

QTEST_MAIN(TestMsgWriter)

#include "test_msg_writer.moc"
