// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fat_volumes.cpp
/// @brief The no-follow primitive, the program-file check and the scripts' SafeFiles on real FAT32
/// and exFAT volumes.
///
/// S.A.K. runs as administrator straight from a flash drive, and most flash drives are FAT32 or
/// exFAT. Those file systems refuse FileAttributeTagInfo, which every no-follow check read, so on
/// 2026-11-11 a clean package on a FAT32 or exFAT drive would not run as administrator at all
/// (found by the VM proof; every other test ran on NTFS). This suite makes two small virtual disks,
/// one FAT32 and one exFAT, mounts them on free drive letters, and runs on each the primitive's
/// file and directory cases, the program-file check on a copy of the program's files, and the
/// SafeFiles holds with TEMP on the volume. Making and mounting a virtual disk needs administrator
/// rights and reaches disks, so it runs in CI's elevated step and skips everywhere else.

#include "sak/no_follow_fs.h"
#include "sak/partition_script_builder.h"
#include "sak/process_runner.h"
#include "sak/program_integrity.h"

#include "../support/test_hardware_guard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdio>
#include <memory>

#include <windows.h>

#include <share.h>

namespace pi = sak::program_integrity;
namespace nf = sak::no_follow;

namespace {

constexpr int kVirtualDiskMiB = 64;
constexpr int kDiskpartTimeoutMs = 120'000;
constexpr int kScriptTimeoutMs = 120'000;

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

/// Attach the new @p vhd, give it one MBR partition on drive @p letter and format it as
/// @p fileSystem, with the Storage cmdlets in one PowerShell.
QString makeVolume(const QString& vhd, const QString& letter, const QString& fileSystem) {
    const sak::ProcessResult result = sak::runPowerShell(
        QStringLiteral("$ErrorActionPreference = 'Stop'\n"
                       "Mount-DiskImage -ImagePath %1 | Out-Null\n"
                       "$disk = Get-DiskImage -ImagePath %1 | Get-Disk\n"
                       "Initialize-Disk -Number $disk.Number -PartitionStyle MBR\n"
                       "$p = New-Partition -DiskNumber $disk.Number -UseMaximumSize "
                       "-DriveLetter %2\n"
                       "Format-Volume -Partition $p -FileSystem %3 -NewFileSystemLabel SAKTEST "
                       "-Force -Confirm:$false | Out-Null\n"
                       "'FORMATTED'\n")
            .arg(sak::PartitionScriptBuilder::quotePowerShell(vhd), letter, fileSystem),
        kDiskpartTimeoutMs);
    return result.std_out + result.std_err;
}

struct FatVolume {
    QString name;  ///< FAT32 or exFAT
    QString root;  ///< "X:/"
    QString vhd;
};

QString appFile(const QString& name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

bool copyInto(const QString& source, const QString& folder, const QString& name) {
    return QDir().mkpath(folder) && QFile::copy(source, QDir(folder).filePath(name));
}

QByteArray readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

}  // namespace

class FatVolumeTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void heldFoldersAndStagedFiles_data();
    void heldFoldersAndStagedFiles();
    void heldReadOnlyFileRefusesWriters_data();
    void heldReadOnlyFileRefusesWriters();
    void programFolderOnTheVolume_data();
    void programFolderOnTheVolume();
    void scriptSafeFilesWithTempOnTheVolume_data();
    void scriptSafeFilesWithTempOnTheVolume();

private:
    void addVolumeRows();
    std::unique_ptr<QTemporaryDir> m_work;
    QString m_vhds;  ///< the virtual disks' folder
    QList<FatVolume> m_volumes;
};

