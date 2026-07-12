// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_management_file_system.cpp
/// @brief Unit tests for File Management file-system target bridge.

#include "sak/file_management_file_system.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

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
        QVERIFY2(apfsNote.contains(QStringLiteral("32 TiB")), qPrintable(apfsNote));

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
};

QTEST_MAIN(FileManagementFileSystemTests)
#include "test_file_management_file_system.moc"
