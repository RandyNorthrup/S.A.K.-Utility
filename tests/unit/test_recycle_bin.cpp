// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_recycle_bin.cpp
/// @brief Guard tests for the shared Recycle Bin helper (B8-23). The dangerous
///        inputs are refused BEFORE the shell call, so these run with no side
///        effect; the success path is not exercised (it would recycle a file).

#include "sak/recycle_bin.h"

#include <QString>
#include <QtTest/QtTest>

class RecycleBinTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rejectsEmptyPath() {
        // An empty path has an undefined SHFileOperationW target.
        QVERIFY(!sak::sendPathToRecycleBin(QString()));
        QVERIFY(!sak::sendPathToRecycleBin(QStringLiteral("")));
    }

    void rejectsWildcardPaths() {
        // SHFileOperationW EXPANDS wildcards, so a '*' or '?' would recycle every
        // match rather than the literal path -- a mass-deletion vector. Refused
        // before the shell call, so nothing is touched.
        QVERIFY(!sak::sendPathToRecycleBin(QStringLiteral("X:/scratch/*")));
        QVERIFY(!sak::sendPathToRecycleBin(QStringLiteral("X:/scratch/*.txt")));
        QVERIFY(!sak::sendPathToRecycleBin(QStringLiteral("X:/scratch/file?.txt")));
    }

    void rejectsEmbeddedNul() {
        // An embedded NUL would split pFrom's double-null-terminated list into
        // extra unintended targets.
        QString withNul = QStringLiteral("X:/scratch/a.txt");
        withNul.append(QChar(QChar::Null));
        withNul.append(QStringLiteral("X:/scratch/b.txt"));
        QVERIFY(!sak::sendPathToRecycleBin(withNul));
    }
};

QTEST_GUILESS_MAIN(RecycleBinTests)
#include "test_recycle_bin.moc"
