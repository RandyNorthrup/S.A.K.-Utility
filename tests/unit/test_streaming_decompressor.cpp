// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_streaming_decompressor.cpp
/// @brief Unit tests for StreamingDecompressor and DecompressorFactory lifecycle

#include "sak/decompressor_factory.h"
#include "sak/streaming_decompressor.h"
#include "sak/xz_decompressor.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>

class StreamingDecompressorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Factory + open/close lifecycle
    void gzipDecompressor_openClose();
    void bzip2Decompressor_openClose();
    void xzDecompressor_openClose();

    // Open non-existent file
    void open_nonExistentFile();

    // Read from invalid stream
    void read_beforeOpen();

    // Format names
    void gzip_formatName();
    void bzip2_formatName();
    void xz_formatName();

    // Compressed bytes tracking
    void compressedBytesRead_initiallyZero();

    // atEnd on fresh decompressor
    void atEnd_beforeOpen();

    // Truncated vs complete real gzip stream
    void completeGzip_readsAllBytes();
    void truncatedGzip_reportsError();

    // Truncated vs complete real bzip2 stream. bzip2 is the one format whose decoder
    // never self-reports a mid-block truncation (BZ2_bzDecompress just keeps returning
    // BZ_OK asking for input that never comes), so the streaming no-progress backstop
    // is the SOLE guard that turns a truncated .bz2 into a fail-closed error.
    void completeBzip2_readsAllBytes();
    void truncatedBzip2_reportsError();

    // A normal xz stream still decodes under the bounded decoder memory limit.
    void realXzStreamDecodesUnderMemLimit();

    // A crafted header whose declared dictionary exceeds the 512 MiB decoder memory
    // ceiling must fail closed (LZMA_MEMLIMIT_ERROR), never allocate gigabytes.
    void xzOversizedDictionaryHeaderFailsClosed();

    // Concatenated members (pigz multi-member gzip, cat'd xz) decode in full,
    // and trailing garbage after a member fails closed.
    void concatenatedGzipMembersDecodeFully();
    void concatenatedXzMembersDecodeFully();
    void trailingGarbageAfterMemberFailsClosed();

private:
    QTemporaryDir m_tempDir;

    void writeFile(const QString& name, const QByteArray& content);
    QString filePath(const QString& name) const;
    static QByteArray gzipCompress(const QByteArray& input);
    static QByteArray xzCompress(const QByteArray& input);
    static QByteArray bzip2Compress(const QByteArray& input);
    static QByteArray lzmaAloneHeader(quint32 dictSize);
    static qint64 readFully(sak::StreamingDecompressor& decomp);
};

void StreamingDecompressorTests::initTestCase() {
    QVERIFY(m_tempDir.isValid());

    // Write files with correct magic bytes but invalid compressed content
    // These should still be openable by the factory

    // Gzip magic: 1F 8B 08 00... (minimum gzip header)
    QByteArray gzData;
    gzData.append(static_cast<char>(0x1F));
    gzData.append(static_cast<char>(0x8B));
    gzData.append(static_cast<char>(0x08));
    gzData.append(QByteArray(29, '\0'));
    writeFile("test.gz", gzData);

    // BZip2 magic: 42 5A 68 (BZh)
    QByteArray bz2Data;
    bz2Data.append('B').append('Z').append('h').append('9');
    bz2Data.append(QByteArray(28, '\0'));
    writeFile("test.bz2", bz2Data);

    // XZ magic: FD 37 7A 58 5A 00
    QByteArray xzData;
    xzData.append(static_cast<char>(0xFD));
    xzData.append(static_cast<char>(0x37));
    xzData.append(static_cast<char>(0x7A));
    xzData.append(static_cast<char>(0x58));
    xzData.append(static_cast<char>(0x5A));
    xzData.append(static_cast<char>(0x00));
    xzData.append(QByteArray(26, '\0'));
    writeFile("test.xz", xzData);
}

void StreamingDecompressorTests::writeFile(const QString& name, const QByteArray& content) {
    QFile f(m_tempDir.filePath(name));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
    f.close();
}

QString StreamingDecompressorTests::filePath(const QString& name) const {
    return m_tempDir.filePath(name);
}

// ============================================================================
// Factory + Open/Close Lifecycle
// ============================================================================

// open() only opens the file and initialises the library stream -- it never looks
// at the compressed payload -- so these fixtures (correct magic, junk content) must
// open, report themselves open, and be closed again. A decoder that validated the
// payload at open time, or that left isOpen() true after close(), fails here.
void StreamingDecompressorTests::gzipDecompressor_openClose() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);

    QVERIFY(decomp->open(filePath("test.gz")));
    QVERIFY(decomp->isOpen());
    decomp->close();
    QVERIFY(!decomp->isOpen());
}

