// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_writer.cpp
/// @brief Unit tests for PstWriter. The native PST writer is not MS-PST
/// spec-conformant (no ROOT/BREF, no page/block trailers, no CRCs), so it is
/// fail-closed gated: create() refuses rather than emit a corrupt .pst that no
/// reader can mount (P05-52). These tests assert that gate.

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/pst_writer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestPstWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // create() must fail closed with not_implemented and leave no file behind.
    void createIsGatedAndEmitsNoFile() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString path = temp_dir.path() + "/output.pst";
        sak::PstWriter writer(path);

        auto create_result = writer.create();
        QVERIFY(!create_result.has_value());
        QCOMPARE(create_result.error(), sak::error_code::not_implemented);
        QVERIFY(!writer.isOpen());

        // No corrupt .pst may be produced by the gated writer.
        QVERIFY(!QFile::exists(path));
        QVERIFY(QDir(temp_dir.path()).entryList({QStringLiteral("*.pst")}, QDir::Files).isEmpty());
    }

    // An unwritable path still fails closed (never a partial file).
    void createInvalidPathAlsoFailsClosed() {
        sak::PstWriter writer(QStringLiteral("Z:/nonexistent/path/file.pst"));
        auto result = writer.create();
        QVERIFY(!result.has_value());
        QVERIFY(!writer.isOpen());
    }
};

QTEST_MAIN(TestPstWriter)

#include "test_pst_writer.moc"
