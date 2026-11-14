// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_raw_partition_write.cpp
/// @brief Writing a whole partition that Windows has mounted a file system on.
///
/// HFS+ and APFS Format write the whole partition they replace; HFS+ Repair and file changes
/// write it back. Windows mounts NTFS, FAT32 or exFAT on a partition as soon as it is opened,
/// drive letter or not, and through that file system the partition's last sectors are out of
/// reach: a write there comes back short, with no error. So from the 2026-06-13 base until
/// 2026-11-11, Format to APFS of an NTFS partition failed through the app ("Unable to zero APFS
/// format target: Unknown error", found by the VM proof); every test wrote image files. This suite
/// makes three small virtual disks, NTFS, FAT32 and exFAT, and on each partition writes the first
/// and the last MiB through the C++ writer (sak::openFileOrRawDeviceReadWrite) and the scripts'
/// writer (Sak.RawPartitionWrite), reading both back, and checks that both refuse a partition
/// something else has open. Making and mounting a virtual disk needs administrator rights and
/// reaches disks, so it runs in CI's elevated step and skips everywhere else.

#include "sak/no_follow_fs.h"
#include "sak/partition_manager_types.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_raw_device_io.h"
#include "sak/partition_script_builder.h"
#include "sak/process_runner.h"

#include "../support/test_hardware_guard.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <memory>

#include <windows.h>

#include <winioctl.h>

namespace {

constexpr int kVirtualDiskMiB = 64;
constexpr int kDiskpartTimeoutMs = 120'000;
constexpr int kScriptTimeoutMs = 120'000;
constexpr qint64 kEdgeBytes = 1024 * 1024;
constexpr int kSettleMs = 3000;  ///< how long a failure waits before it reads the edges again
constexpr qint64 kHeldWaitMs = 30'000;         ///< how long a refused take is retried, to report it
constexpr int kToolScriptTimeoutMs = 600'000;  ///< a Format script with a bundled tool
constexpr qint64 kExtSuperblockMagicOffset = 1024 + 0x38;  ///< s_magic, 0xEF53 little-endian

bool processIsElevated() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0 &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

/// Free drive letters, from Z down.
QStringList freeDriveLetters(int count) {
    const DWORD used = GetLogicalDrives();
    QStringList letters;
    for (int letter = 'Z'; letter >= 'H' && letters.size() < count; --letter) {
        if ((used & (1U << static_cast<unsigned>(letter - 'A'))) == 0) {
            letters.append(QString(QChar(letter)));
        }
    }
    return letters;
}

/// Run diskpart on @p lines; its whole output.
QString diskpart(const QTemporaryDir& work, const QStringList& lines) {
    const QString script =
        QDir(work.path())
            .filePath(QStringLiteral("dp-%1.txt").arg(QRandomGenerator::global()->generate()));
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly)) {
        return QStringLiteral("cannot write the diskpart script");
    }
    file.write(lines.join(QStringLiteral("\r\n")).toLatin1() + "\r\n");
    file.close();
    const sak::ProcessResult result =
        sak::runProcess(QStringLiteral("diskpart.exe"),
                        {QStringLiteral("/s"), QDir::toNativeSeparators(script)},
                        kDiskpartTimeoutMs);
    QFile::remove(script);
    return result.std_out + result.std_err;
}

struct PartitionVolume {
    QString name;        ///< NTFS, FAT32 or exFAT
    QString diskpartFs;  ///< ntfs, fat32, exfat
    QString letter;      ///< "X"
    QString vhd;
    QString partition;   ///< \\?\GLOBALROOT\Device\HarddiskN\PartitionM
    DWORD disk{0};
    DWORD partitionNumber{0};
    qint64 size{0};
};

/// The disk and partition numbers behind drive @p letter, and the partition's length.
bool partitionOfLetter(const QString& letter, PartitionVolume* volume) {
    const std::wstring path =
        (QStringLiteral("\\\\.\\") + letter + QStringLiteral(":")).toStdWString();
    // Read access: IOCTL_DISK_GET_LENGTH_INFO needs it.
    HANDLE handle = CreateFileW(path.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    STORAGE_DEVICE_NUMBER number{};
    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle,
                                    IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                    nullptr,
                                    0,
                                    &number,
                                    sizeof(number),
                                    &returned,
                                    nullptr) != 0 &&
                    DeviceIoControl(handle,
                                    IOCTL_DISK_GET_LENGTH_INFO,
                                    nullptr,
                                    0,
                                    &length,
                                    sizeof(length),
                                    &returned,
                                    nullptr) != 0;
    CloseHandle(handle);
    if (!ok) {
        return false;
    }
    volume->disk = number.DeviceNumber;
    volume->partitionNumber = number.PartitionNumber;
    volume->partition = QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
                            .arg(number.DeviceNumber)
                            .arg(number.PartitionNumber);
    volume->size = length.Length.QuadPart;
    return true;
}

QByteArray pattern(char fill) {
    return QByteArray(kEdgeBytes, fill);
}

/// True when the script printed @p marker as a line of its own. A substring check is not enough:
/// an error record quotes the failing line, and "ClearFileSystem(...); 'CLEARED'" put the marker
/// in the error a refused take printed (testpc1, 2026-11-11).
bool printedLine(const sak::ProcessResult& result, const QString& marker) {
    const QStringList lines = result.std_out.split(QLatin1Char('\n'));
    return std::ranges::any_of(lines,
                               [&marker](const QString& line) { return line.trimmed() == marker; });
}

/// What an edge that should be zero holds: its length and, if any, the first non-zero byte's
/// offset with the 32 bytes from there. For a failure message that says what is on the disk.
QString describeZeroEdge(const QByteArray& edge) {
    for (qsizetype i = 0; i < edge.size(); ++i) {
        if (edge.at(i) != '\0') {
            return QStringLiteral("%1 bytes read; first non-zero byte at offset %2: %3")
                .arg(edge.size())
                .arg(i)
                .arg(QString::fromLatin1(edge.mid(i, 32).toHex(' ')));
        }
    }
    return QStringLiteral("%1 bytes read, all zero").arg(edge.size());
}

/// The first and the last MiB of @p partition, read through the read-only opener.
QPair<QByteArray, QByteArray> readEdges(const PartitionVolume& volume, QString* error) {
    auto device = sak::openFileOrRawDeviceReadOnly(volume.partition, error);
    if (!device) {
        return {};
    }
    QByteArray first;
    QByteArray last;
    if (device->seek(0)) {
        first = device->read(kEdgeBytes);
    }
    if (device->seek(volume.size - kEdgeBytes)) {
        last = device->read(kEdgeBytes);
    }
    return {first, last};
}

}  // namespace

class RawPartitionWriteTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void refusedWhileSomethingHasItOpen_data();
    void refusedWhileSomethingHasItOpen();
    void nativeWriterReachesTheWholePartition_data();
    void nativeWriterReachesTheWholePartition();
    void scriptWriterReachesTheWholePartition_data();
    void scriptWriterReachesTheWholePartition();
    void scriptReleaseAndClearTakeThePartition_data();
    void scriptReleaseAndClearTakeThePartition();
    void formatRoundTripSetsAndRestoresThePartitionType();
    void restoreImageThroughTheGeneratedScript();
    void resizeAsDataGivesAnExtPartitionItsTypeBack();
    void hfsReadOnlyCheckStagesAndWritesNothing();
    void hideOnMbrHidesOnlyThatPartition();
    void readOnlyCheckWithoutASnapshotLocksAndWritesNothing();

private:
    void addVolumeRows();
    /// Quick-format @p volume, keeping (or giving it) its letter or removing every letter, so a
    /// test starts from a file system Windows mounts. Prints FORMATTED when done.
    QString format(const PartitionVolume& volume, bool keepLetter);
    /// Attach @p volume's new VHDX, give it one MBR partition on its letter and format it. Prints
    /// FORMATTED when done.
    QString makeVolume(const PartitionVolume& volume);
    /// Attach @p volume's new VHDX, give it one NTFS partition on GPT at its letter, then the
    /// app's own HFS+ Format of it, as the panel queues it. Prints HFS FORMATTED when done.
    QString makeHfsVolume(PartitionVolume* volume);
    std::unique_ptr<QTemporaryDir> m_work;
    QString m_vhds;  ///< the virtual disks' folder
    QList<PartitionVolume> m_volumes;
};

