// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_image_source.cpp
/// @brief Unit tests for ImageSource format detection and ImageMetadata

#include "sak/image_source.h"

#include "../support/compress_fixture.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

using namespace sak;

class TestImageSource : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- ImageMetadata ---------------------------------------------
    void metadata_defaults();
    void metadata_isValid_empty();
    void metadata_isValid_withData();
    void metadata_isValid_zeroSize();
    void metadata_isValid_unknownFormat();

    // -- FileImageSource::detectFormat -----------------------------
    void detectFormat_iso();
    void detectFormat_img();
    void detectFormat_gz();
    void detectFormat_bz2();
    void detectFormat_xz();
    void detectFormat_unknown();
    void detectFormat_emptyPath();

    // -- CompressedImageSource::isCompressed -----------------------
    void isCompressed_gzip();
    void isCompressed_bzip2();
    void isCompressed_xz();
    void isCompressed_iso();
    void isCompressed_emptyPath();
    void measure_reportsTheTrueSize();
    void measure_refusesWhatCannotBeFlashed();
    void measure_stopsWhenAsked();
    void compressedSeek_landsOnTheRightByte();
    void isCompressed_longExtensions();
    void isCompressed_zipNotStreamable();

    // -- FileImageSource construction ------------------------------
    void fileSource_construction();
    void fileSource_openNonExistent();
    void fileSource_readNullBuffer();
};

// ===================================================================
// ImageMetadata
// ===================================================================

void TestImageSource::metadata_defaults() {
    ImageMetadata meta;
    QVERIFY(meta.name.isEmpty());
    QVERIFY(meta.path.isEmpty());
    QCOMPARE(meta.format, ImageFormat::Unknown);
    QCOMPARE(meta.isCompressed, false);
}

void TestImageSource::metadata_isValid_empty() {
    ImageMetadata meta;
    QVERIFY(!meta.isValid());
}

void TestImageSource::metadata_isValid_withData() {
    ImageMetadata meta;
    meta.path = "C:\\test.iso";
    meta.size = 4096;
    meta.format = ImageFormat::ISO;
    QVERIFY(meta.isValid());
}

void TestImageSource::metadata_isValid_zeroSize() {
    ImageMetadata meta;
    meta.path = "C:\\test.iso";
    meta.size = 0;
    meta.format = ImageFormat::ISO;
    QVERIFY(!meta.isValid());
}

void TestImageSource::metadata_isValid_unknownFormat() {
    ImageMetadata meta;
    meta.path = "C:\\test.bin";
    meta.size = 1024;
    meta.format = ImageFormat::Unknown;
    QVERIFY(!meta.isValid());
}

// ===================================================================
// detectFormat
// ===================================================================

void TestImageSource::detectFormat_iso() {
    QCOMPARE(FileImageSource::detectFormat("image.iso"), ImageFormat::ISO);
    // The suffix is lower-cased before the table lookup, so case must not change the verdict --
    // an uppercase name off a real vendor download ("WIN11.ISO") is the common case, and every
    // detectFormat assertion in this file used a lower-case name.
    QCOMPARE(FileImageSource::detectFormat("IMAGE.ISO"), ImageFormat::ISO);
    QCOMPARE(FileImageSource::detectFormat("Image.Iso"), ImageFormat::ISO);
    // Only the EXTENSION decides: a name that merely contains "iso" is not an image.
    QCOMPARE(FileImageSource::detectFormat("isolated.txt"), ImageFormat::Unknown);
}

void TestImageSource::detectFormat_img() {
    QCOMPARE(FileImageSource::detectFormat("disk.img"), ImageFormat::IMG);
}

void TestImageSource::detectFormat_gz() {
    const auto fmt = FileImageSource::detectFormat("archive.img.gz");
    QCOMPARE(fmt, ImageFormat::GZIP);
    // A double-extension name is answered by EITHER table -- suffix()=="gz" matches the
    // kFormats row and completeSuffix()=="img.gz" ends with ".gz" in kCompound -- so it
    // pins neither. Only a BARE single-extension name can reach the kFormats row alone:
    // completeSuffix() is then "gz", and "gz".endsWith(".gz") is false, so the compound
    // table cannot cover for a deleted row.
    QCOMPARE(FileImageSource::detectFormat("ubuntu.gz"), ImageFormat::GZIP);
}

