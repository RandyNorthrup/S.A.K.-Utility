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

// Independent recursive oracle over the returned tree -- deliberately NOT
// PstParser::countFolders, so the production recursion is what is under test.
int countTreeFoldersRecursively(const sak::PstFolderTree& tree) {
    int count = 0;
    for (const sak::PstFolder& folder : tree) {
        count += 1 + countTreeFoldersRecursively(folder.children);
    }
    return count;
}

// Touch every accepted-parser accessor that reads attacker-derived structure. Most return
// values are ignored on purpose: a fail-closed std::unexpected is a correct outcome; the
// invariant is that none of these faults or hangs. fileInfo() and folderTree() are the
// exception -- they are RELATED to each other rather than discarded.
QString walkOpenedParser(PstParser& parser) {
    // total_folders is the RECURSIVE folder count of the very tree folderTree() hands back,
    // and it is the number the conversion report and the file-info panel print. The foldered
    // seed runs unmutated through checkSeeds on every execution, so dropping the recursion arm
    // of countFolders -- which every single-root fixture elsewhere still reports as 1 -- fails
    // HERE with 1 != 2. The check is relational, not a hardcoded count, so it also holds for
    // the 1-folder seeds and for every mutant that happens to open.
    const sak::PstFileInfo info = parser.fileInfo();
    const sak::PstFolderTree tree = parser.folderTree();
    const int expected_folders = countTreeFoldersRecursively(tree);
    if (info.total_folders != expected_folders) {
        return QStringLiteral(
                   "fileInfo().total_folders is %1 but folderTree() holds %2 folders "
                   "counted recursively")
            .arg(info.total_folders)
            .arg(expected_folders);
    }

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
    return {};
}

QString pstInvariant(const QByteArray& input, const QString& path) {
    if (!writeWholeFile(path, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }
    PstParser parser;
    parser.open(path);  // synchronous; must return without crashing on any bytes
    if (parser.isOpen()) {
        return walkOpenedParser(parser);
    }

    // A refusal is a TWO-part contract and only the first part ("open() reported false") was
    // ever observed here. loadPstStructure fails an accepted-so-far file through
    // failOpen(..., close_parser == true), whose close() sheds the NBT/BBT caches and the
    // QFile. That second half is load-bearing: allNodeIds and readAttachments carry NO
    // !m_is_open guard -- unlike readItemDetail, readItemProperties, readFolderItems and
    // readAttachmentData -- so they are safe purely because the cache was cleared. Drop the
    // teardown and any mutant that loads both BTrees and then dies in buildFolderHierarchy
    // keeps serving the REJECTED file's nodes and answers SUCCESS with an empty attachment
    // list for them, while a bare "skip the walk" stays green.
    const QVector<uint64_t> refused_nodes = parser.allNodeIds();
    if (!refused_nodes.isEmpty()) {
        return QStringLiteral("open() was refused but the parser still serves %1 NBT node(s)")
            .arg(static_cast<qlonglong>(refused_nodes.size()));
    }
    if (!parser.folderTree().isEmpty()) {
        return QStringLiteral("open() was refused but the parser still serves a folder tree");
    }
    if (parser.readAttachments(sak::email::kNidRootFolder).has_value()) {
        return QStringLiteral(
            "open() was refused but readAttachments answered SUCCESS for the rejected file");
    }
    return {};  // no crash, no hang, and a refusal that actually shed what it had built
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

        // Non-vacuity for the corpus's documented outcomes: pstInvariant scores an ACCEPTED file
        // and a REFUSED one identically once the walk is clean, so the empty store's claim --
        // that it reaches buildFolderHierarchy and fails closed there for want of a root-folder
        // node -- is pinned here rather than left to the harness. A readPropertyContext that
        // answered a missing NBT entry with an empty property set would let
        // buildFolderHierarchyGuarded synthesize an empty root folder, and open() would ACCEPT a
        // store with no folder structure at all, with this fuzz still green.
        const QString reject_seed_path = dir.filePath(QStringLiteral("seed_empty_store.pst"));
        QVERIFY(writeWholeFile(reject_seed_path, sak::pst_fixture::buildEmptyUnicodeStore()));
        PstParser reject_seed_parser;
        reject_seed_parser.open(reject_seed_path);
        QVERIFY2(!reject_seed_parser.isOpen(),
                 "buildEmptyUnicodeStore must be REFUSED: its NBT declares no root-folder node, "
                 "so buildFolderHierarchy must fail closed instead of synthesizing an empty root");

        // ...and the accept-path seed must still OPEN, so the refusal above cannot pass merely
        // because the parser refuses every seed (which would leave the walk exercising nothing).
        const QString accept_seed_path = dir.filePath(QStringLiteral("seed_openable_store.pst"));
        QVERIFY(writeWholeFile(accept_seed_path, sak::pst_fixture::buildOpenableUnicodeStore()));
        PstParser accept_seed_parser;
        accept_seed_parser.open(accept_seed_path);
        QVERIFY2(accept_seed_parser.isOpen(),
                 "buildOpenableUnicodeStore must be ACCEPTED: the refusal pinned above must not "
                 "rest on a parser that refuses everything");

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
        // Exact count on the all-pass path (any failure QVERIFY2(false)-returns above): run()
        // increments iterations_run once per seed plus once per mutation iteration. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }
};

QTEST_GUILESS_MAIN(PstParserFuzzTests)
#include "test_fuzz_pst_parser.moc"
