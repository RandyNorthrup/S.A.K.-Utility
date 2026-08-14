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
    QByteArray content;
    for (int i = 0; i < 256; ++i) {
        content += QByteArrayLiteral("the quick brown fox jumps over the lazy dog 0123456789\n");
    }
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
QString backupCodecInvariant(const QByteArray& input, const QDir& dir) {
    const QString container = dir.filePath(QStringLiteral("fuzz.bak"));
    const QString dest = dir.filePath(QStringLiteral("restored.out"));
    if (!writeWholeFile(container, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }

    // Metadata classify must never crash and must return a defined kind (the enum values are the
    // only three; any other bit pattern would be undefined behaviour surfacing).
    const sak::BackupContainerKind kind = sak::backupContainerKind(container);
    if (kind != sak::BackupContainerKind::None && kind != sak::BackupContainerKind::Plain &&
        kind != sak::BackupContainerKind::Encrypted) {
        return QStringLiteral("backupContainerKind returned an undefined kind");
    }

    QFile::remove(dest);
    const auto first = sak::readBackupFile(container, dest, kSeedPassword);
    // Fail-closed: a rejected container must not have produced the destination file.
    if (!first.has_value() && QFile::exists(dest)) {
        return QStringLiteral("decode failed but left an unauthenticated destination file behind");
    }

    // Determinism: a second decode of the same bytes must reach the same verdict.
    QFile::remove(dest);
    const auto second = sak::readBackupFile(container, dest, kSeedPassword);
    if (first.has_value() != second.has_value()) {
        return QStringLiteral("readBackupFile is non-deterministic on identical input");
    }
    return {};
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
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
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
        QVERIFY(sak::readBackupFile(container, dest, kSeedPassword).has_value());
        QVERIFY(QFile::exists(dest));
    }
};

QTEST_GUILESS_MAIN(BackupCodecFuzzTests)
#include "test_fuzz_backup_codec.moc"
