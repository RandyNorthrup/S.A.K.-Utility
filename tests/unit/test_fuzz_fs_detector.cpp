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
constexpr qsizetype kSwapSignatureBytes = 10;  // "SWAPSPACE2" / "SWAP-SPACE"

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

// HFS+ volume header carrying an explicit geometry triple. The capacity guard is four arms
// (block size >= 512, power of two, total blocks > 0, free <= total) and an all-zero header
// short-circuits on the FIRST one, so probing the others needs an otherwise-valid header.
std::optional<sak::PartitionFileSystemDetection> detectHfsGeometry(uint32_t blockSize,
                                                                   uint32_t totalBlocks,
                                                                   uint32_t freeBlocks) {
    constexpr qsizetype kHfsBlockSizeOffset = 40;
    constexpr qsizetype kHfsTotalBlocksOffset = 44;
    constexpr qsizetype kHfsFreeBlocksOffset = 48;
    QByteArray probe(kSeedBytes, '\0');
    sak::testfixtures::writeAscii(&probe, kHfsHeaderOffset, "H+");
    sak::testfixtures::writeBe32(&probe, kHfsHeaderOffset + kHfsBlockSizeOffset, blockSize);
    sak::testfixtures::writeBe32(&probe, kHfsHeaderOffset + kHfsTotalBlocksOffset, totalBlocks);
    sak::testfixtures::writeBe32(&probe, kHfsHeaderOffset + kHfsFreeBlocksOffset, freeBlocks);
    return sak::PartitionFileSystemDetector::detectBytes(probe,
                                                         static_cast<uint64_t>(probe.size()));
}

