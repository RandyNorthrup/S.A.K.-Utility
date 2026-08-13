// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_ext_reader.cpp
/// @brief Mutation-fuzz of the read-only ext2/ext3/ext4 file browser (G14-9).
///
/// PartitionExtFileSystemReader walks an attacker-supplied disk image: superblock, group
/// descriptor, inode table, directory blocks, and (for ext4) extent trees -- every field of which
/// is untrusted. A malformed or hostile image must be rejected fail-closed and must never crash,
/// hang, read out of bounds, or return an unbounded listing. This harness drives the real
/// listDirectoryFromImage() / readFileFromImage() entry points over thousands of mutated images
/// and asserts, for EVERY input:
///
///   1. No crash and no hang (a fault never returns the empty string; a hang trips the ctest
///      timeout with the seed recorded in fuzz_harness.h).
///   2. Fail-closed carries a reason: a not-ok result always names at least one blocker, so a
///      rejection can never masquerade as an empty-but-successful listing
///      ([[no-fallbacks-fail-closed]]).
///   3. A successful listing is bounded: it never returns more than the requested entry cap, even
///      when the directory records are corrupted to claim more.
///   4. A file read is bounded: the returned data never exceeds the caller's byte cap.
///
/// The seed corpus carries the genuine walkable image from tests/support/ext_fixture.h (both the
/// direct-block and the ext4 extent-mapped variant) so mutations reach the accept path, plus
/// zero/0xFF/truncated/magic-only images so they reach the superblock and sizing rejections.

#include "sak/partition_ext_file_system_reader.h"

#include "../fuzz/fuzz_harness.h"
#include "../support/ext_fixture.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <vector>

namespace {

constexpr int kListEntryCap = 256;       // bounded listing invariant checks against this
constexpr uint64_t kFileReadCap = 4096;  // bounded read invariant checks against this
constexpr int kMagicOnlyImageBytes = 2048;

std::vector<QByteArray> extReaderCorpus() {
    QByteArray magic_only(kMagicOnlyImageBytes, '\0');
    sak::testfixtures::writeLe16(&magic_only,
                                 sak::testfixtures::ext::kTestExtSuperblockOffset +
                                     sak::testfixtures::ext::kTestExtMagicOffset,
                                 sak::testfixtures::ext::kTestExtMagic);

    const QByteArray accept = sak::testfixtures::ext::extReaderFixture();
    return {
        QByteArray(),
        QByteArray(kMagicOnlyImageBytes, '\0'),          // no magic -> superblock rejection
        QByteArray(kMagicOnlyImageBytes, '\xFF'),        // all ones
        magic_only,                                      // magic set, everything else invalid
        accept,                                          // direct-block accept path
        sak::testfixtures::ext::extReaderFixture(true),  // ext4 extent-mapped accept path
        accept.left(accept.size() / 2),                  // truncated but magic present
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

// Check the four invariants for a single directory listing. Returns "" on success.
QString checkListing(const sak::PartitionExtFileReadResult& listing, const char* where) {
    if (!listing.ok && listing.blockers.isEmpty()) {
        return QStringLiteral("%1: listing failed with no blocker (not fail-closed)")
            .arg(QString::fromLatin1(where));
    }
    if (listing.entries.size() > kListEntryCap) {
        return QStringLiteral("%1: listing returned %2 entries, past the cap %3")
            .arg(QString::fromLatin1(where))
            .arg(listing.entries.size())
            .arg(kListEntryCap);
    }
    return {};
}

// Drive the ext reader over @p input written into @p dir; return "" if every invariant held.
QString extReaderInvariant(const QByteArray& input, const QDir& dir) {
    const QString path = dir.filePath(QStringLiteral("fuzz.ext.img"));
    if (!writeWholeFile(path, input)) {
        return QStringLiteral("could not stage fuzz input to a temp file");
    }

    const auto root =
        sak::PartitionExtFileSystemReader::listDirectoryFromImage(path, QString(), kListEntryCap);
    if (const QString detail = checkListing(root, "root"); !detail.isEmpty()) {
        return detail;
    }

    // Walk a nested path and a not-a-directory path to exercise resolvePath's traversal branches.
    const auto nested = sak::PartitionExtFileSystemReader::listDirectoryFromImage(
        path, QStringLiteral("/docs"), kListEntryCap);
    if (const QString detail = checkListing(nested, "nested"); !detail.isEmpty()) {
        return detail;
    }

    if (root.ok) {
        for (const auto& entry : root.entries) {
            if (!entry.regular_file) {
                continue;
            }
            const auto file = sak::PartitionExtFileSystemReader::readFileFromImage(path,
                                                                                   entry.path,
                                                                                   kFileReadCap);
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
            break;  // one regular-file read per input is enough to exercise the path
        }
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("ext reader fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class ExtReaderFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void readerNeverCrashesAndStaysFailClosedOnAnyBytes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());

        const sak::fuzz::Target target = [&out](const QByteArray& input) {
            return extReaderInvariant(input, out);
        };
        const std::vector<QByteArray> corpus = extReaderCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }

    // The shared accept-path fixture must actually open, or the fuzz above would never reach the
    // reader's deep parsing -- this pins the seed so a fixture regression is caught here, not
    // silently as reduced coverage.
    void sharedFixtureOpensAndListsBothVariants() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());
        for (bool extentMapped : {false, true}) {
            const QString path = out.filePath(extentMapped ? QStringLiteral("extent.img")
                                                           : QStringLiteral("direct.img"));
            QVERIFY(writeWholeFile(path, sak::testfixtures::ext::extReaderFixture(extentMapped)));
            const auto root = sak::PartitionExtFileSystemReader::listDirectoryFromImage(
                path, QString(), kListEntryCap);
            QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
            QVERIFY(!root.entries.isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(ExtReaderFuzzTests)
#include "test_fuzz_ext_reader.moc"
