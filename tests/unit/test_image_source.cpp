// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_image_source.cpp
/// @brief Unit tests for ImageSource format detection and ImageMetadata

#include "sak/image_source.h"

#include <QTemporaryFile>
#include <QtTest/QtTest>

using namespace sak;

class TestImageSource : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── ImageMetadata ─────────────────────────────────────────────
    void metadata_defaults();
    void metadata_isValid_empty();
    void metadata_isValid_withData();
    void metadata_isValid_zeroSize();
    void metadata_isValid_unknownFormat();

    // ── FileImageSource::detectFormat ─────────────────────────────
    void detectFormat_iso();
    void detectFormat_img();
    void detectFormat_gz();
    void detectFormat_bz2();
    void detectFormat_xz();
    void detectFormat_unknown();
    void detectFormat_emptyPath();

    // ── CompressedImageSource::isCompressed ───────────────────────
    void isCompressed_gzip();
    void isCompressed_bzip2();
    void isCompressed_xz();
    void isCompressed_iso();
    void isCompressed_emptyPath();

    // ── FileImageSource construction ──────────────────────────────
    void fileSource_construction();
    void fileSource_openNonExistent();
};

// ═══════════════════════════════════════════════════════════════════
// ImageMetadata
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
// detectFormat
// ═══════════════════════════════════════════════════════════════════

void TestImageSource::detectFormat_iso() {
    QCOMPARE(FileImageSource::detectFormat("image.iso"), ImageFormat::ISO);
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
}

void TestImageSource::detectFormat_emptyPath() {
    QCOMPARE(FileImageSource::detectFormat(""), ImageFormat::Unknown);
}

// ═══════════════════════════════════════════════════════════════════
// isCompressed
// ═══════════════════════════════════════════════════════════════════

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
}

void TestImageSource::isCompressed_emptyPath() {
    QVERIFY(!CompressedImageSource::isCompressed(""));
}

// ═══════════════════════════════════════════════════════════════════
// FileImageSource
// ═══════════════════════════════════════════════════════════════════

void TestImageSource::fileSource_construction() {
    FileImageSource source("C:\\nonexistent.iso");
    QVERIFY(!source.isOpen());
}

void TestImageSource::fileSource_openNonExistent() {
    FileImageSource source("C:\\definitely_does_not_exist_12345.iso");
    const bool opened = source.open();
    QVERIFY(!opened);
    QVERIFY(!source.isOpen());
}

QTEST_MAIN(TestImageSource)
#include "test_image_source.moc"