void TestImageSource::detectFormat_bz2() {
    const auto fmt = FileImageSource::detectFormat("archive.img.bz2");
    QCOMPARE(fmt, ImageFormat::BZIP2);
    // A compound name is classified by BOTH tables at once -- suffix()=="bz2" hits the kFormats
    // row and completeSuffix()=="img.bz2" ends with ".bz2" -- so the line above cannot tell which
    // table answered. A BARE name reaches only the kFormats row, because completeSuffix()=="bz2"
    // does NOT end with ".bz2"; dropping that row would degrade a plain .bz2 image to Unknown,
    // which the flasher refuses outright.
    QCOMPARE(FileImageSource::detectFormat("ubuntu.bz2"), ImageFormat::BZIP2);
}

void TestImageSource::detectFormat_xz() {
    const auto fmt = FileImageSource::detectFormat("archive.img.xz");
    QCOMPARE(fmt, ImageFormat::XZ);
    // "archive.img.xz" is answered by BOTH tables at once (suffix()=="xz" and
    // completeSuffix()=="img.xz"), so it cannot tell the single-extension row apart from the
    // compound one. A bare .xz -- the usual shape of a raw disk image off a distro mirror --
    // has completeSuffix()=="xz", which does NOT end with ".xz", so only the kFormats row can
    // classify it.
    QCOMPARE(FileImageSource::detectFormat("ubuntu.xz"), ImageFormat::XZ);
}

void TestImageSource::detectFormat_unknown() {
    QCOMPARE(FileImageSource::detectFormat("readme.txt"), ImageFormat::Unknown);
    // The other half of the extension catalog, which no case in the tree reached: dropping any
    // of these four rows misclassifies a real image as Unknown while every existing
    // detectFormat assertion still passes.
    QCOMPARE(FileImageSource::detectFormat("install.wic"), ImageFormat::WIC);
    QCOMPARE(FileImageSource::detectFormat("bundle.zip"), ImageFormat::ZIP);
    QCOMPARE(FileImageSource::detectFormat("apple.dmg"), ImageFormat::DMG);
    QCOMPARE(FileImageSource::detectFormat("floppy.dsk"), ImageFormat::DSK);
    // A near-miss extension must stay Unknown rather than fall into a neighbouring row.
    QCOMPARE(FileImageSource::detectFormat("archive.gzipped"), ImageFormat::Unknown);
    QCOMPARE(FileImageSource::detectFormat("readme.txt.bak"), ImageFormat::Unknown);
}

void TestImageSource::detectFormat_emptyPath() {
    QCOMPARE(FileImageSource::detectFormat(""), ImageFormat::Unknown);
}

// ===================================================================
// isCompressed
// ===================================================================

void TestImageSource::isCompressed_gzip() {
    QVERIFY(CompressedImageSource::isCompressed("file.img.gz"));
}

void TestImageSource::isCompressed_bzip2() {
    QVERIFY(CompressedImageSource::isCompressed("file.img.bz2"));
}

void TestImageSource::isCompressed_xz() {
    QVERIFY(CompressedImageSource::isCompressed("file.img.xz"));
}

void TestImageSource::isCompressed_iso() {
    QVERIFY(!CompressedImageSource::isCompressed("file.iso"));
    // "file.iso" is refused for TWO reasons at once -- the extension is not compressed AND no
    // such file exists -- so the content probe behind isCompressed was never reached. Pin it
    // with real files: uncompressed bytes under a name carrying no compressed extension stay
    // false...
    QTemporaryFile plain;
    QVERIFY(plain.open());
    const QByteArray plainBytes("CD001 plain bytes, no compression header");
    QCOMPARE(plain.write(plainBytes), qint64(plainBytes.size()));
    plain.close();
    QVERIFY(!CompressedImageSource::isCompressed(plain.fileName()));
    // ...while a gzip stream is recognised from its MAGIC even when the name carries no
    // compressed extension at all. A .iso that is really a gzip image must never be written
    // raw to the device.
    QTemporaryFile gzipBody;
    QVERIFY(gzipBody.open());
    const QByteArray gzipMagic = QByteArray::fromHex("1f8b0800000000000003");
    QCOMPARE(gzipBody.write(gzipMagic), qint64(gzipMagic.size()));
    gzipBody.close();
    QVERIFY(CompressedImageSource::isCompressed(gzipBody.fileName()));
}

