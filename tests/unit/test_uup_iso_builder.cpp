// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/uup_iso_builder.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
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
    void replaceFinalIso_refusesNonRegularPathsAndLeavesPriorImage();

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
    QSignalSpy phaseSpy(&builder, &UupIsoBuilder::phaseChanged);
    QVERIFY(phaseSpy.isValid());

    builder.cancel();

    // cancel() on a never-started builder must stay SILENT. The Failed transition and
    // its "Build cancelled by user" announcement live inside the phase fence at
    // src/core/uup_iso_builder.cpp:284-287; phaseChanged is the only way the panel
    // learns of a cancellation, and ~UupIsoBuilder() calls cancel() unconditionally
    // (:180-182), so hoisting that emission out of the fence would make every
    // destroyed-but-never-started builder announce a bogus cancellation.
    QCOMPARE(phaseSpy.count(), 0);
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

    // A DIRECTORY named like an expected file is not the payload. Declared size stays 0 so
    // the `file.size > 0` size compare cannot mask the isFile() arm -- only isFile() can
    // report this entry.
    UupDumpApi::FileInfo folder;
    folder.fileName = "folder.esd";
    folder.size = 0;
    QVERIFY(QDir(dir.path()).mkdir(folder.fileName));

    // An OVER-LONG file is not the artifact aria2 was told to fetch: the size compare is
    // EXACT, not a >= floor.
    UupDumpApi::FileInfo overlong;
    overlong.fileName = "long.wim";
    overlong.size = 2;                        // declared 2 ...
    writeFile(QDir(dir.path()).filePath(overlong.fileName),
              QByteArrayLiteral("toolong"));  // ...actually 7

    // A REAL, COMPLETE file whose API metadata omitted a size (FileInfo::size defaults to 0,
    // uup_dump_api.h:61) is NOT missing: the `file.size > 0` guard at uup_iso_builder.cpp:1282
    // stops an UNDECLARED size being read as "expected exactly 0 bytes". Drop that guard and
    // every size-less entry is reported, so onAria2Finished (:874-882) aborts a set that
    // downloaded perfectly. Unlike folder.esd above, this one IS a regular file, so it reaches
    // the size compare instead of short-circuiting at isFile().
    UupDumpApi::FileInfo undeclaredSize;
    undeclaredSize.fileName = "nosize.esd";      // size deliberately left at its 0 default
    writeFile(QDir(dir.path()).filePath(undeclaredSize.fileName),
              QByteArrayLiteral("real bytes"));  // 10 bytes on disk vs 0 declared

    // An unnamed API entry cannot be checked at all, so it is reported, never skipped.
    UupDumpApi::FileInfo unnamed;  // fileName deliberately left empty
    unnamed.size = 7;

    const QStringList missing = UupIsoBuilder::missingFiles(
        {present, truncated, absent, folder, overlong, undeclaredSize, unnamed}, dir.path());
    // missingFiles preserves input order and appends one entry per refusal arm it reaches:
    // truncated (short), absent, non-regular-file, over-long (exact compare), unnamed.
    // ok.esd (present, exact size) and nosize.esd (no size declared) are the entries excluded.
    QCOMPARE(missing,
             QStringList(
                 {"short.wim", "gone.esd", "folder.esd", "long.wim", "<unnamed UUP file entry>"}));
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

    // FIRST-EVER build: the destination does not exist yet. finalizeSuccessfulConversion
    // (uup_iso_builder.cpp:1545) promotes "<final>.partial" onto a user-chosen path whose
    // DIRECTORY is all that prepareConversion creates (:933); :939-942 deliberately leave
    // the final file untouched, so the no-prior-image fast path (:1302-1304) is THE
    // production path for a first build. Deleting it drops control into the aside-rename
    // block, where QFile::rename(finalPath, backupPath) at :1309 fails because finalPath
    // does not exist, and the build reports "could not be moved into place" (:1547-1550)
    // with the ISO never published. Every other fixture here pre-creates finalPath, so
    // nothing else in this file would catch that.
    const QString firstBuildPath = QDir(dir.path()).filePath("first.iso");
    const QString firstTempPath = QDir(dir.path()).filePath("first.iso.partial");
    writeFile(firstTempPath, QByteArrayLiteral("FIRST-ISO-BYTES"));
    QVERIFY(!QFile::exists(firstBuildPath));  // no prior image: the common production case

    QVERIFY(UupIsoBuilder::replaceFinalIso(firstTempPath, firstBuildPath));
    QVERIFY(!QFile::exists(firstTempPath));             // temp consumed by the rename
    QVERIFY(!QFile::exists(firstBuildPath + ".prev"));  // no backup dance when nothing to back up
    QFile firstOut(firstBuildPath);
    QVERIFY(firstOut.open(QIODevice::ReadOnly));
    QCOMPARE(firstOut.readAll(), QByteArrayLiteral("FIRST-ISO-BYTES"));
    firstOut.close();
}

