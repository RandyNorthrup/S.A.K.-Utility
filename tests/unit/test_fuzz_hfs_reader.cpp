// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_hfs_reader.cpp
/// @brief Mutation-fuzz of the read-only HFS+ file browser (G14-8).
///
/// PartitionHfsFileSystemReader walks an attacker-supplied HFS+ image: the volume header, the
/// allocation fork, the catalog B-tree (header node, index/leaf nodes, variable-length keyed
/// records) and each file's fork extents -- every field of which is untrusted. A malformed or
/// hostile image must be rejected fail-closed and must never crash, hang, read out of bounds, or
/// return an unbounded listing. This harness drives the real listDirectoryFromImage() /
/// readFileFromImage() entry points over thousands of mutated images and asserts, for EVERY input:
///
///   1. No crash and no hang (a fault never returns the empty string; a hang trips the ctest
///      timeout with the seed recorded in fuzz_harness.h).
///   2. Fail-closed carries a reason: a not-ok result always names at least one blocker, so a
///      rejection can never masquerade as an empty-but-successful listing
///      ([[no-fallbacks-fail-closed]]).
///   3. A successful listing is bounded: it never returns more than the requested entry cap, even
///      when the B-tree node records are corrupted to claim more.
///   4. A file read is bounded: the returned data never exceeds the caller's byte cap.
///
/// The seed corpus reuses the genuine walkable image from tests/support/hfs_fixture.h so mutations
/// reach the deep catalog/fork parsing, plus zero/0xFF/magic-only/truncated images so they reach
/// the volume-header and B-tree sizing rejections.

#include "sak/partition_hfs_file_system_reader.h"

#include "../fuzz/fuzz_harness.h"
#include "../support/byte_writer.h"
#include "../support/hfs_fixture.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <vector>

namespace {

constexpr int kListEntryCap = 256;          // bounded listing invariant checks against this
constexpr uint64_t kFileReadCap = 4096;     // bounded read invariant checks against this
constexpr int kMagicOnlyImageBytes = 8192;  // holds the volume header at offset 1024

std::vector<QByteArray> hfsReaderCorpus() {
    QByteArray magic_only(kMagicOnlyImageBytes, '\0');
    sak::testfixtures::writeAscii(&magic_only, sak::testfixtures::hfs::kTestHfsHeaderOffset, "H+");

    const QByteArray accept = sak::testfixtures::hfs::hfsReaderFixture();
    return {
        QByteArray(),
        QByteArray(kMagicOnlyImageBytes, '\0'),    // no signature -> volume-header rejection
        QByteArray(kMagicOnlyImageBytes, '\xFF'),  // all ones
        magic_only,                                // "H+" set, everything else invalid
        accept,                                    // full accept path
        accept.left(accept.size() / 2),            // truncated but header present
    };
}

bool writeWholeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool wrote = file.write(bytes) == bytes.size();
    file.close();  // release the handle so the reader can open it read-only on Windows
    return wrote;
}

// Check the fail-closed and bounded-listing invariants for a single listing. Returns "" on success.
QString checkListing(const sak::PartitionHfsFileReadResult& listing, const QString& where) {
    if (!listing.ok && listing.blockers.isEmpty()) {
        return where + QStringLiteral(": listing failed with no blocker (not fail-closed)");
    }
    if (listing.entries.size() > kListEntryCap) {
        return where + QStringLiteral(": listing returned %1 entries, past the cap %2")
                           .arg(listing.entries.size())
                           .arg(kListEntryCap);
    }
    return {};
}