void RawPartitionWriteTests::initTestCase() {
    SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED();
    if (!processIsElevated()) {
        QSKIP(
            "Makes and mounts NTFS, FAT32 and exFAT virtual disks, which needs administrator "
            "rights: CI's elevated step runs it");
    }
    m_work = std::make_unique<QTemporaryDir>();
    QVERIFY(m_work->isValid());
    // The virtual disks get a folder of their own under TEMP, removed at the end.
    m_vhds = QDir::toNativeSeparators(
        QDir::temp().filePath(QStringLiteral("sak-raw-vhd-") + sak::no_follow::randomToken()));
    QVERIFY(QDir().mkpath(m_vhds));
    const QStringList letters = freeDriveLetters(3);
    QCOMPARE(letters.size(), 3);
    const QList<QPair<QString, QString>> kinds = {
        {QStringLiteral("NTFS"), QStringLiteral("ntfs")},
        {QStringLiteral("FAT32"), QStringLiteral("fat32")},
        {QStringLiteral("exFAT"), QStringLiteral("exfat")}};
    for (int i = 0; i < kinds.size(); ++i) {
        PartitionVolume volume;
        volume.name = kinds[i].first;
        volume.diskpartFs = kinds[i].second;
        volume.letter = letters[i];
        volume.vhd = QDir::toNativeSeparators(
            QDir(m_vhds).filePath(QStringLiteral("raw-%1.vhdx").arg(kinds[i].second)));
        // Listed before anything is checked, so cleanupTestCase detaches it even on a failure.
        m_volumes.append(volume);
        // Two statements, not one sum: C++ leaves the order of a sum's operands open, and MSVC
        // ran makeVolume first, before the disk existed ("The system cannot find the file").
        QString out =
            diskpart(*m_work,
                     {QStringLiteral("create vdisk file=\"%1\" maximum=%2 type=expandable")
                          .arg(volume.vhd)
                          .arg(kVirtualDiskMiB)});
        out += makeVolume(volume);
        QVERIFY2(out.contains(QStringLiteral("FORMATTED")), qPrintable(out));
        QVERIFY2(partitionOfLetter(letters[i], &m_volumes.last()), qPrintable(out));
        QVERIFY2(m_volumes.last().size > 2 * kEdgeBytes, qPrintable(out));
    }
}

void RawPartitionWriteTests::cleanupTestCase() {
    if (!m_work) {
        return;
    }
    for (const PartitionVolume& volume : m_volumes) {
        (void)diskpart(*m_work,
                       {QStringLiteral("select vdisk file=\"%1\"").arg(volume.vhd),
                        QStringLiteral("detach vdisk")});
    }
    if (!m_vhds.isEmpty()) {
        (void)sak::no_follow::removeTree(m_vhds);
    }
}

void RawPartitionWriteTests::addVolumeRows() {
    QTest::addColumn<int>("index");
    QTest::addColumn<bool>("keepLetter");
    for (int i = 0; i < m_volumes.size(); ++i) {
        // The app's case is a partition with no letter; exFAT keeps its letter, so the writer is
        // also seen taking a volume that Explorer can reach.
        QTest::newRow(qPrintable(m_volumes[i].name))
            << i << (m_volumes[i].name == QStringLiteral("exFAT"));
    }
}

QString RawPartitionWriteTests::makeVolume(const PartitionVolume& volume) {
    // The Storage cmdlets, in one PowerShell.
    const sak::ProcessResult result = sak::runPowerShell(
        QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                       "Mount-DiskImage -ImagePath %1 | Out-Null\n"
                       "$disk = Get-DiskImage -ImagePath %1 | Get-Disk\n"
                       "Initialize-Disk -Number $disk.Number -PartitionStyle MBR\n"
                       "$p = New-Partition -DiskNumber $disk.Number -UseMaximumSize "
                       "-DriveLetter %2\n"
                       "Format-Volume -Partition $p -FileSystem %3 -NewFileSystemLabel SAKRAW "
                       "-Force -Confirm:$false | Out-Null\n"
                       "'FORMATTED'\n")
            .arg(sak::PartitionScriptBuilder::quotePowerShell(volume.vhd),
                 volume.letter,
                 volume.name),
        kDiskpartTimeoutMs);
    return result.std_out + result.std_err;
}

QString RawPartitionWriteTests::format(const PartitionVolume& volume, bool keepLetter) {
    // Format-Volume, as makeVolume does.
    const QString letterStep =
        keepLetter ? QStringLiteral(
                         "if ($p.DriveLetter -ne '%1') { Set-Partition -InputObject $p "
                         "-NewDriveLetter %1 }\n")
                         .arg(volume.letter)
                   : QStringLiteral(
                         "foreach ($a in @($p.AccessPaths | Where-Object { $_ -match "
                         "'^[A-Z]:\\\\$' })) { Remove-PartitionAccessPath -InputObject "
                         "$p -AccessPath $a }\n");
    const sak::ProcessResult result = sak::runPowerShell(
        QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                       "Update-HostStorageCache\n"
                       "$p = Get-Partition -DiskNumber %1 -PartitionNumber %2\n"
                       "Format-Volume -Partition $p -FileSystem %3 -NewFileSystemLabel SAKRAW "
                       "-Force -Confirm:$false | Out-Null\n"
                       "$p = Get-Partition -DiskNumber %1 -PartitionNumber %2\n")
                .arg(volume.disk)
                .arg(volume.partitionNumber)
                .arg(volume.name) +
            letterStep + QStringLiteral("'FORMATTED'\n"),
        kDiskpartTimeoutMs);
    return result.std_out + result.std_err;
}

void RawPartitionWriteTests::refusedWhileSomethingHasItOpen_data() {
    addVolumeRows();
}

// A file held open on the volume: both writers refuse the partition and say why, and nothing on
// it changes.
void RawPartitionWriteTests::refusedWhileSomethingHasItOpen() {
    QFETCH(int, index);
    const PartitionVolume& volume = m_volumes.at(index);
    QFile held(volume.letter + QStringLiteral(":/held.txt"));
    QVERIFY(held.open(QIODevice::ReadWrite));
    held.write("in use");
    held.flush();

    QString error;
    const auto device = sak::openFileOrRawDeviceReadWrite(volume.partition, &error);
    QVERIFY(!device);
    QVERIFY2(error.contains(QStringLiteral("in use")), qPrintable(error));

    const sak::ProcessResult result = sak::runPowerShell(
        sak::PartitionScriptBuilder::scriptTypesLoaderScript() +
            QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                           "try { $s = [Sak.RawPartitionWrite]::Open(%1); $s.Dispose(); 'OPENED' } "
                           "catch { 'REFUSED ' + $_.Exception.Message }\n")
                .arg(sak::PartitionScriptBuilder::quotePowerShell(volume.partition)),
        kScriptTimeoutMs);
    const QString out = (result.std_out + result.std_err).trimmed();
    QVERIFY2(out.contains(QStringLiteral("REFUSED")) && out.contains(QStringLiteral("in use")),
             qPrintable(out));
    held.close();
    QVERIFY(QFile::exists(volume.letter + QStringLiteral(":/held.txt")));
}

void RawPartitionWriteTests::nativeWriterReachesTheWholePartition_data() {
    addVolumeRows();
}

// The C++ writer, which the APFS writer and the HFS+ file writer open targets with. Its flush on
// FAT32 and exFAT goes the same way as the scripts' writer: "The device is not ready" is accepted
// only after every written block reads back as written.
void RawPartitionWriteTests::nativeWriterReachesTheWholePartition() {
    QFETCH(int, index);
    QFETCH(bool, keepLetter);
    const PartitionVolume& volume = m_volumes.at(index);
    const QString formatted = format(volume, keepLetter);
    QVERIFY2(formatted.contains(QStringLiteral("FORMATTED")), qPrintable(formatted));
    {
        QString error;
        const auto device = sak::openFileOrRawDeviceReadWrite(volume.partition, &error);
        QVERIFY2(device, qPrintable(error));
        QVERIFY(device->seek(0));
        QCOMPARE(device->write(pattern('\x5A')), kEdgeBytes);
        QVERIFY(device->seek(volume.size - kEdgeBytes));
        QCOMPARE(device->write(pattern('\xA5')), kEdgeBytes);
        QVERIFY2(sak::flushDeviceBuffers(device.get(), &error), qPrintable(error));
    }
    QString error;
    const auto edges = readEdges(volume, &error);
    QVERIFY2(edges.first == pattern('\x5A'), qPrintable(error));
    QVERIFY2(edges.second == pattern('\xA5'), qPrintable(error));
}