void TestUupIsoBuilder::replaceFinalIso_leavesNoBackupArtifact() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString finalPath = QDir(dir.path()).filePath("win.iso");
    const QString tempPath = QDir(dir.path()).filePath("win.iso.partial");
    writeFile(finalPath, QByteArrayLiteral("OLD-ISO"));
    writeFile(tempPath, QByteArrayLiteral("FRESH-ISO-BYTES"));
    // Plant a stale backup from a crashed earlier run so the ".prev" name is REALLY in play:
    // a successful swap must clear it (uup_iso_builder.cpp:1308 then :1316), which an
    // implementation that never backs up at all would leave sitting on disk.
    writeFile(finalPath + ".prev", QByteArrayLiteral("STALE-FROM-A-CRASHED-RUN"));

    QVERIFY(UupIsoBuilder::replaceFinalIso(tempPath, finalPath));
    QVERIFY(!QFile::exists(finalPath + ".prev"));
    QVERIFY(!QFile::exists(tempPath));

    // The rename-aside is what makes the swap survivable: if the prior image cannot be moved
    // aside, the replacement must abort BEFORE the good ISO is touched. Occupying the ".prev"
    // name with a directory makes that aside-rename fail (uup_iso_builder.cpp:1309-1311).
    // A destructive remove-then-rename reports success here with the only good ISO destroyed.
    const QString keepPath = QDir(dir.path()).filePath("keep.iso");
    const QString freshPath = QDir(dir.path()).filePath("keep.iso.partial");
    writeFile(keepPath, QByteArrayLiteral("OLD-ISO"));
    writeFile(freshPath, QByteArrayLiteral("FRESH-ISO-BYTES"));
    QVERIFY(QDir(dir.path()).mkdir("keep.iso.prev"));

    QVERIFY(!UupIsoBuilder::replaceFinalIso(freshPath, keepPath));
    QFile kept(keepPath);
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArrayLiteral("OLD-ISO"));  // prior good image untouched
    kept.close();
    QVERIFY(QFile::exists(freshPath));  // converted output still there to retry with
}
void TestUupIsoBuilder::replaceFinalIso_refusesNonRegularPathsAndLeavesPriorImage() {
    // Only a real regular file may be promoted, and only a real regular file may be
    // replaced (uup_iso_builder.cpp:1293-1301). A non-regular path at either end must be
    // refused BEFORE any rename, so the prior good image survives untouched and no
    // ".prev" backup artifact is left behind.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());

    // -- Guard 1: tempPath is a directory, not a regular file -------------------
    const QString finalPath = root.filePath("win.iso");
    const QString tempDirPath = root.filePath("win.iso.partial");
    writeFile(finalPath, QByteArrayLiteral("OLD-ISO"));  // the prior good ISO
    QVERIFY(QDir(dir.path()).mkdir("win.iso.partial"));

    QVERIFY(!UupIsoBuilder::replaceFinalIso(tempDirPath, finalPath));
    QVERIFY(QDir(tempDirPath).exists());           // the directory was not consumed
    QVERIFY(!QFile::exists(finalPath + ".prev"));  // nothing was moved aside
    QFile kept(finalPath);
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArrayLiteral("OLD-ISO"));  // prior image untouched
    kept.close();

    // -- Guard 2: finalPath exists but is a directory ---------------------------
    const QString finalDirPath = root.filePath("out.iso");
    const QString tempPath = root.filePath("out.iso.partial");
    QVERIFY(QDir(dir.path()).mkdir("out.iso"));
    writeFile(tempPath, QByteArrayLiteral("FRESH-ISO-BYTES"));

    QVERIFY(!UupIsoBuilder::replaceFinalIso(tempPath, finalDirPath));
    QVERIFY(QDir(finalDirPath).exists());             // still a directory, not overwritten
    QVERIFY(!QFile::exists(finalDirPath + ".prev"));  // nothing was moved aside
    QFile fresh(tempPath);
    QVERIFY(fresh.open(QIODevice::ReadOnly));
    QCOMPARE(fresh.readAll(), QByteArrayLiteral("FRESH-ISO-BYTES"));  // temp not consumed
    fresh.close();
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

    // Accept side of the SAME sign boundary: 0 is FileInfo's default (uup_dump_api.h:61), it
    // survives the API parser (uup_dump_api.cpp:610 rejects only `size < 0`), and the rest of
    // the builder reads it as "no size declared" (the `size > 0` guards at
    // uup_iso_builder.cpp:483 and :1282). So `file.size < 0` (uup_iso_builder.cpp:192) must stay
    // strict: a `<= 0` there would refuse a legal metadata set with the bogus "negative or
    // overflowing size total" error at uup_iso_builder.cpp:243-248.
    UupDumpApi::FileInfo undeclared;  // size deliberately left at its 0 default
    const auto withUndeclared = UupIsoBuilder::computeTotalDownloadBytes({a, undeclared});
    QVERIFY(withUndeclared.has_value());
    QCOMPARE(*withUndeclared, qint64{10});  // the 0 entry contributes nothing, it does not abort

    const auto loneUndeclared = UupIsoBuilder::computeTotalDownloadBytes({undeclared});
    QVERIFY(loneUndeclared.has_value());
    QCOMPARE(*loneUndeclared, qint64{0});
}

