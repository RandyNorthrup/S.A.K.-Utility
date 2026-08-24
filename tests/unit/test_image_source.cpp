// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_image_source.cpp
/// @brief Unit tests for ImageSource format detection and ImageMetadata

#include "sak/image_source.h"

#include <QSignalSpy>
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
    void isCompressed_longExtensions();
    void isCompressed_zipNotStreamable();

    // -- FileImageSource construction ------------------------------
    void fileSource_construction();
    void fileSource_openNonExistent();
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
}

void TestImageSource::detectFormat_bz2() {
    const auto fmt = FileImageSource::detectFormat("archive.img.bz2");
    QCOMPARE(fmt, ImageFormat::BZIP2);
}

void TestImageSource::detectFormat_xz() {
    const auto fmt = FileImageSource::detectFormat("archive.img.xz");
    QCOMPARE(fmt, ImageFormat::XZ);
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
    // readError: exactly one report, carrying the documented prefix. The tail is the OS
    // errorString (locale-dependent), so only the invariant prefix is pinned.
    QCOMPARE(errorSpy.count(), 1);
    const QString reported = errorSpy.at(0).at(0).toString();
    QVERIFY2(reported.startsWith(QStringLiteral("Failed to open file: ")), qPrintable(reported));
    // The failed open must leave the source cleanly closed rather than half-initialised: a
    // second attempt re-tries and reports again instead of taking an "already open"
    // short-circuit and returning true.
    QVERIFY(!source.open());
    QCOMPARE(errorSpy.count(), 2);
    QVERIFY(!source.isOpen());
}

QTEST_MAIN(TestImageSource)
#include "test_image_source.moc"
