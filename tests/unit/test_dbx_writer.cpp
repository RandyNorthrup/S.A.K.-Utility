// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_dbx_writer.cpp
/// @brief Unit tests for DbxWriter Outlook Express output

#include "sak/dbx_writer.h"
#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestDbxWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Write single message
    // ====================================================================

    void testWriteSingleMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::DbxWriter writer(temp_dir.path());

        sak::PstItemDetail item;
        item.subject = QStringLiteral("DBX Test");
        item.sender_name = QStringLiteral("Tester");
        item.sender_email = QStringLiteral("test@example.com");
        item.body_plain = QStringLiteral("Hello from DBX.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QStringLiteral("Inbox"));
        QVERIFY(result.has_value());

        writer.finalize();
        QVERIFY(writer.totalBytesWritten() > 0);
    }

    // ====================================================================
    // DBX magic bytes
    // ====================================================================

    void testDbxMagicBytes() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::DbxWriter writer(temp_dir.path());

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Magic Test");
        item.sender_email = QStringLiteral("magic@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        std::ignore = writer.writeMessage(item, no_attachments, QStringLiteral("Inbox"));
        writer.finalize();

        // Find the .dbx file
        QDir dir(temp_dir.path());
        QStringList dbx_files = dir.entryList({QStringLiteral("*.dbx")}, QDir::Files, QDir::Name);
        if (!dbx_files.isEmpty()) {
            QFile file(dir.filePath(dbx_files.first()));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QByteArray header = file.read(4);

            // DBX magic: 0xCFAD12FE
            uint32_t magic = 0;
            memcpy(&magic, header.constData(), 4);
            QCOMPARE(magic, sak::DbxWriter::kDbxFolderMagic);
        }
    }

    // ====================================================================
    // Multiple folders
    // ====================================================================

    void testMultipleFolders() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::DbxWriter writer(temp_dir.path());

        QVector<QPair<QString, QByteArray>> no_attachments;

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Inbox msg");
        item.sender_email = QStringLiteral("a@test.com");
        item.body_plain = QStringLiteral("Inbox");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());
        std::ignore = writer.writeMessage(item, no_attachments, QStringLiteral("Inbox"));

        item.subject = QStringLiteral("Sent msg");
        std::ignore = writer.writeMessage(item, no_attachments, QStringLiteral("Sent"));

        writer.finalize();
        QVERIFY(writer.totalBytesWritten() > 0);
    }

    // ====================================================================
    // Initial state
    // ====================================================================

    void testInitialState() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::DbxWriter writer(temp_dir.path());
        QCOMPARE(writer.totalBytesWritten(), 0);
    }
};

QTEST_MAIN(TestDbxWriter)

#include "test_dbx_writer.moc"
