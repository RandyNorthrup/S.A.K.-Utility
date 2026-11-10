// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_backup_codec.cpp
/// @brief Mutation-fuzz of the backup-file container decoder (G14 parser sweep).
///
/// readBackupFile() restores a file from a backup container that may have been produced by another
/// tool, transferred over an untrusted medium, or simply corrupted on disk. The container carries a
/// magic header, a compression layer (zlib), and an authenticated-encryption layer (AES-GCM). Its
/// central security contract is fail-closed: the plaintext is staged beside the destination and
/// only renamed into place AFTER the tag verifies, so an unauthenticated or corrupt payload must
/// never appear under the final name. This harness generates one genuine encrypted+compressed
/// container with the real writer and drives the decoder over thousands of mutated copies of it,
/// asserting for EVERY input:
///
///   1. No crash and no hang (a fault never returns the empty string; a hang trips the ctest
///      timeout with the reproducer recorded in fuzz_harness.h).
///   2. backupContainerKind() -- the only metadata readable without the password -- never crashes
///      and always returns a defined kind.
///   3. Fail-closed: when the decode fails, the destination file does NOT exist, so a corrupted or
///      forged container never leaves an unauthenticated payload behind
///      ([[no-fallbacks-fail-closed]]).
///   4. Determinism: decoding the same bytes twice yields the same success/failure verdict.

#include "sak/backup_file_codec.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <vector>

namespace {

const QString kSeedPassword = QStringLiteral("fuzz-pw");

// The container's magic, mirrored from backup_file_codec.cpp:25-38 so the harness can compute the
// expected kind INDEPENDENTLY of the function it is checking.
constexpr int kMagicBytes = 8;
constexpr int kSeedLines = 256;
constexpr qint64 kSeedPlainBytes = 14'080;    // kSeedLines * 55 bytes
constexpr int kShippedFuzzIterations = 2000;  // tests/fuzz/fuzz_harness.h kDefaultIterations
constexpr int kExpectedCorpusSeeds = 5;

QByteArray plainMagicBytes() {
    return QByteArray("SAKBFC1", kMagicBytes - 1).append('\0');
}

QByteArray encryptedMagicBytes() {
    return QByteArray("SAKBFE1", kMagicBytes - 1).append('\0');
}

// The exact payload the seed container carries, hoisted so the accept path can be required to
// reproduce it byte for byte rather than merely report a length.
QByteArray seedPlaintext() {
    QByteArray content;
    for (int i = 0; i < kSeedLines; ++i) {
        content += QByteArrayLiteral("the quick brown fox jumps over the lazy dog 0123456789\n");
    }
    return content;
}

bool writeWholeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool wrote = file.write(bytes) == bytes.size();
    file.close();
    return wrote;
}

