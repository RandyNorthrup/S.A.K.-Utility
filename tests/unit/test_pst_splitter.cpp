// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_splitter.cpp
/// @brief Unit tests for PstSplitter volume rotation

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/pst_splitter.h"
#include "sak/pst_writer.h"

#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestPstSplitter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Single volume (data fits)
    // ====================================================================

    void testSingleVolume() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        // 100 MB limit — will not split for one small message
        QString base = temp_dir.path() + "/archive.pst";
        constexpr qint64 kHundredMB = 100LL * 1024 * 1024;
        sak::PstSplitter splitter(base, kHundredMB);

        QVERIFY(splitter.create().has_value());

        auto folder = splitter.createFolder(sak::PstWriter::kNidRootFolder,
                                            QStringLiteral("Inbox"),
                                            QStringLiteral("IPF.Note"));
        QVERIFY(folder.has_value());

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Small Message");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Brief.");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        QVERIFY(splitter.writeMessage(folder.value(), item, no_attachments).has_value());

        QVERIFY(splitter.finalizeAll().has_value());
        QCOMPARE(splitter.volumeCount(), 1);
        QVERIFY(splitter.totalBytesWritten() > 0);
    }

    // ====================================================================
    // Error — Invalid base path
    // ====================================================================

    void testInvalidBasePath() {
        sak::PstSplitter splitter(QStringLiteral("Z:/no/such/path/archive.pst"), 1024);
        auto result = splitter.create();
        QVERIFY(!result.has_value());
    }

    // ====================================================================
    // Volume count starts at zero before create
    // ====================================================================

    void testVolumeCountBeforeCreate() {
        sak::PstSplitter splitter(QStringLiteral("dummy.pst"), 1024);
        QCOMPARE(splitter.volumeCount(), 0);
        QCOMPARE(splitter.totalBytesWritten(), 0);
    }
};

QTEST_MAIN(TestPstSplitter)

#include "test_pst_splitter.moc"
