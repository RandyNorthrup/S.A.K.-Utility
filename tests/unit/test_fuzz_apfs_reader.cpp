// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_apfs_reader.cpp
/// @brief Mutation-fuzz of the read-only APFS container reader (G14-7).
///
/// PartitionApfsFileSystemReader walks an attacker-supplied APFS container: the nx_superblock, the
/// checkpoint descriptor/data rings, the container and volume object maps, the volume superblock,
/// and the catalog (file-system) B-tree -- every field of which is untrusted. A malformed or
/// hostile container must be rejected fail-closed and must never crash, hang, read out of bounds,
/// or return an unbounded listing.
///
/// An APFS container has a hard 64 MiB floor, far too large to copy-and-mutate whole-buffer per
/// iteration. Instead this harness generates ONE genuine walkable container with the real APFS
/// writer, keeps it on disk, and per input overlays a mutated copy of only the front window (the
/// nx_superblock + checkpoint + object-map region) onto the container's head, leaving the valid
/// tail intact. Every mutant is therefore a real container with a corrupted head -- exactly the
/// bytes the container-level parser trusts first. For EVERY input it asserts:
///
///   1. No crash and no hang (a fault never returns the empty string; a hang trips the ctest
///      timeout with the seed recorded in fuzz_harness.h).
///   2. Fail-closed carries a reason: a not-ok result always names at least one blocker
///      ([[no-fallbacks-fail-closed]]).
///   3. A successful listing is bounded: it never exceeds the requested entry cap.
///   4. A file read is bounded: the returned data never exceeds the caller's byte cap.
///
/// The unmutated window overlays onto itself (identity), so the accept path -- a fully valid
/// container listing root.txt -- is exercised too and pinned by a separate lock-in test.

#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_apfs_writer.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <vector>

namespace {

constexpr int kListEntryCap = 256;
constexpr uint64_t kFileReadCap = 4096;
constexpr qsizetype kHeaderWindowBytes = 2 * 1024 *
                                         1024;  // nx_superblock + checkpoint + omap front

// The options the image-only APFS writer needs to synthesize a fixture container (mirrors the
// certified image-only options used by the partition writer tests).
sak::PartitionApfsWriteOptions imageOnlyWriterOptions() {
    sak::PartitionApfsWriteOptions options;
    options.enable_experimental_writer = true;
    options.destructive_certification_evidence = true;
    options.allow_encrypted_or_protected_volume = true;
    options.allow_compressed_file_mutation = true;
    options.allow_snapshots = true;
    options.allow_multi_volume_container = true;
    options.max_payload_bytes = 64ULL * 1024ULL;
    options.evidence_id = QStringLiteral("external.apfs-fuzz-fixture");
    return options;
}

// Format a minimal container and commit one root file, so the reader has a walkable tree. Returns
// the path to the walkable container, or an empty string on failure.
QString buildWalkableContainer(const QDir& dir) {
    const sak::PartitionApfsWriteOptions options = imageOnlyWriterOptions();
    const QString base = dir.filePath(QStringLiteral("base.apfs"));
    const auto formatted = sak::PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = base,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("FUZZ"),
         .options = options});
    if (!formatted.ok) {
        return {};
    }
    const QString walkable = dir.filePath(QStringLiteral("work.apfs"));
    const auto committed = sak::PartitionApfsWriter::commitImageOnlyFileWrite(
        {.source_image_path = base,
         .written_image_path = walkable,
         .file_name = QStringLiteral("root.txt"),
         .file_data = QByteArrayLiteral("root me"),
         .options = options});
    if (!committed.ok) {
        return {};
    }
    return walkable;
}

// Overlay @p mutant onto a pristine copy of @p baseWindow and write it over the head of @p path,
// leaving the container's valid tail intact. Deterministic: the head is rebuilt from baseWindow
// every time, so mutations never accumulate across iterations.
bool overlayHeadWindow(const QString& path,
                       const QByteArray& baseWindow,
                       const QByteArray& mutant) {
    const qsizetype n = std::min(mutant.size(), baseWindow.size());
    const QByteArray head = mutant.left(n) + baseWindow.mid(n);
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return false;
    }
    const bool wrote = file.write(head) == head.size();
    file.close();
    return wrote;
}

