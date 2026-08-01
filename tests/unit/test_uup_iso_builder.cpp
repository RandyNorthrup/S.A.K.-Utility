// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/uup_iso_builder.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

/**
 * @brief Unit tests for UupIsoBuilder.
 *
 * Covers construction defaults, phase queries, and
 * running state. Actual ISO building requires network
 * access and external tools; not tested here.
 */
class TestUupIsoBuilder : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction defaults ───────────────────────────────
    void testInitialPhase();
    void testNotRunningInitially();

    // ── Cancel when idle ────────────────────────────────────
    void testCancelWhenIdle();

    // ── Download-set completeness (B10-21) ──────────────────
    void missingFiles_allPresentComplete_returnsEmpty();
    void missingFiles_absentOrTruncated_reported();

    // ── Convert-then-replace (B10-21) ───────────────────────
    void replaceFinalIso_movesOverExisting();
};

// ============================================================================
// Construction
// ============================================================================

void TestUupIsoBuilder::testInitialPhase() {
    UupIsoBuilder builder;
    QCOMPARE(builder.currentPhase(), UupIsoBuilder::Phase::Idle);
}

void TestUupIsoBuilder::testNotRunningInitially() {
    UupIsoBuilder builder;
    QVERIFY(!builder.isRunning());
}

// ============================================================================
// Cancel
// ============================================================================

void TestUupIsoBuilder::testCancelWhenIdle() {
    UupIsoBuilder builder;
    builder.cancel();
    QCOMPARE(builder.currentPhase(), UupIsoBuilder::Phase::Idle);
}

// ============================================================================
// Download-set completeness (B10-21)
// ============================================================================

static void writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    f.close();
}

void TestUupIsoBuilder::missingFiles_allPresentComplete_returnsEmpty() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    UupDumpApi::FileInfo a;
    a.fileName = "core.esd";
    a.size = 5;
    UupDumpApi::FileInfo b;
    b.fileName = "boot.wim";
    b.size = 3;
    writeFile(QDir(dir.path()).filePath(a.fileName), QByteArrayLiteral("hello"));  // 5 bytes
    writeFile(QDir(dir.path()).filePath(b.fileName), QByteArrayLiteral("abc"));    // 3 bytes

    QVERIFY(UupIsoBuilder::missingFiles({a, b}, dir.path()).isEmpty());
}

void TestUupIsoBuilder::missingFiles_absentOrTruncated_reported() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    UupDumpApi::FileInfo present;
    present.fileName = "ok.esd";
    present.size = 4;
    writeFile(QDir(dir.path()).filePath(present.fileName), QByteArrayLiteral("data"));  // 4 bytes

    UupDumpApi::FileInfo truncated;
    truncated.fileName = "short.wim";
    truncated.size = 100;                  // declared 100 ...
    writeFile(QDir(dir.path()).filePath(truncated.fileName),
              QByteArrayLiteral("tiny"));  // ...only 4

    UupDumpApi::FileInfo absent;
    absent.fileName = "gone.esd";
    absent.size = 10;

    const QStringList missing = UupIsoBuilder::missingFiles({present, truncated, absent},
                                                            dir.path());
    QCOMPARE(missing.size(), 2);
    QVERIFY(missing.contains("short.wim"));
    QVERIFY(missing.contains("gone.esd"));
    QVERIFY(!missing.contains("ok.esd"));
}

// ============================================================================
// Convert-then-replace (B10-21)
// ============================================================================

void TestUupIsoBuilder::replaceFinalIso_movesOverExisting() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString finalPath = QDir(dir.path()).filePath("win.iso");
    const QString tempPath = QDir(dir.path()).filePath("win.iso.partial");
    writeFile(finalPath, QByteArrayLiteral("OLD-ISO"));         // a prior good ISO
    writeFile(tempPath, QByteArrayLiteral("FRESH-ISO-BYTES"));  // freshly converted

    QVERIFY(UupIsoBuilder::replaceFinalIso(tempPath, finalPath));
    QVERIFY(!QFile::exists(tempPath));  // temp consumed by the move

    QFile out(finalPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), QByteArrayLiteral("FRESH-ISO-BYTES"));
    out.close();
}

QTEST_MAIN(TestUupIsoBuilder)
#include "test_uup_iso_builder.moc"
