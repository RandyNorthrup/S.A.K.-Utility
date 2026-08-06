// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_archive_service.cpp
/// @brief Unit tests for the archive engine's exclusive-create hardening
///        (Codex B6-19): compressToZip must not clobber an existing output, and
///        a normal compress -> extract round trip must still work.

#include "sak/file_explorer_archive_service.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using sak::FileExplorerArchiveService;

class FileExplorerArchiveServiceTests : public QObject {
    Q_OBJECT

    static void writeFile(const QString& path, const QByteArray& bytes) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(bytes), static_cast<qint64>(bytes.size()));
        f.close();
    }

private Q_SLOTS:
    void compressToZip_refusesExistingOutputWithoutClobber();
    void compressThenExtract_roundTrips();
    void compressToZip_missingSourceFailsClosed();
    void compressToZip_countsDirectoriesTowardEntryCap();
    void extractZip_refusesAnEntryWhoseDeclaredSizeIsALie();
};

void FileExplorerArchiveServiceTests::compressToZip_refusesExistingOutputWithoutClobber() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString src = dir.filePath(QStringLiteral("src.txt"));
    writeFile(src, QByteArrayLiteral("hello"));

    // A file already sits at the output path (simulating a raced-in file that
    // slipped past the op-layer's exists() check).
    const QString out = dir.filePath(QStringLiteral("out.zip"));
    writeFile(out, QByteArrayLiteral("ORIGINAL"));

    const auto result = FileExplorerArchiveService::compressToZip(out, {src});

    QVERIFY2(!result.ok, "must refuse to write over an existing output");
    QVERIFY(!result.blockers.isEmpty());

    // The pre-existing file must be byte-for-byte untouched (not clobbered, not
    // removed by the failure cleanup).
    QFile check(out);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), QByteArrayLiteral("ORIGINAL"));
}

void FileExplorerArchiveServiceTests::compressThenExtract_roundTrips() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString src = dir.filePath(QStringLiteral("data.txt"));
    writeFile(src, QByteArrayLiteral("payload-12345"));

    const QString zip = dir.filePath(QStringLiteral("archive.zip"));
    const auto compressed = FileExplorerArchiveService::compressToZip(zip, {src});
    QVERIFY2(compressed.ok, qPrintable(compressed.blockers.join(QStringLiteral("; "))));
    QVERIFY(QFile::exists(zip));

    const QString outdir = dir.filePath(QStringLiteral("extracted"));
    const auto extracted = FileExplorerArchiveService::extractZip(zip, outdir);
    QVERIFY2(extracted.ok, qPrintable(extracted.blockers.join(QStringLiteral("; "))));

    QFile roundtripped(QDir(outdir).filePath(QStringLiteral("data.txt")));
    QVERIFY(roundtripped.open(QIODevice::ReadOnly));
    QCOMPARE(roundtripped.readAll(), QByteArrayLiteral("payload-12345"));
}

void FileExplorerArchiveServiceTests::compressToZip_missingSourceFailsClosed() {
    // A source the caller asked to archive that no longer exists must fail the
    // compress, not be quietly downgraded to a warning that ships an archive
    // silently missing the requested item (B8-21). The half-written archive is
    // removed on failure.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("present.txt"));
    writeFile(present, QByteArrayLiteral("here"));
    const QString missing = dir.filePath(QStringLiteral("gone.txt"));  // never created
    const QString out = dir.filePath(QStringLiteral("out.zip"));

    const auto result = FileExplorerArchiveService::compressToZip(out, {present, missing});

    QVERIFY2(!result.ok, "a missing requested source must fail the compress");
    QVERIFY(!result.blockers.isEmpty());
    QVERIFY2(!QFile::exists(out), "the partial archive must be removed, not left behind");
}

void FileExplorerArchiveServiceTests::compressToZip_countsDirectoriesTowardEntryCap() {
    // Directories must count toward the archive entry cap. Before B8-21 only
    // files incremented the counter, so a tree of empty directories never
    // tripped the loop guard and bypassed the cap entirely. A bundle with one
    // wrapper dir, one subdirectory, and one file must report three entries.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("bundle/sub")));
    writeFile(root.filePath(QStringLiteral("bundle/a.txt")), QByteArrayLiteral("x"));
    const QString out = dir.filePath(QStringLiteral("out.zip"));

    const auto result =
        FileExplorerArchiveService::compressToZip(out, {root.filePath(QStringLiteral("bundle"))});

    QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
    QCOMPARE(result.entries, 3);
}

void FileExplorerArchiveServiceTests::extractZip_refusesAnEntryWhoseDeclaredSizeIsALie() {
    // R5-P9-8: the extractor sized and accepted every entry on the archive's OWN declared size
    // and never checked what the decode actually produced. A short or failed inflate was written
    // out and counted in result.entries, so the caller was told the archive extracted while the
    // file on disk was truncated -- and afterwards indistinguishable from the real one.
    //
    // The archive is built normally and then its declared uncompressed size is inflated in both
    // the local file header (offset 22) and the central directory record (offset 24). Nothing
    // else is touched, so the entry still decodes; it simply decodes to fewer bytes than it
    // claims, which is exactly the case the old code accepted.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = dir.filePath(QStringLiteral("data.txt"));
    writeFile(src, QByteArrayLiteral("payload-12345"));
    const QString zip = dir.filePath(QStringLiteral("archive.zip"));
    QVERIFY(FileExplorerArchiveService::compressToZip(zip, {src}).ok);

    QFile archive(zip);
    QVERIFY(archive.open(QIODevice::ReadOnly));
    QByteArray bytes = archive.readAll();
    archive.close();

    const auto patchSizeAt = [&bytes](const QByteArray& signature, int sizeOffset) {
        const qsizetype at = bytes.indexOf(signature);
        QVERIFY2(at >= 0, "expected zip record signature not found in the built archive");
        const quint32 lie = 4096;
        for (int i = 0; i < 4; ++i) {
            bytes[at + sizeOffset + i] = static_cast<char>((lie >> (8 * i)) & 0xFF);
        }
    };
    patchSizeAt(QByteArrayLiteral("PK\x03\x04"), 22);  // local file header, uncompressed size
    patchSizeAt(QByteArrayLiteral("PK\x01\x02"), 24);  // central directory, uncompressed size
    writeFile(zip, bytes);

    const QString outdir = dir.filePath(QStringLiteral("extracted"));
    const auto extracted = FileExplorerArchiveService::extractZip(zip, outdir);

    QVERIFY2(!extracted.ok, "an entry that decodes to fewer bytes than it declares must fail");
    QVERIFY(!extracted.blockers.isEmpty());
    // The truncated payload must not be left on disk claiming to be the extracted file.
    QVERIFY2(!QFile::exists(QDir(outdir).filePath(QStringLiteral("data.txt"))),
             "the short entry must not be written");
}

QTEST_GUILESS_MAIN(FileExplorerArchiveServiceTests)
#include "test_file_explorer_archive_service.moc"