// ===================================================================
// CompressedImageSource::measure and seek
// ===================================================================

namespace {
// Not one repeated byte: a seek or a measure that lands in the wrong place has to show.
QByteArray imagePayload(qsizetype bytes) {
    QByteArray payload(bytes, Qt::Uninitialized);
    for (qsizetype i = 0; i < bytes; ++i) {
        payload[i] = static_cast<char>((i * 31) ^ (i >> 8));
    }
    return payload;
}

QString writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    QFile file(dir.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return file.fileName();
}
}  // namespace

// What the flash coordinator relies on: every format reports its true uncompressed length, which
// here is deliberately larger than 4 x the 1 MB read buffer so one read could not hide a short
// count.
void TestImageSource::measure_reportsTheTrueSize() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload = imagePayload(5 * 1024 * 1024 + 123);

    struct Format {
        QString name;
        QByteArray bytes;
    };
    const QList<Format> formats = {
        {QStringLiteral("image.img.gz"), sak_test::gzipCompress(payload)},
        {QStringLiteral("image.img.bz2"), sak_test::bzip2Compress(payload)},
        {QStringLiteral("image.img.xz"), sak_test::xzCompress(payload)}};

    for (const auto& format : formats) {
        const QString path = writeFile(dir, format.name, format.bytes);
        QVERIFY(!path.isEmpty());
        qint64 lastRead = -1;
        qint64 reportedTotal = -1;
        const auto outcome = CompressedImageSource::measure(
            path, {}, [&](qint64 compressedRead, qint64 compressedTotal) {
                lastRead = compressedRead;
                reportedTotal = compressedTotal;
            });
        QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        QVERIFY(!outcome.cancelled);
        QCOMPARE(outcome.uncompressedBytes, qint64(payload.size()));
        // Progress is about the compressed file, and ends at all of it.
        QCOMPARE(reportedTotal, qint64(format.bytes.size()));
        QCOMPARE(lastRead, qint64(format.bytes.size()));

        // The size reaches size() only once it is recorded, and survives the reopen every
        // checksum and verify pass does.
        CompressedImageSource source(path);
        QVERIFY(source.open());
        QCOMPARE(source.size(), qint64(-1));
        source.setUncompressedSize(outcome.uncompressedBytes);
        QCOMPARE(source.size(), qint64(payload.size()));
        source.close();
        QVERIFY(source.open());
        QCOMPARE(source.size(), qint64(payload.size()));
    }
}

void TestImageSource::measure_refusesWhatCannotBeFlashed() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload = imagePayload(256 * 1024);

    // Truncated: the file ends before the decoder does.
    QByteArray truncated = sak_test::gzipCompress(payload);
    truncated.chop(truncated.size() / 2);
    auto outcome =
        CompressedImageSource::measure(writeFile(dir, QStringLiteral("cut.img.gz"), truncated), {});
    QCOMPARE(outcome.uncompressedBytes, qint64(-1));
    QVERIFY2(!outcome.error.isEmpty(), "a truncated stream must not measure");
    // The decoder's own reason, not the empty-stream one a swallowed read error would turn into.
    QVERIFY2(!outcome.error.contains(QStringLiteral("nothing")), qPrintable(outcome.error));

    // Corrupt: a .xz name over bytes that are not xz.
    outcome = CompressedImageSource::measure(
        writeFile(dir, QStringLiteral("junk.img.xz"), imagePayload(4096)), {});
    QCOMPARE(outcome.uncompressedBytes, qint64(-1));
    QVERIFY2(!outcome.error.isEmpty(), "a corrupt stream must not measure");
    QVERIFY2(!outcome.error.contains(QStringLiteral("nothing")), qPrintable(outcome.error));

    // A tar archive decodes perfectly and is still not a disk.
    QByteArray tarBlock(1024, '\0');
    tarBlock.replace(0, 9, "file.txt\0");
    tarBlock.replace(257, 5, "ustar");
    outcome = CompressedImageSource::measure(
        writeFile(dir, QStringLiteral("rootfs.tar.gz"), sak_test::gzipCompress(tarBlock)), {});
    QCOMPARE(outcome.uncompressedBytes, qint64(-1));
    QVERIFY2(outcome.error.contains(QStringLiteral("tar archive")), qPrintable(outcome.error));

    // The same bytes with no tar marker are a perfectly good image, so it is the marker that
    // refused the one above and not the content or the length.
    tarBlock.replace(257, 5, "plain");
    outcome = CompressedImageSource::measure(
        writeFile(dir, QStringLiteral("plain.img.gz"), sak_test::gzipCompress(tarBlock)), {});
    QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
    QCOMPARE(outcome.uncompressedBytes, qint64(tarBlock.size()));

    // Nothing inside: a run with zero bytes to write must end here.
    outcome = CompressedImageSource::measure(
        writeFile(dir, QStringLiteral("empty.img.gz"), sak_test::gzipCompress(QByteArray())), {});
    QCOMPARE(outcome.uncompressedBytes, qint64(-1));
    QVERIFY2(outcome.error.contains(QStringLiteral("nothing")), qPrintable(outcome.error));
}