void RawPartitionWriteTests::scriptWriterReachesTheWholePartition_data() {
    addVolumeRows();
}

// The scripts' writer, which HFS+ Format, Repair and file changes copy the staged image with.
// FileStream does not notice a short write, so only the read-back can tell. On FAT32 and exFAT
// the flush after the dismount answers "The device is not ready"; the writer accepts that one
// answer only when every block it wrote reads back as written (it throws otherwise), so the
// FAT32 and exFAT rows pass through that check, and this read-back confirms it independently.
void RawPartitionWriteTests::scriptWriterReachesTheWholePartition() {
    QFETCH(int, index);
    QFETCH(bool, keepLetter);
    const PartitionVolume& volume = m_volumes.at(index);
    const QString formatted = format(volume, keepLetter);
    QVERIFY2(formatted.contains(QStringLiteral("FORMATTED")), qPrintable(formatted));
    const sak::ProcessResult result = sak::runPowerShell(
        sak::PartitionScriptBuilder::scriptTypesLoaderScript() +
            QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                           "$s = [Sak.RawPartitionWrite]::Open(%1)\n"
                           "try {\n"
                           "  $first = New-Object byte[] %2\n"
                           "  for ($i = 0; $i -lt $first.Length; $i++) { $first[$i] = 0x3C }\n"
                           "  $last = New-Object byte[] %2\n"
                           "  for ($i = 0; $i -lt $last.Length; $i++) { $last[$i] = 0xC3 }\n"
                           "  [void]$s.Seek(0, 'Begin'); $s.Write($first, 0, $first.Length)\n"
                           "  [void]$s.Seek(%3, 'Begin'); $s.Write($last, 0, $last.Length)\n"
                           "  $s.Flush($true)\n"
                           "} finally { $s.Dispose() }\n"
                           "'WROTE'\n")
                .arg(sak::PartitionScriptBuilder::quotePowerShell(volume.partition))
                .arg(kEdgeBytes)
                .arg(volume.size - kEdgeBytes),
        kScriptTimeoutMs);
    const QString out = (result.std_out + result.std_err).trimmed();
    QVERIFY2(printedLine(result, QStringLiteral("WROTE")), qPrintable(out));
    QString error;
    const auto edges = readEdges(volume, &error);
    QVERIFY2(edges.first == pattern('\x3C'), qPrintable(error));
    QVERIFY2(edges.second == pattern(static_cast<char>(0xC3)), qPrintable(error));
}

void RawPartitionWriteTests::scriptReleaseAndClearTakeThePartition_data() {
    addVolumeRows();
}

// The two calls the generated scripts make before a bundled tool or the APFS writer opens a
// partition (in place of Dismount-Volume, which Windows PowerShell 5.1 does not have): Release is
// refused while a file on the volume is open, and ClearFileSystem leaves the first and the last
// MiB zero, so Windows finds no file system there when the tool opens it.
void RawPartitionWriteTests::scriptReleaseAndClearTakeThePartition() {
    QFETCH(int, index);
    const PartitionVolume& volume = m_volumes.at(index);
    const QString formatted = format(volume, true);
    QVERIFY2(formatted.contains(QStringLiteral("FORMATTED")), qPrintable(formatted));
    QFile held(volume.letter + QStringLiteral(":/held.txt"));
    QVERIFY(held.open(QIODevice::ReadWrite));
    held.write("in use");
    held.flush();
    const sak::ProcessResult refused = sak::runPowerShell(
        sak::PartitionScriptBuilder::scriptTypesLoaderScript() +
            QStringLiteral("try { [Sak.RawPartitionWrite]::Release(%1); 'RELEASED' } catch { "
                           "'REFUSED ' + $_.Exception.Message }\n")
                .arg(sak::PartitionScriptBuilder::quotePowerShell(volume.partition)),
        kScriptTimeoutMs);
    const QString refused_out = (refused.std_out + refused.std_err).trimmed();
    QVERIFY2(refused_out.contains(QStringLiteral("REFUSED")) &&
                 refused_out.contains(QStringLiteral("in use")),
             qPrintable(refused_out));
    held.close();
    const sak::ProcessResult cleared = sak::runPowerShell(
        sak::PartitionScriptBuilder::scriptTypesLoaderScript() +
            // Stop: under the default Continue a refused ClearFileSystem fails only its own
            // statement, and 'CLEARED' still printed (testpc1, 2026-11-11).
            QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                           "[Sak.RawPartitionWrite]::ClearFileSystem(%1, %2); 'CLEARED'\n")
                .arg(sak::PartitionScriptBuilder::quotePowerShell(volume.partition))
                .arg(volume.size),
        kScriptTimeoutMs);
    const QString cleared_out = (cleared.std_out + cleared.std_err).trimmed();
    if (!printedLine(cleared, QStringLiteral("CLEARED"))) {
        // Refused: say how long whatever holds the partition keeps it, so the failure says
        // whether the writer waits too little or something never lets go.
        QElapsedTimer held_for;
        held_for.start();
        QString later = QStringLiteral("still refused after %1 s").arg(kHeldWaitMs / 1000);
        while (held_for.elapsed() < kHeldWaitMs) {
            const sak::ProcessResult retry = sak::runPowerShell(
                sak::PartitionScriptBuilder::scriptTypesLoaderScript() +
                    QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                                   "[Sak.RawPartitionWrite]::Release(%1); 'RELEASED'\n")
                        .arg(sak::PartitionScriptBuilder::quotePowerShell(volume.partition)),
                kScriptTimeoutMs);
            if (printedLine(retry, QStringLiteral("RELEASED"))) {
                later = QStringLiteral("taken %1 ms after the refusal").arg(held_for.elapsed());
                break;
            }
        }
        QFAIL(
            qPrintable(QStringLiteral("ClearFileSystem refused (%1): %2").arg(later, cleared_out)));
    }
    QString error;
    const auto edges = readEdges(volume, &error);
    const QByteArray zeros(kEdgeBytes, '\0');
    if (edges.first != zeros || edges.second != zeros) {
        // Say what is there, and whether it is still there a moment later.
        QTest::qWait(kSettleMs);
        QString again_error;
        const auto again = readEdges(volume, &again_error);
        QFAIL(qPrintable(QStringLiteral("%1 | first MiB: %2; 3 s later: %3 | last MiB: %4; 3 s "
                                        "later: %5 | %6 %7")
                             .arg(cleared_out,
                                  describeZeroEdge(edges.first),
                                  describeZeroEdge(again.first),
                                  describeZeroEdge(edges.second),
                                  describeZeroEdge(again.second),
                                  error,
                                  again_error)));
    }
}

namespace {

/// The app's own Format script for @p volume's partition, as the panel queues it.
sak::PartitionScript formatScript(const PartitionVolume& volume,
                                  const QString& file_system,
                                  bool non_windows) {
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Partition;
    target.disk_number = sak::DiskNumber{volume.disk};
    target.partition_number = sak::PartitionNumber{volume.partitionNumber};
    target.size_bytes = static_cast<uint64_t>(volume.size);
    QJsonObject payload;
    payload[QStringLiteral("file_system")] = file_system;
    payload[QStringLiteral("label")] = QStringLiteral("SAKTRIP");
    if (non_windows) {
        payload[QStringLiteral("non_native_file_system_tool")] = true;
        payload[QStringLiteral("target_path")] = volume.partition;
        payload[QStringLiteral("target_wipe_confirmed")] = true;
    }
    return sak::PartitionScriptBuilder().buildScript(sak::PartitionOperationPlanner::makeOperation(
        sak::PartitionOperationType::Format, target, payload));
}

/// "<MBR type> <LETTER|NOLETTER> <file system or NOVOLUME>" for @p volume's partition now.
QString partitionState(const PartitionVolume& volume) {
    const sak::ProcessResult state = sak::runPowerShell(
        QStringLiteral("$p = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop\n"
                       "$v = $null\n"
                       "try { $v = $p | Get-Volume -ErrorAction Stop } catch { $v = $null }\n"
                       "'STATE ' + [int]$p.MbrType + ' ' + $(if ([string]$p.DriveLetter -match "
                       "'^[A-Za-z]$') { 'LETTER' } else { 'NOLETTER' }) + ' ' + $(if ($v) { "
                       "$v.FileSystem } else { 'NOVOLUME' })\n")
            .arg(volume.disk)
            .arg(volume.partitionNumber),
        kScriptTimeoutMs);
    const QStringList lines = state.std_out.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.trimmed().startsWith(QStringLiteral("STATE "))) {
            return line.trimmed().mid(QStringLiteral("STATE ").size());
        }
    }
    return state.std_out + state.std_err;
}