void StreamingDecompressorTests::bzip2Decompressor_openClose() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.bz2"));
    QVERIFY(decomp != nullptr);

    QVERIFY(decomp->open(filePath("test.bz2")));
    QVERIFY(decomp->isOpen());
    decomp->close();
    QVERIFY(!decomp->isOpen());
}

void StreamingDecompressorTests::xzDecompressor_openClose() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.xz"));
    QVERIFY(decomp != nullptr);

    QVERIFY(decomp->open(filePath("test.xz")));
    QVERIFY(decomp->isOpen());
    decomp->close();
    QVERIFY(!decomp->isOpen());
}

// ============================================================================
// Open Non-Existent
// ============================================================================

void StreamingDecompressorTests::open_nonExistentFile() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);

    // Fail closed: a missing file leaves the decompressor shut with the reason
    // recorded, never half-open with an initialised stream and no file behind it.
    QVERIFY(!decomp->open(filePath("nonexistent_file.gz")));
    QVERIFY(!decomp->isOpen());
    // The "Failed to open file: " prefix is a fixed production literal; only the
    // QFile::errorString() tail is OS/locale variable, so pin the prefix.
    QVERIFY(decomp->lastError().startsWith(QStringLiteral("Failed to open file: ")));
}

// ============================================================================
// Read Before Open
// ============================================================================

void StreamingDecompressorTests::read_beforeOpen() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);

    // Reading before open must be an ERROR (-1), not a 0 that a caller would read
    // as a clean end-of-stream and treat as "the image was empty".
    char buffer[64];
    const qint64 bytesRead = decomp->read(buffer, sizeof(buffer));
    QCOMPARE(bytesRead, static_cast<qint64>(-1));
    QCOMPARE(decomp->lastError(), QStringLiteral("Decompressor not open"));
}

// ============================================================================
// Format Names
// ============================================================================

void StreamingDecompressorTests::gzip_formatName() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);
    QCOMPARE(decomp->formatName().toLower(), QString("gzip"));
}

void StreamingDecompressorTests::bzip2_formatName() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.bz2"));
    QVERIFY(decomp != nullptr);
    QCOMPARE(decomp->formatName().toLower(), QString("bzip2"));
}

void StreamingDecompressorTests::xz_formatName() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.xz"));
    QVERIFY(decomp != nullptr);
    QCOMPARE(decomp->formatName().toLower(), QString("xz"));
}

// ============================================================================
// Compressed Bytes Tracking
// ============================================================================

void StreamingDecompressorTests::compressedBytesRead_initiallyZero() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);

    QCOMPARE(decomp->compressedBytesRead(), qint64{0});
    QCOMPARE(decomp->decompressedBytesProduced(), qint64{0});
}

// ============================================================================
// atEnd
// ============================================================================

void StreamingDecompressorTests::atEnd_beforeOpen() {
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);

    // atEnd() means "the decoder reported end-of-stream", so a fresh decompressor
    // must answer false: a true here would make a `while (!atEnd()) read()` caller
    // skip the file entirely and produce nothing.
    QVERIFY(!decomp->atEnd());
}

// ============================================================================
// Truncated stream detection (must fail closed, not report a clean short read)
// ============================================================================