void FatVolumeTests::initTestCase() {
    SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED();
    if (!processIsElevated()) {
        QSKIP(
            "Makes and mounts FAT32 and exFAT virtual disks, which needs administrator rights: "
            "CI's elevated step runs it");
    }
    m_work = std::make_unique<QTemporaryDir>();
    QVERIFY(m_work->isValid());
    // The virtual disks get a folder of their own under TEMP, removed at the end.
    m_vhds = QDir::toNativeSeparators(
        QDir::temp().filePath(QStringLiteral("sak-fat-vhd-") + nf::randomToken()));
    QVERIFY(QDir().mkpath(m_vhds));
    const QStringList letters = freeDriveLetters(2);
    QCOMPARE(letters.size(), 2);
    const QList<QPair<QString, QString>> kinds = {
        {QStringLiteral("FAT32"), QStringLiteral("fat32")},
        {QStringLiteral("exFAT"), QStringLiteral("exfat")}};
    for (int i = 0; i < kinds.size(); ++i) {
        const QString vhd = QDir::toNativeSeparators(
            QDir(m_vhds).filePath(QStringLiteral("%1.vhdx").arg(kinds[i].second)));
        // Two statements, not one sum: C++ leaves the order of a sum's operands open, and MSVC
        // ran makeVolume first, before the disk existed ("The system cannot find the file").
        QString out =
            diskpart(*m_work,
                     {QStringLiteral("create vdisk file=\"%1\" maximum=%2 type=expandable")
                          .arg(vhd)
                          .arg(kVirtualDiskMiB)});
        out += makeVolume(vhd, letters[i], kinds[i].first);
        m_volumes.append({kinds[i].first, letters[i] + QStringLiteral(":/"), vhd});
        wchar_t file_system[MAX_PATH] = {};
        const std::wstring root = (letters[i] + QStringLiteral(":\\")).toStdWString();
        QVERIFY2(GetVolumeInformationW(
                     root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, file_system, MAX_PATH) !=
                     0,
                 qPrintable(out));
        QCOMPARE(QString::fromWCharArray(file_system).toLower(), kinds[i].second);
    }
}

void FatVolumeTests::cleanupTestCase() {
    if (!m_work) {
        return;
    }
    for (const FatVolume& volume : m_volumes) {
        (void)diskpart(*m_work,
                       {QStringLiteral("select vdisk file=\"%1\"").arg(volume.vhd),
                        QStringLiteral("detach vdisk")});
    }
    if (!m_vhds.isEmpty()) {
        (void)nf::removeTree(m_vhds);
    }
}

void FatVolumeTests::addVolumeRows() {
    QTest::addColumn<QString>("root");
    for (const FatVolume& volume : m_volumes) {
        QTest::newRow(qPrintable(volume.name)) << volume.root;
    }
}

void FatVolumeTests::heldFoldersAndStagedFiles_data() {
    addVolumeRows();
}

