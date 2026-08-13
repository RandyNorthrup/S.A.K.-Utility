// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_decompressor.cpp
/// @brief Mutation-fuzz of the streaming archive decompressors (gzip / bzip2 / xz) (G14-10).
///
/// StreamingDecompressor feeds attacker-supplied compressed bytes to zlib / libbz2 / liblzma
/// and hands the inflated output to the ISO pipeline and the archive services. A malformed or
/// hostile stream must be rejected fail-closed, and -- critically -- a decompression BOMB (a few
/// bytes that expand to gigabytes) must be stopped by setMaxDecompressedBytes rather than exhaust
/// memory. This harness drives the real DecompressorFactory -> open() -> read() pipeline of all
/// three decoders over thousands of mutated inputs and asserts three invariants for EVERY input:
///
///   1. No crash and no hang (implicit: a fault never returns the empty string; a hang trips the
///      ctest timeout with the seed recorded in fuzz_harness.h).
///   2. The running decompressed total never exceeds the configured cap -- the bomb guard holds
///      even as the stream is corrupted.
///   3. Terminal failure is sticky: once read() returns < 0 the decompressor stays failed, so a
///      caller cannot resume a half-broken stream and read past the error.
///
/// The seed corpus carries real zlib-produced gzip streams (including a highly compressible one
/// that expands past the cap, to exercise the bomb path) plus bzip2 and xz magic headers, so
/// mutations reach both the decoders' accept paths and their header/format rejection.

#include "sak/decompressor_factory.h"
#include "sak/streaming_decompressor.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <array>
#include <memory>
#include <vector>

#include <zlib.h>

namespace {

constexpr int kReadBufferSize = 65'536;
constexpr qint64 kMaxDecompressed = 1024 * 1024;  // bomb cap: 1 MiB, far above the text seed
constexpr int kMaxReadCalls = 4096;  // backstop; the cap fails the read long before this
constexpr int kGzipWindowBitsWithGzipWrapper = 15 + 16;
constexpr int kBombPayloadBytes = 2 * 1024 * 1024;  // 2 MiB of zeros -> tiny gzip, expands past cap
constexpr int kDeflateChunk = 16'384;

// A genuine gzip stream produced by zlib, so mutants reach the inflate accept path rather than
// bouncing off the header.
QByteArray gzipCompress(const QByteArray& input) {
    z_stream zs{};
    if (deflateInit2(&zs,
                     Z_DEFAULT_COMPRESSION,
                     Z_DEFLATED,
                     kGzipWindowBitsWithGzipWrapper,
                     8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    zs.avail_in = static_cast<uInt>(input.size());
    QByteArray out;
    std::array<char, kDeflateChunk> buffer{};
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buffer.data());
        zs.avail_out = static_cast<uInt>(buffer.size());
        ret = deflate(&zs, Z_FINISH);
        out.append(buffer.data(), static_cast<qsizetype>(buffer.size() - zs.avail_out));
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return ret == Z_STREAM_END ? out : QByteArray();
}

std::vector<QByteArray> decompressorCorpus() {
    const QByteArray gzip_text = gzipCompress(QByteArrayLiteral(
        "The quick brown fox jumps over the lazy dog. 0123456789. The quick brown fox."));
    const QByteArray gzip_bomb = gzipCompress(QByteArray(kBombPayloadBytes, '\0'));
    return {
        QByteArray(),
        gzip_text,
        gzip_bomb,
        gzip_text.left(gzip_text.size() / 2),                           // truncated gzip
        QByteArrayLiteral("\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\xFF"),  // gzip magic + junk
        QByteArrayLiteral("BZh91AY&SY") + QByteArray(24, '\x7F'),       // bzip2 magic + junk
        QByteArrayLiteral("\xFD"
                          "7zXZ\x00") +
            QByteArray(24, '\xFF'),  // xz magic + junk
    };
}

bool writeWholeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool wrote = file.write(bytes) == bytes.size();
    file.close();  // release the handle so the decompressor can open it read-only on Windows
    return wrote;
}

// Drive one decoder (chosen by @p path's extension) over @p input; return "" if all three
// invariants held, else the violated one.
QString runOneDecoder(const QByteArray& input, const QString& path) {
    if (!writeWholeFile(path, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }
    auto decomp = sak::DecompressorFactory::create(path);
    if (!decomp || !decomp->open(path)) {
        return {};  // no decoder or a fail-closed open -- both correct outcomes
    }
    decomp->setMaxDecompressedBytes(kMaxDecompressed);

    // The bomb guard fails the read AFTER the chunk that crosses the cap, so the produced total
    // may overshoot by at most one read buffer before read() returns -1. Anything beyond that
    // means the guard stopped bounding expansion.
    const qint64 kProducedCeiling = kMaxDecompressed + kReadBufferSize;
    std::array<char, kReadBufferSize> buffer{};
    bool failed = false;
    for (int i = 0; i < kMaxReadCalls; ++i) {
        const qint64 n = decomp->read(buffer.data(), buffer.size());
        if (decomp->decompressedBytesProduced() > kProducedCeiling) {
            return QStringLiteral("decompressed total ran past the bomb cap + one buffer");
        }
        if (n < 0) {
            failed = true;
            break;
        }
        if (n == 0) {
            break;  // clean end of stream
        }
    }
    // Invariant 3: a terminal failure must stick -- the next read cannot succeed.
    if (failed && decomp->read(buffer.data(), buffer.size()) >= 0) {
        return QStringLiteral("read succeeded after a terminal failure (not sticky)");
    }
    return {};
}

// Every decoder sees every input: the gzip seeds exercise the inflate accept path; all three see
// each other's bytes as malformed, exercising header/format rejection.
QString decompressorInvariant(const QByteArray& input, const QDir& dir) {
    static const std::array<const char*, 3> kNames{"fuzz.gz", "fuzz.bz2", "fuzz.xz"};
    for (const char* name : kNames) {
        const QString detail = runOneDecoder(input, dir.filePath(QString::fromLatin1(name)));
        if (!detail.isEmpty()) {
            return QString::fromLatin1(name) + QStringLiteral(": ") + detail;
        }
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("decompressor fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class DecompressorFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void decodersNeverCrashOrOverrunOnAnyBytes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());

        const sak::fuzz::Target target = [&out](const QByteArray& input) {
            return decompressorInvariant(input, out);
        };
        const std::vector<QByteArray> corpus = decompressorCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }
};

QTEST_GUILESS_MAIN(DecompressorFuzzTests)
#include "test_fuzz_decompressor.moc"
