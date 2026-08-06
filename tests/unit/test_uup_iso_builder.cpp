// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/uup_iso_builder.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

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
    // -- Construction defaults -------------------------------
    void testInitialPhase();
    void testNotRunningInitially();

    // -- Cancel when idle ------------------------------------
    void testCancelWhenIdle();

    // -- Download-set completeness (B10-21) ------------------
    void missingFiles_allPresentComplete_returnsEmpty();
    void missingFiles_absentOrTruncated_reported();

    // -- Convert-then-replace (B10-21) -----------------------
    void replaceFinalIso_movesOverExisting();
    void replaceFinalIso_leavesNoBackupArtifact();

    // -- Metadata size accumulation (R3-14) ------------------
    void computeTotalDownloadBytes_sumsValidSizes();
    void computeTotalDownloadBytes_rejectsNegativeSize();
    void computeTotalDownloadBytes_rejectsOverflow();

    // -- ISO 9660 structural signature (R3-05) ---------------
    void hasIso9660Signature_acceptsValidPvd();
    void hasIso9660Signature_rejectsNonIso();
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

void TestUupIsoBuilder::replaceFinalIso_leavesNoBackupArtifact() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString finalPath = QDir(dir.path()).filePath("win.iso");
    const QString tempPath = QDir(dir.path()).filePath("win.iso.partial");
    writeFile(finalPath, QByteArrayLiteral("OLD-ISO"));
    writeFile(tempPath, QByteArrayLiteral("FRESH-ISO-BYTES"));

    QVERIFY(UupIsoBuilder::replaceFinalIso(tempPath, finalPath));
    // The prior image is moved aside by rename during the swap, then dropped: no
    // ".prev" backup artifact must survive a successful replacement.
    QVERIFY(!QFile::exists(finalPath + ".prev"));
    QVERIFY(!QFile::exists(tempPath));
}

// ============================================================================
// Metadata size accumulation (R3-14): reject negative and overflowing totals.
// ============================================================================

void TestUupIsoBuilder::computeTotalDownloadBytes_sumsValidSizes() {
    UupDumpApi::FileInfo a;
    a.size = 100;
    UupDumpApi::FileInfo b;
    b.size = 250;
    const auto total = UupIsoBuilder::computeTotalDownloadBytes({a, b});
    QVERIFY(total.has_value());
    QCOMPARE(*total, qint64{350});
}

void TestUupIsoBuilder::computeTotalDownloadBytes_rejectsNegativeSize() {
    UupDumpApi::FileInfo a;
    a.size = 10;
    UupDumpApi::FileInfo bad;
    bad.size = -1;
    QVERIFY(!UupIsoBuilder::computeTotalDownloadBytes({a, bad}).has_value());
}

void TestUupIsoBuilder::computeTotalDownloadBytes_rejectsOverflow() {
    UupDumpApi::FileInfo a;
    a.size = std::numeric_limits<qint64>::max();
    UupDumpApi::FileInfo b;
    b.size = 1;
    QVERIFY(!UupIsoBuilder::computeTotalDownloadBytes({a, b}).has_value());
}

// ============================================================================
// ISO 9660 structural signature (R3-05): "CD001" at byte offset 0x8001.
// ============================================================================

void TestUupIsoBuilder::hasIso9660Signature_acceptsValidPvd() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("valid.iso");

    QByteArray image(0x8006, '\0');
    image[0x8001] = 'C';
    image[0x8002] = 'D';
    image[0x8003] = '0';
    image[0x8004] = '0';
    image[0x8005] = '1';
    writeFile(path, image);

    QVERIFY(UupIsoBuilder::hasIso9660Signature(path));
}

void TestUupIsoBuilder::hasIso9660Signature_rejectsNonIso() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A short, non-ISO payload (a zero-exit converter that produced garbage).
    const QString shortPath = QDir(dir.path()).filePath("garbage.iso");
    writeFile(shortPath, QByteArrayLiteral("not-an-iso"));
    QVERIFY(!UupIsoBuilder::hasIso9660Signature(shortPath));

    // Right length but wrong signature bytes at the PVD offset.
    const QString wrongPath = QDir(dir.path()).filePath("wrongsig.iso");
    writeFile(wrongPath, QByteArray(0x8006, 'X'));
    QVERIFY(!UupIsoBuilder::hasIso9660Signature(wrongPath));

    // Missing file entirely.
    QVERIFY(!UupIsoBuilder::hasIso9660Signature(QDir(dir.path()).filePath("nope.iso")));
}

QTEST_MAIN(TestUupIsoBuilder)
#include "test_uup_iso_builder.moc"