// Produce one genuine encrypted + compressed container with the real writer; return its bytes, or
// an empty array on failure. Exercising the encrypted path puts the AES-GCM tag-verify (the
// fail-closed guarantee) and the zlib layer both under the fuzzer.
QByteArray buildSeedContainer(const QDir& dir) {
    const QString source = dir.filePath(QStringLiteral("plain.txt"));
    const QByteArray content = seedPlaintext();
    if (!writeWholeFile(source, content)) {
        return {};
    }
    const QString container = dir.filePath(QStringLiteral("seed.bak"));
    const sak::BackupCodecOptions options{
        .compress = true, .compression_level = 6, .encrypt = true, .password = kSeedPassword};
    if (!sak::writeBackupFile(source, container, options).has_value()) {
        return {};
    }
    QFile file(container);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

// Write @p input as a container, then drive the decoder over it; return "" if invariants held.
QString checkAcceptedDecode(const sak::BackupCodecResult& result, const QString& dest) {
    // ACCEPTANCE is the strong claim, and it used to carry no assertion at all: every invariant
    // in this target fired only on the FAILURE branch, so any mutation that WIDENS acceptance was
    // invisible -- which is exactly where fail-open lives. The container is authenticated, so
    // whatever this decoder accepts, the only payload it may ever publish is the genuine one.
    if (result.plain_bytes != kSeedPlainBytes) {
        return QStringLiteral("an accepted container restored %1 bytes, not the genuine %2")
            .arg(result.plain_bytes)
            .arg(kSeedPlainBytes);
    }
    QFile restored(dest);
    if (!restored.open(QIODevice::ReadOnly)) {
        return QStringLiteral("decode succeeded but published no destination file");
    }
    if (restored.readAll() != seedPlaintext()) {
        return QStringLiteral(
            "an accepted container published bytes that are not the genuine "
            "payload");
    }
    return {};
}

// Metadata classify, cross-checked against an INDEPENDENT reading of the same bytes. The old
// check compared the returned kind against the three enumerators, which can never be false: every
// return in backupContainerKind is an enumerator literal (backup_file_codec.cpp:319-329) and
// nothing read from the file is ever cast into the enum. That threw away precisely what a fuzzer
// is good at -- manufacturing near-miss magics by the thousand -- because nothing pinned WHICH
// kind a given eight bytes must map to. It matters concretely: readBackupFile seeks past the magic
// (:588) and the AEAD tag covers only the encryptor header and the ciphertext, so byte 7 of the
// magic sits OUTSIDE the authenticated region and this exact compare is the only thing guarding it.
QString checkContainerKind(const QByteArray& input, const QString& container) {
    const QByteArray head = input.left(kMagicBytes);
    const sak::BackupContainerKind expected_kind = (head == plainMagicBytes())
                                                       ? sak::BackupContainerKind::Plain
                                                   : (head == encryptedMagicBytes())
                                                       ? sak::BackupContainerKind::Encrypted
                                                       : sak::BackupContainerKind::None;
    if (sak::backupContainerKind(container) != expected_kind) {
        return QStringLiteral("backupContainerKind disagreed with the container's first %1 bytes")
            .arg(kMagicBytes);
    }
    return {};
}

// One decode's full verdict: fail-closed on the refusal side, genuine-payload on the accept side.
// @p result is nullptr when the decoder refused.
QString checkOneDecode(const sak::BackupCodecResult* result, const QString& dest) {
    if (result == nullptr) {
        // Fail-closed: a rejected container must not have produced the destination file.
        if (QFile::exists(dest)) {
            return QStringLiteral(
                "decode failed but left an unauthenticated destination file behind");
        }
        return {};
    }
    return checkAcceptedDecode(*result, dest);
}

QString backupCodecInvariant(const QByteArray& input, const QDir& dir) {
    const QString container = dir.filePath(QStringLiteral("fuzz.bak"));
    const QString dest = dir.filePath(QStringLiteral("restored.out"));
    if (!writeWholeFile(container, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }
    const QString classified = checkContainerKind(input, container);
    if (!classified.isEmpty()) {
        return classified;
    }

    QFile::remove(dest);
    const auto first = sak::readBackupFile(container, dest, kSeedPassword);
    const QString first_verdict = checkOneDecode(first.has_value() ? &*first : nullptr, dest);
    if (!first_verdict.isEmpty()) {
        return first_verdict;
    }

    // Determinism: a second decode of the same bytes must reach the same verdict -- and this time
    // WITHOUT clearing the destination first. That removal is what made publishRestored's
    // "destination already exists" arm (backup_file_codec.cpp:560-563) unreachable: no test
    // anywhere in the tree restores over an existing file, so the arm had zero coverage, and this
    // harness is one line away from covering it on every accepted input.
    const auto second = sak::readBackupFile(container, dest, kSeedPassword);
    if (first.has_value() != second.has_value()) {
        return QStringLiteral("readBackupFile is non-deterministic on identical input");
    }
    // ... and the second decode must publish the same payload, not merely the same verdict.
    return checkOneDecode(second.has_value() ? &*second : nullptr, dest);
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("backup codec fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class BackupCodecFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void decoderNeverCrashesAndStaysFailClosedOnAnyBytes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());

        const QByteArray seed = buildSeedContainer(out);
        QVERIFY2(!seed.isEmpty(), "backup writer could not synthesize the seed container");
        // ... and the seed must still BE what this fuzz needs: encrypted AND compressed, which is
        // why buildSeedContainer asks for both. Only non-emptiness was pinned, so if the writer
        // ever stopped applying one of the two transforms the seed would remain a valid,
        // non-empty, round-trippable container, both slots here would stay green, and the campaign
        // would silently fuzz a layer that is no longer present. Each transform is proved by an
        // independent observable: the magic names the encryption layer, and a container smaller
        // than its own plaintext can only be a compressed one.
        QCOMPARE(seed.left(kMagicBytes), encryptedMagicBytes());
        QVERIFY2(static_cast<qint64>(seed.size()) < kSeedPlainBytes,
                 qPrintable(QStringLiteral("seed is %1 bytes, not smaller than its %2-byte "
                                           "plaintext -- the zlib layer is missing")
                                .arg(seed.size())
                                .arg(kSeedPlainBytes)));

        std::vector<QByteArray> corpus{
            seed,
            QByteArray(),
            QByteArray(seed.size(), '\0'),
            QByteArray(seed.size(), '\xFF'),
            seed.left(seed.size() / 2),  // truncated
        };
        const sak::fuzz::Target target = [&out](const QByteArray& input) {
            return backupCodecInvariant(input, out);
        };
        const int budget = sak::fuzz::iterationsFromEnv();
        QVERIFY2(budget > 0, "the clamp must never hand run() a non-positive iteration budget");
        if (!qEnvironmentVariableIsSet("SAK_FUZZ_ITERS")) {
            // The shipped default, pinned to a LITERAL. Deriving BOTH sides of the count below
            // from the same call is self-satisfying -- with a budget of 0 the mutation loop runs
            // no iterations, iterations_run equals the 5 seed checks, and 5 + 0 still matches, so
            // the equation holds for any budget including none.
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
        // iteration, so the exact count is corpus.size() + the iteration budget. The corpus size
        // is pinned too: a seed silently dropped would move both sides of a size()-derived
        // expectation together and shrink the fuzz surface with nothing to notice it.
        QCOMPARE(static_cast<int>(corpus.size()), kExpectedCorpusSeeds);
        QCOMPARE(outcome.iterations_run, kExpectedCorpusSeeds + budget);
    }

    /// The corpus's four deliberate-garbage entries must be REFUSED, and refused fail-closed.
    /// The fuzz target returns "" (pass) whether readBackupFile accepts or rejects, so there was
    /// no input anywhere in this suite for which a container that MUST be rejected was asserted
    /// to have been: four of the five corpus entries are garbage and every one of them was scored
    /// green no matter what the decoder did with it. That degrades the whole campaign to a no-op
    /// the moment a guard starts accepting.
    void garbageContainersAreRefusedFailClosed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());
        const QByteArray seed = buildSeedContainer(out);
        QVERIFY(!seed.isEmpty());

        const QString container = out.filePath(QStringLiteral("garbage.bak"));
        const QString dest = out.filePath(QStringLiteral("garbage.out"));
        const QVector<QPair<QString, QByteArray>> refusable{
            {QStringLiteral("empty"), QByteArray()},
            {QStringLiteral("all-zero"), QByteArray(seed.size(), '\0')},
            {QStringLiteral("all-0xFF"), QByteArray(seed.size(), '\xFF')},
            {QStringLiteral("truncated"), seed.left(seed.size() / 2)},
            // A genuine magic over a garbage body: this one CLEARS the container-kind gate at
            // backup_file_codec.cpp:579-582 and must be stopped later, by the header guard and
            // ultimately the AEAD tag. The four entries above are all refused at the very first
            // gate, so without this the decoder's actual authentication was never the thing
            // doing the rejecting.
            {QStringLiteral("valid magic, forged body"),
             encryptedMagicBytes() + QByteArray(seed.size() - kMagicBytes, '\x5A')},
            // The genuine container with its LAST byte flipped -- the AEAD tag's own territory.
            {QStringLiteral("tag-flipped"),
             [&seed] {
                 QByteArray tampered = seed;
                 tampered[tampered.size() - 1] = static_cast<char>(tampered.back() ^ 0xFF);
                 return tampered;
             }()},
        };

        for (const auto& [label, bytes] : refusable) {
            QFile::remove(dest);
            QVERIFY2(writeWholeFile(container, bytes), qPrintable(label));
            const auto result = sak::readBackupFile(container, dest, kSeedPassword);
            QVERIFY2(!result.has_value(),
                     qPrintable(QStringLiteral("%1 was ACCEPTED by the decoder").arg(label)));
            QVERIFY2(!QFile::exists(dest),
                     qPrintable(
                         QStringLiteral("%1 left an unauthenticated file behind").arg(label)));
        }
    }

    /// Near-miss magics. backupContainerKind is an exact eight-byte compare, and byte 7 -- the
    /// trailing NUL -- lies OUTSIDE the AEAD-authenticated region (readBackupFile seeks past the
    /// magic at backup_file_codec.cpp:588), so this compare is the only thing guarding it. Every
    /// probe in the tree used tokens sharing no prefix with a magic, which no loosening of the
    /// compare would refuse.
    void nearMissMagicsAreNotContainers() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("probe.bak"));

        const QVector<QPair<QByteArray, sak::BackupContainerKind>> probes{
            {plainMagicBytes(), sak::BackupContainerKind::Plain},
            {encryptedMagicBytes(), sak::BackupContainerKind::Encrypted},
            // Byte 7 is the unauthenticated one: a single edit there must un-classify the file.
            {QByteArray("SAKBFC1X", kMagicBytes), sak::BackupContainerKind::None},
            {QByteArray("SAKBFE1X", kMagicBytes), sak::BackupContainerKind::None},
            // One character off, sharing a six-byte prefix with BOTH magics.
            {QByteArray("SAKBFD1", kMagicBytes - 1).append('\0'), sak::BackupContainerKind::None},
            {QByteArray("SAKBFC2", kMagicBytes - 1).append('\0'), sak::BackupContainerKind::None},
            // Case matters: the compare is byte-exact, not a folded one.
            {QByteArray("sakbfc1", kMagicBytes - 1).append('\0'), sak::BackupContainerKind::None},
        };

        // The tail is 0xAA, never NUL: padding a seven-byte prefix with zeros would RECONSTITUTE
        // the magic (both magics end in '\0'), which is a property of the fixture rather than of
        // the code under test.
        for (const auto& [head, expected] : probes) {
            QVERIFY(writeWholeFile(path, head + QByteArray(64, '\xAA')));
            QVERIFY2(sak::backupContainerKind(path) == expected, qPrintable(head.toHex()));
        }

        // A file SHORTER than the magic cannot be a container: read() returns fewer than eight
        // bytes, so neither exact compare can match.
        for (int length = 0; length < kMagicBytes; ++length) {
            QVERIFY(writeWholeFile(path, plainMagicBytes().left(length)));
            QVERIFY2(
                sak::backupContainerKind(path) == sak::BackupContainerKind::None,
                qPrintable(
                    QStringLiteral("a %1-byte file was classified as a container").arg(length)));
        }
    }

    // The seed container must actually round-trip, or the fuzz above would never reach the decode
    // path -- this pins the accept path.
    void seedContainerRoundTrips() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());
        const QByteArray seed = buildSeedContainer(out);
        QVERIFY(!seed.isEmpty());

        const QString container = out.filePath(QStringLiteral("seed.bak"));
        const QString dest = out.filePath(QStringLiteral("roundtrip.out"));
        const auto result = sak::readBackupFile(container, dest, kSeedPassword);
        QVERIFY(result.has_value());
        // The decode must restore the full original payload (256 lines x 55 bytes); a bare
        // has_value() would still pass if it restored a different-length payload.
        QCOMPARE(result->plain_bytes, kSeedPlainBytes);
        // The sibling field. readBackupFile's stored_bytes is asserted NOWHERE in the tree -- the
        // only assertion on it covers the WRITE path -- so it could be wired to any number at all
        // with the entire suite green. The container's own on-disk size is an exact,
        // independently-derived expectation, and it is already in scope.
        QCOMPARE(result->stored_bytes, static_cast<qint64>(seed.size()));
        QVERIFY(QFile::exists(dest));
        // ... and the restored bytes are the genuine payload, not merely the right LENGTH.
        QFile restored(dest);
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), seedPlaintext());
    }
};

QTEST_GUILESS_MAIN(BackupCodecFuzzTests)
#include "test_fuzz_backup_codec.moc"
