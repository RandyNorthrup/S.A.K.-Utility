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
    void compressToZip_refusesAnArchiveInsideASourceFolder();
    void extractZip_refusesAnEntryWhoseDeclaredSizeIsALie();
    void extractZip_refusesAnEntryThatOverProducesItsDeclaredSize();
};

void FileExplorerArchiveServiceTests::compressToZip_refusesAnArchiveInsideASourceFolder() {
    // compressToZip refuses on four distinct conditions, and every refusal assertion in the tree
    // landed on the SAME one (the NewOnly exclusive-create refusal, proved here and again in the
    // panel suite). This guard -- which refuses an archive landing INSIDE a folder being
    // compressed -- was reached by nothing: its message string appears exactly once in the whole
    // repository, in the production source itself. Deleting it leaves every suite green while the
    // recursive walk reaches the archive file mid-write and folds a partial copy of the zip back
    // into itself: a self-referential, silently corrupt archive reported as a successful compress.
    // The exclusive-create refusal cannot stand in for it, because the archive path does not exist
    // yet when this case fires.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("bundle/sub")));
    writeFile(root.filePath(QStringLiteral("bundle/a.txt")), QByteArrayLiteral("x"));
    const QString folder = root.filePath(QStringLiteral("bundle"));

    // Directly inside the source folder.
    const QString inside = root.filePath(QStringLiteral("bundle/out.zip"));
    const auto direct = FileExplorerArchiveService::compressToZip(inside, {folder});
    QVERIFY2(!direct.ok, "an archive written inside a source folder must be refused");
    QCOMPARE(direct.blockers.size(), 1);
    QCOMPARE(direct.blockers.first(),
             QStringLiteral("Refused to write the archive inside source folder %1.").arg(folder));
    QVERIFY2(!QFile::exists(inside), "the refused archive must never be created");

    // ... and nested deeper, which the startsWith arm of the same guard covers.
    const QString nested = root.filePath(QStringLiteral("bundle/sub/out.zip"));
    const auto deep = FileExplorerArchiveService::compressToZip(nested, {folder});
    QVERIFY2(!deep.ok, "an archive nested deeper inside a source folder must be refused");
    QCOMPARE(deep.blockers.size(), 1);
    QVERIFY2(!QFile::exists(nested), "the refused archive must never be created");

    // Control: a SIBLING path sharing the folder's name prefix is not inside it, and must still
    // compress -- the guard appends '/' precisely so "bundle-2" is not read as "inside bundle".
    const QString sibling = root.filePath(QStringLiteral("bundle-2.zip"));
    const auto ok = FileExplorerArchiveService::compressToZip(sibling, {folder});
    QVERIFY2(ok.ok, qPrintable(ok.blockers.join(QStringLiteral("; "))));
    QVERIFY(QFile::exists(sibling));
}

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
    QCOMPARE(result.blockers.size(), 1);
    QCOMPARE(result.blockers.first(),
             QStringLiteral("Could not create archive %1 (it already exists or is not writable).")
                 .arg(out));

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
    // The result STRUCT, not just ok plus the file existing. output_path is the path the terminal
    // card and the model are told the archive landed at and is asserted nowhere in the
    // repository; entries is never stated on its own for a single-file compress; and warnings is
    // never required to be empty on a clean run, even though the compress path silently downgrades
    // a symlink or a special entry to a warning -- so a mis-classified regular file would
    // warn-and-skip while ok stayed true and the archive shipped without it.
    QCOMPARE(compressed.output_path, zip);
    QCOMPARE(compressed.entries, 1);
    QVERIFY2(compressed.warnings.isEmpty(),
             qPrintable(compressed.warnings.join(QStringLiteral("; "))));

    const QString outdir = dir.filePath(QStringLiteral("extracted"));
    const auto extracted = FileExplorerArchiveService::extractZip(zip, outdir);
    QVERIFY2(extracted.ok, qPrintable(extracted.blockers.join(QStringLiteral("; "))));
    // Same on the extract side, where only ok and blockers were ever read by any test in the
    // repository. Both fields are user-facing: they become the model's "Extract succeeded: N
    // entr(y|ies)" message and its data["output_path"], so both could be dropped with the whole
    // suite green and the model told an archive extracted 0 entries into "". The empty warnings
    // list matters here too -- a corrupt central-directory record and a symlink entry are
    // downgraded to warnings while ok stays true, and a clean single-file round trip is the right
    // place to require that none fired.
    QCOMPARE(extracted.output_path, outdir);
    QCOMPARE(extracted.entries, 1);
    QVERIFY2(extracted.warnings.isEmpty(),
             qPrintable(extracted.warnings.join(QStringLiteral("; "))));

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
    QCOMPARE(result.blockers.size(), 1);
    QCOMPARE(
        result.blockers.first(),
        QStringLiteral("Source %1 no longer exists; the archive was not written.").arg(missing));
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
    QCOMPARE(extracted.blockers.size(), 1);
    QCOMPARE(extracted.blockers.first(),
             QStringLiteral("Extraction of entry data.txt produced 13 bytes, not the declared 4096 "
                            "(truncated or corrupt)."));
    // The truncated payload must not be left on disk claiming to be the extracted file.
    QVERIFY2(!QFile::exists(QDir(outdir).filePath(QStringLiteral("data.txt"))),
             "the short entry must not be written");
    // The refusal must not report entries it never validated: buildArchiveResult ships
    // result.entries to the model as "Extract succeeded: N entr(y|ies)", so counting an entry
    // before it is checked reports work that did not happen.
    QCOMPARE(extracted.entries, 0);
}

