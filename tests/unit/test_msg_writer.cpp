// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_msg_writer.cpp
/// @brief Unit tests for MsgWriter. The compound-file (CFB/MS-OXMSG) writer is
/// not spec-conformant -- an independent CFB reader (olefile) could enumerate
/// only 1 of 4 streams and read it as 0 bytes -- so it is fail-closed gated:
/// writeMessage() refuses rather than emit a .msg Outlook/MAPI cannot open
/// (P05-30). These tests assert that gate.

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/msg_writer.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

class TestMsgWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // writeMessage() must fail closed with not_implemented and emit no .msg file.
    void writeMessageIsGatedAndEmitsNoFile() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MsgWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("MSG Test");
        item.sender_email = QStringLiteral("alice@example.com");
        item.body_plain = QStringLiteral("Hello from MSG writer.");
        item.date = QDateTime(QDate(2025, 3, 15), QTime(14, 30, 0), QTimeZone::utc());

        QVector<sak::MapiProperty> props;
        QVector<QPair<QString, QByteArray>> no_attachments;
        auto result = writer.writeMessage(item, props, no_attachments, QString());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::not_implemented);
        QCOMPARE(writer.totalBytesWritten(), 0);
        QVERIFY(QDir(temp_dir.path()).entryList({QStringLiteral("*.msg")}, QDir::Files).isEmpty());
    }

    void testInitialBytesWrittenZero() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MsgWriter writer(temp_dir.path(), false, false);
        QCOMPARE(writer.totalBytesWritten(), 0);
    }
};

QTEST_MAIN(TestMsgWriter)

#include "test_msg_writer.moc"