// Check one directory entry: recurse into a subdirectory (bounded listing) or read a regular file
// (bounded read, fail-closed). Returns "" if its invariants held.
QString checkEntry(const QString& path, const sak::PartitionHfsFileEntry& entry) {
    if (entry.directory) {
        const auto nested = sak::PartitionHfsFileSystemReader::listDirectoryFromImage(
            path, entry.path, kListEntryCap);
        return checkListing(nested, QStringLiteral("nested"));
    }
    if (!entry.regular_file) {
        return {};
    }
    const auto file =
        sak::PartitionHfsFileSystemReader::readFileFromImage(path, entry.path, kFileReadCap);
    if (!file.ok && file.blockers.isEmpty()) {
        return QStringLiteral("readFile(%1) failed with no blocker (not fail-closed)")
            .arg(entry.path);
    }
    if (static_cast<uint64_t>(file.data.size()) > kFileReadCap) {
        return QStringLiteral("readFile(%1) returned %2 bytes, past the cap %3")
            .arg(entry.path)
            .arg(file.data.size())
            .arg(kFileReadCap);
    }
    // The reader never clamps: an over-cap fork is refused outright
    // (partition_hfs_internal.h:9126) and an ok read returns EXACTLY the fork's logical size
    // (readForkBytes loops to remaining==0 and readAt fails on a short read, :10444/:10464),
    // which the listing already reported as entry.size_bytes (:11851, :12032; a hard-link alias
    // is enriched from the same inode the read resolves, :12048 vs :9099). The ceiling alone
    // would stay green for a reader that silently truncated every hostile fork to the cap.
    // A listed size of 0 is exempt: a decmpfs record carries data_size 0 and returns
    // decompressed bytes (:8849).
    if (file.ok && entry.size_bytes != 0 &&
        static_cast<uint64_t>(file.data.size()) != entry.size_bytes) {
        return QStringLiteral("readFile(%1) returned %2 bytes, not the listed size %3")
            .arg(entry.path)
            .arg(file.data.size())
            .arg(entry.size_bytes);
    }
    return {};
}

