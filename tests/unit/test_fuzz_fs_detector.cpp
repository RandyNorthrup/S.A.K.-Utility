// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_fs_detector.cpp
/// @brief Mutation-fuzz of the raw file-system signature detector (G14 raw-block gate).
///
/// PartitionFileSystemDetector::detectBytes is the very first thing to touch an untrusted disk:
/// it reads magic signatures and raw geometry across every supported family (FAT / exFAT / NTFS /
/// ext / HFS+ / APFS / ISO / XFS / ...) straight out of attacker-controlled bytes, before any
/// reader runs. A hostile image must never crash it, hang it, or drive an out-of-bounds read; and
/// because it is a pure function, the same bytes must always yield the same verdict. This harness
/// drives detectBytes over thousands of mutated signature-bearing buffers and asserts, for EVERY
/// input:
///
///   1. No crash and no hang, at the declared partition size, at size 0 (unknown), and over a
///      truncated prefix -- the three ways a caller invokes it.
///   2. Determinism: detecting the same bytes twice yields the identical verdict (family and the
///      reported geometry), so no read runs off the end into indeterminate memory.
///   3. A returned detection is well-formed: it always names a non-empty family.
///
/// The seed corpus pokes each family's real magic (NTFS/EXFAT OEM tag, ext 0xEF53, HFS+ "H+",
/// APFS "NXSB", the FAT boot signature) into a compact buffer via the shared byte pokers, so
/// mutation reaches each family's signature parser rather than bouncing off a zeroed image.

#include "sak/partition_file_system_detector.h"

#include "../fuzz/fuzz_harness.h"
#include "../support/byte_writer.h"

#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>

#include <optional>
#include <vector>

namespace {

constexpr int kSeedBytes = 4096;  // holds every family magic below (max offset 1080, ext) with room
constexpr qsizetype kBootSignatureOffset = 510;
constexpr qsizetype kOemTagOffset = 3;
constexpr qsizetype kExtMagicOffset = 1024 + 0x38;  // ext superblock magic, 0xEF53
constexpr qsizetype kHfsHeaderOffset = 1024;
constexpr qsizetype kApfsMagicOffset = 32;
constexpr uint16_t kBootSignature = 0xAA55;
constexpr uint16_t kExtMagic = 0xEF53;

QByteArray seedWith(void (*poke)(QByteArray*)) {
    QByteArray bytes(kSeedBytes, '\0');
    poke(&bytes);
    return bytes;
}

std::vector<QByteArray> detectorCorpus() {
    using sak::testfixtures::writeAscii;
    using sak::testfixtures::writeLe16;
    std::vector<QByteArray> corpus{
        QByteArray(),
        QByteArray(kSeedBytes, '\0'),
        QByteArray(kSeedBytes, '\xFF'),
    };
    corpus.push_back(seedWith([](QByteArray* b) {  // FAT boot signature only
        writeLe16(b, kBootSignatureOffset, kBootSignature);
    }));
    corpus.push_back(seedWith([](QByteArray* b) {  // NTFS OEM tag + boot signature
        writeAscii(b, kOemTagOffset, "NTFS    ");
        writeLe16(b, kBootSignatureOffset, kBootSignature);
    }));
    corpus.push_back(seedWith([](QByteArray* b) {  // exFAT OEM tag
        writeAscii(b, kOemTagOffset, "EXFAT   ");
        writeLe16(b, kBootSignatureOffset, kBootSignature);
    }));
    corpus.push_back(seedWith([](QByteArray* b) {  // ext superblock magic
        writeLe16(b, kExtMagicOffset, kExtMagic);
    }));
    corpus.push_back(seedWith([](QByteArray* b) {  // HFS+ volume header
        writeAscii(b, kHfsHeaderOffset, "H+");
    }));
    corpus.push_back(seedWith([](QByteArray* b) {  // APFS container superblock
        writeAscii(b, kApfsMagicOffset, "NXSB");
    }));
    return corpus;
}

bool sameDetection(const std::optional<sak::PartitionFileSystemDetection>& a,
                   const std::optional<sak::PartitionFileSystemDetection>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a.has_value()) {
        return true;
    }
    return a->file_system == b->file_system && a->total_bytes == b->total_bytes &&
           a->free_bytes == b->free_bytes && a->source == b->source && a->details == b->details;
}

// Run the detector three ways over @p input; return "" if every invariant held.
QString detectorInvariant(const QByteArray& input) {
    const auto sized =
        sak::PartitionFileSystemDetector::detectBytes(input, static_cast<uint64_t>(input.size()));

    // Determinism: a pure function over the same bytes must agree with itself. A mismatch means a
    // read ran off the end into indeterminate memory.
    const auto again =
        sak::PartitionFileSystemDetector::detectBytes(input, static_cast<uint64_t>(input.size()));
    if (!sameDetection(sized, again)) {
        return QStringLiteral("detectBytes is non-deterministic on identical input");
    }
    if (sized.has_value() && sized->file_system.isEmpty()) {
        return QStringLiteral("detection returned an empty file-system name");
    }

    // The other two call shapes a caller uses: unknown partition size, and a truncated prefix.
    // Their only contract here is that they must not crash or hang.
    const auto unsized = sak::PartitionFileSystemDetector::detectBytes(input, 0);
    if (unsized.has_value() && unsized->file_system.isEmpty()) {
        return QStringLiteral("detection (size 0) returned an empty file-system name");
    }
    const QByteArray prefix = input.left(input.size() / 2);
    const auto truncated =
        sak::PartitionFileSystemDetector::detectBytes(prefix, static_cast<uint64_t>(prefix.size()));
    if (truncated.has_value() && truncated->file_system.isEmpty()) {
        return QStringLiteral("detection (truncated) returned an empty file-system name");
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("fs detector fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class FsDetectorFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void detectorNeverCrashesAndStaysDeterministicOnAnyBytes() {
        const sak::fuzz::Target target = [](const QByteArray& input) {
            return detectorInvariant(input);
        };
        const std::vector<QByteArray> corpus = detectorCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }
};

QTEST_GUILESS_MAIN(FsDetectorFuzzTests)
#include "test_fuzz_fs_detector.moc"
