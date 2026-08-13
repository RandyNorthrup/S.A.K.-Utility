// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_pst_structure.cpp
/// @brief Structure-aware mutation-fuzz of the PST parser's LTP accept path (G14-5 depth).
///
/// The plain byte-mutation fuzz (test_fuzz_pst_parser) corrupts a seed anywhere, which
/// almost always breaks an MS-PST CRC and bounces the file off the FIRST integrity gate --
/// so the deep BTree/heap/property code past those gates never runs under it. This harness
/// is the complement: it starts from an openable store (tests/support/pst_fixture.h -- the
/// single-folder store, the foldered store whose root has a hierarchy Table Context, or the
/// messaging store whose root has a contents Table Context listing a message), mutates the BODY of
/// exactly one region (a Node/Block BTree page, a PC block, or the single-row TC block),
/// then RE-STAMPS that region's PAGETRAILER / BLOCKTRAILER so the file stays byte-integral. The
/// parser therefore ACCEPTS every integrity check and walks the mutated structure -- hostile entry
/// counts and levels, a corrupt HNHDR/HNPAGEMAP/BTHHEADER, a corrupt TCINFO/column/row-matrix, a
/// root NID or data BID that points nowhere -- exercising the fail-closed bounds logic in
/// loadNodeBTree / loadBlockBTree / readPropertyContext / readHeapOnNode / readTableContext /
/// parseTcInfo / buildTcRows that the plain fuzz cannot reach.
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
#include <utility>
#include <vector>

namespace {

// Which openable store an arena belongs to.
enum class Store {
    Openable,
    Foldered,
    Messaging
};

// One mutable region of a store: a [begin, begin + length) BODY span whose bytes the parser reads
// as structure, plus how to re-stamp the trailer that authenticates it. Mutating only within body
// and re-stamping keeps the whole file integral, so the parser accepts it and walks the structure.
struct Arena {
    Store store;
    int body_begin;
    int body_length;
    int region_offset;   // page or block offset
    bool is_block;       // true -> stampBlockTrailer(region_offset, block_cb, block_bid)
    int block_cb;        // block data length (is_block only)
    uint64_t block_bid;  // block BID (is_block only)
};

// Arenas for the openable store (3) plus, for each TC store (foldered + messaging, which share an
// identical three-block layout and differ only in node type), five more: NBT page, BBT page, root
// PC, single-row TC block, leaf PC. Built once in a loop so the shared layout is not duplicated.
const std::vector<Arena>& arenas() {
    namespace pf = sak::pst_fixture;
    static const std::vector<Arena> kArenas = [] {
        std::vector<Arena> v;
        v.push_back({Store::Openable,
                     pf::kOpenableNbtOffset,
                     pf::kLeafPageBodyLen,
                     pf::kOpenableNbtOffset,
                     false,
                     0,
                     0});
        v.push_back({Store::Openable,
                     pf::kOpenableBbtOffset,
                     pf::kLeafPageBodyLen,
                     pf::kOpenableBbtOffset,
                     false,
                     0,
                     0});
        v.push_back({Store::Openable,
                     pf::kOpenableBlockOffset,
                     pf::kRootBlockCb,
                     pf::kOpenableBlockOffset,
                     true,
                     pf::kRootBlockCb,
                     pf::kRootFolderDataBid});
        for (const Store store : {Store::Foldered, Store::Messaging}) {
            v.push_back({store,
                         pf::kOpenableNbtOffset,
                         pf::kLeafPageBodyLen,
                         pf::kOpenableNbtOffset,
                         false,
                         0,
                         0});
            v.push_back({store,
                         pf::kOpenableBbtOffset,
                         pf::kLeafPageBodyLen,
                         pf::kOpenableBbtOffset,
                         false,
                         0,
                         0});
            v.push_back({store,
                         pf::kFolderedRootBlockOffset,
                         pf::kRootBlockCb,
                         pf::kFolderedRootBlockOffset,
                         true,
                         pf::kRootBlockCb,
                         pf::kRootFolderDataBid});
            v.push_back({store,
                         pf::kFolderedTcBlockOffset,
                         pf::kTcBlockCb,
                         pf::kFolderedTcBlockOffset,
                         true,
                         pf::kTcBlockCb,
                         pf::kHierarchyDataBid});
            v.push_back({store,
                         pf::kFolderedChildBlockOffset,
                         pf::kRootBlockCb,
                         pf::kFolderedChildBlockOffset,
                         true,
                         pf::kRootBlockCb,
                         pf::kChildFolderDataBid});
        }
        return v;
    }();
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

QByteArray buildStore(Store store) {
    switch (store) {
    case Store::Foldered:
        return sak::pst_fixture::buildFolderedUnicodeStore();
    case Store::Messaging:
        return sak::pst_fixture::buildMessagingUnicodeStore();
    default:
        return sak::pst_fixture::buildOpenableUnicodeStore();
    }
}

// Produce one integral-but-structurally-corrupt mutant: mutate one region's body, re-stamp it.
QByteArray makeStructuralMutant(sak::fuzz::Prng& prng) {
    const Arena& arena = arenas()[prng.below(static_cast<uint32_t>(arenas().size()))];
    QByteArray file = buildStore(arena.store);
    const uint32_t rounds = 1u + prng.below(kMaxStructMutations);
    for (uint32_t i = 0; i < rounds; ++i) {
        mutateOneByteInArena(file, arena.body_begin, arena.body_length, prng);
    }
    if (arena.is_block) {
        sak::pst_fixture::stampBlockTrailer(file,
                                            arena.region_offset,
                                            arena.block_cb,
                                            arena.block_bid,
                                            static_cast<uint64_t>(arena.region_offset));
    } else {
        sak::pst_fixture::restampLeafPageTrailer(file, arena.region_offset);
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
        static_cast<void>(parser.readFolderItems(nid, 0, kNodeWalkBudget));
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
    // Both unmutated seeds must open (guards the fixtures + re-stamp helpers), then every
    // structural mutant of either must be handled without a fault or a hang.
    void structuralMutantsNeverCrash() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("fuzz_struct.pst"));

        for (const auto& [seed, label] :
             {std::pair{sak::pst_fixture::buildOpenableUnicodeStore(), "openable"},
              std::pair{sak::pst_fixture::buildFolderedUnicodeStore(), "foldered"},
              std::pair{sak::pst_fixture::buildMessagingUnicodeStore(), "messaging"}}) {
            QVERIFY(writeWholeFile(path, seed));
            PstParser seed_parser;
            seed_parser.open(path);
            QVERIFY2(seed_parser.isOpen(), label);
            seed_parser.close();
        }

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