void TestImageSource::measure_stopsWhenAsked() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir,
                                   QStringLiteral("image.img.gz"),
                                   sak_test::gzipCompress(imagePayload(3 * 1024 * 1024)));
    int polls = 0;
    const auto outcome = CompressedImageSource::measure(path, [&polls]() {
        ++polls;
        return polls > 1;  // One read happens, then the stop is honoured.
    });
    QVERIFY(outcome.cancelled);
    QCOMPARE(outcome.uncompressedBytes, qint64(-1));
    QVERIFY(outcome.error.isEmpty());
    QCOMPARE(polls, 2);
}

// Sample verification reads blocks by offset. Before this the seek always failed, so verifying a
// compressed image in Quick mode read back no blocks at all and failed every run.
void TestImageSource::compressedSeek_landsOnTheRightByte() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload = imagePayload(3 * 1024 * 1024 + 17);
    const QString path =
        writeFile(dir, QStringLiteral("image.img.xz"), sak_test::xzCompress(payload));
    CompressedImageSource source(path);

    // Closed: nothing to seek in.
    QVERIFY(!source.seek(0));
    QVERIFY(source.open());
    QVERIFY(!source.seek(-1));

    QByteArray block(4096, Qt::Uninitialized);
    const auto readsFrom = [&](qint64 offset) {
        return source.seek(offset) && source.position() == offset &&
               source.read(block.data(), block.size()) == block.size() &&
               block == payload.mid(offset, block.size());
    };
    QVERIFY(readsFrom(2 * 1024 * 1024));     // Forward, more than one discard buffer away.
    QVERIFY(readsFrom(100));                 // Backward starts the stream over.
    QVERIFY(readsFrom(100 + block.size()));  // Already there: no reopen and no skipped bytes.

    // Past the end: refused, not a silent stop at the last byte.
    QVERIFY(!source.seek(payload.size() + 1));
}

void TestImageSource::isCompressed_emptyPath() {
    QVERIFY(!CompressedImageSource::isCompressed(""));
}

// isCompressed now delegates to DecompressorFactory (single source of truth), so
// the long-form extensions it can stream must classify as compressed -- the old
// hand-rolled {gz,bz2,xz} set silently wrote these raw.
void TestImageSource::isCompressed_longExtensions() {
    QVERIFY(CompressedImageSource::isCompressed("file.img.gzip"));
    QVERIFY(CompressedImageSource::isCompressed("file.bzip2"));
    QVERIFY(CompressedImageSource::isCompressed("file.lzma"));
}

// A .zip is a multi-member archive DecompressorFactory cannot stream into a raw
// image, so isCompressed must NOT report it as a streamable compressed source
// (the coordinator refuses it separately rather than raw-writing the archive).
void TestImageSource::isCompressed_zipNotStreamable() {
    QVERIFY(!CompressedImageSource::isCompressed("file.zip"));
}

// ===================================================================
// FileImageSource
// ===================================================================