void FileExplorerArchiveServiceTests::extractZip_refusesAnEntryThatOverProducesItsDeclaredSize() {
    // The production guard is an EXACT compare, `data.size() != info.size`, but the only fixture
    // in the tree drove it with 13 produced vs 4096 declared -- the UNDER-produce side. Loosening
    // it to `data.size() < info.size` keeps every assertion in the sibling test green, and the
    // OVER-produce side is the security-relevant one: extractZipEntry charges the running total
    // and the per-file ceiling with the DECLARED size, so an entry that declares a handful of
    // bytes and actually inflates to something enormous clears both bomb ceilings and is then
    // written at its real length -- precisely the bypass those ceilings exist to stop.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = dir.filePath(QStringLiteral("data.txt"));
    // Compressible and comfortably larger than the lie, so the entry is DEFLATED: a tiny stored
    // entry would be truncated to its declared size by the reader, which is exactly why the
    // existing 13-byte fixture cannot reach this direction.
    const QByteArray payload = QByteArray(1100, 'z');
    writeFile(src, payload);
    const QString zip = dir.filePath(QStringLiteral("archive.zip"));
    QVERIFY(FileExplorerArchiveService::compressToZip(zip, {src}).ok);

    QFile archive(zip);
    QVERIFY(archive.open(QIODevice::ReadOnly));
    QByteArray bytes = archive.readAll();
    archive.close();

    // Patch ONLY the central directory's uncompressed size: that is where info.size comes from,
    // and leaving the local header's true length in place keeps the entry decoding normally.
    const qsizetype central = bytes.indexOf(QByteArrayLiteral("PK\x01\x02"));
    QVERIFY2(central >= 0, "central directory record not found in the built archive");
    const quint32 lie = 5;
    for (int i = 0; i < 4; ++i) {
        bytes[central + 24 + i] = static_cast<char>((lie >> (8 * i)) & 0xFF);
    }
    writeFile(zip, bytes);

    const QString outdir = dir.filePath(QStringLiteral("extracted"));
    const auto extracted = FileExplorerArchiveService::extractZip(zip, outdir);

    QVERIFY2(!extracted.ok, "an entry that decodes to MORE bytes than it declares must fail");
    QCOMPARE(extracted.blockers.size(), 1);
    QCOMPARE(extracted.blockers.first(),
             QStringLiteral("Extraction of entry data.txt produced %1 bytes, not the declared 5 "
                            "(truncated or corrupt).")
                 .arg(payload.size()));
    QVERIFY2(!QFile::exists(QDir(outdir).filePath(QStringLiteral("data.txt"))),
             "the over-producing entry must not be written");
    QCOMPARE(extracted.entries, 0);
}

QTEST_GUILESS_MAIN(FileExplorerArchiveServiceTests)
#include "test_file_explorer_archive_service.moc"