// Drive the HFS reader over @p input written into @p dir; return "" if every invariant held.
QString hfsReaderInvariant(const QByteArray& input, const QDir& dir) {
    const QString path = dir.filePath(QStringLiteral("fuzz.hfs.img"));
    if (!writeWholeFile(path, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }

    const auto root =
        sak::PartitionHfsFileSystemReader::listDirectoryFromImage(path, QString(), kListEntryCap);
    if (const QString detail = checkListing(root, QStringLiteral("root")); !detail.isEmpty()) {
        return detail;
    }
    if (!root.ok) {
        return {};
    }

    // A bounded listing must also be an HONEST one: childRecords scans one past the cap
    // (partition_hfs_internal.h:11876) so an over-full directory is trimmed AND warned about
    // (:11908-11912) instead of being reported as a complete listing. checkListing only proves
    // the listing is not LARGER than the cap, which the trim guarantees unconditionally, and at
    // kListEntryCap=256 the cap path is never entered. Re-list the same directory at a cap of 1
    // whenever the full listing held more than one entry: the cap-1 scan visits a strict prefix
    // of the records/nodes the cap-256 scan already accepted, so it can never raise a new blocker
    // and can only fail where the full listing already failed (which returned above).
    // Match the EXACT truncation string -- warnings are already non-empty on the accept-path
    // fixture (load() appends a journal-replay warning, cf. test_partition_manager_core.cpp:3501),
    // so a mere !warnings.isEmpty() check would pass vacuously.
    if (root.entries.size() > 1) {
        const auto capped =
            sak::PartitionHfsFileSystemReader::listDirectoryFromImage(path, QString(), 1);
        if (!capped.ok) {
            return QStringLiteral("cap-1 listing failed where the cap-%1 listing succeeded: %2")
                .arg(kListEntryCap)
                .arg(capped.blockers.join(QStringLiteral("; ")));
        }
        if (capped.entries.size() != 1) {
            return QStringLiteral("cap-1 listing returned %1 entries, expected exactly 1")
                .arg(capped.entries.size());
        }
        if (!capped.warnings.contains(
                QStringLiteral("HFS+ directory listing truncated at the entry cap; some entries "
                               "were not listed"))) {
            return QStringLiteral(
                "cap-1 listing dropped entries but reported no truncation warning (a silently "
                "truncated listing masquerading as complete)");
        }
    }
    for (const auto& entry : root.entries) {
        if (const QString detail = checkEntry(path, entry); !detail.isEmpty()) {
            return detail;
        }
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("hfs reader fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class HfsReaderFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void readerNeverCrashesAndStaysFailClosedOnAnyBytes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());

        const sak::fuzz::Target target = [&out](const QByteArray& input) {
            return hfsReaderInvariant(input, out);
        };
        const std::vector<QByteArray> corpus = hfsReaderCorpus();
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

    // The shared accept-path fixture must actually open, or the fuzz above would never reach the
    // reader's deep parsing -- this pins the seed so a fixture regression is caught here.
    void sharedFixtureOpensAndListsRoot() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());
        const QString path = out.filePath(QStringLiteral("hfs.img"));
        QVERIFY(writeWholeFile(path, sak::testfixtures::hfs::hfsReaderFixture()));
        const auto root = sak::PartitionHfsFileSystemReader::listDirectoryFromImage(path,
                                                                                    QString(),
                                                                                    kListEntryCap);
        QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
        // The fixture catalog holds exactly two root children (parent CNID 2): the folder Docs
        // (id 16) and the file hello.txt (id 17, "hello from hfs\n" = 15 bytes); note.txt lives
        // under Docs (parent 16) and must not appear at root. !isEmpty() would still pass if a
        // regression dropped hello.txt, mis-ordered the pair, flipped a type flag, or reported a
        // wrong size -- pin the whole listing.
        QCOMPARE(root.entries.size(), static_cast<qsizetype>(2));
        QCOMPARE(root.entries[0].name, QStringLiteral("Docs"));
        QCOMPARE(root.entries[0].path, QStringLiteral("/Docs"));
        QCOMPARE(root.entries[0].type, QStringLiteral("Directory"));
        QVERIFY(root.entries[0].directory);
        QVERIFY(!root.entries[0].regular_file);
        QCOMPARE(root.entries[0].catalog_id, static_cast<uint32_t>(16));
        QCOMPARE(root.entries[1].name, QStringLiteral("hello.txt"));
        QCOMPARE(root.entries[1].path, QStringLiteral("/hello.txt"));
        QCOMPARE(root.entries[1].type, QStringLiteral("File"));
        QVERIFY(root.entries[1].regular_file);
        QVERIFY(!root.entries[1].directory);
        QCOMPARE(root.entries[1].catalog_id, static_cast<uint32_t>(17));
        QCOMPARE(root.entries[1].size_bytes, static_cast<uint64_t>(15));

        // Clamp-vs-refuse, pinned on the clean fixture: a cap at or above the file yields EXACTLY
        // the fork bytes, and a cap below it is REFUSED with a reason and no data -- a reader that
        // served a truncated prefix would satisfy the fuzz harness's "<= cap" ceiling forever.
        const QString hello_path = root.entries[1].path;
        const auto whole =
            sak::PartitionHfsFileSystemReader::readFileFromImage(path, hello_path, kFileReadCap);
        QVERIFY2(whole.ok, qPrintable(whole.blockers.join(QStringLiteral("; "))));
        QCOMPARE(whole.data, QByteArrayLiteral("hello from hfs\n"));
        QCOMPARE(static_cast<uint64_t>(whole.data.size()), root.entries[1].size_bytes);
        const auto capped = sak::PartitionHfsFileSystemReader::readFileFromImage(
            path, hello_path, static_cast<uint64_t>(4));
        QVERIFY(!capped.ok);
        QVERIFY(capped.data.isEmpty());
        QVERIFY(capped.blockers.join(QStringLiteral(" ")).contains(QStringLiteral("read cap")));
    }
};

QTEST_GUILESS_MAIN(HfsReaderFuzzTests)
#include "test_fuzz_hfs_reader.moc"
