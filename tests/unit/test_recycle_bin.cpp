// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_recycle_bin.cpp
/// @brief Guard tests for the shared Recycle Bin helper (B8-23). The dangerous
///        inputs are refused BEFORE the shell call, so these run with no side
///        effect; the success path is not exercised (it would recycle a file).

#include "sak/recycle_bin.h"

#include <QDir>
#include <QString>
#include <QtTest/QtTest>

class RecycleBinTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rejectsEmptyPath() {
        // An empty path has an undefined SHFileOperationW target. A bare bool cannot tell
        // the empty-input guard from a downstream one, so pin the exact refusal reason.
        QString err;
        QVERIFY(!sak::sendPathToRecycleBin(QString(), &err));
#ifdef Q_OS_WIN
        QCOMPARE(err,
                 QStringLiteral("Refusing to recycle a path that is empty or carries a "
                                "wildcard or embedded NUL."));
#else
        QCOMPARE(err, QStringLiteral("The Recycle Bin is only available on Windows."));
#endif
        QString err2;
        QVERIFY(!sak::sendPathToRecycleBin(QStringLiteral(""), &err2));
        QCOMPARE(err2, err);  // "" takes the same empty-input branch
    }

    void rejectsWildcardPaths() {
        // SHFileOperationW EXPANDS wildcards, so a '*' or '?' would recycle every
        // match rather than the literal path -- a mass-deletion vector. Refused
        // before the shell call; pin the exact input-refusal reason so the guard that
        // fired (not a downstream one) is proven.
#ifdef Q_OS_WIN
        const QString expected = QStringLiteral(
            "Refusing to recycle a path that is empty or "
            "carries a wildcard or embedded NUL.");
#else
        const QString expected = QStringLiteral("The Recycle Bin is only available on Windows.");
#endif
        const QStringList wildcards = {QStringLiteral("X:/scratch/*"),
                                       QStringLiteral("X:/scratch/*.txt"),
                                       QStringLiteral("X:/scratch/file?.txt")};
        for (const QString& p : wildcards) {
            QString err;
            QVERIFY(!sak::sendPathToRecycleBin(p, &err));
            QCOMPARE(err, expected);
        }
    }

    void rejectsEmbeddedNul() {
        // An embedded NUL would split pFrom's double-null-terminated list into
        // extra unintended targets.
        QString withNul = QStringLiteral("X:/scratch/a.txt");
        withNul.append(QChar(QChar::Null));
        withNul.append(QStringLiteral("X:/scratch/b.txt"));
        QString err;
        QVERIFY(!sak::sendPathToRecycleBin(withNul, &err));
#ifdef Q_OS_WIN
        QCOMPARE(err,
                 QStringLiteral("Refusing to recycle a path that is empty or carries a "
                                "wildcard or embedded NUL."));
#else
        QCOMPARE(err, QStringLiteral("The Recycle Bin is only available on Windows."));
#endif
    }

    void refusesVolumesWithoutARecycleBin() {
        // R5 p9_filemgmt-7: SHFileOperationW with FOF_ALLOWUNDO deletes PERMANENTLY on
        // a volume with no Recycle Bin, so a "recycle" there silently becomes an
        // unrecoverable delete while the caller reports a recoverable one. A UNC share
        // is the canonical case and is refused on the path string alone -- before any
        // volume or network call -- so this test contacts no host.
        const QString unc = QStringLiteral("\\\\sak-no-such-host\\share\\payload.txt");
        QVERIFY(!sak::pathVolumeHasRecycleBin(unc));
        QString err;
        QVERIFY(!sak::sendPathToRecycleBin(unc, &err));
#ifdef Q_OS_WIN
        // Passes the input guard (no empty/wildcard/NUL) and is refused by the volume guard on
        // the resolved absolute path (QFileInfo normalizes the UNC to forward slashes).
        QCOMPARE(err,
                 QStringLiteral("'//sak-no-such-host/share/payload.txt' is not on a volume "
                                "with a Recycle Bin; refusing to turn a recycle into a "
                                "permanent delete."));
#else
        QCOMPARE(err, QStringLiteral("The Recycle Bin is only available on Windows."));
#endif
        // The forward-slash spelling normalizes to the same UNC path.
        QVERIFY(
            !sak::pathVolumeHasRecycleBin(QStringLiteral("//sak-no-such-host/share/payload.txt")));
        QVERIFY(!sak::pathVolumeHasRecycleBin(QString()));
    }

    void acceptsFixedVolumeWithARecycleBin() {
        // The volume guard must be a real discriminator, not a blanket refusal: the
        // fixed volume this test runs from does have a Recycle Bin, so an ordinary
        // local recycle still reaches the shell call. (The slot itself stays
        // unconditional so moc registers it on every platform; only the expected
        // verdict is platform-specific -- there is no bin to reach off Windows.)
#ifdef Q_OS_WIN
        QVERIFY(sak::pathVolumeHasRecycleBin(QDir::currentPath()));
#else
        QVERIFY(!sak::pathVolumeHasRecycleBin(QDir::currentPath()));
#endif
    }
};

QTEST_GUILESS_MAIN(RecycleBinTests)
#include "test_recycle_bin.moc"
