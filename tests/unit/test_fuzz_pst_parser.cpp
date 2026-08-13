// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_pst_parser.cpp
/// @brief Mutation-fuzz of the PST/OST binary parser through its real open() path
///        (G14-5).
///
/// PstParser is the flagship attacker-supplied-bytes surface in the tree: it walks
/// MS-PST header, CRC, page-trailer, BTree and heap structures, all hardened to fail
/// closed. This harness drives the actual PstParser::open() -> folderTree() ->
/// allNodeIds() -> readItemDetail() pipeline over thousands of mutated files and
/// asserts the one invariant that must hold for EVERY byte string: the parser never
/// crashes and never hangs. A rejected file (isOpen() == false) is a correct outcome,
/// not a failure -- the point is that no malformed input drives a fault, an
/// out-of-bounds read, or unbounded work.
///
/// The seed corpus carries CRC-valid ANSI and Unicode headers (stamped with the same
/// MS-PST weak CRC-32 the parser authenticates against), so mutations reach past the
/// header-integrity gate into the page-read and BTree-load layer rather than bouncing
/// off the magic check. It also carries two page-trailer-valid stores from
/// tests/support/pst_fixture.h: an empty-BTree store (reaches buildFolderHierarchy before
/// failing closed) and an OPENABLE store whose unmutated form drives open() to SUCCESS,
/// so the walk exercises the LTP/messaging accept path (readPropertyContext, readHeapOnNode,
/// the folder-tree walk) and mutations hit the success-then-corrupt branch of each gate.

#include "sak/email_constants.h"
#include "sak/pst_parser.h"

#include "../fuzz/fuzz_harness.h"
#include "../support/pst_fixture.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QVector>

#include <array>
#include <cstdint>
#include <vector>

namespace {

// MS-PST header field offsets (MS-PST 2.2.2.6), named so the seed builder reads as
// spec rather than as bare hex.
constexpr int kMagicByte0 = 0x21;  // '!'
constexpr int kMagicByte1 = 0x42;  // 'B'
constexpr int kMagicByte2 = 0x44;  // 'D'
constexpr int kMagicByte3 = 0x4E;  // 'N'
constexpr int kContentTypeOffset = 8;
constexpr int kVersionOffset = 10;
constexpr int kCrcPartialOffset = 4;
constexpr int kCrcPartialStart = 8;
constexpr int kCrcPartialLen = 471;
constexpr int kCrcFullOffset = 0x20C;
constexpr int kCrcFullStart = 8;
constexpr int kCrcFullLen = 516;
constexpr int kCryptOffsetUnicode = 513;
constexpr int kCryptOffsetAnsi = 461;
constexpr int kHeaderSize = 580;
constexpr int kRootOffset = 0xB4;
constexpr int kRootFileSizeField = 4;
constexpr int kRootNbtField = 44;
constexpr int kRootBbtField = 60;

// One zeroed page after the header for the root pointers to address; the BTree load
// reads it, finds no valid PAGETRAILER, and fails closed -- which is the code path
// the walk exercises.
constexpr int kSeedPageSpan = sak::email::kLegacyUnicodePageSize;

// MS-PST weak CRC-32: reflected polynomial, zero init, no final inversion. Matches
// the parser's own ComputeCRC, so a stamped seed is a genuine spec-conformant header.
constexpr uint32_t kWeakCrcPoly = 0xED'B8'83'20u;
constexpr int kByteBits = 8;
constexpr int kCrcTableSize = 256;

void writeLe16(QByteArray& data, int offset, uint16_t value) {
    data[offset] = static_cast<char>(value & 0xFF);
    data[offset + 1] = static_cast<char>((value >> kByteBits) & 0xFF);
}

void writeLe32(QByteArray& data, int offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * kByteBits)) & 0xFF);
    }
}

void writeLe64(QByteArray& data, int offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * kByteBits)) & 0xFF);
    }
}

uint32_t weakCrc(const QByteArray& data, int offset, int len) {
    static const std::array<uint32_t, kCrcTableSize> table = [] {
        std::array<uint32_t, kCrcTableSize> built{};
        for (uint32_t i = 0; i < kCrcTableSize; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < kByteBits; ++bit) {
                c = (c & 1u) ? (kWeakCrcPoly ^ (c >> 1)) : (c >> 1);
            }
            built[i] = c;
        }
        return built;
    }();
    uint32_t crc = 0;
    for (int i = 0; i < len; ++i) {
        const auto byte = static_cast<uint8_t>(data.at(offset + i));
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> kByteBits);
    }
    return crc;
}

void stampMagic(QByteArray& header) {
    header[0] = static_cast<char>(kMagicByte0);
    header[1] = static_cast<char>(kMagicByte1);
    header[2] = static_cast<char>(kMagicByte2);
    header[3] = static_cast<char>(kMagicByte3);
}