QByteArray StreamingDecompressorTests::gzipCompress(const QByteArray& input) {
    z_stream zs{};
    // windowBits 15 + 16 selects the gzip wrapper.
    const int init =
        deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (init != Z_OK) {
        return {};
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    zs.avail_in = static_cast<uInt>(input.size());

    QByteArray out;
    char buffer[16'384];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);
        ret = deflate(&zs, Z_FINISH);
        out.append(buffer, static_cast<qsizetype>(sizeof(buffer) - zs.avail_out));
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return ret == Z_STREAM_END ? out : QByteArray();
}

qint64 StreamingDecompressorTests::readFully(sak::StreamingDecompressor& decomp) {
    qint64 total = 0;
    char buffer[8192];
    for (;;) {
        const qint64 n = decomp.read(buffer, sizeof(buffer));
        if (n < 0) {
            return -1;  // error (e.g. truncated stream)
        }
        if (n == 0) {
            return total;  // clean end-of-stream
        }
        total += n;
    }
}

void StreamingDecompressorTests::completeGzip_readsAllBytes() {
    QByteArray payload(300'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    const QByteArray gz = gzipCompress(payload);
    QVERIFY(!gz.isEmpty());
    writeFile("complete.gz", gz);

    auto decomp = sak::DecompressorFactory::create(filePath("complete.gz"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("complete.gz")));
    QCOMPARE(readFully(*decomp), static_cast<qint64>(payload.size()));
    // readFully() stopped on a 0-byte read, which only happens once the decoder has
    // reported end-of-stream; the running counter must agree with what was handed out.
    QVERIFY(decomp->atEnd());
    QCOMPARE(decomp->decompressedBytesProduced(), static_cast<qint64>(payload.size()));
}

void StreamingDecompressorTests::truncatedGzip_reportsError() {
    QByteArray payload(300'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    QByteArray gz = gzipCompress(payload);
    QVERIFY(gz.size() > 200);
    gz.chop(gz.size() / 2);  // drop the second half: real compressed data + trailer
    writeFile("truncated.gz", gz);

    auto decomp = sak::DecompressorFactory::create(filePath("truncated.gz"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("truncated.gz")));
    // A truncated stream must fail closed (return -1), not report a clean EOF
    // over the partial output, and it must say why (the message differs depending
    // on whether zlib or the truncation check catches it first).
    QCOMPARE(readFully(*decomp), static_cast<qint64>(-1));
    QVERIFY(!decomp->atEnd());
    QVERIFY(!decomp->lastError().isEmpty());
}

// Encode @p input as a real .bz2 stream with the same libbz2 the decompressor decodes
// with, so these tests exercise Bzip2Decompressor::decompressStep on a genuine payload
// (blockSize100k=9, the largest block; verbosity=0 quiet; workFactor=0 -> library default).
QByteArray StreamingDecompressorTests::bzip2Compress(const QByteArray& input) {
    // libbz2's documented one-shot output-buffer floor: sourceLen + sourceLen/100 + 600.
    unsigned int destLen = static_cast<unsigned int>(input.size() + input.size() / 100 + 600);
    QByteArray out(static_cast<qsizetype>(destLen), Qt::Uninitialized);
    const int ret = BZ2_bzBuffToBuffCompress(out.data(),
                                             &destLen,
                                             const_cast<char*>(input.constData()),
                                             static_cast<unsigned int>(input.size()),
                                             9,
                                             0,
                                             0);
    if (ret != BZ_OK) {
        return {};
    }
    out.truncate(static_cast<qsizetype>(destLen));
    return out;
}

void StreamingDecompressorTests::completeBzip2_readsAllBytes() {
    // A complete .bz2 must decode to the exact original bytes. This is the covering
    // assert for the bzip2 stream-end completeness check (BZ_STREAM_END): inverting it
    // makes a normal BZ_OK step report end-of-stream prematurely (the decoder then tries
    // to parse the block remainder as a fresh member and fails), so the full-payload
    // compare below no longer holds.
    QByteArray payload(200'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 41 + 13) & 0xFF);
    }
    const QByteArray bz = bzip2Compress(payload);
    QVERIFY(!bz.isEmpty());
    writeFile("complete.bz2", bz);

    auto decomp = sak::DecompressorFactory::create(filePath("complete.bz2"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("complete.bz2")));

    QByteArray decoded;
    char buffer[8192];
    for (;;) {
        const qint64 n = decomp->read(buffer, sizeof(buffer));
        QVERIFY(n >= 0);  // never fails on a complete stream
        if (n == 0) {
            break;        // clean end-of-stream
        }
        decoded.append(buffer, static_cast<qsizetype>(n));
    }
    QCOMPARE(decoded.size(), payload.size());
    QVERIFY(decoded == payload);
    QVERIFY(decomp->atEnd());
    QCOMPARE(decomp->decompressedBytesProduced(), static_cast<qint64>(payload.size()));
}

void StreamingDecompressorTests::truncatedBzip2_reportsError() {
    // A truncated .bz2 must fail closed (return -1), not report a clean EOF over the
    // partial output. bzip2 is unique here: BZ2_bzDecompress keeps returning BZ_OK
    // waiting for the rest of the block, so -- unlike zlib (Z_BUF_ERROR) and liblzma
    // (LZMA_BUF_ERROR on the second no-progress call) -- the decoder itself NEVER
    // reports the truncation. The streaming no-progress backstop (isTruncatedStream:
    // input exhausted, buffer empty, output unchanged) is the sole guard that turns
    // this into an error; disabling it would spin the pump loop forever on a stalled
    // decoder instead of failing closed.
    QByteArray payload(200'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 41 + 13) & 0xFF);
    }
    QByteArray bz = bzip2Compress(payload);
    QVERIFY(bz.size() > 200);
    bz.chop(bz.size() / 2);  // drop the second half, leaving the final block incomplete
    writeFile("truncated.bz2", bz);

    auto decomp = sak::DecompressorFactory::create(filePath("truncated.bz2"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("truncated.bz2")));
    QCOMPARE(readFully(*decomp), static_cast<qint64>(-1));
    QVERIFY(!decomp->atEnd());
    QCOMPARE(decomp->lastError(),
             QStringLiteral("Compressed stream is truncated: the input ended before the decoder "
                            "reached end-of-stream"));
}

// Encode @p input as a real .xz stream (default preset -> 8 MiB dictionary).
QByteArray StreamingDecompressorTests::xzCompress(const QByteArray& input) {
    const size_t bound = lzma_stream_buffer_bound(static_cast<size_t>(input.size()));
    QByteArray out(static_cast<qsizetype>(bound), Qt::Uninitialized);
    size_t out_pos = 0;
    const lzma_ret ret =
        lzma_easy_buffer_encode(LZMA_PRESET_DEFAULT,
                                LZMA_CHECK_CRC64,
                                nullptr,
                                reinterpret_cast<const uint8_t*>(input.constData()),
                                static_cast<size_t>(input.size()),
                                reinterpret_cast<uint8_t*>(out.data()),
                                &out_pos,
                                bound);
    if (ret != LZMA_OK) {
        return {};
    }
    out.truncate(static_cast<qsizetype>(out_pos));
    return out;
}

void StreamingDecompressorTests::realXzStreamDecodesUnderMemLimit() {
    // B8-11: the bounded decoder memory limit (512 MiB) must still admit a normal
    // xz stream (default preset needs an 8 MiB dictionary), so capping memory does
    // not break real files -- only a crafted oversized-dictionary header fails
    // closed (LZMA_MEMLIMIT_ERROR).
    QByteArray payload(300'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 37 + 11) & 0xFF);
    }
    const QByteArray xz = xzCompress(payload);
    QVERIFY(!xz.isEmpty());
    writeFile("real.xz", xz);

    auto decomp = sak::DecompressorFactory::create(filePath("real.xz"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("real.xz")));
    QCOMPARE(readFully(*decomp), static_cast<qint64>(payload.size()));
}

// Build a legacy .lzma-alone header: a properties byte (0x5D == lc=3,lp=0,pb=2, the
// standard preset), a little-endian uint32 dictionary size, and a little-endian uint64
// uncompressed size (all-0xFF == unknown/streaming). The alone format carries the
// dictionary size as a raw uint32, so it is the only way to hand-craft a header that
// declares an oversized dictionary -- no liblzma encoder will emit one.
QByteArray StreamingDecompressorTests::lzmaAloneHeader(quint32 dictSize) {
    QByteArray h;
    h.append(static_cast<char>(0x5D));
    for (int i = 0; i < 4; ++i) {
        h.append(static_cast<char>((dictSize >> (i * 8)) & 0xFF));
    }
    h.append(QByteArray(8, static_cast<char>(0xFF)));
    return h;
}

void StreamingDecompressorTests::xzOversizedDictionaryHeaderFailsClosed() {
    // B8-11 negative, the companion to realXzStreamDecodesUnderMemLimit: a crafted header whose
    // declared dictionary exceeds the 512 MiB decoder memory ceiling must fail closed with
    // LZMA_MEMLIMIT_ERROR, never allocate ~gigabytes from a tiny file. Driven straight through
    // XzDecompressor (lzma_auto_decoder detects the .lzma-alone format) so factory routing on the
    // file extension does not matter. open() only initialises the decoder -- it never reads the
    // payload -- so it succeeds; the memory ceiling is enforced when lzma_code parses the header.
    const QString memlimitError = QString("Decompression error: lzma error code %1")
                                      .arg(static_cast<int>(LZMA_MEMLIMIT_ERROR));

    // 768 MiB is a legal LZMA dictionary (xz allows up to ~1.5 GiB) but is over the 512 MiB cap.
    writeFile("oversized_dict.lzma", lzmaAloneHeader(768u * 1024 * 1024) + QByteArray(64, '\0'));
    sak::XzDecompressor over;
    QVERIFY(over.open(filePath("oversized_dict.lzma")));
    QCOMPARE(readFully(over), static_cast<qint64>(-1));  // fail closed, no gigabyte allocation
    QVERIFY(!over.atEnd());
    QCOMPARE(over.lastError(),
             memlimitError);  // rejected for MEMORY specifically, isolating the cap

    // Guard-isolation control: the SAME crafted-alone shape with a sub-cap (1 MiB) dictionary is
    // NOT rejected for memory -- it fails later on the garbage LZMA payload with a different lzma
    // error. The only difference from the case above is the dictionary-size field, so the memlimit
    // rejection is caused by the 512 MiB cap, not by the stream being malformed in general.
    writeFile("undercap_dict.lzma", lzmaAloneHeader(1u * 1024 * 1024) + QByteArray(64, '\0'));
    sak::XzDecompressor under;
    QVERIFY(under.open(filePath("undercap_dict.lzma")));
    QCOMPARE(readFully(under), static_cast<qint64>(-1));  // still garbage -> fails closed
    QVERIFY(under.lastError() != memlimitError);          // but NOT for memory
}

void StreamingDecompressorTests::concatenatedGzipMembersDecodeFully() {
    // B8-12: pigz emits multi-member gzip -- two members concatenated in one file.
    // The decoder must decode BOTH, not stop after the first (which would flash a
    // truncated image).
    QByteArray a(120'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < a.size(); ++i) {
        a[i] = static_cast<char>((i * 13 + 5) & 0xFF);
    }
    QByteArray b(90'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < b.size(); ++i) {
        b[i] = static_cast<char>((i * 29 + 17) & 0xFF);
    }
    const QByteArray ga = gzipCompress(a);
    const QByteArray gb = gzipCompress(b);
    QVERIFY(!ga.isEmpty());
    QVERIFY(!gb.isEmpty());
    writeFile("multi.gz", ga + gb);

    auto decomp = sak::DecompressorFactory::create(filePath("multi.gz"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("multi.gz")));
    QCOMPARE(readFully(*decomp), static_cast<qint64>(a.size() + b.size()));
}

void StreamingDecompressorTests::concatenatedXzMembersDecodeFully() {
    // B8-12: two .xz streams concatenated (cat a.xz b.xz) must both decode.
    QByteArray a(80'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < a.size(); ++i) {
        a[i] = static_cast<char>((i * 7 + 3) & 0xFF);
    }
    QByteArray b(50'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < b.size(); ++i) {
        b[i] = static_cast<char>((i * 19 + 23) & 0xFF);
    }
    const QByteArray xa = xzCompress(a);
    const QByteArray xb = xzCompress(b);
    QVERIFY(!xa.isEmpty());
    QVERIFY(!xb.isEmpty());
    writeFile("multi.xz", xa + xb);

    auto decomp = sak::DecompressorFactory::create(filePath("multi.xz"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("multi.xz")));
    QCOMPARE(readFully(*decomp), static_cast<qint64>(a.size() + b.size()));
}

void StreamingDecompressorTests::trailingGarbageAfterMemberFailsClosed() {
    // B8-12: bytes after the last member that are not a valid next member must fail
    // closed, not be silently accepted as a clean end.
    QByteArray a(60'000, Qt::Uninitialized);
    for (qsizetype i = 0; i < a.size(); ++i) {
        a[i] = static_cast<char>((i * 11 + 2) & 0xFF);
    }
    const QByteArray ga = gzipCompress(a);
    QVERIFY(!ga.isEmpty());
    // Append clearly non-gzip trailing bytes (gzip magic is 1F 8B).
    writeFile("garbage.gz", ga + QByteArray(4096, static_cast<char>(0xFF)));

    auto decomp = sak::DecompressorFactory::create(filePath("garbage.gz"));
    QVERIFY(decomp != nullptr);
    QVERIFY(decomp->open(filePath("garbage.gz")));
    QCOMPARE(readFully(*decomp), static_cast<qint64>(-1));  // fail closed
    // The trailing bytes are not a member, so the stream never reaches a legitimate
    // end: reporting atEnd() here would mean the garbage had been accepted as one.
    QVERIFY(!decomp->atEnd());
    // The zlib message text is library-provided, but the numeric code is pinnable:
    // re-entering the decoder on the non-member trailing bytes fails the gzip header
    // check -> Z_DATA_ERROR (the compile-time constant -3), formatted as the "(%2)" tail.
    QVERIFY(decomp->lastError().endsWith(QStringLiteral("(-3)")));
}

QTEST_GUILESS_MAIN(StreamingDecompressorTests)
#include "test_streaming_decompressor.moc"
