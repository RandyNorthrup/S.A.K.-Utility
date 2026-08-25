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

// Stickiness is a claim about EVERY later read, not just the next one, so probe it more than
// once: a single probe cannot tell a sticky flag from the common "surface the error, then clear
// it" idiom.
constexpr int kStickyProbes = 4;

// The seed corpus, by index, so the controls below can name the seed they mean. The count is
// pinned too: a seed silently dropped from decompressorCorpus() would otherwise just shrink the
// fuzz surface with nothing to notice it.
constexpr int kExpectedCorpusSeeds = 7;
constexpr std::size_t kGzipTextSeed = 1;
constexpr std::size_t kGzipBombSeed = 2;
constexpr std::size_t kTruncatedGzipSeed = 3;

// The shipped fuzz budget (tests/fuzz/fuzz_harness.h kDefaultIterations), as a literal. See the
// pin in decodersNeverCrashOrOverrunOnAnyBytes for why re-reading iterationsFromEnv() cannot
// stand in for it.
constexpr int kShippedFuzzIterations = 2000;

// The plaintext behind the gzip_text seed, hoisted so the positive control can require the
// decoder to reproduce it EXACTLY rather than merely not overrun.
QByteArray gzipTextPlain() {
    return QByteArrayLiteral(
        "The quick brown fox jumps over the lazy dog. 0123456789. The quick brown fox.");
}

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
    const QByteArray gzip_text = gzipCompress(gzipTextPlain());
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

// The harness's one INDEPENDENT count. Measuring the expansion only through
// decompressedBytesProduced() reads the bomb guard's own input -- the accumulator at
// streaming_decompressor.cpp:102-103 is exactly what exceededMaxOutput() compares at :174 -- so
// widening or breaking it moves the guard and the observation together and the ceiling check goes
// blind. Requiring it to equal the bytes read() actually handed back also catches a read that
// credits the wrong number.
QString checkProducedMatchesDelivered(const sak::StreamingDecompressor& decomp, qint64 delivered) {
    if (decomp.decompressedBytesProduced() != delivered) {
        return QStringLiteral(
            "decompressedBytesProduced() disagrees with the bytes read() returned");
    }
    return {};
}

// Invariant 3: a terminal failure must STICK -- not merely report itself once. One probe cannot
// distinguish a sticky flag from a failure that surfaces and then clears, which is a common idiom;
// the contract at streaming_decompressor.h:175-179 holds until the next open()/close(), so every
// later read must refuse too.
QString checkFailureIsSticky(sak::StreamingDecompressor& decomp) {
    std::array<char, kReadBufferSize> buffer{};
    for (int probe = 0; probe < kStickyProbes; ++probe) {
        if (decomp.read(buffer.data(), buffer.size()) >= 0) {
            return QStringLiteral("read succeeded after a terminal failure (not sticky)");
        }
    }
    return {};
}

// Invariant 2: pump the decoder to its terminal state, bounding the expansion the whole way.
// Sets @p failed when the stream ended in a terminal error.
QString pumpAndCheckExpansion(sak::StreamingDecompressor& decomp, bool& failed) {
    // The bomb guard fails the read AFTER the chunk that crosses the cap, so the produced total
    // may overshoot by at most one read buffer before read() returns -1. Anything beyond that
    // means the guard stopped bounding expansion.
    const qint64 kProducedCeiling = kMaxDecompressed + kReadBufferSize;
    std::array<char, kReadBufferSize> buffer{};
    qint64 delivered = 0;
    for (int i = 0; i < kMaxReadCalls; ++i) {
        const qint64 n = decomp.read(buffer.data(), buffer.size());
        if (n > 0) {
            delivered += n;
            const QString mismatch = checkProducedMatchesDelivered(decomp, delivered);
            if (!mismatch.isEmpty()) {
                return mismatch;
            }
        }
        if (decomp.decompressedBytesProduced() > kProducedCeiling) {
            return QStringLiteral("decompressed total ran past the bomb cap + one buffer");
        }
        if (n < 0) {
            failed = true;
            return {};
        }
        if (n == 0) {
            // A CLEAN end of stream is only honest below the cap. The guard fails the read that
            // crosses the ceiling (streaming_decompressor.cpp:105-108 returns -1, never the 0
            // that means "complete"), so a stream that stopped expanding AND reported completion
            // has fail-open TRUNCATED -- the flasher would write a partial disk image and call it
            // whole. The ceiling check above structurally cannot see this: the cap is exactly 16
            // read buffers, so the read that crosses it lands the total at 17 * 65536 = 1114112,
            // which is precisely kProducedCeiling, and that compare is `>`.
            if (decomp.decompressedBytesProduced() > kMaxDecompressed) {
                return QStringLiteral(
                    "a clean end of stream was reported above the bomb cap (truncation sold as "
                    "completion)");
            }
            return {};
        }
    }
    return {};
}

