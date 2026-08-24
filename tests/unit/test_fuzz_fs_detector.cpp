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
        // On the all-pass path (guaranteed here: any failure QVERIFY2(false)-returns above),
        // run() increments iterations_run once per seed (checkSeeds) plus once per mutation
        // iteration, so the exact count is corpus.size() + the iteration budget. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations -- the whole
        // campaign silently evaporating while the seed rounds alone satisfied it.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }

    // -------------------------------------------------------------------------------------------
    // Exact-value covering suite. The fuzz slot above proves detectBytes never crashes and stays
    // deterministic, but it pins NO bytes -> family value, so a magic-byte, magic-offset, family-
    // string, or magic-length mutation survives it. These cases craft a minimal buffer carrying
    // exactly one family's signature at its real offset and assert the EXACT family string, plus
    // the fail-closed "unknown" verdict on unsigned/garbage bytes. Kills the fs_detector.json
    // mutation catalog. detectBytes is called with the buffer's own size as the partition size,
    // matching the primary call shape in detectFromDevice.
    // -------------------------------------------------------------------------------------------

    void detectsApfsContainerByNxsbMagic() {
        QByteArray image(kSeedBytes, '\0');
        sak::testfixtures::writeAscii(&image, kApfsMagicOffset, "NXSB");
        const auto detection = sak::PartitionFileSystemDetector::detectBytes(
            image, static_cast<uint64_t>(image.size()));
        QVERIFY(detection.has_value());
        QCOMPARE(detection->file_system, QStringLiteral("APFS"));
        QCOMPARE(detection->source, QStringLiteral("RawSignature"));
        // The family name is one of five fields; the detail catalog IS the product of the parse.
        // A zero block size is not a usable geometry: nothing may be reported as capacity, and
        // the sanity block must surface BOTH warnings rather than an "consistent" OK line.
        QCOMPARE(detection->total_bytes, 0ULL);
        QCOMPARE(detection->free_bytes, 0ULL);
        QCOMPARE(detection->details,
                 QStringList({QStringLiteral("Container UUID: "
                                             "00000000-0000-0000-0000-000000000000"),
                              QStringLiteral("Features: 0x0000000000000000"),
                              QStringLiteral("Read-only compatible features: "
                                             "0x0000000000000000"),
                              QStringLiteral("Incompatible features: 0x0000000000000000"),
                              QStringLiteral("Checkpoint descriptor next index: 0"),
                              QStringLiteral("Checkpoint data next index: 0"),
                              QStringLiteral("Checkpoint descriptor start index: 0"),
                              QStringLiteral("Checkpoint data start index: 0"),
                              QStringLiteral("Volume OID slots used: 0"),
                              QStringLiteral("Metadata sanity warning: APFS block size is "
                                             "outside supported sane bounds"),
                              QStringLiteral("Metadata sanity warning: APFS block count is "
                                             "zero")}));
    }

    void rejectsApfsWhenFourthMagicByteWrong() {
        // Only 3 of the 4 magic bytes match ("NXS" + wrong byte). A full 4-byte compare must fail
        // closed; a shortened length compare would wrongly report APFS.
        QByteArray image(kSeedBytes, '\0');
        sak::testfixtures::writeAscii(&image, kApfsMagicOffset, "NXSX");
        const auto detection = sak::PartitionFileSystemDetector::detectBytes(
            image, static_cast<uint64_t>(image.size()));
        QVERIFY(!detection.has_value());
        // No test anywhere perturbs magic byte 0, 1 or 2, so a compare window shifted one byte
        // in still accepts real "NXSB" and still rejects "NXSX" -- while a buffer carrying
        // "ZXSB" would be reported to the technician as an APFS container. Probe every position.
        constexpr qsizetype kApfsMagicByteCount = 4;
        for (qsizetype index = 0; index < kApfsMagicByteCount; ++index) {
            QByteArray probe(kSeedBytes, '\0');
            sak::testfixtures::writeAscii(&probe, kApfsMagicOffset, "NXSB");
            probe[kApfsMagicOffset + index] = 'Z';
            const auto probeDetection = sak::PartitionFileSystemDetector::detectBytes(
                probe, static_cast<uint64_t>(probe.size()));
            QVERIFY2(!probeDetection.has_value(),
                     qPrintable(QStringLiteral("APFS magic byte %1 was not compared").arg(index)));
        }
    }

    void detectsNtfsBySignedBootSectorOemTag() {
        QByteArray image(kSeedBytes, '\0');
        sak::testfixtures::writeAscii(&image, kOemTagOffset, "NTFS    ");
        sak::testfixtures::writeLe16(&image, kBootSignatureOffset, kBootSignature);
        const auto detection = sak::PartitionFileSystemDetector::detectBytes(
            image, static_cast<uint64_t>(image.size()));
        QVERIFY(detection.has_value());
        QCOMPARE(detection->file_system, QStringLiteral("NTFS"));
        QCOMPARE(detection->source, QStringLiteral("RawSignature"));
        // A boot-sector verdict is a NAME ONLY: no geometry and no detail lines are claimed.
        QCOMPARE(detection->total_bytes, 0ULL);
        QCOMPARE(detection->free_bytes, 0ULL);
        QCOMPARE(detection->details, QStringList());
        // Production requires BOTH the OEM tag and the boot signature, but only the accepting
        // combination was tested: strip the 0xAA55 and the sector is no longer a boot sector,
        // so detection must fail closed rather than decide the family on the tag alone.
        QByteArray unsignedImage(kSeedBytes, '\0');
        sak::testfixtures::writeAscii(&unsignedImage, kOemTagOffset, "NTFS    ");
        QVERIFY(!sak::PartitionFileSystemDetector::detectBytes(
                     unsignedImage, static_cast<uint64_t>(unsignedImage.size()))
                     .has_value());
    }

    void detectsExt2ByEf53SuperblockMagic() {
        // 0xEF53 at 0x438 with no feature flags -> the plain ext2 branch.
        QByteArray image(kSeedBytes, '\0');
        sak::testfixtures::writeLe16(&image, kExtMagicOffset, kExtMagic);
        const auto detection = sak::PartitionFileSystemDetector::detectBytes(
            image, static_cast<uint64_t>(image.size()));
        QVERIFY(detection.has_value());
        QCOMPARE(detection->file_system, QStringLiteral("ext2"));
        QCOMPARE(detection->source, QStringLiteral("RawSignature"));
        // Zero total blocks fails the geometry guard: no capacity, no block detail lines, and no
        // volume-label line for an all-zero label field.
        QCOMPARE(detection->total_bytes, 0ULL);
        QCOMPARE(detection->free_bytes, 0ULL);
        QCOMPARE(detection->details,
                 QStringList({QStringLiteral("Inodes: 0"),
                              QStringLiteral("Free inodes: 0"),
                              QStringLiteral("Blocks per group: 0"),
                              QStringLiteral("Inodes per group: 0"),
                              QStringLiteral("Journaled: No"),
                              QStringLiteral("Feature compat: 0x00000000"),
                              QStringLiteral("Feature incompat: 0x00000000"),
                              QStringLiteral("Feature ro compat: 0x00000000")}));
    }

    void detectsHfsPlusByVolumeHeaderSignature() {
        QByteArray image(kSeedBytes, '\0');
        sak::testfixtures::writeAscii(&image, kHfsHeaderOffset, "H+");
        const auto detection = sak::PartitionFileSystemDetector::detectBytes(
            image, static_cast<uint64_t>(image.size()));
        QVERIFY(detection.has_value());
        QCOMPARE(detection->file_system, QStringLiteral("HFS+"));
        QCOMPARE(detection->source, QStringLiteral("RawSignature"));
        // Block size 0 fails the geometry guard: no capacity and no block detail lines, and with
        // no wrapper present, no wrapper lines either.
        QCOMPARE(detection->total_bytes, 0ULL);
        QCOMPARE(detection->free_bytes, 0ULL);
        QCOMPARE(detection->details,
                 QStringList({QStringLiteral("Version: 0"),
                              QStringLiteral("Files: 0"),
                              QStringLiteral("Folders: 0"),
                              QStringLiteral("Journaled: No")}));
    }

    void garbageAndEmptyBytesFailClosedToUnknown() {
        const auto allOnes = sak::PartitionFileSystemDetector::detectBytes(
            QByteArray(kSeedBytes, '\xFF'), static_cast<uint64_t>(kSeedBytes));
        QVERIFY(!allOnes.has_value());
        // An all-0xFF buffer is refused by every family guard at once, so any single surviving
        // guard satisfies it. 0xAA55 alone names no family: with no NTFS/exFAT OEM tag and no
        // FAT type string the boot-sector branch must fall through to unknown, never default.
        QByteArray signedBootSectorOnly(kSeedBytes, '\0');
        sak::testfixtures::writeLe16(&signedBootSectorOnly, kBootSignatureOffset, kBootSignature);
        QVERIFY(!sak::PartitionFileSystemDetector::detectBytes(
                     signedBootSectorOnly, static_cast<uint64_t>(signedBootSectorOnly.size()))
                     .has_value());
        const auto empty = sak::PartitionFileSystemDetector::detectBytes(QByteArray(), 0);
        QVERIFY(!empty.has_value());
    }
};

QTEST_GUILESS_MAIN(FsDetectorFuzzTests)
#include "test_fuzz_fs_detector.moc"