// The primitive's folder and file cases on the volume: a held folder, a created and a descended
// one, a staged file published with and without replacing, a copy, a move, a new file, what a
// name is, and the tree removed again.
void FatVolumeTests::heldFoldersAndStagedFiles() {
    QFETCH(QString, root);
    const QString base = root + QStringLiteral("nf-") + nf::randomToken();
    QVERIFY(QDir().mkpath(base));
    {
        const auto held = nf::HeldDirectory::open(base);
        QVERIFY2(held, held ? "" : qPrintable(held.error()));
        QCOMPARE(nf::entryKindOfHandle(held->handle()).value_or(nf::EntryKind::Other),
                 nf::EntryKind::Directory);
        QVERIFY(!nf::volumeKeepsAcls(held->handle()));

        const auto made = held->createChild(QStringLiteral("made"),
                                            QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"));
        QVERIFY2(made, made ? "" : qPrintable(made.error()));
        const auto deep = held->descend(QStringLiteral("a/b/c"), true);
        QVERIFY2(deep, deep ? "" : qPrintable(deep.error()));

        auto staged = nf::StagedFile::create(*deep, QStringLiteral("file.txt"));
        QVERIFY2(staged, staged ? "" : qPrintable(staged.error()));
        staged->device().write("first");
        QVERIFY(staged->publish(nf::Publish::NoReplace));
        auto again = nf::StagedFile::create(*deep, QStringLiteral("file.txt"));
        QVERIFY(again);
        again->device().write("second");
        QVERIFY2(!again->publish(nf::Publish::NoReplace), "a publish replaced an existing name");
        auto replacing = nf::StagedFile::create(*deep, QStringLiteral("file.txt"));
        QVERIFY(replacing);
        replacing->device().write("third");
        const auto replaced = replacing->publish(nf::Publish::ReplaceName);
        QVERIFY2(replaced, replaced ? "" : qPrintable(replaced.error()));
        QCOMPARE(readAll(deep->filePath(QStringLiteral("file.txt"))), QByteArray("third"));

        const auto copied = nf::copyFileInto(deep->filePath(QStringLiteral("file.txt")),
                                             *made,
                                             QStringLiteral("copy.txt"),
                                             nf::Publish::NoReplace);
        QVERIFY2(copied, copied ? "" : qPrintable(copied.error()));
        const auto moved = nf::moveInto(made->filePath(QStringLiteral("copy.txt")),
                                        *held,
                                        QStringLiteral("moved.txt"),
                                        nf::Publish::NoReplace);
        QVERIFY2(moved, moved ? "" : qPrintable(moved.error()));
        QCOMPARE(readAll(held->filePath(QStringLiteral("moved.txt"))), QByteArray("third"));
        QCOMPARE(nf::entryKind(held->filePath(QStringLiteral("moved.txt"))), nf::EntryKind::File);
        QCOMPARE(nf::entryKind(held->filePath(QStringLiteral("a"))), nf::EntryKind::Directory);
        QCOMPARE(nf::entryKind(held->filePath(QStringLiteral("none"))), nf::EntryKind::Missing);

        auto fresh = nf::createNewFile(*held, QStringLiteral("new.bin"), true);
        QVERIFY2(fresh, fresh ? "" : qPrintable(fresh.error()));
        QVERIFY(!nf::createNewFile(*held, QStringLiteral("new.bin"), true));
        fresh->reset();
    }  // every folder let go: the tree can go
    QVERIFY(nf::removeTree(base));
    QVERIFY(!QFileInfo::exists(base));
}

void FatVolumeTests::heldReadOnlyFileRefusesWriters_data() {
    addVolumeRows();
}

// A file another program reads by name (a diskpart script) is held read-only on FAT as on NTFS:
// the reader diskpart uses opens it, a writer and a delete are refused.
void FatVolumeTests::heldReadOnlyFileRefusesWriters() {
    QFETCH(QString, root);
    const QString base = root + QStringLiteral("hold-") + nf::randomToken();
    QVERIFY(QDir().mkpath(base));
    const auto cleanup = qScopeGuard([base] { (void)nf::removeTree(base); });
    const auto folder = nf::HeldDirectory::open(base);
    QVERIFY2(folder, folder ? "" : qPrintable(folder.error()));
    auto held = nf::writeHeldReadOnlyFile(*folder, QStringLiteral("dp.txt"), "list disk\r\n");
    QVERIFY2(held, held ? "" : qPrintable(held.error()));
    const std::wstring wide =
        QDir::toNativeSeparators(folder->filePath(QStringLiteral("dp.txt"))).toStdWString();
    FILE* as_diskpart = _wfsopen(wide.c_str(), L"r", _SH_DENYNO);
    QVERIFY(as_diskpart != nullptr);
    std::fclose(as_diskpart);
    HANDLE writer = CreateFileW(wide.c_str(),
                                GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
    const DWORD writer_error = GetLastError();
    QVERIFY(writer == INVALID_HANDLE_VALUE);
    QCOMPARE(writer_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    QVERIFY(DeleteFileW(wide.c_str()) == 0);
    held->reset();
}

void FatVolumeTests::programFolderOnTheVolume_data() {
    addVolumeRows();
}

// The elevation's program-file check on a copy of program files on the volume: a clean folder is
// held, and a DLL that is not part of S.A.K. is refused and named. This is what refused a clean
// package on FAT32 and exFAT before the fix.
void FatVolumeTests::programFolderOnTheVolume() {
    QFETCH(QString, root);
    pi::captureRunningImageIdentity();
    const QString program = root + QStringLiteral("pkg-") + nf::randomToken();
    QVERIFY(copyInto(QCoreApplication::applicationFilePath(),
                     program,
                     QStringLiteral("sak_elevated_helper.exe")));
    QVERIFY(
        copyInto(appFile(QStringLiteral("Qt6Core.dll")), program, QStringLiteral("Qt6Core.dll")));
    QVERIFY(copyInto(appFile(QStringLiteral("platforms/qwindows.dll")),
                     program + "/platforms",
                     QStringLiteral("qwindows.dll")));
    QVERIFY(copyInto(appFile(QStringLiteral("sak_script_types.dll")),
                     program,
                     QStringLiteral("sak_script_types.dll")));
    {
        const auto hold = pi::ProgramHold::verify(program);
        QVERIFY2(hold,
                 qPrintable(hold ? QString() : hold.error().file + ": " + hold.error().reason));
        QCOMPARE(hold->heldFiles().size(), 4);
    }
    QVERIFY(copyInto(appFile(QStringLiteral("z.dll")), program, QStringLiteral("version.dll")));
    const auto refused = pi::ProgramHold::verify(program);
    QVERIFY(!refused);
    QVERIFY2(refused.error().file.endsWith(QStringLiteral("version.dll"), Qt::CaseInsensitive),
             qPrintable(refused.error().file + ": " + refused.error().reason));
    QVERIFY(nf::removeTree(program));
}

void FatVolumeTests::scriptSafeFilesWithTempOnTheVolume_data() {
    addVolumeRows();
}

// The scripts' own types with TEMP on the volume: a held folder, a file written and held
// read-only (a writer refused), one made new, and one replaced by name.
void FatVolumeTests::scriptSafeFilesWithTempOnTheVolume() {
    QFETCH(QString, root);
    const QString temp =
        QDir::toNativeSeparators(root + QStringLiteral("tmp-") + nf::randomToken());
    QVERIFY(QDir().mkpath(temp));
    const QString script =
        sak::PartitionScriptBuilder::scriptTypesLoaderScript() +
        QStringLiteral(
            "$ErrorActionPreference = 'Stop'\n"
            "$env:TEMP = %1; $env:TMP = %1\n"
            "$d = Join-Path ([System.IO.Path]::GetTempPath()) 'work'\n"
            "New-Item -ItemType Directory -Path $d | Out-Null\n"
            "$folder = [Sak.SafeFiles]::HoldDirectory($d); 'HELD FOLDER'\n"
            "$p = Join-Path $d 'dp.txt'\n"
            "$h = [Sak.SafeFiles]::WriteHeldReadOnly($p, [Text.Encoding]::ASCII.GetBytes('list "
            "disk'))\n"
            "$r = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite'); $r.Dispose(); 'READ'\n"
            "try { $w = [IO.File]::Open($p, 'Open', 'Write', 'ReadWrite'); $w.Dispose(); 'WROTE' } "
            "catch { 'NOWRITE' }\n"
            "$h.Dispose()\n"
            "$n = [Sak.SafeFiles]::CreateNew((Join-Path $d 'new.img')); $n.Dispose(); 'NEW'\n"
            "$x = [Sak.SafeFiles]::CreateReplacing((Join-Path $d 'new.img')); $x.Dispose(); "
            "'REPLACED'\n"
            "$folder.Dispose()\n")
            .arg(sak::PartitionScriptBuilder::quotePowerShell(temp));
    const sak::ProcessResult result = sak::runPowerShell(script, kScriptTimeoutMs);
    const QString out = (result.std_out + result.std_err).trimmed();
    for (const char* step : {"HELD FOLDER", "READ", "NOWRITE", "NEW", "REPLACED"}) {
        QVERIFY2(out.contains(QLatin1String(step)), qPrintable(out));
    }
    QVERIFY(nf::removeTree(temp));
}

QTEST_GUILESS_MAIN(FatVolumeTests)
#include "test_fat_volumes.moc"
