// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_management_file_system.cpp
/// @brief Unit tests for File Management file-system target bridge.

#include "sak/file_management_file_system.h"
#include "sak/partition_apfs_writer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class FileManagementFileSystemTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void localTargetListsAndReadsFiles() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile file(QDir(temp.path()).filePath(QStringLiteral("note.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("hello target bridge");
        file.close();

        const auto target = sak::FileManagementFileSystemBridge::localTarget(temp.path());
        QVERIFY(target.local_file_system);
        QVERIFY(target.can_organize);

        const auto listing =
            sak::FileManagementFileSystemBridge::listDirectory(target, temp.path(), 100);
        QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
        QCOMPARE(listing.entries.size(), 1);
        QCOMPARE(listing.entries.first().name, QStringLiteral("note.txt"));

        const auto read = sak::FileManagementFileSystemBridge::readFile(
            target, listing.entries.first().path, 1024);
        QVERIFY(read.ok);
        QCOMPARE(QString::fromUtf8(read.data), QStringLiteral("hello target bridge"));
    }

    void readWasCapped_trustsTheReaderNotOnlyTheByteCount() {
        // R5 CRITICAL. The APFS reader clamps every request to its own 512 MiB ceiling, and the
        // callers' cap is the same 512 MiB. So a 700 MB file comes back as EXACTLY max_bytes
        // with the reader's truncated flag set, and the byte-count test -- bytes_read >
        // max_bytes -- is false for it. That flag used to be dropped on the way through
        // FileManagementReadResult, so a hash reported itself uncapped over a prefix and an
        // export reported complete; the move path then deleted the intact 700 MB source.
        using B = sak::FileManagementFileSystemBridge;
        constexpr uint64_t cap = 512ULL * 1024 * 1024;

        // The exact shape of the bug: cut at precisely the cap, invisible to the count.
        QVERIFY2(B::readWasCapped(true, cap, cap),
                 "a reader-truncated read at exactly the cap must count as capped");

        // The caller's own probe still works: one byte past the cap.
        QVERIFY(B::readWasCapped(false, cap + 1, cap));

        // A genuinely complete file at exactly the cap is NOT capped -- that is the case the
        // one-extra-byte probe exists to distinguish, and it must not become a false positive.
        QVERIFY(!B::readWasCapped(false, cap, cap));
        QVERIFY(!B::readWasCapped(false, 10, cap));

        // A 0 cap means "no limit", so only the reader can report a short read.
        QVERIFY(!B::readWasCapped(false, cap + 1, 0));
        QVERIFY(B::readWasCapped(true, 10, 0));
    }

    void readFileWithMaxUint64CapReadsWholeFile() {
        // B8-18: a UINT64_MAX cap must not wrap (max_bytes + 1 -> 0) and read nothing;
        // it must read the whole file. A 0 cap ("no limit") reads all too.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString filePath = QDir(temp.path()).filePath(QStringLiteral("whole.bin"));
        const QByteArray payload(4096, 'Z');
        {
            QFile file(filePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), static_cast<qint64>(payload.size()));
        }
        const auto target = sak::FileManagementFileSystemBridge::localTarget(temp.path());

        const auto uncapped = sak::FileManagementFileSystemBridge::readFile(
            target, filePath, std::numeric_limits<uint64_t>::max());
        QVERIFY2(uncapped.ok, qPrintable(uncapped.blockers.join(QStringLiteral("; "))));
        QCOMPARE(uncapped.data, payload);  // whole file, not empty

        const auto no_cap = sak::FileManagementFileSystemBridge::readFile(target, filePath, 0);
        QVERIFY(no_cap.ok);
        QCOMPARE(no_cap.data, payload);
    }

    void readFileRejectsFilesOverMaxBytes() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString filePath = QDir(temp.path()).filePath(QStringLiteral("big.bin"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(100, 'x'));
        file.close();

        const auto target = sak::FileManagementFileSystemBridge::localTarget(temp.path());

        // Over the cap: must be rejected, not silently truncated to the limit.
        const auto capped = sak::FileManagementFileSystemBridge::readFile(target, filePath, 10);
        QVERIFY(!capped.ok);
        QVERIFY(!capped.blockers.isEmpty());

        // Within the cap: full content is returned.
        const auto full = sak::FileManagementFileSystemBridge::readFile(target, filePath, 1024);
        QVERIFY(full.ok);
        QCOMPARE(full.data.size(), 100);
    }

    void writeFileLocalAtomicRoundTrip() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString filePath = QDir(temp.path()).filePath(QStringLiteral("data.bin"));
        const auto target = sak::FileManagementFileSystemBridge::localTarget(temp.path());

        const QByteArray first = QByteArrayLiteral("original contents");
        const auto write1 = sak::FileManagementFileSystemBridge::writeFile(target, filePath, first);
        QVERIFY2(write1.ok, qPrintable(write1.blockers.join(QStringLiteral("; "))));
        QCOMPARE(write1.bytes_written, static_cast<uint64_t>(first.size()));

        // Overwrite replaces the contents wholesale (atomic commit).
        const QByteArray second = QByteArrayLiteral("new");
        const auto write2 =
            sak::FileManagementFileSystemBridge::writeFile(target, filePath, second);
        QVERIFY2(write2.ok, qPrintable(write2.blockers.join(QStringLiteral("; "))));

        const auto read = sak::FileManagementFileSystemBridge::readFile(target, filePath, 1024);
        QVERIFY(read.ok);
        QCOMPARE(read.data, second);
    }

    void writeFileFromHostPathStreamsLocalCopyWithNoCap() {
        // writeFileFromHostPath streams a host file into the destination through a fixed
        // window (peak RAM one window), so the copy is byte-exact and has no size cap. The
        // payload spans several 1 MiB windows and is not window-aligned.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir dir(temp.path());
        const QString srcPath = dir.filePath(QStringLiteral("src.bin"));
        QByteArray payload(3'000'000, Qt::Uninitialized);
        for (qsizetype i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>((i * 91 + 13) & 0xFF);
        }
        {
            QFile sf(srcPath);
            QVERIFY(sf.open(QIODevice::WriteOnly));
            QCOMPARE(sf.write(payload), static_cast<qint64>(payload.size()));
        }
        const QString destPath = dir.filePath(QStringLiteral("dest.bin"));
        const auto target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        const auto result =
            sak::FileManagementFileSystemBridge::writeFileFromHostPath(target, destPath, srcPath);
        QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
        QCOMPARE(result.bytes_written, static_cast<uint64_t>(payload.size()));
        QFile df(destPath);
        QVERIFY(df.open(QIODevice::ReadOnly));
        QCOMPARE(df.readAll(), payload);
    }

    void transferObserverStreamsByteDeltasAndCancels() {
        // The transfer observer sees every 1 MiB window land and its cancel
        // poll aborts between windows with the canceled blocker and no
        // partial destination left behind (status-center progress plumbing).
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir dir(temp.path());
        const QString srcPath = dir.filePath(QStringLiteral("src.bin"));
        const QByteArray payload(3'000'000, 'x');
        {
            QFile sf(srcPath);
            QVERIFY(sf.open(QIODevice::WriteOnly));
            QCOMPARE(sf.write(payload), static_cast<qint64>(payload.size()));
        }
        const auto target = sak::FileManagementFileSystemBridge::localTarget(dir.path());

        qint64 seen = 0;
        int windows = 0;
        const sak::FileManagementTransferObserver counting{
            .on_bytes =
                [&seen, &windows](const qint64 delta) {
                    seen += delta;
                    ++windows;
                },
            .cancelled = {},
        };
        const QString destPath = dir.filePath(QStringLiteral("dest.bin"));
        const auto written = sak::FileManagementFileSystemBridge::writeFileFromHostPath(
            target, destPath, srcPath, counting);
        QVERIFY2(written.ok, qPrintable(written.blockers.join(QStringLiteral("; "))));
        QCOMPARE(seen, static_cast<qint64>(payload.size()));
        QVERIFY(windows >= 3);

        // Cancel before the first window: blocked with the canceled marker,
        // and the partial destination is removed.
        const sak::FileManagementTransferObserver canceling{
            .on_bytes = {},
            .cancelled = []() { return true; },
        };
        const QString canceledPath = dir.filePath(QStringLiteral("canceled.bin"));
        const auto canceled = sak::FileManagementFileSystemBridge::writeFileFromHostPath(
            target, canceledPath, srcPath, canceling);
        QVERIFY(!canceled.ok);
        QVERIFY(canceled.blockers.contains(sak::kFileManagementTransferCancelledBlocker));
        QVERIFY(!QFile::exists(canceledPath));

        // copyFileToHost honors the same observer; QSaveFile discards the
        // canceled temporary so no destination appears.
        qint64 out_seen = 0;
        const sak::FileManagementTransferObserver out_counting{
            .on_bytes = [&out_seen](const qint64 delta) { out_seen += delta; },
            .cancelled = {},
        };
        const QString outPath = dir.filePath(QStringLiteral("out.bin"));
        const auto exported = sak::FileManagementFileSystemBridge::copyFileToHost(
            target, srcPath, outPath, 0, out_counting);
        QVERIFY2(exported.ok, qPrintable(exported.blockers.join(QStringLiteral("; "))));
        QCOMPARE(out_seen, static_cast<qint64>(payload.size()));
        const QString outCanceled = dir.filePath(QStringLiteral("out-canceled.bin"));
        const auto export_canceled = sak::FileManagementFileSystemBridge::copyFileToHost(
            target, srcPath, outCanceled, 0, canceling);
        QVERIFY(!export_canceled.ok);
        QVERIFY(export_canceled.blockers.contains(sak::kFileManagementTransferCancelledBlocker));
        QVERIFY(!QFile::exists(outCanceled));
    }

    void manualApfsTargetIsReadOnlySearchableButNotOrganizable() {
        const auto target = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("C:/fixtures/apfs.img"), QStringLiteral("APFS"));
        QVERIFY(!target.local_file_system);
        QVERIFY(target.read_only);
        QVERIFY(target.can_browse);
        QVERIFY(target.can_duplicate_scan);
        QVERIFY(target.can_advanced_search);
        QVERIFY(!target.can_organize);
        QVERIFY(!target.blockers.isEmpty());
    }

    void rawDevicePathClassificationIsCaseInsensitive() {
        // Windows device paths are case-insensitive: a lowercase "globalroot" names the same
        // raw partition and must be classified identically, not misread as an image file.
        const uint64_t size = 128ULL * 1024ULL * 1024ULL;
        const auto upper = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2"),
            QStringLiteral("APFS"),
            size);
        const auto lower = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\globalroot\\device\\harddisk4\\partition2"),
            QStringLiteral("APFS"),
            size);
        QCOMPARE(lower.kind, upper.kind);
        QCOMPARE(lower.can_write_files, upper.can_write_files);
        QVERIFY(lower.can_write_files);
    }

    void apfsRawWritesSpanCertifiedMultiCibRange() {
        // A1/A2: the in-place COW engine is Apple-certified across the single-CIB,
        // multi-CIB, metadata-overflow, and CAB tiers, so the File Explorer write gate
        // must accept generated containers from 64 MiB through the 24 TiB ceiling.
        const auto singleChunk = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2"),
            QStringLiteral("APFS"),
            128ULL * 1024ULL * 1024ULL);
        QVERIFY(singleChunk.can_browse);
        QVERIFY(singleChunk.can_write_files);

        // Multi-CIB / metadata-overflow size that the pre-fix 128 MiB cap wrongly blocked.
        const auto multiCib = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition4"),
            QStringLiteral("APFS"),
            4ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
        QVERIFY(multiCib.can_write_files);

        // Below the 64 MiB floor remains read-only.
        const auto tooSmall = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition5"),
            QStringLiteral("APFS"),
            32ULL * 1024ULL * 1024ULL);
        QVERIFY(!tooSmall.can_write_files);

        // Above the 24 TiB ceiling remains fail-closed with the certified-range blocker.
        const auto oversized = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition3"),
            QStringLiteral("APFS"),
            32ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
        QVERIFY(oversized.can_browse);
        QVERIFY(!oversized.can_write_files);
        QVERIFY(oversized.blockers.join(' ').contains(QStringLiteral("24 TiB")));
    }

    void apfsNestedDirectoryDeletePassesParentPath() {
        // The bridge now sends a nested directory delete to the COW engine with the leaf name
        // ("sub") plus the parent path ("/docs") -- not the root ancestor as the delete name.
        // A manual (unopened) target reaches the engine, which fails closed at the device layer
        // (this fixture points at a non-existent device), NOT with the old "nested ... not yet
        // supported" bridge guard. Absence of that guard blocker proves the path is threaded.
        const auto target = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2"),
            QStringLiteral("APFS"),
            128ULL * 1024ULL * 1024ULL);
        QVERIFY(target.can_write_files);
        const auto result = sak::FileManagementFileSystemBridge::deleteDirectory(
            target, QStringLiteral("/docs/sub"));
        QVERIFY(!result.ok);  // the fixture device cannot be opened
        QVERIFY2(!result.blockers.join(QStringLiteral(" "))
                      .contains(QStringLiteral("nested directory delete is not yet supported")),
                 qPrintable(result.blockers.join(QStringLiteral("; "))));
    }

    void renderPreviewDecodesTextAndDumpsBinary() {
        // Text bytes decode verbatim, are not flagged binary, and carry the caller's
        // truncation flag through.
        const auto text = sak::FileManagementFileSystemBridge::renderPreview(
            QByteArrayLiteral("line one\nline two\n"), false);
        QVERIFY(!text.is_binary);
        QVERIFY(!text.truncated);
        QCOMPARE(text.text, QStringLiteral("line one\nline two\n"));
        QCOMPARE(text.shown_bytes, static_cast<uint64_t>(18));

        const auto truncatedText =
            sak::FileManagementFileSystemBridge::renderPreview(QByteArrayLiteral("partial"), true);
        QVERIFY(!truncatedText.is_binary);
        QVERIFY(truncatedText.truncated);

        // A NUL byte forces the hex+ASCII dump path; the dump carries the offset column,
        // the hex for the leading byte, and the printable ASCII gutter.
        QByteArray binary("AB\x00\x01Z", 5);
        const auto dump = sak::FileManagementFileSystemBridge::renderPreview(binary, false);
        QVERIFY(dump.is_binary);
        QVERIFY2(dump.text.contains(QStringLiteral("00000000")), qPrintable(dump.text));
        QVERIFY2(dump.text.contains(QStringLiteral("41 42")), qPrintable(dump.text));
        QVERIFY2(dump.text.contains(QStringLiteral("|AB..Z|")), qPrintable(dump.text));
    }

    void renderPreviewHexDumpCapsLargeBinary() {
        // A binary payload larger than the hex-dump window is capped and marked truncated,
        // and shown_bytes reflects only the dumped window.
        QByteArray big(9000, '\x00');
        const auto dump = sak::FileManagementFileSystemBridge::renderPreview(big, false);
        QVERIFY(dump.is_binary);
        QVERIFY(dump.truncated);
        QCOMPARE(dump.shown_bytes, static_cast<uint64_t>(4096));
    }

    void hashFileComputesSha256OfLocalFile() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray payload("hash this content through the bridge");
        QFile file(QDir(temp.path()).filePath(QStringLiteral("hash.bin")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(payload), static_cast<qint64>(payload.size()));
        file.close();

        const auto target = sak::FileManagementFileSystemBridge::localTarget(temp.path());
        const auto listing =
            sak::FileManagementFileSystemBridge::listDirectory(target, temp.path(), 10);
        QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));

        const auto result = sak::FileManagementFileSystemBridge::hashFile(
            target, listing.entries.first().path, 512ULL * 1024 * 1024);
        QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
        QVERIFY(!result.capped);
        QCOMPARE(result.hashed_bytes, static_cast<uint64_t>(payload.size()));

        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        QCOMPARE(result.sha256, expected);
    }

    void copyFileToHostCopiesLocalFileByteExactWithHash() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir dir(temp.path());
        const QString srcPath = dir.filePath(QStringLiteral("source.bin"));
        // Payload spans several copy windows and is not window-aligned.
        QByteArray payload(3'000'003, Qt::Uninitialized);
        for (qsizetype i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>((i * 37 + 5) & 0xFF);
        }
        {
            QFile sf(srcPath);
            QVERIFY(sf.open(QIODevice::WriteOnly));
            QCOMPARE(sf.write(payload), static_cast<qint64>(payload.size()));
        }

        const auto target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        const QString destPath = dir.filePath(QStringLiteral("exported.bin"));
        const auto result = sak::FileManagementFileSystemBridge::copyFileToHost(
            target, srcPath, destPath, 512ULL * 1024 * 1024);

        QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
        QVERIFY(!result.capped);
        QCOMPARE(result.bytes_written, static_cast<uint64_t>(payload.size()));

        QFile df(destPath);
        QVERIFY(df.open(QIODevice::ReadOnly));
        QCOMPARE(df.readAll(), payload);

        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        QCOMPARE(result.sha256, expected);
    }

    void copyFileToHostFailsClosedWhenSourceMissing() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir dir(temp.path());
        const auto target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        const QString destPath = dir.filePath(QStringLiteral("out.bin"));
        const auto result = sak::FileManagementFileSystemBridge::copyFileToHost(
            target, dir.filePath(QStringLiteral("nope.bin")), destPath, 1024);
        QVERIFY(!result.ok);
        QVERIFY(!result.blockers.isEmpty());
        // The destination is not left behind on failure.
        QVERIFY(!QFile::exists(destPath));
    }

    void copyFileToHostPreservesExistingDestinationOnFailure() {
        // Overwriting an existing file must be atomic: when the copy fails (missing
        // source), the pre-existing destination bytes stay untouched instead of being
        // truncated or deleted.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir dir(temp.path());
        const QString destPath = dir.filePath(QStringLiteral("keep.bin"));
        const QByteArray original = QByteArrayLiteral("precious original bytes");
        {
            QFile dest(destPath);
            QVERIFY(dest.open(QIODevice::WriteOnly));
            QCOMPARE(dest.write(original), original.size());
        }
        const auto target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        const auto result = sak::FileManagementFileSystemBridge::copyFileToHost(
            target, dir.filePath(QStringLiteral("nope.bin")), destPath, 1024);
        QVERIFY(!result.ok);
        QFile dest(destPath);
        QVERIFY(dest.open(QIODevice::ReadOnly));
        QCOMPARE(dest.readAll(), original);
    }

    void identifierLabelIsFileSystemSpecific() {
        using Bridge = sak::FileManagementFileSystemBridge;
        QCOMPARE(Bridge::identifierLabel(QStringLiteral("APFS")), QStringLiteral("Object ID"));
        QCOMPARE(Bridge::identifierLabel(QStringLiteral("HFS+")), QStringLiteral("Catalog ID"));
        QCOMPARE(Bridge::identifierLabel(QStringLiteral("hfsx")), QStringLiteral("Catalog ID"));
        QCOMPARE(Bridge::identifierLabel(QStringLiteral("ext4")), QStringLiteral("Inode"));
        QCOMPARE(Bridge::identifierLabel(QStringLiteral("NTFS")), QStringLiteral("Identifier"));
    }

    void safetyNotesNameTheRealBlocker() {
        using Bridge = sak::FileManagementFileSystemBridge;

        // A size-unknown APFS target explains the real gate: the certified engine needs a
        // known container size to be range-gated (a missing image reports size 0).
        const auto arbitraryApfs = Bridge::manualTarget(QStringLiteral("C:/fixtures/apfs.img"),
                                                        QStringLiteral("APFS"));
        QVERIFY(!arbitraryApfs.can_write_files);
        const QString apfsNote = Bridge::safetyNotes(arbitraryApfs).join(QStringLiteral(" "));
        QVERIFY2(apfsNote.contains(QStringLiteral("known container size")), qPrintable(apfsNote));
        QVERIFY2(apfsNote.contains(sak::apfsCapacityRangeText()), qPrintable(apfsNote));

        // A write-capable APFS slice states the certified-engine path and that both
        // S.A.K.-generated and real Apple-created (foreign) containers are supported.
        const auto writableApfs =
            Bridge::manualTarget(QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2"),
                                 QStringLiteral("APFS"),
                                 128ULL * 1024ULL * 1024ULL);
        QVERIFY(writableApfs.can_write_files);
        const QString writableNote = Bridge::safetyNotes(writableApfs).join(QStringLiteral(" "));
        QVERIFY2(writableNote.contains(QStringLiteral("COW engine")), qPrintable(writableNote));
        QVERIFY2(writableNote.contains(QStringLiteral("foreign")), qPrintable(writableNote));

        // XFS/Btrfs and ext each get their own specific note.
        QVERIFY(Bridge::safetyNotes(
                    Bridge::manualTarget(QStringLiteral("C:/x.img"), QStringLiteral("XFS")))
                    .join(QStringLiteral(" "))
                    .contains(QStringLiteral("metadata-only")));
        QVERIFY(Bridge::safetyNotes(
                    Bridge::manualTarget(QStringLiteral("C:/e.img"), QStringLiteral("ext4")))
                    .join(QStringLiteral(" "))
                    .contains(QStringLiteral("read-only browse/read/copy-out")));
    }

    void exportDirectoryToHostRecursesLocalTree() {
        // Recursive copy-out: a nested source tree exports byte-complete into the host
        // destination, with directories re-created and counts reported.
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        const QDir src(source.path());
        QVERIFY(src.mkpath(QStringLiteral("inner/deeper")));
        const auto writeFile = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        writeFile(src.filePath(QStringLiteral("top.txt")), QByteArrayLiteral("top-level"));
        writeFile(src.filePath(QStringLiteral("inner/mid.bin")),
                  QByteArrayLiteral("mid \x00\x01 bytes"));
        writeFile(src.filePath(QStringLiteral("inner/deeper/leaf.txt")),
                  QByteArrayLiteral("leaf content"));

        const auto target = sak::FileManagementFileSystemBridge::localTarget(source.path());
        const QString destRoot = QDir(destination.path()).filePath(QStringLiteral("exported"));
        const auto result = sak::FileManagementFileSystemBridge::exportDirectoryToHost(
            target, source.path(), destRoot, 0);
        QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
        QVERIFY(result.complete);
        QCOMPARE(result.files_exported, 3);
        QCOMPARE(result.directories_created, 2);
        QCOMPARE(result.capped_files, 0);

        const auto readAll = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return QByteArray();
            }
            return file.readAll();
        };
        QCOMPARE(readAll(QDir(destRoot).filePath(QStringLiteral("top.txt"))),
                 QByteArrayLiteral("top-level"));
        QCOMPARE(readAll(QDir(destRoot).filePath(QStringLiteral("inner/mid.bin"))),
                 QByteArrayLiteral("mid \x00\x01 bytes"));
        QCOMPARE(readAll(QDir(destRoot).filePath(QStringLiteral("inner/deeper/leaf.txt"))),
                 QByteArrayLiteral("leaf content"));
    }

    void importDirectoryFromHostRecursesLocalTree() {
        // Recursive import (paste-in direction): a nested host tree lands byte-complete
        // under the destination, directories re-created, counts reported, and a
        // non-directory source fails closed.
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        const QDir src(source.path());
        QVERIFY(src.mkpath(QStringLiteral("inner/deeper")));
        const auto writeFile = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        writeFile(src.filePath(QStringLiteral("top.txt")), QByteArrayLiteral("top-level"));
        writeFile(src.filePath(QStringLiteral("inner/mid.bin")),
                  QByteArrayLiteral("mid \x00\x01 bytes"));
        writeFile(src.filePath(QStringLiteral("inner/deeper/leaf.txt")),
                  QByteArrayLiteral("leaf content"));

        const auto target = sak::FileManagementFileSystemBridge::localTarget(destination.path());
        const QString destRoot = QDir(destination.path()).filePath(QStringLiteral("imported"));
        const auto result = sak::FileManagementFileSystemBridge::importDirectoryFromHost(
            target, source.path(), destRoot);
        QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
        QVERIFY(result.complete);
        QCOMPARE(result.files_imported, 3);
        QCOMPARE(result.directories_created, 2);
        QCOMPARE(result.symlinks_skipped, 0);

        const auto readAll = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return QByteArray();
            }
            return file.readAll();
        };
        QCOMPARE(readAll(QDir(destRoot).filePath(QStringLiteral("top.txt"))),
                 QByteArrayLiteral("top-level"));
        QCOMPARE(readAll(QDir(destRoot).filePath(QStringLiteral("inner/mid.bin"))),
                 QByteArrayLiteral("mid \x00\x01 bytes"));
        QCOMPARE(readAll(QDir(destRoot).filePath(QStringLiteral("inner/deeper/leaf.txt"))),
                 QByteArrayLiteral("leaf content"));

        const auto notDir = sak::FileManagementFileSystemBridge::importDirectoryFromHost(
            target,
            src.filePath(QStringLiteral("top.txt")),
            QDir(destination.path()).filePath(QStringLiteral("bad")));
        QVERIFY(!notDir.ok);
        QVERIFY2(notDir.blockers.join(QStringLiteral(" ")).contains(QStringLiteral("directory")),
                 qPrintable(notDir.blockers.join(QStringLiteral(" "))));
    }

    void importDropsEntriesPastDepthBoundWithSignal() {
        // A tree deeper than the import depth bound copies with entries dropped
        // as a warning while ok stays true; entries_skipped makes that drop an
        // explicit signal the move path can gate on (so it never deletes the
        // intact source of an incomplete move).
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        QString deep = QStringLiteral("root");
        for (int level = 0; level < 40; ++level) {
            deep += QStringLiteral("/d");
        }
        QVERIFY(QDir(source.path()).mkpath(deep));
        {
            QFile bottom(QDir(source.path()).filePath(deep + QStringLiteral("/leaf.txt")));
            QVERIFY(bottom.open(QIODevice::WriteOnly));
            QVERIFY(bottom.write("deep leaf") > 0);
        }

        const auto dstTarget = sak::FileManagementFileSystemBridge::localTarget(destination.path());
        const auto imported = sak::FileManagementFileSystemBridge::importDirectoryFromHost(
            dstTarget,
            QDir(source.path()).filePath(QStringLiteral("root")),
            QDir(destination.path()).filePath(QStringLiteral("moved-root")));
        QVERIFY(imported.ok);
        QVERIFY(imported.entries_skipped > 0);
        // ok alone overclaims: complete must report the dropped entries so a caller
        // cannot mistake this partial import for a whole one (B8-19).
        QVERIFY(!imported.complete);
        // The source tree was never touched by the copy.
        QVERIFY(QFileInfo(QDir(source.path()).filePath(QStringLiteral("root"))).isDir());
    }

    void listLocalDirectoryUnreadableDirectoryFailsClosed() {
#ifdef Q_OS_WIN
        // QDirIterator surfaces no error, so a directory that EXISTS but cannot be opened used
        // to produce ok=true with zero entries: the caller was told the folder is empty. An
        // enumeration that FAILED must never be reported as one that found nothing.
        //
        // The unreadable state needs no privilege: holding the directory open with dwShareMode
        // 0 makes every later open fail with ERROR_SHARING_VIOLATION, while the attribute query
        // behind QDir::exists() still succeeds.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString lockedPath = QDir(temp.path()).filePath(QStringLiteral("locked"));
        QVERIFY(QDir().mkpath(lockedPath));
        {
            QFile file(QDir(lockedPath).filePath(QStringLiteral("inside.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("payload"), qint64(7));
        }
        const auto target = sak::FileManagementFileSystemBridge::localTarget(lockedPath);

        // Control: while readable the entry is listed, so the failure below is the lock alone.
        const auto readable =
            sak::FileManagementFileSystemBridge::listDirectory(target, lockedPath, 100);
        QVERIFY2(readable.ok, qPrintable(readable.blockers.join(QStringLiteral("; "))));
        QCOMPARE(readable.entries.size(), qsizetype(1));

        const std::wstring native = QDir::toNativeSeparators(lockedPath).toStdWString();
        const HANDLE exclusive = CreateFileW(native.c_str(),
                                             GENERIC_READ,
                                             0,  // no sharing: every subsequent open is refused
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS,
                                             nullptr);
        QVERIFY(exclusive != INVALID_HANDLE_VALUE);
        const auto release = qScopeGuard([exclusive]() { CloseHandle(exclusive); });

        const auto blocked =
            sak::FileManagementFileSystemBridge::listDirectory(target, lockedPath, 100);
        QVERIFY(!blocked.ok);
        QVERIFY(blocked.entries.isEmpty());
        QVERIFY2(!blocked.blockers.isEmpty(),
                 "an unreadable directory must report a blocker, not an empty success");
#else
        QSKIP("Exclusive directory handles are a Windows-only concept");
#endif
    }

    void listLocalDirectoryEmptyDirectoryIsAnHonestSuccess() {
        // The counterpart to the test above: a genuinely empty directory is NOT a failure. The
        // two outcomes must stay distinguishable in the result, not by convention.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto target = sak::FileManagementFileSystemBridge::localTarget(temp.path());
        const auto listing =
            sak::FileManagementFileSystemBridge::listDirectory(target, temp.path(), 100);
        QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
        QVERIFY(listing.entries.isEmpty());
        QVERIFY(listing.blockers.isEmpty());
    }

    void importFailsClosedWhenASourceLevelCannotBeRead() {
#ifdef Q_OS_WIN
        // QDir::entryInfoList on an unreadable directory returns an EMPTY list with no error, so
        // the import walked right past the level, copied nothing out of it, and still reported
        // ok/complete -- a partial copy presented as a whole one (and the move path deletes the
        // source of a copy it believes complete). The bounded collector surfaces the read
        // failure as a blocker instead.
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        const QDir src(source.path());
        QVERIFY(src.mkpath(QStringLiteral("root/sub")));
        const auto writeOne = [](const QString& path) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("payload"), qint64(7));
        };
        writeOne(src.filePath(QStringLiteral("root/top.txt")));
        writeOne(src.filePath(QStringLiteral("root/sub/buried.txt")));

        const std::wstring native =
            QDir::toNativeSeparators(src.filePath(QStringLiteral("root/sub"))).toStdWString();
        const HANDLE exclusive = CreateFileW(native.c_str(),
                                             GENERIC_READ,
                                             0,  // no sharing: every subsequent open is refused
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS,
                                             nullptr);
        QVERIFY(exclusive != INVALID_HANDLE_VALUE);
        const auto release = qScopeGuard([exclusive]() { CloseHandle(exclusive); });

        const auto target = sak::FileManagementFileSystemBridge::localTarget(destination.path());
        const auto imported = sak::FileManagementFileSystemBridge::importDirectoryFromHost(
            target,
            src.filePath(QStringLiteral("root")),
            QDir(destination.path()).filePath(QStringLiteral("copy")));
        QVERIFY(!imported.ok);
        QVERIFY(!imported.complete);
        QVERIFY2(!imported.blockers.isEmpty(),
                 "an unreadable source level must be a blocker, not a silently empty level");
        // The readable part still copied: the failure is reported, not swallowed, and not
        // inflated into a claim that nothing transferred.
        QCOMPARE(imported.files_imported, 1);
#else
        QSKIP("Exclusive directory handles are a Windows-only concept");
#endif
    }

    void listLocalDirectoryEnumeratesHiddenEntries() {
        // A host-hidden file must reach the listing so the view's show-hidden
        // toggle can act on it. Before B8-20 the source listing dropped hidden
        // (and system) entries entirely, leaving that toggle dead -- and hiding
        // files a folder copy would still transfer.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        const auto writeOne = [](const QString& path) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("x"), qint64(1));
        };
        writeOne(root.filePath(QStringLiteral("visible.txt")));
        const QString hiddenPath = root.filePath(QStringLiteral("secret.dat"));
        writeOne(hiddenPath);
#ifdef Q_OS_WIN
        const std::wstring native = QDir::toNativeSeparators(hiddenPath).toStdWString();
        QVERIFY(SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0);
        const QString hiddenName = QStringLiteral("secret.dat");
#else
        const QString hiddenName = QStringLiteral(".secret.dat");
        QVERIFY(QFile::rename(hiddenPath, root.filePath(hiddenName)));
#endif
        const auto target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        const auto listing =
            sak::FileManagementFileSystemBridge::listDirectory(target, dir.path(), 100);
        QVERIFY(listing.ok);
        QStringList names;
        bool hidden_flag = false;
        for (const auto& entry : listing.entries) {
            names << entry.name;
            if (entry.name == hiddenName) {
                hidden_flag = entry.hidden;
            }
        }
        QVERIFY2(names.contains(QStringLiteral("visible.txt")), qPrintable(names.join(QChar(','))));
        QVERIFY2(names.contains(hiddenName), qPrintable(names.join(QChar(','))));
#ifdef Q_OS_WIN
        // The hidden attribute rides through on the entry so the view can filter it.
        QVERIFY(hidden_flag);
#endif
    }

    void listLocalDirectoryCapsToSortedPrefixWithWarning() {
        // The cap keeps the display-order-first N entries (a bounded top-N heap,
        // not an arbitrary iteration subset) and flags the truncation, so an
        // enormous directory yields a small sorted window in O(cap) memory (B8-20).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        for (char letter = 'a'; letter <= 'h'; ++letter) {
            QFile file(root.filePath(QString(QLatin1Char(letter)) + QStringLiteral(".txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("x"), qint64(1));
        }
        const auto target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        const auto listing =
            sak::FileManagementFileSystemBridge::listDirectory(target, dir.path(), 3);
        QVERIFY(listing.ok);
        QCOMPARE(listing.entries.size(), qsizetype(3));
        QCOMPARE(listing.entries.at(0).name, QStringLiteral("a.txt"));
        QCOMPARE(listing.entries.at(1).name, QStringLiteral("b.txt"));
        QCOMPARE(listing.entries.at(2).name, QStringLiteral("c.txt"));
        QVERIFY(!listing.warnings.isEmpty());
        QVERIFY2(listing.warnings.join(QChar(' ')).contains(QStringLiteral("truncated")),
                 qPrintable(listing.warnings.join(QChar(' '))));
    }

    void deleteDirectoryTreeRemovesNestedLocalTree() {
        // The move-source cleanup used by folder cut-paste: a populated local tree is
        // removed whole.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QDir dir(root.path());
        QVERIFY(dir.mkpath(QStringLiteral("gone/deep")));
        QFile file(dir.filePath(QStringLiteral("gone/deep/leaf.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArrayLiteral("x")) == 1);
        file.close();

        const auto target = sak::FileManagementFileSystemBridge::localTarget(root.path());
        const auto result = sak::FileManagementFileSystemBridge::deleteDirectoryTree(
            target, dir.filePath(QStringLiteral("gone")));
        QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
        QVERIFY(!QDir(dir.filePath(QStringLiteral("gone"))).exists());
    }

    void removeExistingEntryResolvesKindAtExecutionTime() {
        // The deferred Replace delete: a directory tree occupying the path is
        // removed whole, a vacant path succeeds without touching anything, and
        // a raw target whose listing cannot be trusted fails closed.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QDir dir(root.path());
        QVERIFY(dir.mkpath(QStringLiteral("occupied/nested")));
        QFile child(dir.filePath(QStringLiteral("occupied/nested/old.bin")));
        QVERIFY(child.open(QIODevice::WriteOnly));
        QVERIFY(child.write(QByteArrayLiteral("old")) == 3);
        child.close();

        const auto target = sak::FileManagementFileSystemBridge::localTarget(root.path());
        const auto removed = sak::FileManagementFileSystemBridge::removeExistingEntry(
            target, dir.filePath(QStringLiteral("occupied")), 100);
        QVERIFY2(removed.ok, qPrintable(removed.blockers.join(QStringLiteral("; "))));
        QVERIFY(!QDir(dir.filePath(QStringLiteral("occupied"))).exists());

        const auto vacant = sak::FileManagementFileSystemBridge::removeExistingEntry(
            target, dir.filePath(QStringLiteral("never-existed.txt")), 100);
        QVERIFY(vacant.ok);

        // Raw target with a missing backing image: the occupant cannot be
        // verified, so nothing is deleted and the result carries a blocker.
        sak::FileManagementTarget raw = sak::FileManagementFileSystemBridge::manualTarget(
            dir.filePath(QStringLiteral("missing.img")), QStringLiteral("apfs"));
        raw.local_file_system = false;
        const auto unverified = sak::FileManagementFileSystemBridge::removeExistingEntry(
            raw, QStringLiteral("/dest/a.txt"), 100);
        QVERIFY(!unverified.ok);
        QVERIFY(!unverified.blockers.isEmpty());
    }

    // Read a raw file back through the bridge for byte comparison. Takes
    // parameters so QtTest does not run it as a test slot.
    QByteArray rawReadAll(const sak::FileManagementTarget& target, const QString& path) {
        const auto read =
            sak::FileManagementFileSystemBridge::readFile(target, path, 16ULL * 1024 * 1024);
        return read.ok ? read.data : QByteArray();
    }

    // Leg-1 observer proof on the certified APFS write: whole-file byte delta
    // on success, cancel poll blocks before the engine runs. Takes parameters
    // so QtTest does not run it as a test slot; also performs the /solo.txt
    // write the rest of the matrix depends on.
    void verifyRawWriteObserver(const sak::FileManagementTarget& target,
                                const QDir& hostDir,
                                const qint64 solo_size) {
        using Bridge = sak::FileManagementFileSystemBridge;
        qint64 raw_seen = 0;
        const sak::FileManagementTransferObserver raw_counting{
            .on_bytes = [&raw_seen](const qint64 delta) { raw_seen += delta; },
            .cancelled = {},
        };
        const auto soloWrite =
            Bridge::writeFileFromHostPath(target,
                                          QStringLiteral("/solo.txt"),
                                          hostDir.filePath(QStringLiteral("solo.txt")),
                                          raw_counting);
        QVERIFY2(soloWrite.ok, qPrintable(soloWrite.blockers.join(QStringLiteral("; "))));
        QCOMPARE(raw_seen, solo_size);
        const sak::FileManagementTransferObserver raw_canceling{
            .on_bytes = {},
            .cancelled = []() { return true; },
        };
        const auto rawCanceled =
            Bridge::writeFileFromHostPath(target,
                                          QStringLiteral("/canceled.txt"),
                                          hostDir.filePath(QStringLiteral("solo.txt")),
                                          raw_canceling);
        QVERIFY(!rawCanceled.ok);
        QVERIFY(rawCanceled.blockers.contains(sak::kFileManagementTransferCancelledBlocker));
    }

    void rawApfsTransferMatrixEndToEnd() {
        // C3e raw proof: every File Explorer transfer leg against a REAL (foreign,
        // mkapfs-created) APFS image through the same bridge calls the panel kernel
        // makes. Gated on SAK_APFS_RAW_IMAGE (+ optional SAK_APFS_RAW_IMAGE_B for the
        // staged raw-to-raw leg); the cert flow creates the images in WSL and runs
        // apfsck on them afterwards - this test leaves them mutated on purpose.
        const QString imagePath = qEnvironmentVariable("SAK_APFS_RAW_IMAGE");
        if (imagePath.isEmpty()) {
            QSKIP("SAK_APFS_RAW_IMAGE not set; the raw transfer matrix runs in the C3e cert flow.");
        }
        // The engine's raw-target gate accepts only Windows device paths in
        // production; the image fixture opts in through the same test seam the
        // certified commit tests use (test_partition_manager_core precedent).
        sak::PartitionApfsWriter::setRawDeviceTargetPredicateForTesting(
            [](const QString&) { return true; });
        const auto reset_predicate = qScopeGuard(
            []() { sak::PartitionApfsWriter::setRawDeviceTargetPredicateForTesting({}); });
        using Bridge = sak::FileManagementFileSystemBridge;
        const auto target = Bridge::manualTarget(imagePath, QStringLiteral("APFS"));
        QVERIFY2(target.can_write_files, qPrintable(target.blockers.join(QStringLiteral("; "))));

        // Host fixture tree.
        QTemporaryDir host;
        QVERIFY(host.isValid());
        const QDir hostDir(host.path());
        QVERIFY(hostDir.mkpath(QStringLiteral("bundle/deep")));
        const auto writeFile = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        const QByteArray innerBytes = QByteArrayLiteral("inner raw payload");
        const QByteArray leafBytes = QByteArrayLiteral("leaf raw payload \x00\x7f bytes");
        const QByteArray soloBytes = QByteArrayLiteral("solo raw payload");
        writeFile(hostDir.filePath(QStringLiteral("bundle/inner.txt")), innerBytes);
        writeFile(hostDir.filePath(QStringLiteral("bundle/deep/leaf.txt")), leafBytes);
        writeFile(hostDir.filePath(QStringLiteral("solo.txt")), soloBytes);

        // Leg 1 - local -> raw: streamed file import + recursive directory import.
        // The transfer observer reports the whole-file delta on the certified
        // write, and its cancel poll blocks the write before it starts.
        verifyRawWriteObserver(target, hostDir, static_cast<qint64>(soloBytes.size()));
        if (QTest::currentTestFailed()) {
            return;
        }
        const auto imported = Bridge::importDirectoryFromHost(
            target, hostDir.filePath(QStringLiteral("bundle")), QStringLiteral("/bundle"));
        QVERIFY2(imported.ok, qPrintable(imported.blockers.join(QStringLiteral("; "))));
        QCOMPARE(imported.files_imported, 2);
        QCOMPARE(imported.directories_created, 1);
        QCOMPARE(rawReadAll(target, QStringLiteral("/bundle/deep/leaf.txt")), leafBytes);

        // Leg 2 - raw -> local: file copy-out + recursive directory export, byte-exact.
        QTemporaryDir out;
        QVERIFY(out.isValid());
        const QString outSolo = QDir(out.path()).filePath(QStringLiteral("solo.txt"));
        QVERIFY(Bridge::copyFileToHost(target, QStringLiteral("/solo.txt"), outSolo, 0).ok);
        QFile outSoloFile(outSolo);
        QVERIFY(outSoloFile.open(QIODevice::ReadOnly));
        QCOMPARE(outSoloFile.readAll(), soloBytes);
        const QString outBundle = QDir(out.path()).filePath(QStringLiteral("bundle"));
        const auto exported =
            Bridge::exportDirectoryToHost(target, QStringLiteral("/bundle"), outBundle, 0);
        QVERIFY2(exported.ok, qPrintable(exported.blockers.join(QStringLiteral("; "))));
        QFile outLeaf(QDir(outBundle).filePath(QStringLiteral("deep/leaf.txt")));
        QVERIFY(outLeaf.open(QIODevice::ReadOnly));
        QCOMPARE(outLeaf.readAll(), leafBytes);

        verifyRawMoveDeleteAndCrossImage(target, soloBytes, leafBytes, outBundle);
    }

    void rawHfsTransferMatrixEndToEnd() {
        // C8 raw proof: the explorer transfer legs against a REAL Apple
        // (hdiutil-created, non-journaled) HFS+ image through the same bridge
        // calls the panel kernel makes. Gated on SAK_HFS_RAW_IMAGE; the cert
        // flow ships the mutated image back to a Mac for fsck_hfs afterwards.
        const QString imagePath = qEnvironmentVariable("SAK_HFS_RAW_IMAGE");
        if (imagePath.isEmpty()) {
            QSKIP("SAK_HFS_RAW_IMAGE not set; the raw HFS transfer matrix runs in the C8 flow.");
        }
        using Bridge = sak::FileManagementFileSystemBridge;
        const auto target = Bridge::manualTarget(imagePath, QStringLiteral("HFS+"));
        QVERIFY2(target.can_write_files, qPrintable(target.blockers.join(QStringLiteral("; "))));

        QTemporaryDir host;
        QVERIFY(host.isValid());
        const QDir hostDir(host.path());
        QVERIFY(hostDir.mkpath(QStringLiteral("bundle/deep")));
        const auto writeFile = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        const QByteArray innerBytes = QByteArrayLiteral("inner hfs payload");
        const QByteArray leafBytes = QByteArrayLiteral("leaf hfs payload \x01\x7e bytes");
        const QByteArray soloBytes = QByteArrayLiteral("solo hfs payload");
        writeFile(hostDir.filePath(QStringLiteral("bundle/inner.txt")), innerBytes);
        writeFile(hostDir.filePath(QStringLiteral("bundle/deep/leaf.txt")), leafBytes);
        writeFile(hostDir.filePath(QStringLiteral("solo.txt")), soloBytes);

        // Leg 1 - local -> raw: observer-reported streamed import + recursion.
        verifyRawWriteObserver(target, hostDir, static_cast<qint64>(soloBytes.size()));
        if (QTest::currentTestFailed()) {
            return;
        }
        const auto imported = Bridge::importDirectoryFromHost(
            target, hostDir.filePath(QStringLiteral("bundle")), QStringLiteral("/bundle"));
        QVERIFY2(imported.ok, qPrintable(imported.blockers.join(QStringLiteral("; "))));
        QCOMPARE(imported.files_imported, 2);
        QCOMPARE(rawReadAll(target, QStringLiteral("/bundle/deep/leaf.txt")), leafBytes);

        // Leg 2 - raw -> local: file copy-out + recursive export, byte-exact.
        QTemporaryDir out;
        QVERIFY(out.isValid());
        // HFS enforces the read cap strictly (0 is not "unlimited" as on the
        // APFS path), so pass an explicit window.
        constexpr quint64 kHfsReadCap = 1024ULL * 1024;
        const QString outSolo = QDir(out.path()).filePath(QStringLiteral("solo.txt"));
        QVERIFY(
            Bridge::copyFileToHost(target, QStringLiteral("/solo.txt"), outSolo, kHfsReadCap).ok);
        QFile outSoloFile(outSolo);
        QVERIFY(outSoloFile.open(QIODevice::ReadOnly));
        QCOMPARE(outSoloFile.readAll(), soloBytes);
        const auto exported =
            Bridge::exportDirectoryToHost(target,
                                          QStringLiteral("/bundle"),
                                          QDir(out.path()).filePath(QStringLiteral("bundle")),
                                          kHfsReadCap);
        QVERIFY2(exported.ok, qPrintable(exported.blockers.join(QStringLiteral("; "))));

        verifyRawHfsMoveAndDelete(target, soloBytes, leafBytes);
    }

    // Same-target HFS moves through the certified catalog engine plus the
    // depth-first tree delete. Takes parameters so QtTest does not run it as
    // a test slot.
    void verifyRawHfsMoveAndDelete(const sak::FileManagementTarget& target,
                                   const QByteArray& soloBytes,
                                   const QByteArray& leafBytes) {
        using Bridge = sak::FileManagementFileSystemBridge;
        QVERIFY(Bridge::createDirectory(target, QStringLiteral("/dest")).ok);
        QVERIFY(Bridge::renameEntry(
                    target, QStringLiteral("/solo.txt"), QStringLiteral("/dest/solo.txt"))
                    .ok);
        QCOMPARE(rawReadAll(target, QStringLiteral("/dest/solo.txt")), soloBytes);
        const auto dirMove =
            Bridge::renameEntry(target, QStringLiteral("/bundle"), QStringLiteral("/dest/bundle"));
        QVERIFY2(dirMove.ok, qPrintable(dirMove.blockers.join(QStringLiteral("; "))));
        QCOMPARE(rawReadAll(target, QStringLiteral("/dest/bundle/deep/leaf.txt")), leafBytes);
        const auto removed = Bridge::deleteDirectoryTree(target, QStringLiteral("/dest"));
        QVERIFY2(removed.ok, qPrintable(removed.blockers.join(QStringLiteral("; "))));
        const auto listing = Bridge::listDirectory(target, QStringLiteral("/"), 100);
        for (const auto& entry : listing.entries) {
            QVERIFY(entry.name != QStringLiteral("dest"));
        }
    }

    // Same-target raw moves, staged raw-to-raw, and the depth-first raw tree
    // delete. Takes parameters so QtTest does not run it as a test slot.
    void verifyRawMoveDeleteAndCrossImage(const sak::FileManagementTarget& target,
                                          const QByteArray& soloBytes,
                                          const QByteArray& leafBytes,
                                          const QString& stagedBundle) {
        using Bridge = sak::FileManagementFileSystemBridge;

        // Leg 3 - same-target move: file reparent + whole-directory reparent via
        // renameEntry (the COW engine dispatches directories to the directory commit).
        QVERIFY(Bridge::createDirectory(target, QStringLiteral("/dest")).ok);
        QVERIFY(Bridge::renameEntry(
                    target, QStringLiteral("/solo.txt"), QStringLiteral("/dest/solo.txt"))
                    .ok);
        QCOMPARE(rawReadAll(target, QStringLiteral("/dest/solo.txt")), soloBytes);
        const auto dirMove =
            Bridge::renameEntry(target, QStringLiteral("/bundle"), QStringLiteral("/dest/bundle"));
        QVERIFY2(dirMove.ok, qPrintable(dirMove.blockers.join(QStringLiteral("; "))));
        QCOMPARE(rawReadAll(target, QStringLiteral("/dest/bundle/deep/leaf.txt")), leafBytes);

        // Leg 4 - raw -> raw across containers, staged through the host (what
        // transferRawEntryStaged does): the tree exported in leg 2 imports into image B.
        const QString imageBPath = qEnvironmentVariable("SAK_APFS_RAW_IMAGE_B");
        if (!imageBPath.isEmpty()) {
            const auto targetB = Bridge::manualTarget(imageBPath, QStringLiteral("APFS"));
            QVERIFY2(targetB.can_write_files,
                     qPrintable(targetB.blockers.join(QStringLiteral("; "))));
            const auto crossImport =
                Bridge::importDirectoryFromHost(targetB, stagedBundle, QStringLiteral("/bundle"));
            QVERIFY2(crossImport.ok, qPrintable(crossImport.blockers.join(QStringLiteral("; "))));
            QCOMPARE(rawReadAll(targetB, QStringLiteral("/bundle/deep/leaf.txt")), leafBytes);
        }

        // Leg 5 - raw tree delete: APFS refuses non-empty directory deletes by design,
        // so deleteDirectoryTree empties depth-first through certified per-entry commits.
        const auto treeDelete = Bridge::deleteDirectoryTree(target, QStringLiteral("/dest/bundle"));
        QVERIFY2(treeDelete.ok, qPrintable(treeDelete.blockers.join(QStringLiteral("; "))));
        const auto listing = Bridge::listDirectory(target, QStringLiteral("/dest"), 100);
        QVERIFY(listing.ok);
        for (const auto& entry : listing.entries) {
            QVERIFY2(entry.name != QStringLiteral("bundle"), "tree delete left the directory");
        }
    }

    void apfsImageTargetWithKnownSizeIsWriteCapable() {
        // Foreign wiring: any APFS raw/image target whose container size is known and
        // inside the certified engine range is write-capable through the bridge; the
        // engine itself fails closed on anything it has not certified for the container.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString imagePath = QDir(temp.path()).filePath(QStringLiteral("container.img"));
        {
            QFile image(imagePath);
            QVERIFY(image.open(QIODevice::WriteOnly));
            QVERIFY(image.resize(64LL * 1024LL * 1024LL));  // sparse 64 MiB
        }
        const auto target =
            sak::FileManagementFileSystemBridge::manualTarget(imagePath, QStringLiteral("APFS"));
        QCOMPARE(target.size_bytes, 64ULL * 1024ULL * 1024ULL);
        QVERIFY(target.can_write_files);

        // An unknown-size raw APFS partition stays read-only with the size blocker.
        const auto unknown = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk63\\Partition9"),
            QStringLiteral("APFS"));
        QVERIFY(!unknown.can_write_files);
        QVERIFY2(unknown.blockers.join(QStringLiteral(" "))
                     .contains(QStringLiteral("known container size")),
                 qPrintable(unknown.blockers.join(QStringLiteral(" "))));
    }

    void inventoryPartitionBuildsRawAlias() {
        sak::PartitionInventory inventory;
        sak::PartitionDiskInfo disk;
        disk.disk_number = 4;
        sak::PartitionInfoEx partition;
        partition.disk_number = 4;
        partition.partition_number = 2;
        sak::PartitionVolumeInfo volume;
        volume.file_system = QStringLiteral("HFS+");
        partition.volume = volume;
        disk.partitions.append(partition);
        inventory.disks.append(disk);

        const auto targets = sak::FileManagementFileSystemBridge::targetsFromInventory(inventory);
        const auto it = std::find_if(targets.cbegin(), targets.cend(), [](const auto& target) {
            return target.id == QStringLiteral("disk:4:partition:2");
        });
        QVERIFY(it != targets.cend());
        QCOMPARE(it->root_path, QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2"));
        QVERIFY(it->can_browse);
        QVERIFY(!it->can_organize);
    }

    // B8-01: a local recursive directory delete must refuse an empty path (which
    // QDir treats as the CWD) or a filesystem root, before touching the disk.
    void deleteDirectoryRefusesUnsafePaths() {
        using B = sak::FileManagementFileSystemBridge;
        QVERIFY(B::isUnsafeLocalDeletePath(QString()));
        QVERIFY(B::isUnsafeLocalDeletePath(QStringLiteral("   ")));
        QVERIFY(B::isUnsafeLocalDeletePath(QStringLiteral("C:/")));
        QVERIFY(B::isUnsafeLocalDeletePath(QStringLiteral("C:\\")));
        QVERIFY(B::isUnsafeLocalDeletePath(QStringLiteral("/")));

        // A real nested directory is NOT flagged (so normal deletes still work).
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString nested = QDir(temp.path()).filePath(QStringLiteral("victim"));
        QVERIFY(QDir().mkpath(nested));
        QVERIFY(!B::isUnsafeLocalDeletePath(nested));

        // The bridge refuses an empty path and does NOT delete the CWD contents.
        const auto target = B::localTarget(temp.path());
        const auto result = B::deleteDirectory(target, QString());
        QVERIFY(!result.ok);
        QVERIFY(!result.blockers.isEmpty());
        QVERIFY(QDir(nested).exists());  // nothing was deleted

        // A real nested delete still succeeds.
        const auto ok_result = B::deleteDirectory(target, nested);
        QVERIFY(ok_result.ok);
        QVERIFY(!QDir(nested).exists());
    }

    // B8-02: a raw foreign-filesystem entry name must be confined to a bare host
    // filename so it cannot escape the export directory.
    void confinedHostNameRejectsNonBareNames() {
        using B = sak::FileManagementFileSystemBridge;
        // An already-bare foreign name passes through unchanged.
        QCOMPARE(B::confinedHostName(QStringLiteral("report.txt")), QStringLiteral("report.txt"));
        // A name carrying ANY path structure is REJECTED, not collapsed onto its last segment:
        // QFileInfo::fileName would reduce "../../evil.sh" to "evil.sh" (and "a\b", since a
        // backslash is a legal byte in an APFS/HFS+ name, to "b"), which would then overwrite a
        // real sibling of that name on the host. Fail closed instead.
        QVERIFY(B::confinedHostName(QStringLiteral("../../evil.sh")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("sub/dir/leaf")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("a\\b")).isEmpty());
        // Traversal tokens and empty are rejected too.
        QVERIFY(B::confinedHostName(QStringLiteral("..")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral(".")).isEmpty());
        QVERIFY(B::confinedHostName(QString()).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("a/../..")).isEmpty());
    }

    // Codex-3 finding 1: a foreign (untrusted image) entry name must be hardened
    // against Windows filename hazards before it names a host file, not merely
    // stripped of path components.
    void confinedHostNameRejectsWindowsHazards() {
        using B = sak::FileManagementFileSystemBridge;
        // NTFS alternate-data-stream separator would divert the export into a stream.
        QVERIFY(B::confinedHostName(QStringLiteral("foo:bar.txt")).isEmpty());
        // Other reserved / wildcard characters.
        QVERIFY(B::confinedHostName(QStringLiteral("a|b.txt")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("a?b.txt")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("a<b>.txt")).isEmpty());
        // Reserved device names, including with an extension and case-folded.
        QVERIFY(B::confinedHostName(QStringLiteral("NUL")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("nul.txt")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("CoM1")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("LPT9.log")).isEmpty());
        // Trailing dot / space that Windows silently strips (would collapse onto a sibling).
        QVERIFY(B::confinedHostName(QStringLiteral("name ")).isEmpty());
        QVERIFY(B::confinedHostName(QStringLiteral("name.")).isEmpty());
        // A device-name-like STEM that is actually a normal file stays allowed.
        QCOMPARE(B::confinedHostName(QStringLiteral("console.txt")), QStringLiteral("console.txt"));
        QCOMPARE(B::confinedHostName(QStringLiteral("com10.log")), QStringLiteral("com10.log"));
    }

    // Codex-3 finding 2: every bridge mutation entrypoint re-checks writability
    // itself (defense in depth over the engine's evidence gate) so a headless / MCP
    // caller cannot bypass an upstream UI gate. A not-writable local target is
    // refused before any filesystem call.
    void bridgeMutationsFailClosedOnNonWritableTarget() {
        using B = sak::FileManagementFileSystemBridge;
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        sak::FileManagementTarget locked;
        locked.file_system = QStringLiteral("NTFS");
        locked.root_path = temp.path();
        locked.local_file_system = true;
        locked.read_only = true;
        locked.can_write_files = false;  // e.g. a read-only-mounted local volume
        const QString target_file = QDir(temp.path()).filePath(QStringLiteral("blocked.txt"));

        QVERIFY(!B::createDirectory(locked, target_file).ok);
        QVERIFY(!B::writeFile(locked, target_file, QByteArrayLiteral("x")).ok);
        QVERIFY(!B::deleteFile(locked, target_file).ok);
        QVERIFY(!B::removeExistingEntry(locked, target_file, 100).ok);
        // Nothing was created on disk by the refused writes.
        QVERIFY(!QFile::exists(target_file));
    }

    // B8-03: a raw non-native target the inventory reports read-only (hardware
    // write-protect) must NOT be reported writable, even though its size is in the
    // certified engine range.
    void readOnlyRawTargetIsNotWritable() {
        using B = sak::FileManagementFileSystemBridge;
        const uint64_t size = 64ULL * 1024ULL * 1024ULL;  // in the certified APFS range
        const auto build = [&](bool read_only) {
            sak::PartitionInventory inv;
            sak::PartitionDiskInfo disk;
            disk.disk_number = 7;
            sak::PartitionInfoEx part;
            part.disk_number = 7;
            part.partition_number = 1;
            part.size_bytes = size;
            part.is_read_only = read_only;
            sak::PartitionVolumeInfo vol;
            vol.file_system = QStringLiteral("APFS");
            part.volume = vol;
            disk.partitions.append(part);
            inv.disks.append(disk);
            const auto targets = B::targetsFromInventory(inv);
            for (const auto& t : targets) {
                if (t.id == QStringLiteral("disk:7:partition:1")) {
                    return t;
                }
            }
            return sak::FileManagementTarget{};
        };

        const auto writable = build(false);
        QVERIFY(!writable.local_file_system);
        QVERIFY(writable.can_write_files);  // known-size raw APFS is write-capable

        const auto locked = build(true);
        QVERIFY(locked.read_only);
        QVERIFY(!locked.can_write_files);  // write-protected -> refused (B8-03)
        QVERIFY(!locked.can_organize);
    }

    // B8-04: the certified raw HFS/APFS writer's enable + destructive/hardware
    // certification-evidence attestations (and the per-op mutation confirmation) are
    // derived from the target actually being writable. A read-only / non-writable
    // target must fail closed in the writer engine instead of the bridge asserting
    // evidence it does not have -- previously every attestation was hard-coded true,
    // so can_write_files / read_only were advisory badges the writer never enforced.
    void writerEvidenceGateFollowsTargetWritability() {
        using B = sak::FileManagementFileSystemBridge;

        // The pure predicate every option builder + confirmation flag is derived from.
        const auto make = [](bool can_write, bool read_only) {
            sak::FileManagementTarget t;
            t.can_write_files = can_write;
            t.read_only = read_only;
            return t;
        };
        // can_write_files is the sole writability signal: applyCapabilities forces it
        // FALSE on a real write-protect, and forces read_only TRUE for every non-local
        // target purely as a browsing badge. So a writable raw target legitimately has
        // can_write_files == true WITH read_only == true (the badge) and MUST permit
        // mutation -- gating on read_only (the old B8-04 clause) disabled every
        // advertised raw HFS/APFS write (Codex-3 finding 3).
        QVERIFY(B::targetPermitsMutation(make(true, false)));    // genuinely writable local
        QVERIFY(B::targetPermitsMutation(make(true, true)));     // writable raw (read_only = badge)
        QVERIFY(!B::targetPermitsMutation(make(false, false)));  // not write-capable
        QVERIFY(!B::targetPermitsMutation(make(false, true)));   // write-protected

        // End to end: a non-writable raw APFS target (never a real device -- a temp
        // path that is not an APFS container) must have its mutation refused. The
        // bridge no longer hard-codes the writer attestations, so can_write_files
        // (false here) is enforced at the mutation entrypoint.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        sak::FileManagementTarget locked;
        locked.file_system = QStringLiteral("APFS");
        locked.root_path = QDir(temp.path()).filePath(QStringLiteral("not-a-device.img"));
        locked.local_file_system = false;
        locked.kind = sak::FileManagementTargetKind::ImageFile;
        locked.size_bytes = 64ULL * 1024ULL * 1024ULL;  // in the certified range
        locked.read_only = true;
        locked.can_write_files = false;
        const auto result =
            B::writeFile(locked, QStringLiteral("/x.txt"), QByteArrayLiteral("data"));
        QVERIFY(!result.ok);
        QVERIFY(!result.blockers.isEmpty());
    }

    // B8-05: a raw-target "Replace" whose destination has no exact (case-correct)
    // occupant must not delete a differing-case file on a case-sensitive volume.
    // Collisions are detected case-insensitively upstream, so "foo" can be asked to
    // replace "Foo"; only a known case-insensitive volume may treat them as one file.
    void rawReplaceCaseActionIsDataLossSafe() {
        using B = sak::FileManagementFileSystemBridge;
        using A = sak::RawReplaceCaseAction;

        // Two+ case-only matches prove a case-sensitive volume -> refuse (any fs).
        QCOMPARE(B::rawReplaceCaseAction(QStringLiteral("apfs"), 2), A::RefuseMultipleCase);
        QCOMPARE(B::rawReplaceCaseAction(QStringLiteral("hfsplus"), 3), A::RefuseMultipleCase);

        // No case match at all -> the exact name is vacant (any fs).
        QCOMPARE(B::rawReplaceCaseAction(QStringLiteral("apfs"), 0), A::TreatAsVacant);
        QCOMPARE(B::rawReplaceCaseAction(QStringLiteral("hfsx"), 0), A::TreatAsVacant);

        // Exactly one differing-case occupant: the volume's case-sensitivity decides.
        QCOMPARE(B::rawReplaceCaseAction(QStringLiteral("hfsplus"), 1),
                 A::DeleteDifferingCase);  // case-insensitive -> same file, replace it
        QCOMPARE(B::rawReplaceCaseAction(QStringLiteral("hfsx"), 1),
                 A::TreatAsVacant);        // case-sensitive -> a distinct file, leave it
        QCOMPARE(B::rawReplaceCaseAction(QStringLiteral("apfs"), 1),
                 A::RefuseAmbiguous);      // case-sensitivity unknown -> refuse, no data loss
    }
};

QTEST_MAIN(FileManagementFileSystemTests)
#include "test_file_management_file_system.moc"
