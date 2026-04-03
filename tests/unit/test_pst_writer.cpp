// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_writer.cpp
/// @brief Unit tests for PstWriter binary PST creation

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/pst_writer.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

class TestPstWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Create empty PST and finalize
    // ====================================================================

    void testCreateEmptyPst() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString path = temp_dir.path() + "/output.pst";
        sak::PstWriter writer(path);

        auto create_result = writer.create();
        QVERIFY(create_result.has_value());
        QVERIFY(writer.isOpen());

        auto finalize_result = writer.finalize();
        QVERIFY(finalize_result.has_value());

        QFile file(path);
        QVERIFY(file.exists());
        QVERIFY(file.size() > 0);
    }

    // ====================================================================
    // Happy Path — Write a single message
    // ====================================================================

    void testWriteSingleMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString path = temp_dir.path() + "/single.pst";
        sak::PstWriter writer(path);

        auto create_result = writer.create();
        QVERIFY(create_result.has_value());

        auto folder_result = writer.createFolder(sak::PstWriter::kNidRootFolder,
                                                 QStringLiteral("Inbox"),
                                                 QStringLiteral("IPF.Note"));
        QVERIFY(folder_result.has_value());

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Test Message");
        item.sender_name = QStringLiteral("Sender");
        item.sender_email = QStringLiteral("sender@example.com");
        item.body_plain = QStringLiteral("Hello world.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        auto write_result = writer.writeMessage(folder_result.value(), item, no_attachments);
        QVERIFY(write_result.has_value());

        auto finalize_result = writer.finalize();
        QVERIFY(finalize_result.has_value());

        QVERIFY(writer.currentSize() > 0);
    }

    // ====================================================================
    // PST Magic Bytes Validation
    // ====================================================================

    void testPstMagicBytes() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString path = temp_dir.path() + "/magic.pst";
        sak::PstWriter writer(path);
        QVERIFY(writer.create().has_value());
        QVERIFY(writer.finalize().has_value());

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray header = file.read(4);
        QCOMPARE(header.size(), 4);

        // MS-PST magic: "!BDN" = 0x2142444E little-endian = 0x4E444221
        uint32_t magic = 0;
        memcpy(&magic, header.constData(), 4);
        QCOMPARE(magic, sak::PstWriter::kPstMagic);
    }

    // ====================================================================
    // Message with Attachments
    // ====================================================================

    void testWriteMessageWithAttachments() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString path = temp_dir.path() + "/attach.pst";
        sak::PstWriter writer(path);
        QVERIFY(writer.create().has_value());

        auto folder_result = writer.createFolder(sak::PstWriter::kNidRootFolder,
                                                 QStringLiteral("Inbox"),
                                                 QStringLiteral("IPF.Note"));
        QVERIFY(folder_result.has_value());

        sak::PstItemDetail item;
        item.subject = QStringLiteral("With Attachment");
        item.sender_email = QStringLiteral("test@example.com");
        item.body_plain = QStringLiteral("See attached.");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> attachments;
        attachments.append({QStringLiteral("file.txt"), QByteArray("file content here")});

        auto write_result = writer.writeMessage(folder_result.value(), item, attachments);
        QVERIFY(write_result.has_value());
        QVERIFY(writer.finalize().has_value());
    }

    // ====================================================================
    // Nested Folders
    // ====================================================================

    void testNestedFolders() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString path = temp_dir.path() + "/nested.pst";
        sak::PstWriter writer(path);
        QVERIFY(writer.create().has_value());

        auto inbox = writer.createFolder(sak::PstWriter::kNidRootFolder,
                                         QStringLiteral("Inbox"),
                                         QStringLiteral("IPF.Note"));
        QVERIFY(inbox.has_value());

        auto sub = writer.createFolder(inbox.value(),
                                       QStringLiteral("Projects"),
                                       QStringLiteral("IPF.Note"));
        QVERIFY(sub.has_value());
        QVERIFY(sub.value() != inbox.value());

        QVERIFY(writer.finalize().has_value());
    }

    // ====================================================================
    // Display Name
    // ====================================================================

    void testSetDisplayName() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString path = temp_dir.path() + "/named.pst";
        sak::PstWriter writer(path);
        writer.setDisplayName(QStringLiteral("My Archive"));
        QVERIFY(writer.create().has_value());
        QVERIFY(writer.finalize().has_value());
        QVERIFY(QFile::exists(path));
    }

    // ====================================================================
    // Error — Create with invalid path
    // ====================================================================

    void testCreateInvalidPath() {
        sak::PstWriter writer(QStringLiteral("Z:/nonexistent/path/file.pst"));
        auto result = writer.create();
        QVERIFY(!result.has_value());
    }
};

QTEST_MAIN(TestPstWriter)

#include "test_pst_writer.moc"