void TestImageSource::fileSource_construction() {
    FileImageSource source("C:\\nonexistent.iso");
    QVERIFY(!source.isOpen());
    // The constructor populates the whole metadata block from the path alone; pin the fields it
    // derives, not just the closed flag.
    const ImageMetadata fileMeta = source.metadata();
    QCOMPARE(fileMeta.name, QStringLiteral("nonexistent.iso"));
    QCOMPARE(fileMeta.path, QStringLiteral("C:\\nonexistent.iso"));
    QCOMPARE(fileMeta.format, ImageFormat::ISO);
    QCOMPARE(fileMeta.isCompressed, false);
    QCOMPARE(fileMeta.uncompressedSize, qint64(0));
    // No device exists before open(), so every cursor accessor must answer from the closed
    // state instead of dereferencing it, and seek/read must fail closed.
    QCOMPARE(source.position(), qint64(0));
    QVERIFY(source.atEnd());
    QVERIFY(!source.seek(0));
    QByteArray sink(8, '\0');
    QCOMPARE(source.read(sink.data(), sink.size()), qint64(-1));
}

void TestImageSource::fileSource_openNonExistent() {
    FileImageSource source("C:\\definitely_does_not_exist_12345.iso");
    QSignalSpy errorSpy(&source, &ImageSource::readError);
    const bool opened = source.open();
    QVERIFY(!opened);
    QVERIFY(!source.isOpen());
    // A refusal that is only RETURNED is invisible to the flash coordinator, which listens on
    // readError. The prefix is a fixed literal, so the whole information content is the TAIL,
    // which the coordinator forwards verbatim as the user-facing reason. Pin it without
    // hard-coding a locale-dependent string: a QFile opened ReadOnly on the same path in this
    // process reports the identical OS reason.
    QCOMPARE(errorSpy.count(), 1);
    const QString reported = errorSpy.at(0).at(0).toString();
    QFile probe(QStringLiteral("C:\\definitely_does_not_exist_12345.iso"));
    QVERIFY(!probe.open(QIODevice::ReadOnly));
    QCOMPARE(reported, QStringLiteral("Failed to open file: ") + probe.errorString());
    // The tail must be the OS reason, never an echo of the path the caller already supplied.
    QVERIFY2(!reported.contains(QStringLiteral("definitely_does_not_exist_12345")),
             qPrintable(reported));
    // The failed open must leave the source cleanly closed rather than half-initialised: a
    // second attempt re-tries and reports again instead of taking an "already open"
    // short-circuit and returning true.
    QVERIFY(!source.open());
    QCOMPARE(errorSpy.count(), 2);
    QVERIFY(!source.isOpen());
}
// read() is public on the ImageSource interface, so the buffer arrives from outside the class
// and image_source.cpp:88-91 refuses a null one with the documented -1. The closed-source call
// in fileSource_construction cannot prove that guard: a closed source answers -1 for the
// !isOpen() reason (image_source.cpp:92-94) no matter what buffer it is handed. Pin the guard
// where it is the ONLY thing that can produce -1 -- on a source with a live handle and real
// bytes underneath, which is exactly the state flash_worker.cpp:787/1091/1129 read in.
void TestImageSource::fileSource_readNullBuffer() {
    QTemporaryFile image;
    QVERIFY(image.open());
    const QByteArray body("0123456789ABCDEF");
    QCOMPARE(image.write(body), qint64(body.size()));
    image.close();

    FileImageSource source(image.fileName());
    QVERIFY(source.open());
    QVERIFY(source.isOpen());
    // The handle really is live and readable: a valid buffer returns bytes here...
    QByteArray sink(8, '\0');
    QCOMPARE(source.read(sink.data(), sink.size()), qint64(8));
    QCOMPARE(sink, body.left(8));
    // ...so the -1 below is attributable ONLY to the null-buffer guard. Without it the pointer
    // goes straight into QIODevice::read on an open device -- a memcpy into address 0.
    QCOMPARE(source.read(nullptr, sink.size()), qint64(-1));
    // The refusal is not destructive: the source stays open and the cursor never moved.
    QVERIFY(source.isOpen());
    QCOMPARE(source.position(), qint64(8));
    QCOMPARE(source.read(sink.data(), sink.size()), qint64(8));
    QCOMPARE(sink, body.mid(8, 8));
    source.close();
}


QTEST_MAIN(TestImageSource)
#include "test_image_source.moc"
