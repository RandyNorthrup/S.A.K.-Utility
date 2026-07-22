// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_dbx_writer.cpp
/// @brief Unit tests for DbxWriter. The Outlook Express DBX writer is not
/// spec-conformant (no OE5/6 B-tree index of MessageInfo nodes), so it is
/// fail-closed gated: writeMessage() refuses rather than emit a .dbx no reader
/// can enumerate (P05-01). These tests assert that gate.

#include "sak/dbx_writer.h"
#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

class TestDbxWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // writeMessage() must fail closed with not_implemented and emit no .dbx file.
    void writeMessageIsGatedAndEmitsNoFile() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::DbxWriter writer(temp_dir.path());

        sak::PstItemDetail item;
        item.subject = QStringLiteral("DBX Test");
        item.sender_email = QStringLiteral("test@example.com");
        item.body_plain = QStringLiteral("Hello from DBX.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        auto result = writer.writeMessage(item, no_attachments, QStringLiteral("Inbox"));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::not_implemented);
        writer.finalize();

        QCOMPARE(writer.totalBytesWritten(), 0);
        QVERIFY(QDir(temp_dir.path()).entryList({QStringLiteral("*.dbx")}, QDir::Files).isEmpty());
    }

    void testInitialState() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::DbxWriter writer(temp_dir.path());
        QCOMPARE(writer.totalBytesWritten(), 0);
    }
};

QTEST_MAIN(TestDbxWriter)

#include "test_dbx_writer.moc"