// The detail lines an HFS+ header yields when the geometry guard REFUSES it: the three block
// lines are suppressed, leaving only the four header lines.
QStringList hfsHeaderOnlyDetails() {
    return QStringList({QStringLiteral("Version: 0"),
                        QStringLiteral("Files: 0"),
                        QStringLiteral("Folders: 0"),
                        QStringLiteral("Journaled: No")});
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

    void unknownPartitionSizeStillBoundsChecksSwapPageOffsets() {
        // detectBytes(bytes, 0) is the "partition size unknown" call shape. It bypasses the
        // page-size filter in swapSignatureInfo (partition_file_system_detector.cpp:708), so the
        // 8K/16K/64K swap pages are probed at offsets 8182/16374/65526 even against a 4 KiB
        // buffer -- offsets the sized shape (and therefore the determinism check above) never
        // reaches. Only hasBytes() keeps those reads inside the caller's bytes. Back the 4 KiB
        // view with a LARGER real allocation that truly carries "SWAPSPACE2" at the out-of-range
        // offset: a bounds-checked probe must ignore bytes the caller never handed over, while an
        // unchecked compare reports "Linux swap" read from past the end. A determinism re-run
        // cannot substitute for this: heap bytes just past the buffer are stable between two
        // back-to-back calls, so sameDetection() stays green under the same break.
        for (const qsizetype pageSize : {8192, 16'384, 65'536}) {
            QByteArray backing(pageSize, '\0');
            sak::testfixtures::writeAscii(&backing, pageSize - kSwapSignatureBytes, "SWAPSPACE2");
            const QByteArray view = QByteArray::fromRawData(backing.constData(), kSeedBytes);
            const auto unsized = sak::PartitionFileSystemDetector::detectBytes(view, 0);
            QVERIFY2(!unsized.has_value(),
                     qPrintable(QStringLiteral("size-0 probe read the %1-byte swap page offset "
                                               "past the end of a %2-byte buffer")
                                    .arg(pageSize)
                                    .arg(kSeedBytes)));
        }
        // Positive control: the rejections above are bounds, not blindness -- inside the bytes the
        // caller DID hand over, the size-0 shape still detects the 4096-byte swap page.
        QByteArray inRange(kSeedBytes, '\0');
        sak::testfixtures::writeAscii(&inRange, kSeedBytes - kSwapSignatureBytes, "SWAPSPACE2");
        const auto detected = sak::PartitionFileSystemDetector::detectBytes(inRange, 0);
        QVERIFY(detected.has_value());
        QCOMPARE(detected->file_system, QStringLiteral("Linux swap"));
        QVERIFY(detected->details.contains(QStringLiteral("Detected page size: 4096")));
    }

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
        // Zeroing the WHOLE 16-bit word above proves only that ONE of the two byte compares
        // refuses it -- either arm alone satisfies that assertion, so neither is load-bearing.
        // The signature is a TWO-byte compare (0x55 at 510 AND 0xAA at 511), so perturb exactly
        // one byte at a time: a sector ending 0x55 0x00, or 0x00 0xAA, is not a boot sector and
        // must never be reported to the technician as NTFS.
        constexpr uint16_t kFirstSignatureByteOnly = 0x0055;   // 0x55 at 510, 0x00 at 511
        constexpr uint16_t kSecondSignatureByteOnly = 0xAA00;  // 0x00 at 510, 0xAA at 511
        for (const uint16_t halfSignature : {kFirstSignatureByteOnly, kSecondSignatureByteOnly}) {
            QByteArray halfSigned(kSeedBytes, '\0');
            sak::testfixtures::writeAscii(&halfSigned, kOemTagOffset, "NTFS    ");
            sak::testfixtures::writeLe16(&halfSigned, kBootSignatureOffset, halfSignature);
            QVERIFY2(!sak::PartitionFileSystemDetector::detectBytes(
                          halfSigned, static_cast<uint64_t>(halfSigned.size()))
                          .has_value(),
                     qPrintable(QStringLiteral("half boot signature %1 was accepted as NTFS")
                                    .arg(halfSignature)));
        }

        // The OEM tag match is an EIGHT-byte compare, but every fixture in the suite writes the
        // tag whole, so a shortened (or shifted) compare window still accepts every positive
        // case. A vendor boot sector tagged "NTFSBOOT" is NOT an NTFS volume; reporting it as
        // one would send a technician at the wrong reader. Pin the full width, and probe each
        // byte position the way the APFS case does.
        QByteArray vendorTag(kSeedBytes, '\0');
        sak::testfixtures::writeAscii(&vendorTag, kOemTagOffset, "NTFSBOOT");
        sak::testfixtures::writeLe16(&vendorTag, kBootSignatureOffset, kBootSignature);
        QVERIFY(!sak::PartitionFileSystemDetector::detectBytes(
                     vendorTag, static_cast<uint64_t>(vendorTag.size()))
                     .has_value());
        constexpr qsizetype kNtfsOemTagByteCount = 8;
        for (qsizetype index = 0; index < kNtfsOemTagByteCount; ++index) {
            QByteArray probe(kSeedBytes, '\0');
            sak::testfixtures::writeAscii(&probe, kOemTagOffset, "NTFS    ");
            sak::testfixtures::writeLe16(&probe, kBootSignatureOffset, kBootSignature);
            probe[kOemTagOffset + index] = 'Z';
            const auto probeDetection = sak::PartitionFileSystemDetector::detectBytes(
                probe, static_cast<uint64_t>(probe.size()));
            QVERIFY2(!probeDetection.has_value(),
                     qPrintable(QStringLiteral("NTFS OEM byte %1 was not compared").arg(index)));
        }
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
        const QStringList noGeometryDetails = detection->details;  // block lines suppressed

        // Production compares BOTH magic bytes (partition_file_system_detector.cpp:512-518), but
        // the only ext-shaped negative in this file is the all-0xFF buffer, whose 0xFF at 0x438 is
        // already refused by the FIRST arm -- so nothing here distinguishes "both bytes matched"
        // from "the low byte matched". Perturb each byte in turn: with the second arm gone, any
        // stray 'S' (0x53) at 0x438 in unrelated payload is reported to the technician as ext2.
        constexpr qsizetype kExtMagicByteCount = 2;
        for (qsizetype index = 0; index < kExtMagicByteCount; ++index) {
            QByteArray probe(kSeedBytes, '\0');
            sak::testfixtures::writeLe16(&probe, kExtMagicOffset, kExtMagic);
            probe[kExtMagicOffset + index] = '\x01';
            QVERIFY2(!sak::PartitionFileSystemDetector::detectBytes(
                          probe, static_cast<uint64_t>(probe.size()))
                          .has_value(),
                     qPrintable(QStringLiteral("ext magic byte %1 was not compared").arg(index)));
        }

        // The zero superblock reaches exactly ONE of the geometry guard's five arms: with
        // s_log_block_size 0 the block size is 1024, which clears the floor, the 1 MiB ceiling
        // and the power-of-two arms, so only `totalBlocks == 0` refuses it above. Drive the arm
        // nothing else in the suite reaches -- test_partition_manager_core.cpp:1831-1853 only
        // builds the all-pass 4096/2048/512 case -- isolated so it is the sole rejecter.
        // 4096-byte blocks clear arms 1-3 and 1000 blocks clears arm 4, so only
        // `freeBlocks > totalBlocks` can refuse this superblock. Without that arm the technician
        // is shown 8192000 free bytes inside a 4096000-byte volume.
        constexpr qsizetype kExtBlocksCountLoOffset = 1024 + 0x4;
        constexpr qsizetype kExtFreeBlocksCountLoOffset = 1024 + 0xC;
        constexpr qsizetype kExtLogBlockSizeOffset = 1024 + 0x18;
        QByteArray freeOverTotal(kSeedBytes, '\0');
        sak::testfixtures::writeLe16(&freeOverTotal, kExtMagicOffset, kExtMagic);
        sak::testfixtures::writeLe32(&freeOverTotal, kExtLogBlockSizeOffset, 2);  // 4096-byte
        sak::testfixtures::writeLe32(&freeOverTotal, kExtBlocksCountLoOffset, 1000);
        sak::testfixtures::writeLe32(&freeOverTotal, kExtFreeBlocksCountLoOffset, 2000);
        const auto inconsistent = sak::PartitionFileSystemDetector::detectBytes(
            freeOverTotal, static_cast<uint64_t>(freeOverTotal.size()));
        QVERIFY(inconsistent.has_value());
        QCOMPARE(inconsistent->file_system, QStringLiteral("ext2"));
        QCOMPARE(inconsistent->total_bytes, 0ULL);
        QCOMPARE(inconsistent->free_bytes, 0ULL);
        QCOMPARE(inconsistent->details, noGeometryDetails);
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
        QCOMPARE(detection->details, hfsHeaderOnlyDetails());

        // Sane geometry: capacity IS reported and the three block lines are appended in order.
        const auto accepted = detectHfsGeometry(4096, 1000, 250);
        QVERIFY(accepted.has_value());
        QCOMPARE(accepted->total_bytes, 4096ULL * 1000ULL);
        QCOMPARE(accepted->free_bytes, 4096ULL * 250ULL);
        QCOMPARE(accepted->details,
                 QStringList({QStringLiteral("Version: 0"),
                              QStringLiteral("Files: 0"),
                              QStringLiteral("Folders: 0"),
                              QStringLiteral("Journaled: No"),
                              QStringLiteral("Block size: 4096"),
                              QStringLiteral("Total blocks: 1000"),
                              QStringLiteral("Free blocks: 250")}));
    }

    void hfsPlusGeometryGuardRejectsEachHostileField() {
        // One hostile field each. A rejected header is still named HFS+, but claims NO capacity:
        // 256 trips only "block size >= 512", 3000 only "power of two", zero total blocks only
        // "total blocks > 0", and free 2000 of 1000 only "free <= total" -- which is the arm that
        // would otherwise report 8.19 MB free on a 4.10 MB volume.
        struct GeometryCase {
            uint32_t block_size;
            uint32_t total_blocks;
            uint32_t free_blocks;
        };
        const std::vector<GeometryCase> rejectedGeometry{
            {.block_size = 256, .total_blocks = 1000, .free_blocks = 250},
            {.block_size = 3000, .total_blocks = 1000, .free_blocks = 250},
            {.block_size = 4096, .total_blocks = 0, .free_blocks = 0},
            {.block_size = 4096, .total_blocks = 1000, .free_blocks = 2000}};
        for (const GeometryCase& testCase : rejectedGeometry) {
            const auto rejected =
                detectHfsGeometry(testCase.block_size, testCase.total_blocks, testCase.free_blocks);
            QVERIFY(rejected.has_value());
            QCOMPARE(rejected->file_system, QStringLiteral("HFS+"));
            QVERIFY2(rejected->total_bytes == 0ULL && rejected->free_bytes == 0ULL,
                     qPrintable(QStringLiteral("HFS+ geometry %1/%2/%3 was accepted as capacity")
                                    .arg(testCase.block_size)
                                    .arg(testCase.total_blocks)
                                    .arg(testCase.free_blocks)));
            QCOMPARE(rejected->details, hfsHeaderOnlyDetails());
        }
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