/// The Restore Image the panel queues: @p image_path (holding @p image) onto the whole disk
/// @p disk_fields names (Number|UniqueId|SerialNumber|Size), its identity pinned as the panel pins
/// it.
sak::PartitionOperation restoreImageOperation(const QStringList& disk_fields,
                                              const QString& image_path,
                                              const QByteArray& image) {
    const uint64_t disk_size = disk_fields[3].toULongLong();
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Disk;
    target.disk_number = sak::DiskNumber{disk_fields[0].toUInt()};
    target.size_bytes = disk_size;
    target.disk_unique_id = disk_fields[1].trimmed();
    target.disk_serial_number = disk_fields[2].trimmed();
    target.disk_size_bytes = disk_size;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = image_path;
    payload[QStringLiteral("target_path")] =
        QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk_fields[0]);
    payload[QStringLiteral("source_size_bytes")] = QString::number(image.size());
    payload[QStringLiteral("target_size_bytes")] = QString::number(disk_size);
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    payload[QStringLiteral("overwrite_confirmed")] = true;
    payload[QStringLiteral("source_sha256")] =
        QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex());
    payload[QStringLiteral("target_disk_unique_id")] = target.disk_unique_id;
    payload[QStringLiteral("target_disk_serial_number")] = target.disk_serial_number;
    payload[QStringLiteral("target_disk_size_bytes")] = QString::number(disk_size);
    return sak::PartitionOperationPlanner::makeOperation(sak::PartitionOperationType::RestoreImage,
                                                         target,
                                                         payload);
}

}  // namespace

// Owner ruling 2026-11-11 (through the coordinator): a partition S.A.K. formats with a file system
// Windows does not mount loses its letters and takes that file system's partition type, so
// Windows never offers to format it; Windows Format then sets a Windows data type back first,
// since Format-Volume finds no volume on a Linux or Apple type (measured on testpc1). The app's
// own two Format scripts, ext4 then NTFS, on the same lettered MBR partition: 0x83 and no letter
// after the first, NTFS (0x07) after the second.
void RawPartitionWriteTests::formatRoundTripSetsAndRestoresThePartitionType() {
    const PartitionVolume& volume = m_volumes.at(0);
    const QString formatted = format(volume, true);
    QVERIFY2(formatted.contains(QStringLiteral("FORMATTED")), qPrintable(formatted));
    QCOMPARE(partitionState(volume), QStringLiteral("7 LETTER NTFS"));

    const sak::PartitionScript ext4 = formatScript(volume, QStringLiteral("ext4"), true);
    QVERIFY2(ext4.valid(), qPrintable(ext4.blockers.join(QStringLiteral("; "))));
    const sak::ProcessResult made = sak::runPowerShell(ext4.script, kToolScriptTimeoutMs);
    QVERIFY2(made.exit_code == 0, qPrintable(made.std_out + made.std_err));
    QCOMPARE(partitionState(volume), QStringLiteral("131 NOLETTER NOVOLUME"));
    QString error;
    auto device = sak::openFileOrRawDeviceReadOnly(volume.partition, &error);
    QVERIFY2(device, qPrintable(error));
    QVERIFY(device->seek(kExtSuperblockMagicOffset));
    QCOMPARE(device->read(2), QByteArray("\x53\xEF", 2));
    device.reset();

    const sak::PartitionScript ntfs = formatScript(volume, QStringLiteral("NTFS"), false);
    QVERIFY2(ntfs.valid(), qPrintable(ntfs.blockers.join(QStringLiteral("; "))));
    const sak::ProcessResult back = sak::runPowerShell(ntfs.script, kToolScriptTimeoutMs);
    QVERIFY2(back.exit_code == 0, qPrintable(back.std_out + back.std_err));
    QVERIFY2(back.std_out.contains(QStringLiteral("set it back to a Windows data partition")),
             qPrintable(back.std_out));
    QVERIFY2(partitionState(volume).startsWith(QStringLiteral("7 ")) &&
                 partitionState(volume).endsWith(QStringLiteral(" NTFS")),
             qPrintable(partitionState(volume)));
}

// Opsproof F20 (2026-11-12): every Restore Image stopped at once with "Unable to find type
// [SakLockedFilePath]", the script naming a type before loading the DLL that defines it. The
// app's own Restore Image script, image file to a whole virtual disk with its identity pinned as
// the panel pins it; the disk's first bytes then read back as the image.
void RawPartitionWriteTests::restoreImageThroughTheGeneratedScript() {
    const QString vhd =
        QDir::toNativeSeparators(QDir(m_vhds).filePath(QStringLiteral("restore.vhdx")));
    const QString made =
        diskpart(*m_work,
                 {QStringLiteral("create vdisk file=\"%1\" maximum=%2 type=expandable")
                      .arg(vhd)
                      .arg(kVirtualDiskMiB),
                  QStringLiteral("select vdisk file=\"%1\"").arg(vhd),
                  QStringLiteral("attach vdisk")});
    const auto detach = qScopeGuard([this, &vhd] {
        (void)diskpart(*m_work,
                       {QStringLiteral("select vdisk file=\"%1\"").arg(vhd),
                        QStringLiteral("detach vdisk")});
    });
    const sak::ProcessResult found = sak::runPowerShell(
        QStringLiteral("$d = Get-DiskImage -ImagePath %1 | Get-Disk\n"
                       "'DISK ' + $d.Number + '|' + $d.UniqueId + '|' + $d.SerialNumber + '|' + "
                       "$d.Size\n")
            .arg(sak::PartitionScriptBuilder::quotePowerShell(vhd)),
        kScriptTimeoutMs);
    const QString line = found.std_out.section(QStringLiteral("DISK "), 1)
                             .section(QLatin1Char('\n'), 0, 0)
                             .trimmed();
    const QStringList fields = line.split(QLatin1Char('|'));
    QVERIFY2(fields.size() == 4, qPrintable(made + found.std_out + found.std_err));
    const uint32_t disk = fields[0].toUInt();
    QVERIFY2(disk > 0 && fields[3].toULongLong() > 0, qPrintable(line));

    const QByteArray image = pattern('\x6B') + pattern('\x2D');
    const QString image_path =
        QDir::toNativeSeparators(QDir(m_work->path()).filePath(QStringLiteral("restore.img")));
    QFile image_file(image_path);
    QVERIFY(image_file.open(QIODevice::WriteOnly));
    QCOMPARE(image_file.write(image), image.size());
    image_file.close();

    const sak::PartitionScript script =
        sak::PartitionScriptBuilder().buildScript(restoreImageOperation(fields, image_path, image));
    QVERIFY2(script.valid(), qPrintable(script.blockers.join(QStringLiteral("; "))));
    const sak::ProcessResult restored = sak::runPowerShell(script.script, kToolScriptTimeoutMs);
    QVERIFY2(restored.exit_code == 0, qPrintable(restored.std_out + restored.std_err));

    QString error;
    auto device = sak::openFileOrRawDeviceReadOnly(
        QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk), &error);
    QVERIFY2(device, qPrintable(error));
    QVERIFY(device->seek(0));
    QVERIFY2(device->read(image.size()) == image, "the disk does not read back as the image");
}

namespace {

constexpr auto kLinuxGptType = "{0fc63daf-8483-4772-8e79-3d69d8477de4}";
constexpr qint64 kResizeStartBytes = 16LL * 1024 * 1024;
constexpr qint64 kResizeGrownBytes = 32LL * 1024 * 1024;
constexpr qint64 kResizeRefusedBytes = 1024LL * 1024 * 1024;  ///< more than the disk holds
constexpr qint64 kHfsHeadBytes = 4096;  ///< the sectors that hold the volume header
constexpr qint64 kHfsHeaderOffset = 1024;
constexpr qint64 kHfsFileCountOffset = kHfsHeaderOffset + 32;   ///< fileCount, big-endian
constexpr qint64 kHfsFreeBlocksOffset = kHfsHeaderOffset + 48;  ///< freeBlocks, big-endian

/// The text after @p prefix on the first line of @p out that starts with it, or "" when none does.
QString lineAfter(const QString& out, const QString& prefix) {
    for (const QString& line : out.split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(prefix)) {
            return line.trimmed().mid(prefix.size());
        }
    }
    return {};
}