// A CRC-valid Unicode PST header whose root pointers address a zeroed page region.
QByteArray buildUnicodeHeaderSeed() {
    QByteArray file(kHeaderSize + kSeedPageSpan, '\0');
    stampMagic(file);
    writeLe16(file, kContentTypeOffset, sak::email::kPstContentType);
    writeLe16(file, kVersionOffset, sak::email::kUnicodeVersion);
    file[kCryptOffsetUnicode] = static_cast<char>(sak::email::kEncryptNone);
    writeLe64(file, kRootOffset + kRootFileSizeField, static_cast<uint64_t>(file.size()));
    writeLe64(file, kRootOffset + kRootNbtField, static_cast<uint64_t>(kHeaderSize));
    writeLe64(file, kRootOffset + kRootBbtField, static_cast<uint64_t>(kHeaderSize));
    writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
    writeLe32(file, kCrcFullOffset, weakCrc(file, kCrcFullStart, kCrcFullLen));
    return file;
}

// A CRC-valid ANSI PST header (dwCRCPartial only, per MS-PST for the ANSI format).
QByteArray buildAnsiHeaderSeed() {
    QByteArray file(kHeaderSize, '\0');
    stampMagic(file);
    writeLe16(file, kContentTypeOffset, sak::email::kPstContentType);
    writeLe16(file, kVersionOffset, sak::email::kAnsiVersion);
    file[kCryptOffsetAnsi] = static_cast<char>(sak::email::kEncryptNone);
    writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
    return file;
}

std::vector<QByteArray> pstCorpus() {
    return {
        QByteArray(),
        QByteArray("!BDN"),
        QByteArray(kHeaderSize, '\0'),
        QByteArray(kHeaderSize, '\xFF'),
        buildUnicodeHeaderSeed(),
        buildAnsiHeaderSeed(),
        // A store with genuine header CRCs AND genuine Node/Block BTree PAGETRAILERs, but
        // empty BTrees. Unlike the header-only seeds it survives the trailer checks, so the
        // parser walks INTO parseBTreePage, verifyPageTrailer (success), and buildFolderHierarchy
        // before failing closed there (no root folder node). That reaches a layer the header-only
        // seeds never did.
        sak::pst_fixture::buildEmptyUnicodeStore(),
        // An OPENABLE store: a root-folder NBT entry + BBT entry + Heap-on-Node PC block, so the
        // unmutated seed drives PstParser::open() all the way to SUCCESS -- readPropertyContext,
        // readHeapOnNode, and the folder-tree walk on an accepted file. Mutations of it exercise
        // the SUCCESS-then-corrupt branches of every integrity gate (header CRC, page trailer,
        // block trailer, HN/BTH bounds), the LTP/messaging accept path the reject-only seeds
        // never touched (see docs/CODEX_REVIEW_5_REMEDIATION.md, R5-G14-5).
        sak::pst_fixture::buildOpenableUnicodeStore(),
        // A FOLDERED store: the root folder's hierarchy Table Context lists one child folder, so
        // the unmutated seed additionally drives the TC/row-matrix accept path (readTableContext,
        // parseTcInfo, buildTcRows, materializeTcRow, buildTcCell, extractChildNids) and the
        // recursion into the child's PC. Mutations reach those TC parsers with hostile input.
        sak::pst_fixture::buildFolderedUnicodeStore(),
        // A MESSAGING store: the root folder's CONTENTS Table Context lists one message. The walk's
        // readFolderItems drives readContentsTable -> the summary loop, and readItemDetail on the
        // message node drives readMessage. Mutations reach the contents-table + message-read code.
        sak::pst_fixture::buildMessagingUnicodeStore(),
    };
}

// Number of cached nodes the walk touches per input. A cap keeps a mutated file that
// happened to register many nodes from dominating the run; crash-safety is proven per
// node, and the cap is well above what a 580-byte-plus seed can produce.
constexpr int kNodeWalkBudget = 64;

bool writeWholeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool wrote = file.write(bytes) == bytes.size();
    file.close();  // release the handle so PstParser can open it read-only on Windows
    return wrote;
}

// Touch every accepted-parser accessor that reads attacker-derived structure. Return
// value is ignored on purpose: a fail-closed std::unexpected is a correct outcome; the
// invariant is only that none of these faults or hangs.
void walkOpenedParser(PstParser& parser) {
    static_cast<void>(parser.fileInfo());
    static_cast<void>(parser.folderTree());
    const QVector<uint64_t> node_ids = parser.allNodeIds();
    int budget = kNodeWalkBudget;
    for (uint64_t nid : node_ids) {
        if (budget-- <= 0) {
            break;
        }
        static_cast<void>(parser.readItemDetail(nid));
        static_cast<void>(parser.readItemProperties(nid));
        static_cast<void>(parser.readAttachments(nid));
        static_cast<void>(parser.readFolderItems(nid, 0, kNodeWalkBudget));
    }
}

QString pstInvariant(const QByteArray& input, const QString& path) {
    if (!writeWholeFile(path, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }
    PstParser parser;
    parser.open(path);  // synchronous; must return without crashing on any bytes
    if (parser.isOpen()) {
        walkOpenedParser(parser);
    }
    return {};  // no crash and no hang == invariant satisfied
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("PST fuzz failed after %1 inputs: %2\n  reproducer bytes (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class PstParserFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void openNeverCrashesOnAnyBytes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("fuzz.pst"));

        const sak::fuzz::Target target = [&path](const QByteArray& input) {
            return pstInvariant(input, path);
        };
        const std::vector<QByteArray> corpus = pstCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }
};

QTEST_GUILESS_MAIN(PstParserFuzzTests)
#include "test_fuzz_pst_parser.moc"
