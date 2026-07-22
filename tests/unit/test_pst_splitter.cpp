// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_splitter.cpp
/// @brief Unit tests for PstSplitter volume rotation

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/pst_splitter.h"
#include "sak/pst_writer.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestPstSplitter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // create() delegates to PstWriter::create(), which is fail-closed gated
    // because the native PST output is not spec-conformant (P05-52). The split
    // path is therefore gated by the same guard and emits no volume files.
    // ====================================================================

    void createIsGatedByWriter() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QString base = temp_dir.path() + "/archive.pst";
        constexpr qint64 kHundredMB = 100LL * 1024 * 1024;
        sak::PstSplitter splitter(base, kHundredMB);

        auto result = splitter.create();
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::not_implemented);

        // No corrupt volume .pst files may be produced.
        QVERIFY(QDir(temp_dir.path()).entryList({QStringLiteral("*.pst")}, QDir::Files).isEmpty());
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