/// PowerShell: a line "<tag> <GPT type> <no-default-letter> <hidden> <read-only> <size> [<paths
/// other than its \\?\Volume{...}\ path>]" for $sakDisk's partition $sakPart, as Windows reads it.
QString partitionFactsLine(const QString& tag) {
    return QStringLiteral(
               "Update-HostStorageCache\n"
               "$q = Get-Partition -DiskNumber $sakDisk -PartitionNumber $sakPart\n"
               "'%1 ' + \"$($q.GptType)\".ToLowerInvariant() + ' ' + [bool]$q.NoDefaultDriveLetter "
               "+ "
               "' ' + [bool]$q.IsHidden + ' ' + [bool]$q.IsReadOnly + ' ' + $q.Size + ' [' + "
               "((@($q.AccessPaths) | Where-Object { $_ -and -not "
               "\"$_\".StartsWith('\\\\?\\Volume{') "
               "}) -join ',') + ']'\n")
        .arg(tag);
}

/// The app's resize-as-data function on @p disk's partition @p number, to @p size bytes: "DONE" or
/// "THREW <message>" after RESULT, and the partition's facts after STATE (partitionFactsLine).
QString resizeAsData(uint32_t disk, uint32_t number, qint64 size) {
    const sak::ProcessResult run = sak::runPowerShell(
        QStringLiteral("$ErrorActionPreference = 'Stop'\n$sakDisk = %1\n$sakPart = %2\n")
                .arg(disk)
                .arg(number) +
            sak::PartitionScriptBuilder::resizeAsDataPartitionFunctionScript() +
            QStringLiteral("try { Invoke-SakResizeAsDataPartition -Disk %1 -Number %2 -Size %3 | "
                           "Out-Null; 'RESULT DONE' } catch { 'RESULT THREW ' + "
                           "$_.Exception.Message }\n")
                .arg(disk)
                .arg(number)
                .arg(size) +
            partitionFactsLine(QStringLiteral("STATE")),
        kScriptTimeoutMs);
    return run.std_out + run.std_err;
}

/// @p facts (partitionFactsLine, without its tag) with the size field set to @p size.
QString withSize(const QString& facts, qint64 size) {
    QStringList fields = facts.split(QLatin1Char(' '));
    constexpr qsizetype kSizeField = 4;
    if (fields.size() > kSizeField) {
        fields[kSizeField] = QString::number(size);
    }
    return fields.join(QLatin1Char(' '));
}

/// SHA-256 of @p volume's whole partition, read through the read-only opener; empty on a failure.
QByteArray partitionHash(const PartitionVolume& volume, QString* error) {
    auto device = sak::openFileOrRawDeviceReadOnly(volume.partition, error);
    if (!device || !device->seek(0)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = volume.size;
    while (remaining > 0) {
        const QByteArray block = device->read(std::min(remaining, kEdgeBytes));
        if (block.isEmpty()) {
            *error = QStringLiteral("the partition read ended %1 bytes short").arg(remaining);
            return {};
        }
        hash.addData(block);
        remaining -= block.size();
    }
    return hash.result();
}

/// The app's own read-only check of @p volume's partition as HFS+, as Check Non-Windows File
/// System has the helper build it.
sak::PartitionScript hfsReadOnlyCheckScript(const PartitionVolume& volume) {
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Partition;
    target.disk_number = sak::DiskNumber{volume.disk};
    target.partition_number = sak::PartitionNumber{volume.partitionNumber};
    target.size_bytes = static_cast<uint64_t>(volume.size);
    const QJsonObject payload{{QStringLiteral("non_native_file_system_tool"), true},
                              {QStringLiteral("read_only_check"), true},
                              {QStringLiteral("file_system"), QStringLiteral("HFS+")},
                              {QStringLiteral("target_path"), volume.partition}};
    return sak::PartitionScriptBuilder().buildScript(sak::PartitionOperationPlanner::makeOperation(
        sak::PartitionOperationType::CheckFileSystem, target, payload));
}

/// The staged images a check left in @p folder.
QStringList stagedCheckImages(const QString& folder) {
    return QDir(folder).entryList({QStringLiteral("sak-hfs-check-*")}, QDir::Files);
}

/// Damages @p volume's HFS+ volume header: its file count and free-block count no longer match the
/// volume. A raw partition is written in whole sectors: the first 4 KiB, changed in memory.
bool damageHfsHeader(const PartitionVolume& volume, QString* error) {
    auto device = sak::openFileOrRawDeviceReadWrite(volume.partition, error);
    if (!device || !device->seek(0)) {
        return false;
    }
    QByteArray head = device->read(kHfsHeadBytes);
    if (head.size() != kHfsHeadBytes || head.mid(kHfsHeaderOffset, 2) != QByteArray("H+")) {
        *error = QStringLiteral("no HFS+ volume header (%1)")
                     .arg(QString::fromLatin1(head.mid(kHfsHeaderOffset, 2).toHex()));
        return false;
    }
    head.replace(kHfsFileCountOffset, 4, QByteArray("\xFF\xFF\xFF\xF0", 4));
    head.replace(kHfsFreeBlocksOffset, 4, QByteArray(4, '\0'));
    return device->seek(0) && device->write(head) == kHfsHeadBytes &&
           sak::flushDeviceBuffers(device.get(), error);
}

}  // namespace

// Opsproof F27 (merged build 9170b149): every ext Resize on GPT failed, "Windows did not give the
// partition its type back ... This operation is only supported on data partitions", the partition
// unchanged. The resize-as-data function set GPT's no-default-letter attribute before the data
// type, and Windows sets that attribute on a data partition only (StorageWMI 42011); the unit
// test's stand-in took it on any type, so only Windows could show it. A Linux-typed GPT partition
// on a VHDX, as S.A.K.'s ext Format leaves one (no letter, the attribute set), goes through the
// app's own function: grown, then refused a size the disk cannot hold; each time its type,
// attributes, letters and size are read back.
void RawPartitionWriteTests::resizeAsDataGivesAnExtPartitionItsTypeBack() {
    const QString vhd =
        QDir::toNativeSeparators(QDir(m_vhds).filePath(QStringLiteral("resize.vhdx")));
    const QString made =
        diskpart(*m_work,
                 {QStringLiteral("create vdisk file=\"%1\" maximum=%2 type=expandable")
                      .arg(vhd)
                      .arg(kVirtualDiskMiB),
                  QStringLiteral("select vdisk file=\"%1\"").arg(vhd),
                  QStringLiteral("attach vdisk")});
    const auto detach = qScopeGuard([this, &vhd] {
        (void)diskpart(*m_work,
                       {QStringLiteral("select vdisk file=\"%1\"").arg(vhd),
                        QStringLiteral("detach vdisk")});
    });
    const sak::ProcessResult setup = sak::runPowerShell(
        QStringLiteral(
            "$ErrorActionPreference = 'Stop'\n"
            "$sakDisk = (Get-DiskImage -ImagePath %1 | Get-Disk).Number\n"
            "Initialize-Disk -Number $sakDisk -PartitionStyle GPT\n"
            "$sakPart = (New-Partition -DiskNumber $sakDisk -Size %2).PartitionNumber\n"
            "Set-Partition -DiskNumber $sakDisk -PartitionNumber $sakPart -NoDefaultDriveLetter "
            "$true\n"
            "Set-Partition -DiskNumber $sakDisk -PartitionNumber $sakPart -GptType %3\n"
            "Update-HostStorageCache\n"
            "foreach ($a in @((Get-Partition -DiskNumber $sakDisk -PartitionNumber "
            "$sakPart).AccessPaths | Where-Object { $_ -match '^[A-Z]:\\\\$' })) { "
            "Remove-PartitionAccessPath -DiskNumber $sakDisk -PartitionNumber $sakPart "
            "-AccessPath $a }\n"
            "'MADE ' + $sakDisk + ' ' + $sakPart\n")
                .arg(sak::PartitionScriptBuilder::quotePowerShell(vhd))
                .arg(kResizeStartBytes)
                .arg(sak::PartitionScriptBuilder::quotePowerShell(
                    QString::fromLatin1(kLinuxGptType))) +
            partitionFactsLine(QStringLiteral("BEFORE")),
        kScriptTimeoutMs);
    const QString setup_out = made + setup.std_out + setup.std_err;
    const QStringList numbers = lineAfter(setup_out, QStringLiteral("MADE ")).split(u' ');
    const QString before = lineAfter(setup_out, QStringLiteral("BEFORE "));
    QVERIFY2(numbers.size() == 2 && numbers[0].toUInt() > 0 && !before.isEmpty(),
             qPrintable(setup_out));
    const uint32_t disk = numbers[0].toUInt();
    const uint32_t number = numbers[1].toUInt();
    // Windows reads a Linux-typed partition's attribute bits its own way (testpc1: the
    // no-default-letter attribute set before the type change reads False, and the partition reads
    // hidden), so what is checked afterwards is that the partition reads as it did here.
    QVERIFY2(before.startsWith(QString::fromLatin1(kLinuxGptType) + QLatin1Char(' ')) &&
                 before.endsWith(QStringLiteral(" %1 []").arg(kResizeStartBytes)),
             qPrintable(setup_out));

    // Grown: its own type, attributes and letters back, at the new size.
    const QString grown = resizeAsData(disk, number, kResizeGrownBytes);
    QVERIFY2(lineAfter(grown, QStringLiteral("RESULT ")) == QStringLiteral("DONE"),
             qPrintable(grown));
    QVERIFY2(lineAfter(grown, QStringLiteral("STATE ")) == withSize(before, kResizeGrownBytes),
             qPrintable(before + QStringLiteral(" | ") + grown));

    // Refused by Windows: Windows' own reason, and the partition as it was.
    const QString refused = resizeAsData(disk, number, kResizeRefusedBytes);
    const QString result = lineAfter(refused, QStringLiteral("RESULT "));
    QVERIFY2(result.startsWith(QStringLiteral("THREW ")) &&
                 !result.contains(QStringLiteral("did not give the partition its type back")),
             qPrintable(refused));
    QVERIFY2(lineAfter(refused, QStringLiteral("STATE ")) == withSize(before, kResizeGrownBytes),
             qPrintable(before + QStringLiteral(" | ") + refused));
}

