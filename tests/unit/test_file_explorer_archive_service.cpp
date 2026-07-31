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

QTEST_GUILESS_MAIN(FileExplorerArchiveServiceTests)
#include "test_file_explorer_archive_service.moc"
