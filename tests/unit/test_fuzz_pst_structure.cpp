// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_pst_structure.cpp
/// @brief Structure-aware mutation-fuzz of the PST parser's LTP accept path (G14-5 depth).
///
/// The plain byte-mutation fuzz (test_fuzz_pst_parser) corrupts a seed anywhere, which
/// almost always breaks an MS-PST CRC and bounces the file off the FIRST integrity gate --
/// so the deep BTree/heap/property code past those gates never runs under it. This harness
/// is the complement: it starts from the openable store (tests/support/pst_fixture.h), mutates
/// the BODY of exactly one region (the Node BTree page, the Block BTree page, or the root
/// folder's Heap-on-Node PC block), then RE-STAMPS that region's PAGETRAILER / BLOCKTRAILER so
/// the file stays byte-integral. The parser therefore ACCEPTS every integrity check and walks
/// the mutated structure -- hostile entry counts and levels, a corrupt HNHDR/HNPAGEMAP/BTHHEADER,
/// a root NID or data BID that points nowhere -- exercising the fail-closed bounds logic in
/// loadNodeBTree / loadBlockBTree / readPropertyContext / readHeapOnNode that the plain fuzz
/// cannot reach.
///
/// The invariant is unchanged and absolute: for EVERY mutant, PstParser::open() and the
/// subsequent accessor walk must neither crash nor hang. A rejected file (isOpen() == false)
/// is a correct fail-closed outcome; the point is that no integral-but-corrupt structure drives
/// a fault, an out-of-bounds read, or unbounded work. The PRNG is the shared fixed-seed
/// splitmix64 (SAK_FUZZ_SEED / SAK_FUZZ_ITERS widen a nightly), so any failure reproduces
/// byte-for-byte.

#include "sak/email_constants.h"
#include "sak/pst_parser.h"

#include "../fuzz/fuzz_harness.h"
#include "../support/pst_fixture.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QVector>

#include <array>
#include <cstdint>

namespace {

// One mutable region of the openable store: a [begin, begin + length) BODY span whose bytes
// the parser reads as structure, and the offset of the trailer that authenticates it. Mutating
// only within body and re-stamping the trailer keeps the whole file integral.
struct Arena {
    int body_begin;
    int body_length;
    int page_offset;  // for a leaf page: restampLeafPageTrailer(page_offset)
    bool is_block;    // true -> restampBlockTrailer instead of a page trailer
};

const std::array<Arena, 3>& arenas() {
    namespace pf = sak::pst_fixture;
    static const std::array<Arena, 3> kArenas{{
        {pf::kOpenableNbtOffset, pf::kLeafPageBodyLen, pf::kOpenableNbtOffset, false},
        {pf::kOpenableBbtOffset, pf::kLeafPageBodyLen, pf::kOpenableBbtOffset, false},
        {pf::kOpenableBlockOffset, pf::kRootBlockCb, pf::kOpenableBlockOffset, true},
    }};
    return kArenas;
}

constexpr int kBitsPerByte = 8;
constexpr int kInterestingByteCount = 4;
constexpr std::array<uint8_t, kInterestingByteCount> kInterestingBytes{0x00u, 0x7Fu, 0x80u, 0xFFu};
constexpr uint32_t kStructMutationOps =
    3;  // flip-bit / random-byte / boundary-byte (all size-preserving)
constexpr uint32_t kMaxStructMutations = 6;  // stacked mutations per mutant
constexpr int kNodeWalkBudget = 64;

// Apply one size-preserving mutation to a single byte within [begin, begin + length).
void mutateOneByteInArena(QByteArray& file, int begin, int length, sak::fuzz::Prng& prng) {
    const int index = begin + static_cast<int>(prng.below(static_cast<uint32_t>(length)));
    switch (prng.below(kStructMutationOps)) {
    case 0:
        file[index] = static_cast<char>(file.at(index) ^ (1 << prng.below(kBitsPerByte)));
        break;
    case 1:
        file[index] = static_cast<char>(prng.byte());
        break;
    default:
        file[index] = static_cast<char>(kInterestingBytes[prng.below(kInterestingByteCount)]);
        break;
    }
}

// Produce one integral-but-structurally-corrupt mutant: mutate one region's body, re-stamp it.
QByteArray makeStructuralMutant(sak::fuzz::Prng& prng) {
    QByteArray file = sak::pst_fixture::buildOpenableUnicodeStore();
    const Arena& arena = arenas()[prng.below(static_cast<uint32_t>(arenas().size()))];
    const uint32_t rounds = 1u + prng.below(kMaxStructMutations);
    for (uint32_t i = 0; i < rounds; ++i) {
        mutateOneByteInArena(file, arena.body_begin, arena.body_length, prng);
    }
    if (arena.is_block) {
        sak::pst_fixture::restampBlockTrailer(file,
                                              arena.page_offset,
                                              sak::pst_fixture::kRootFolderDataBid);
    } else {
        sak::pst_fixture::restampLeafPageTrailer(file, arena.page_offset);
    }
    return file;
}

bool writeWholeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool wrote = file.write(bytes) == bytes.size();
    file.close();  // release the handle so PstParser can open it read-only on Windows
    return wrote;
}

// Touch every accepted-parser accessor that reads attacker-derived structure. Return values are
// ignored: a fail-closed std::unexpected is correct; the invariant is only that none fault/hang.
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
    }
}

QString structureInvariant(const QByteArray& input, const QString& path) {
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

}  // namespace

class PstStructureFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // The unmutated openable store must still open (guards the fixture + re-stamp helpers), then
    // every structural mutant must be handled without a fault or a hang.
    void structuralMutantsNeverCrash() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("fuzz_struct.pst"));

        QVERIFY(writeWholeFile(path, sak::pst_fixture::buildOpenableUnicodeStore()));
        PstParser seed_parser;
        seed_parser.open(path);
        QVERIFY2(seed_parser.isOpen(), "openable seed must open before structural fuzzing");
        seed_parser.close();

        const int iterations = sak::fuzz::iterationsFromEnv();
        sak::fuzz::Prng prng(sak::fuzz::seedFromEnv());
        for (int i = 0; i < iterations; ++i) {
            const QByteArray mutant = makeStructuralMutant(prng);
            const QString detail = structureInvariant(mutant, path);
            if (!detail.isEmpty()) {
                const QString message =
                    QStringLiteral(
                        "PST structure fuzz failed at iteration %1: %2\n  bytes (hex): %3")
                        .arg(i)
                        .arg(detail, sak::fuzz::reproducerHex(mutant));
                QVERIFY2(false, message.toUtf8().constData());
            }
        }
    }
};

QTEST_GUILESS_MAIN(PstStructureFuzzTests)
#include "test_fuzz_pst_structure.moc"