QString RawPartitionWriteTests::makeHfsVolume(PartitionVolume* volume) {
    QString out = diskpart(*m_work,
                           {QStringLiteral("create vdisk file=\"%1\" maximum=%2 type=expandable")
                                .arg(volume->vhd)
                                .arg(kVirtualDiskMiB),
                            QStringLiteral("select vdisk file=\"%1\"").arg(volume->vhd),
                            QStringLiteral("attach vdisk")});
    const sak::ProcessResult ntfs = sak::runPowerShell(
        QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                       "$disk = Get-DiskImage -ImagePath %1 | Get-Disk\n"
                       "Initialize-Disk -Number $disk.Number -PartitionStyle GPT\n"
                       "$p = New-Partition -DiskNumber $disk.Number -UseMaximumSize "
                       "-DriveLetter %2\n"
                       "Format-Volume -Partition $p -FileSystem NTFS -NewFileSystemLabel SAKHFS "
                       "-Force -Confirm:$false | Out-Null\n"
                       "'NTFS READY'\n")
            .arg(sak::PartitionScriptBuilder::quotePowerShell(volume->vhd), volume->letter),
        kDiskpartTimeoutMs);
    out += ntfs.std_out + ntfs.std_err;
    if (!out.contains(QStringLiteral("NTFS READY")) || !partitionOfLetter(volume->letter, volume)) {
        return out;
    }
    // The app's own HFS+ Format of it, as the panel queues it.
    const sak::PartitionScript hfs = formatScript(*volume, QStringLiteral("HFS+"), true);
    if (!hfs.valid()) {
        return out + hfs.blockers.join(QStringLiteral("; "));
    }
    const sak::ProcessResult formatted = sak::runPowerShell(hfs.script, kToolScriptTimeoutMs);
    out += formatted.std_out + formatted.std_err;
    if (formatted.exit_code == 0) {
        out += QStringLiteral("\nHFS FORMATTED\n");
    }
    return out;
}

// Opsproof F28 (merged build 9170b149): Check Non-Windows File System failed on every healthy HFS+
// volume, "fsck_hfs -n -f //?/GLOBALROOT/... Device is not a block device ... exit code 8":
// fsck_hfs opens a block device or a file, and the check gave it the partition's path. The check
// now reads the partition into a staged image in the run folder and checks that, writing nothing
// back. An HFS+ partition the app's own Format made on a GPT VHDX, checked through the app's own
// script: healthy passes, a damaged volume header fails, the partition's bytes are the same after
// each check, and no image is left; with the run folder on a FAT32 drive the check refuses in
// plain words before it makes an image.
void RawPartitionWriteTests::hfsReadOnlyCheckStagesAndWritesNothing() {
    // The FAT32 volume, formatted again (an earlier case cleared it), for the refusal.
    PartitionVolume& fat = m_volumes[1];
    QCOMPARE(fat.name, QStringLiteral("FAT32"));
    const QString fat_formatted = format(fat, true);
    QVERIFY2(fat_formatted.contains(QStringLiteral("FORMATTED")), qPrintable(fat_formatted));

    const QStringList letters = freeDriveLetters(1);
    QCOMPARE(letters.size(), 1);
    PartitionVolume volume;
    volume.name = QStringLiteral("HFS+");
    volume.letter = letters.first();
    volume.vhd = QDir::toNativeSeparators(QDir(m_vhds).filePath(QStringLiteral("hfs.vhdx")));
    const auto detach = qScopeGuard([this, &volume] {
        (void)diskpart(*m_work,
                       {QStringLiteral("select vdisk file=\"%1\"").arg(volume.vhd),
                        QStringLiteral("detach vdisk")});
    });
    const QString made = makeHfsVolume(&volume);
    QVERIFY2(made.contains(QStringLiteral("HFS FORMATTED")), qPrintable(made));

    const sak::PartitionScript check = hfsReadOnlyCheckScript(volume);
    QVERIFY2(check.valid(), qPrintable(check.blockers.join(QStringLiteral("; "))));
    const QString temp = QDir::tempPath();
    QString error;

    // Healthy: the check passes, and the partition reads the same.
    const QByteArray healthy = partitionHash(volume, &error);
    QVERIFY2(!healthy.isEmpty(), qPrintable(error));
    const sak::ProcessResult passed = sak::runPowerShell(check.script, kToolScriptTimeoutMs);
    const QString passed_out = passed.std_out + passed.std_err;
    QVERIFY2(passed.exit_code == 0 &&
                 passed_out.contains(QStringLiteral("Filesystem tool exit code: 0")),
             qPrintable(passed_out));
    QCOMPARE(partitionHash(volume, &error), healthy);
    QVERIFY2(stagedCheckImages(temp).isEmpty(), qPrintable(stagedCheckImages(temp).join(u' ')));

    // A damaged volume header: its file count and free-block count no longer match the volume.
    QVERIFY2(damageHfsHeader(volume, &error), qPrintable(error));
    const QByteArray damaged = partitionHash(volume, &error);
    QVERIFY2(!damaged.isEmpty() && damaged != healthy, qPrintable(error));
    const sak::ProcessResult failed = sak::runPowerShell(check.script, kToolScriptTimeoutMs);
    const QString failed_out = failed.std_out + failed.std_err;
    QVERIFY2(failed.exit_code != 0 &&
                 failed_out.contains(QStringLiteral("Filesystem tool exit code: ")) &&
                 !failed_out.contains(QStringLiteral("Filesystem tool exit code: 0")),
             qPrintable(failed_out));
    QCOMPARE(partitionHash(volume, &error), damaged);
    QVERIFY2(stagedCheckImages(temp).isEmpty(), qPrintable(stagedCheckImages(temp).join(u' ')));

    // The run folder on a FAT32 drive: refused in plain words, before an image is made.
    const QString fat_root = fat.letter + QStringLiteral(":\\");
    const sak::ProcessResult on_fat =
        sak::runPowerShell(QStringLiteral("$env:TMP = %1\n$env:TEMP = %1\n")
                                   .arg(sak::PartitionScriptBuilder::quotePowerShell(fat_root)) +
                               check.script,
                           kToolScriptTimeoutMs);
    const QString on_fat_out = on_fat.std_out + on_fat.std_err;
    QVERIFY2(on_fat.exit_code != 0 &&
                 on_fat_out.contains(QStringLiteral("FAT32 has no sparse files")) &&
                 on_fat_out.contains(QStringLiteral("Run S.A.K. from an NTFS-formatted drive")),
             qPrintable(on_fat_out));
    QCOMPARE(partitionHash(volume, &error), damaged);
    QVERIFY2(stagedCheckImages(fat_root).isEmpty(),
             qPrintable(stagedCheckImages(fat_root).join(u' ')));
}