// Drive one decoder (chosen by @p path's extension) over @p input; return "" if all three
// invariants held, else the violated one.
QString runOneDecoder(const QByteArray& input, const QString& path) {
    if (!writeWholeFile(path, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }
    auto decomp = sak::DecompressorFactory::create(path);
    // Neither of these can legitimately fail here, and scoring them as "both correct outcomes"
    // -- as this harness used to -- is what makes the whole fuzz able to become a silent no-op.
    // The staged name is always fuzz.gz / fuzz.bz2 / fuzz.xz and detectByExtension matches on the
    // NAME alone, never the bytes (decompressor_factory.cpp:77-91), so a decoder must always be
    // built; open() only opens the QFile and runs a data-independent library init
    // (streaming_decompressor.cpp:27-41), so it must always succeed. Were either to start
    // refusing, every invariant below would go unreached for every input -- thousands of mutants
    // scored as passes -- with nothing anywhere going red.
    if (!decomp) {
        return QStringLiteral("no decoder was built for a known extension");
    }
    if (!decomp->open(path)) {
        return QStringLiteral("open() refused a staged input before a single byte was decoded");
    }
    decomp->setMaxDecompressedBytes(kMaxDecompressed);

    bool failed = false;
    const QString violation = pumpAndCheckExpansion(*decomp, failed);
    if (!violation.isEmpty()) {
        return violation;
    }
    if (failed) {
        return checkFailureIsSticky(*decomp);
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

// How a decoder ENDED. The fuzz invariants above deliberately do not distinguish these -- every
// one of them fires only on an overrun or a resumed failure -- so the deterministic controls need
// their own observable to tell an accepted stream from a rejected one.
enum class TerminalOutcome {
    NeverStarted,
    CleanEof,
    Failed,
    RanOff
};

struct DrainResult {
    TerminalOutcome outcome{TerminalOutcome::RanOff};
    QByteArray produced;
};

// Drive the decoder at @p path to its terminal state under @p cap, collecting what it hands back.
DrainResult drainDecoder(const QString& path, qint64 cap) {
    DrainResult result;
    auto decomp = sak::DecompressorFactory::create(path);
    if (!decomp || !decomp->open(path)) {
        result.outcome = TerminalOutcome::NeverStarted;
        return result;
    }
    decomp->setMaxDecompressedBytes(cap);
    std::array<char, kReadBufferSize> buffer{};
    for (int i = 0; i < kMaxReadCalls; ++i) {
        const qint64 n = decomp->read(buffer.data(), buffer.size());
        if (n < 0) {
            result.outcome = TerminalOutcome::Failed;
            return result;
        }
        if (n == 0) {
            result.outcome = TerminalOutcome::CleanEof;
            return result;
        }
        result.produced.append(buffer.data(), static_cast<qsizetype>(n));
    }
    return result;
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
        // The corpus must be the corpus this file documents. gzipCompress() returns an EMPTY
        // QByteArray on any zlib failure (both arms of it), and nothing used to check the result:
        // if deflateInit2 or deflate ever stopped succeeding, the three gzip seeds would silently
        // become empty buffers, the fuzz would still run its full budget over nothing but empty
        // inputs and three junk magic headers, and every assertion here would still pass. It
        // would take the bomb path with it -- setMaxDecompressedBytes has no caller anywhere in
        // src/, so that seed is the ONLY thing exercising exceededMaxOutput() in the repository.
        QCOMPARE(static_cast<int>(corpus.size()), kExpectedCorpusSeeds);
        QVERIFY2(!corpus[kGzipTextSeed].isEmpty(), "the gzip text seed failed to compress");
        QVERIFY2(!corpus[kGzipBombSeed].isEmpty(), "the gzip bomb seed failed to compress");
        QVERIFY2(!corpus[kTruncatedGzipSeed].isEmpty(), "the truncated gzip seed is empty");
        // ... and the bomb must still BE a bomb: small compressed, past the cap inflated.
        QVERIFY2(static_cast<qint64>(corpus[kGzipBombSeed].size()) < kMaxDecompressed,
                 "the bomb seed is no longer small enough to be a bomb");

        const int budget = sak::fuzz::iterationsFromEnv();
        QVERIFY2(budget > 0, "the clamp must never hand run() a non-positive iteration budget");
        if (!qEnvironmentVariableIsSet("SAK_FUZZ_ITERS")) {
            // The shipped default, pinned to a LITERAL. Checking iterations_run against
            // corpus.size() + iterationsFromEnv() is self-satisfying -- both sides come from the
            // same call, so if the clamp ever answered 0 the mutation loop would run zero times,
            // iterations_run would be 7, and 7 + 0 would still match. No suite in tests/unit pins
            // kDefaultIterations, so a collapsed default would quietly reduce the whole fuzz
            // fleet to a smoke test with nothing red anywhere.
            QCOMPARE(budget, kShippedFuzzIterations);
        }
        const sak::fuzz::FuzzOutcome outcome =
            sak::fuzz::run(corpus, target, budget, sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // On the all-pass path (guaranteed here: any failure QVERIFY2(false)-returns above),
        // run() increments iterations_run once per seed (checkSeeds) plus once per mutation
        // iteration, so the exact count is corpus.size() + the iteration budget. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations.
        QCOMPARE(outcome.iterations_run, kExpectedCorpusSeeds + budget);
    }

    /// The fuzz above scores an ACCEPTED stream and a REJECTED one identically: every invariant in
    /// runOneDecoder fires only on an overrun or a resumed failure, so a clean end of stream is
    /// honoured for every input. That leaves the corpus's rejection seeds unobserved -- turn
    /// read()'s terminal -1 into a 0 (streaming_decompressor.cpp:97-100) and the entire corpus
    /// goes silently green, even though the header comment claims those seeds "exercise
    /// header/format rejection". These are the deterministic controls the fuzz cannot supply.
    void rejectionSeedsAreRejectedAndTheBombIsStopped() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString gz = QDir(dir.path()).filePath(QStringLiteral("control.gz"));
        const std::vector<QByteArray> corpus = decompressorCorpus();
        QCOMPARE(static_cast<int>(corpus.size()), kExpectedCorpusSeeds);

        // POSITIVE control: a genuine gzip stream decodes cleanly to its EXACT input. Without it
        // the file never once proves the decoder produces the right bytes, only that it does not
        // overrun -- a decoder that returned garbage of the right length would satisfy the fuzz.
        QVERIFY(writeWholeFile(gz, corpus[kGzipTextSeed]));
        const DrainResult good = drainDecoder(gz, kMaxDecompressed);
        QVERIFY2(good.outcome == TerminalOutcome::CleanEof,
                 "a valid gzip seed must decode to a clean end of stream");
        QCOMPARE(good.produced, gzipTextPlain());

        // NEGATIVE control: the truncated gzip MUST end in a terminal failure, never a clean end
        // of stream. inflate hits Z_BUF_ERROR with no input left, gzip_decompressor.cpp maps that
        // to a step error, pumpDecoder returns false and read() answers -1.
        QVERIFY(writeWholeFile(gz, corpus[kTruncatedGzipSeed]));
        const DrainResult truncated = drainDecoder(gz, kMaxDecompressed);
        QVERIFY2(truncated.outcome == TerminalOutcome::Failed,
                 "a truncated gzip stream must fail closed, not report a clean end of stream");

        // The BOMB must be stopped BY THE CAP and stopped as a FAILURE. A guard that halted the
        // expansion but reported completion would hand a fixed-capacity consumer a partial image
        // as a whole one. The delivered total is exact, not a floor: the cap is 16 read buffers,
        // and the read that would cross it is failed before its bytes are handed back, so exactly
        // kMaxDecompressed bytes reach the caller.
        QVERIFY(writeWholeFile(gz, corpus[kGzipBombSeed]));
        const DrainResult bomb = drainDecoder(gz, kMaxDecompressed);
        QVERIFY2(bomb.outcome == TerminalOutcome::Failed,
                 "the bomb seed must trip setMaxDecompressedBytes and fail the read");
        QCOMPARE(static_cast<qint64>(bomb.produced.size()), kMaxDecompressed);

        // ... and with NO cap configured the same seed decodes in full, proving the refusal above
        // is the cap talking and not the seed simply being broken.
        const DrainResult uncapped = drainDecoder(gz, 0);
        QVERIFY2(uncapped.outcome == TerminalOutcome::CleanEof,
                 "the bomb seed must decode cleanly when no ceiling is configured");
        QCOMPARE(static_cast<int>(uncapped.produced.size()), kBombPayloadBytes);
    }
};

QTEST_GUILESS_MAIN(DecompressorFuzzTests)
#include "test_fuzz_decompressor.moc"