void TestUupIsoBuilder::computeTotalDownloadBytes_rejectsOverflow() {
    UupDumpApi::FileInfo a;
    a.size = std::numeric_limits<qint64>::max();
    UupDumpApi::FileInfo b;
    b.size = 1;
    QVERIFY(!UupIsoBuilder::computeTotalDownloadBytes({a, b}).has_value());

    // Accept side of the same boundary: a total landing EXACTLY on qint64 max is a legal
    // sum, not an overflow, so `total > max - file.size` (uup_iso_builder.cpp:195) must stay
    // strict. A `>=` there would abort the build with a bogus "negative or overflowing size
    // total" (uup_iso_builder.cpp:243-248) instead of returning the sum.
    UupDumpApi::FileInfo lone;
    lone.size = std::numeric_limits<qint64>::max();
    const auto atMaxAlone = UupIsoBuilder::computeTotalDownloadBytes({lone});  // zero running total
    QVERIFY(atMaxAlone.has_value());
    QCOMPARE(*atMaxAlone, std::numeric_limits<qint64>::max());

    UupDumpApi::FileInfo justUnder;
    justUnder.size = std::numeric_limits<qint64>::max() - 1;
    UupDumpApi::FileInfo one;
    one.size = 1;
    const auto atMaxSummed =
        UupIsoBuilder::computeTotalDownloadBytes({justUnder, one});  // non-zero running total
    QVERIFY(atMaxSummed.has_value());
    QCOMPARE(*atMaxSummed, std::numeric_limits<qint64>::max());
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

    // The identifier must sit at exactly 0x8001 -- an arbitrary payload that merely CONTAINS
    // "CD001" elsewhere is not an ISO and must not be promoted by the gate at
    // src/core/uup_iso_builder.cpp:1515.
    const QString strayPath = QDir(dir.path()).filePath("straysig.iso");
    QByteArray stray(0x8006, 'X');
    stray[0x1000] = 'C';
    stray[0x1001] = 'D';
    stray[0x1002] = '0';
    stray[0x1003] = '0';
    stray[0x1004] = '1';
    writeFile(strayPath, stray);
    QVERIFY(!UupIsoBuilder::hasIso9660Signature(strayPath));

    // One byte early (0x8000 is the volume-descriptor TYPE field, not the identifier):
    // still not a PVD, so an off-by-one seek must not be accepted either.
    const QString offByOnePath = QDir(dir.path()).filePath("offbyone.iso");
    QByteArray early(0x8006, 'X');
    early[0x8000] = 'C';
    early[0x8001] = 'D';
    early[0x8002] = '0';
    early[0x8003] = '0';
    early[0x8004] = '1';
    writeFile(offByOnePath, early);
    QVERIFY(!UupIsoBuilder::hasIso9660Signature(offByOnePath));

    // Missing file entirely.
    QVERIFY(!UupIsoBuilder::hasIso9660Signature(QDir(dir.path()).filePath("nope.iso")));
}

QTEST_MAIN(TestUupIsoBuilder)
#include "test_uup_iso_builder.moc"