QString checkListing(const sak::PartitionApfsFileReadResult& listing, const QString& where) {
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

QString checkEntry(const QString& path, const sak::PartitionApfsFileEntry& entry) {
    if (entry.directory) {
        const auto nested = sak::PartitionApfsFileSystemReader::listDirectoryFromImage(
            path, entry.path, kListEntryCap);
        return checkListing(nested, QStringLiteral("nested"));
    }
    if (!entry.regular_file) {
        return {};
    }
    const auto file =
        sak::PartitionApfsFileSystemReader::readFileFromImage(path, entry.path, kFileReadCap);
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
    return {};
}

// Overlay the mutated head onto @p workPath, then drive the reader over the container.
QString apfsReaderInvariant(const QByteArray& mutant,
                            const QString& workPath,
                            const QByteArray& baseWindow) {
    if (!overlayHeadWindow(workPath, baseWindow, mutant)) {
        return QStringLiteral("could not stage fuzz input to the container head");
    }
    const auto root = sak::PartitionApfsFileSystemReader::listDirectoryFromImage(
        workPath, QStringLiteral("/"), kListEntryCap);
    if (const QString detail = checkListing(root, QStringLiteral("root")); !detail.isEmpty()) {
        return detail;
    }
    if (!root.ok) {
        return {};
    }
    for (const auto& entry : root.entries) {
        if (const QString detail = checkEntry(workPath, entry); !detail.isEmpty()) {
            return detail;
        }
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("apfs reader fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class ApfsReaderFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void readerNeverCrashesAndStaysFailClosedOnAnyBytes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString workPath = buildWalkableContainer(QDir(dir.path()));
        QVERIFY2(!workPath.isEmpty(), "APFS writer could not synthesize the fixture container");

        QByteArray baseWindow;
        {
            QFile file(workPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            baseWindow = file.read(kHeaderWindowBytes);
        }
        QVERIFY(baseWindow.size() == kHeaderWindowBytes);

        const std::vector<QByteArray> corpus{
            baseWindow,
            QByteArray(kHeaderWindowBytes, '\0'),
            QByteArray(kHeaderWindowBytes, '\xFF'),
            QByteArray(4096, '\0') + baseWindow.mid(4096),  // nx_superblock block zeroed
        };
        const sak::fuzz::Target target = [&workPath, &baseWindow](const QByteArray& input) {
            return apfsReaderInvariant(input, workPath, baseWindow);
        };
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // On the all-pass path (guaranteed here: any failure QVERIFY2(false)-returns above),
        // run() increments iterations_run once per seed (checkSeeds) plus once per mutation
        // iteration, so the exact count is corpus.size() + the iteration budget. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }

    // The writer-generated container must actually open and list its root file, or the fuzz above
    // would never reach the reader's deep parsing -- this pins the accept path.
    void writerContainerListsRootFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString workPath = buildWalkableContainer(QDir(dir.path()));
        QVERIFY2(!workPath.isEmpty(), "APFS writer could not synthesize the fixture container");

        const auto root = sak::PartitionApfsFileSystemReader::listDirectoryFromImage(
            workPath, QStringLiteral("/"), kListEntryCap);
        QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
        // Exactly one file was committed (root.txt, "root me" = 7 bytes), so pin the whole
        // listing. A membership-only contains() would still pass on a duplicate root.txt or an
        // inflated listing -- the count-inflation corruption this accept-path lock-in guards.
        QCOMPARE(root.entries.size(), 1);
        QCOMPARE(root.entries.first().name, QStringLiteral("root.txt"));
        QVERIFY(root.entries.first().regular_file);
        QCOMPARE(root.entries.first().size_bytes, static_cast<uint64_t>(7));
    }
};

QTEST_GUILESS_MAIN(ApfsReaderFuzzTests)
#include "test_fuzz_apfs_reader.moc"