namespace {

/// PowerShell: a line "P<n> <hidden> <letter or -> <MBR type, hex>" for partitions 1 and 2 of
/// $sakDisk, as Windows reads them.
QString mbrPartitionsFactsScript() {
    return QStringLiteral(
        "Update-HostStorageCache\n"
        "foreach ($n in 1, 2) { $q = Get-Partition -DiskNumber $sakDisk -PartitionNumber $n; 'P' "
        "+ $n + ' ' + [bool]$q.IsHidden + ' ' + $(if ([string]$q.DriveLetter -match "
        "'^[A-Za-z]$') { [string]$q.DriveLetter } else { '-' }) + ' ' + ('{0:X2}' -f "
        "[int]$q.MbrType) }\n");
}

/// The app's own Hide (@p hidden) or Unhide script for partition @p number of @p disk, with
/// its identity pinned as the panel pins it.
sak::PartitionScript hideScript(uint32_t disk,
                                uint32_t number,
                                const QStringList& identity,
                                bool hidden) {
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Partition;
    target.disk_number = sak::DiskNumber{disk};
    target.partition_number = sak::PartitionNumber{number};
    target.offset_bytes = identity.value(0).toULongLong();
    target.size_bytes = identity.value(1).toULongLong();
    return sak::PartitionScriptBuilder().buildScript(sak::PartitionOperationPlanner::makeOperation(
        sak::PartitionOperationType::SetPartitionHidden,
        target,
        QJsonObject{{QStringLiteral("hidden"), hidden}}));
}

}  // namespace

