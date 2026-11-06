// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_media_read_back.cpp
/// @brief MediaReadBackFile: every byte back, from a handle that bypasses Windows' file cache.

#include "sak/media_read_back.h"

#include "../support/create_file_recorder.h"

#include <QCryptographicHash>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class MediaReadBackTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void everyByteComesBack_data();
    void everyByteComesBack();
    void itsHandleBypassesTheFileCache();
    void aSeekLandsAnywhere();
    void aMissingFileIsNoHash();
    void aCancelledHashIsNoHash();
    void itOnlyReads();
};

namespace {

QByteArray randomBytes(qint64 size) {
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    QRandomGenerator generator(static_cast<quint32>(size) + 17U);
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(generator.bounded(256));
    }
    return bytes;
}

QString writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

}  // namespace

// An unbuffered read has to be a whole number of sectors, so the last piece of a file is where a
// reader goes wrong: sizes either side of a sector and of a chunk, and an empty file.
void MediaReadBackTests::everyByteComesBack_data() {
    constexpr qint64 kChunk = sak::MediaReadBackFile::kChunkBytes;
    QTest::addColumn<qint64>("size");
    QTest::newRow("empty") << qint64{0};
    QTest::newRow("one byte") << qint64{1};
    QTest::newRow("under a sector") << qint64{511};
    QTest::newRow("a 4K sector") << qint64{4096};
    QTest::newRow("over a 4K sector") << qint64{4097};
    QTest::newRow("under a chunk") << kChunk - 1;
    QTest::newRow("a chunk") << kChunk;
    QTest::newRow("over two chunks") << 2 * kChunk + 333;
}

void MediaReadBackTests::everyByteComesBack() {
    QFETCH(qint64, size);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray bytes = randomBytes(size);
    const QString path = writeFile(dir, QStringLiteral("part.swm"), bytes);
    QVERIFY(!path.isEmpty());

    sak::MediaReadBackFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(QString::number(file.windowsError())));
    QCOMPARE(file.size(), size);
    QCOMPARE(file.readAll(), bytes);
    QVERIFY(file.atEnd());
    QCOMPARE(file.windowsError(), 0UL);
    QCOMPARE(sak::sha256FromMedia(path),
             QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
}

// testpc1: the Windows USB's check read a 2.8 GB part back in 17.5 s, from the cache. The bytes
// are the same either way; the flags the handle was opened with are what tell.
void MediaReadBackTests::itsHandleBypassesTheFileCache() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir, QStringLiteral("install2.swm"), randomBytes(5000));
    QVERIFY(!path.isEmpty());
    const sak::test::CreateFileRecorder recorder;
    QVERIFY(!sak::sha256FromMedia(path).isEmpty());
    QVERIFY2(sak::test::CreateFileRecorder::readOnlyUnbuffered(path),
             "sha256FromMedia did not open the file with FILE_FLAG_NO_BUFFERING");
}

// The backup codec's check seeks past the container's magic before it decodes.
void MediaReadBackTests::aSeekLandsAnywhere() {
    constexpr qint64 kChunk = sak::MediaReadBackFile::kChunkBytes;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray bytes = randomBytes(2 * kChunk + 100);
    const QString path = writeFile(dir, QStringLiteral("stored.sakbfc"), bytes);
    QVERIFY(!path.isEmpty());
    sak::MediaReadBackFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    for (const qint64 place : {qint64{8}, kChunk - 5, kChunk, 2 * kChunk + 99, qint64{0}}) {
        QVERIFY(file.seek(place));
        QCOMPARE(file.pos(), place);
        const QByteArray got = file.read(64);
        QCOMPARE(got, bytes.mid(static_cast<qsizetype>(place), 64));
    }
    QVERIFY(!file.seek(bytes.size() + 1));
}

void MediaReadBackTests::aMissingFileIsNoHash() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gone.swm"));
    sak::MediaReadBackFile file(path);
    QVERIFY(!file.open(QIODevice::ReadOnly));
    QCOMPARE(file.windowsError(), static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
    QVERIFY(sak::sha256FromMedia(path).isEmpty());
}

void MediaReadBackTests::aCancelledHashIsNoHash() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir, QStringLiteral("part.swm"), randomBytes(100));
    QVERIFY(!path.isEmpty());
    QVERIFY(sak::sha256FromMedia(path, []() { return true; }).isEmpty());
}

void MediaReadBackTests::itOnlyReads() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFile(dir, QStringLiteral("part.swm"), randomBytes(100));
    QVERIFY(!path.isEmpty());
    sak::MediaReadBackFile file(path);
    QVERIFY(!file.open(QIODevice::ReadWrite));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.write("x"), qint64{-1});
}

QTEST_GUILESS_MAIN(MediaReadBackTests)
#include "test_media_read_back.moc"