// Opsproof F30 (testpc2, merged build 9170b149): on an MBR disk, Hide hid every partition on the
// disk and took all their letters: Set-Partition -IsHidden and -NoDefaultDriveLetter are the whole
// disk's attributes there. One MBR partition is hidden by its type's hidden form (0x07 -> 0x17).
// The app's own Hide and Unhide scripts on partition 1 of a two-partition MBR VHDX: partition 1 is
// hidden with no letter while partition 2 keeps its letter and stays visible, and Unhide gives
// partition 1 its type and a letter back.
namespace {

/// Check File System's script for the volume at @p letter, built as the panel queues it.
sak::PartitionScript checkFileSystemScript(const QString& letter, qint64 size_bytes) {
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Partition;
    target.disk_number = sak::DiskNumber{0};
    target.partition_number = sak::PartitionNumber{1};
    target.drive_letter = letter;
    target.size_bytes = static_cast<uint64_t>(size_bytes);
    return sak::PartitionScriptBuilder().buildScript(sak::PartitionOperationPlanner::makeOperation(
        sak::PartitionOperationType::CheckFileSystem, target));
}

constexpr QLatin1StringView kRemovableTest("[string]$sakScanVolume.DriveType -eq 'Removable'");
constexpr QLatin1StringView kSettleLine("Letting Windows finish with the volume");

/// Writes 256 KiB of random bytes to @p path; their SHA-256, or nothing when the write failed.
QByteArray writeRandomFile(const QString& path) {
    QByteArray bytes(256 * 1024, '\0');
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

/// Check File System of the fixed NTFS volume at @p letter: the online scan as Windows sees the
/// drive, then its "removable" test forced, chkdsk read only, and with the file at @p path held
/// open, in use and not checked; the file is @p digest throughout.
void checkBothRoutes(const QString& letter,
                     const sak::PartitionScript& check,
                     const QString& path,
                     const QByteArray& digest) {
    QVERIFY2(check.script.contains(kRemovableTest), qPrintable(check.script));
    QString forced = check.script;
    forced.replace(kRemovableTest, QStringLiteral("$true"));

    // A fixed drive, as Windows sees this one, still takes the online scan, and settles after it.
    const sak::ProcessResult online = sak::runPowerShell(check.script, kScriptTimeoutMs);
    const QString online_out = online.std_out + online.std_err;
    QVERIFY2(online.exit_code == 0, qPrintable(online_out));
    QVERIFY2(online_out.contains(kSettleLine), qPrintable(online_out));
    QVERIFY2(!online_out.contains(QStringLiteral("CHKDSK")), qPrintable(online_out));

    const sak::ProcessResult checked = sak::runPowerShell(forced, kScriptTimeoutMs);
    const QString checked_out = checked.std_out + checked.std_err;
    QVERIFY2(checked.exit_code == 0, qPrintable(checked_out));
    QVERIFY2(checked_out.contains(QStringLiteral("Running CHKDSK in read-only mode")),
             qPrintable(checked_out));
    QVERIFY2(checked_out.contains(QStringLiteral("found no problems")), qPrintable(checked_out));
    QVERIFY2(checked_out.contains(kSettleLine), qPrintable(checked_out));
    QVERIFY2(!checked_out.contains(QStringLiteral("Repair-Volume")), qPrintable(checked_out));

    // A file held open: the volume is in use, and the check says so and reads nothing.
    QFile held(path);
    QVERIFY(held.open(QIODevice::ReadOnly));
    const sak::ProcessResult refused = sak::runPowerShell(forced, kScriptTimeoutMs);
    const QString refused_out = refused.std_out + refused.std_err;
    QVERIFY2(refused.exit_code != 0, qPrintable(refused_out));
    QVERIFY2(refused_out.contains(
                 QStringLiteral("%1: is in use, so it was not checked.").arg(letter.toUpper())),
             qPrintable(refused_out));
    QVERIFY2(!refused_out.contains(QStringLiteral("Running CHKDSK")), qPrintable(refused_out));
    QCOMPARE(QCryptographicHash::hash(held.readAll(), QCryptographicHash::Sha256), digest);
}

/// What the check of a real removable drive must say, by SAK_TEST_REMOVABLE_NTFS_EXPECT: "none"
/// (the default), "problems" (the PC damaged the drive's file system before the run) or
/// "in-use" (the test holds a file open on it).
struct RemovableExpectation {
    bool fails{false};
    bool chkdsk_runs{true};
    QString says;
};

RemovableExpectation removableExpectation(const QString& expect, const QString& stick) {
    if (expect == QStringLiteral("problems")) {
        return {.fails = true,
                .says = QStringLiteral(
                            "chkdsk found problems on %1: Reading only, it fixed none of them")
                            .arg(stick)};
    }
    if (expect == QStringLiteral("in-use")) {
        return {.fails = true,
                .chkdsk_runs = false,
                .says = QStringLiteral("%1: is in use, so it was not checked.").arg(stick)};
    }
    return {.says = QStringLiteral("found no problems")};
}

/// Check File System of the removable NTFS drive the PC names in SAK_TEST_REMOVABLE_NTFS_LETTER
/// (testpc1's stick), by the script unchanged, if it names one.
void checkTheRemovableDrive() {
    const QString stick = qEnvironmentVariable("SAK_TEST_REMOVABLE_NTFS_LETTER").left(1).toUpper();
    if (stick.isEmpty()) {
        return;
    }
    const QString expect = qEnvironmentVariable("SAK_TEST_REMOVABLE_NTFS_EXPECT", "none");
    const RemovableExpectation expected = removableExpectation(expect, stick);
    const sak::PartitionScript check = checkFileSystemScript(stick, 0);
    QVERIFY2(check.valid(), qPrintable(check.blockers.join(QStringLiteral("; "))));
    QFile held(QStringLiteral("%1:/sak-held-open.bin").arg(stick));
    if (expect == QStringLiteral("in-use")) {
        QVERIFY(held.open(QIODevice::ReadWrite));
    }
    const sak::ProcessResult run = sak::runPowerShell(check.script, kScriptTimeoutMs);
    const QString out = run.std_out + run.std_err;
    qInfo().noquote() << "removable drive" << stick << "expected" << expect << "exit"
                      << run.exit_code << ":" << out.left(3000);
    if (held.isOpen()) {
        held.close();
        QVERIFY(held.remove());
    }
    QCOMPARE(run.exit_code != 0, expected.fails);
    QVERIFY2(out.contains(expected.says), qPrintable(out));
    QCOMPARE(out.contains(QStringLiteral("Running CHKDSK")), expected.chkdsk_runs);
    QVERIFY2(out.contains(kSettleLine), qPrintable(out));
    QVERIFY2(!out.contains(QStringLiteral("snapshot error")), qPrintable(out));
    QVERIFY2(!out.contains(QStringLiteral("Repair-Volume")), qPrintable(out));
}

}  // namespace

// Windows does not snapshot a removable drive, so Check File System of an NTFS USB stick failed
// with "A snapshot error occurred while scanning this drive" (the capture of 1db87df4). Such a
// drive is now locked, dismounted and read by chkdsk without /f, /r, /x or /scan. CI has no stick,
// so a virtual disk takes that route here, its "removable" test forced, through Windows itself:
// the lock and dismount (Sak.ScanVolumeLock), chkdsk's read-only check, a drive in use refused as
// in use, and nothing on it changed. On a PC with a removable NTFS drive named in
// SAK_TEST_REMOVABLE_NTFS_LETTER (testpc1's stick), the script also runs unchanged against it.
void RawPartitionWriteTests::readOnlyCheckWithoutASnapshotLocksAndWritesNothing() {
    const QStringList letters = freeDriveLetters(1);
    QCOMPARE(letters.size(), 1);
    const QString letter = letters.first();
    const QString vhd =
        QDir::toNativeSeparators(QDir(m_vhds).filePath(QStringLiteral("check-ro.vhdx")));
    // Large enough for Windows to keep a snapshot of it on itself, as the online scan needs.
    constexpr int kSnapshotVolumeMiB = 600;
    const QString made =
        diskpart(*m_work,
                 {QStringLiteral("create vdisk file=\"%1\" maximum=%2 type=expandable")
                      .arg(vhd)
                      .arg(kSnapshotVolumeMiB)});
    const auto detach = qScopeGuard([this, &vhd] {
        (void)diskpart(*m_work,
                       {QStringLiteral("select vdisk file=\"%1\"").arg(vhd),
                        QStringLiteral("detach vdisk")});
    });
    PartitionVolume volume;
    volume.name = QStringLiteral("NTFS");
    volume.letter = letter;
    volume.vhd = vhd;
    const QString formatted = made + makeVolume(volume);
    QVERIFY2(formatted.contains(QStringLiteral("FORMATTED")), qPrintable(formatted));
    const QString path = QStringLiteral("%1:/sak-check-ro.bin").arg(letter);
    const QByteArray digest = writeRandomFile(path);
    QVERIFY(!digest.isEmpty());

    const sak::PartitionScript check = checkFileSystemScript(letter,
                                                             kSnapshotVolumeMiB * kEdgeBytes);
    QVERIFY2(check.valid(), qPrintable(check.blockers.join(QStringLiteral("; "))));
    checkBothRoutes(letter, check, path, digest);
    if (QTest::currentTestFailed()) {
        return;
    }
    {
        QFile reread(path);
        QVERIFY(reread.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(reread.readAll(), QCryptographicHash::Sha256), digest);
    }
    checkTheRemovableDrive();
}

void RawPartitionWriteTests::hideOnMbrHidesOnlyThatPartition() {
    const QStringList letters = freeDriveLetters(2);
    QCOMPARE(letters.size(), 2);
    const QString vhd =
        QDir::toNativeSeparators(QDir(m_vhds).filePath(QStringLiteral("hide.vhdx")));
    const QString made =
        diskpart(*m_work,
                 {QStringLiteral("create vdisk file=\"%1\" maximum=%2 type=expandable")
                      .arg(vhd)
                      .arg(kVirtualDiskMiB),
                  QStringLiteral("select vdisk file=\"%1\"").arg(vhd),
                  QStringLiteral("attach vdisk")});
    const auto detach = qScopeGuard([this, &vhd] {
        (void)diskpart(*m_work,
                       {QStringLiteral("select vdisk file=\"%1\"").arg(vhd),
                        QStringLiteral("detach vdisk")});
    });
    const sak::ProcessResult setup = sak::runPowerShell(
        QStringLiteral(
            "$ErrorActionPreference = 'Stop'\n"
            "$sakDisk = (Get-DiskImage -ImagePath %1 | Get-Disk).Number\n"
            "Initialize-Disk -Number $sakDisk -PartitionStyle MBR\n"
            "$p1 = New-Partition -DiskNumber $sakDisk -Size 24MB -DriveLetter %2\n"
            "Format-Volume -Partition $p1 -FileSystem NTFS -NewFileSystemLabel SAKHIDE1 -Force "
            "-Confirm:$false | Out-Null\n"
            "$p2 = New-Partition -DiskNumber $sakDisk -UseMaximumSize -DriveLetter %3\n"
            "Format-Volume -Partition $p2 -FileSystem NTFS -NewFileSystemLabel SAKHIDE2 -Force "
            "-Confirm:$false | Out-Null\n"
            "'MADE ' + $sakDisk + ' ' + $p1.PartitionNumber + ' ' + $p1.Offset + ' ' + "
            "$p1.Size\n")
                .arg(sak::PartitionScriptBuilder::quotePowerShell(vhd), letters[0], letters[1]) +
            mbrPartitionsFactsScript(),
        kDiskpartTimeoutMs);
    const QString setup_out = made + setup.std_out + setup.std_err;
    const QStringList fields = lineAfter(setup_out, QStringLiteral("MADE ")).split(u' ');
    QVERIFY2(fields.size() == 4 && fields[1] == QStringLiteral("1"), qPrintable(setup_out));
    const uint32_t disk = fields[0].toUInt();
    QVERIFY2(lineAfter(setup_out, QStringLiteral("P1 ")) ==
                     QStringLiteral("False %1 07").arg(letters[0]) &&
                 lineAfter(setup_out, QStringLiteral("P2 ")) ==
                     QStringLiteral("False %1 07").arg(letters[1]),
             qPrintable(setup_out));
    const QString facts = QStringLiteral("$sakDisk = %1\n").arg(disk) + mbrPartitionsFactsScript();

    const sak::PartitionScript hide = hideScript(disk, 1, fields.mid(2), true);
    QVERIFY2(hide.valid(), qPrintable(hide.blockers.join(QStringLiteral("; "))));
    const sak::ProcessResult hidden = sak::runPowerShell(hide.script, kScriptTimeoutMs);
    const sak::ProcessResult after_hide = sak::runPowerShell(facts, kScriptTimeoutMs);
    const QString hidden_out = hidden.std_out + hidden.std_err + after_hide.std_out;
    QVERIFY2(hidden.exit_code == 0, qPrintable(hidden_out));
    QVERIFY2(lineAfter(after_hide.std_out, QStringLiteral("P1 ")) == QStringLiteral("True - 17"),
             qPrintable(hidden_out));
    QVERIFY2(lineAfter(after_hide.std_out, QStringLiteral("P2 ")) ==
                 QStringLiteral("False %1 07").arg(letters[1]),
             qPrintable(hidden_out));

    const sak::PartitionScript show = hideScript(disk, 1, fields.mid(2), false);
    QVERIFY2(show.valid(), qPrintable(show.blockers.join(QStringLiteral("; "))));
    const sak::ProcessResult shown = sak::runPowerShell(show.script, kScriptTimeoutMs);
    const sak::ProcessResult after_show = sak::runPowerShell(facts, kScriptTimeoutMs);
    const QString shown_out = shown.std_out + shown.std_err + after_show.std_out;
    QVERIFY2(shown.exit_code == 0, qPrintable(shown_out));
    const QString first = lineAfter(after_show.std_out, QStringLiteral("P1 "));
    QVERIFY2(first.startsWith(QStringLiteral("False ")) && first.endsWith(QStringLiteral(" 07")) &&
                 !first.contains(QStringLiteral(" - ")),
             qPrintable(shown_out));
    QVERIFY2(lineAfter(after_show.std_out, QStringLiteral("P2 ")) ==
                 QStringLiteral("False %1 07").arg(letters[1]),
             qPrintable(shown_out));
}

QTEST_GUILESS_MAIN(RawPartitionWriteTests)
#include "test_raw_partition_write.moc"
