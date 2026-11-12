// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_script_types.cpp
/// @brief sak_script_types.dll in Windows PowerShell 5.1, the way the elevated scripts use it.
///
/// Security brief 4a/4b: the elevated scripts' native types used to be compiled by Add-Type into
/// the temporary folder. They are now one DLL beside the program, loaded from its bytes, and its
/// Sak.SafeFiles is what the scripts write and hold files through. Each case plants a junction
/// that points at a canary folder and checks the canary afterwards.

#include "sak/partition_script_builder.h"
#include "sak/process_runner.h"

#include "../support/junction_fixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const QString kCanaryName = QStringLiteral("canary.txt");
const QByteArray kCanaryBytes = QByteArrayLiteral("must stay exactly this");

/// Run @p body in Windows PowerShell 5.1 after the program's own loader line; stdout, trimmed.
QString runScript(const QString& body) {
    const QString script = sak::PartitionScriptBuilder::scriptTypesLoaderScript() +
                           QStringLiteral("$ErrorActionPreference = 'Stop'\n") + body;
    const sak::ProcessResult result = sak::runPowerShell(script, 60'000, true, true, {});
    return (result.std_out + result.std_err).trimmed();
}

QString quoted(const QString& path) {
    return sak::PartitionScriptBuilder::quotePowerShell(QDir::toNativeSeparators(path));
}

}  // namespace

class ScriptTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void dllIsBesideTheProgram();
    void typesLoadWithoutAddType();
    void holdDirectoryRefusesAJunction();
    void createReplacingRefusesALinkAndReplacesAFile();
    void heldScriptRefusesWritersAndAllowsReaders();
    void shadowDeviceNamesAreChecked();
    void copyVolumeLockSpeaksForCopyPartition();
    void rawPartitionWriteTakesOnlyPartitions();
    void sparseRangesAreReadFromTheFileSystem();

private:
    std::unique_ptr<QTemporaryDir> m_root;
    QString m_canary;
};

void ScriptTypesTests::init() {
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    m_canary = m_root->filePath(QStringLiteral("canary"));
    QVERIFY(sak::test::createCanary(m_canary, kCanaryName, kCanaryBytes));
}

void ScriptTypesTests::dllIsBesideTheProgram() {
    QVERIFY(QFile::exists(QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("sak_script_types.dll"))));
    QVERIFY(sak::PartitionScriptBuilder::scriptTypesLoaderScript().contains(
        QStringLiteral("[System.Reflection.Assembly]::Load([System.IO.File]::ReadAllBytes(")));
}

void ScriptTypesTests::typesLoadWithoutAddType() {
    const QString out = runScript(QStringLiteral(
        "foreach ($t in 'Sak.SafeFiles','Sak.ShadowMapping','Sak.VolApi','Sak.ScanProcess',"
        "'Sak.MediaReadStream','Sak.CloneByteComparer','Sak.ImageVolumeLock','Sak.CopyVolumeLock',"
        "'Sak.RawPartitionWrite','Sak.TakenPartitionStream','Sak.SparseFiles','Sak."
        "DynamicDiskProbe','Sak.DiskFacts','Sak.VolumeRun','"
        "SakLockedFilePath',"
        "'SakMigration.Copy') { if (-not ($t -as [type])) { throw \"missing $t\" } }\n"
        "'all types loaded'\n"));
    QVERIFY2(out.contains(QStringLiteral("all types loaded")), qPrintable(out));
}

void ScriptTypesTests::holdDirectoryRefusesAJunction() {
    const QString link = m_root->filePath(QStringLiteral("backups"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    const QString out = runScript(
        QStringLiteral(
            "try { [void][Sak.SafeFiles]::HoldDirectory(%1); 'HELD' } catch { 'REFUSED' }\n"
            "try { [void][Sak.SafeFiles]::HoldDirectory(%2); 'HELD' } catch { 'REFUSED' }\n")
            .arg(quoted(link), quoted(link + QStringLiteral("/../canary"))));
    QVERIFY2(out.startsWith(QStringLiteral("REFUSED")), qPrintable(out));
    QVERIFY2(out.endsWith(QStringLiteral("HELD")), qPrintable(out));
}

void ScriptTypesTests::createReplacingRefusesALinkAndReplacesAFile() {
    const QString link_folder = m_root->filePath(QStringLiteral("image.img"));
    QVERIFY(sak::test::createJunction(link_folder, m_canary));
    const QString existing = m_root->filePath(QStringLiteral("old.img"));
    {
        QFile file(existing);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("old image");
    }
    const QString out = runScript(
        QStringLiteral("try { ([Sak.SafeFiles]::CreateReplacing(%1)).Dispose(); 'MADE' } catch { "
                       "'REFUSED' }\n"
                       "$s = [Sak.SafeFiles]::CreateReplacing(%2); $b = [byte[]](1,2,3); "
                       "$s.Write($b, 0, 3); $s.Dispose(); 'REPLACED'\n"
                       "try { ([Sak.SafeFiles]::CreateNew(%2)).Dispose(); 'MADE' } catch { "
                       "'EXISTS' }\n")
            .arg(quoted(link_folder), quoted(existing)));
    QVERIFY2(out.contains(QStringLiteral("REFUSED")) && out.contains(QStringLiteral("REPLACED")) &&
                 out.contains(QStringLiteral("EXISTS")),
             qPrintable(out));
    QFile replaced(existing);
    QVERIFY(replaced.open(QIODevice::ReadOnly));
    QCOMPARE(replaced.readAll(), QByteArray("\x01\x02\x03", 3));
    QVERIFY(sak::test::canaryIntact(m_canary, kCanaryName, kCanaryBytes));
}

void ScriptTypesTests::heldScriptRefusesWritersAndAllowsReaders() {
    const QString script = m_root->filePath(QStringLiteral("dp.txt"));
    const QString out =
        runScript(QStringLiteral(
                      "$h = [Sak.SafeFiles]::WriteHeldReadOnly(%1, [Text.Encoding]::ASCII.GetBytes("
                      "'list disk'))\n"
                      "$r = [IO.File]::Open(%1, 'Open', 'Read', 'ReadWrite'); $r.Dispose(); "
                      "'READ'\n"
                      "try { $w = [IO.File]::Open(%1, 'Open', 'Write', 'ReadWrite'); "
                      "$w.Dispose(); 'WROTE' } catch { 'NOWRITE' }\n"
                      "try { [IO.File]::Delete(%1); 'DELETED' } catch { 'NODELETE' }\n"
                      "$h.Dispose(); [IO.File]::Delete(%1); 'GONE'\n")
                      .arg(quoted(script)));
    QVERIFY2(out.contains(QStringLiteral("READ")) && out.contains(QStringLiteral("NOWRITE")) &&
                 out.contains(QStringLiteral("NODELETE")) && out.contains(QStringLiteral("GONE")),
             qPrintable(out));
    QVERIFY(!QFile::exists(script));
}

void ScriptTypesTests::shadowDeviceNamesAreChecked() {
    const QString out = runScript(QStringLiteral(
        "[Sak.ShadowMapping]::NtDevice('\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy7')\n"
        "try { [void][Sak.ShadowMapping]::NtDevice('\\\\?\\GLOBALROOT\\Device\\HarddiskVolume3'); "
        "'ACCEPTED' } catch { 'REFUSED' }\n"));
    QVERIFY2(out.contains(QStringLiteral("\\Device\\HarddiskVolumeShadowCopy7")) &&
                 out.contains(QStringLiteral("REFUSED")),
             qPrintable(out));
}

// Copy Partition holds its source with Create Image's lock (Sak.CopyVolumeLock wraps
// Sak.ImageVolumeLock) and refuses in its own words. Opens an ordinary path that does not exist:
// no volume or disk is touched.
void ScriptTypesTests::copyVolumeLockSpeaksForCopyPartition() {
    const QString missing = m_root->filePath(QStringLiteral("missing/source"));
    const QString out = runScript(
        QStringLiteral("try { [void][Sak.CopyVolumeLock]::Acquire(%1); 'OPENED' } catch { "
                       "$_.Exception.InnerException.Message }\n")
            .arg(quoted(missing)));
    QVERIFY2(out.contains(QStringLiteral("Copy Partition cannot open the source volume")),
             qPrintable(out));
    QVERIFY2(!out.contains(QStringLiteral("Create Image")), qPrintable(out));
}

// The HFS+ copy's writer takes only a partition (test_raw_partition_write writes real ones); the
// whole disk and a staged image file are not partitions, and an image file opens as a file.
void ScriptTypesTests::rawPartitionWriteTakesOnlyPartitions() {
    const QString image = m_root->filePath(QStringLiteral("staged.img"));
    QFile file(image);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QByteArray(4096, '\0')) == 4096);
    file.close();
    const QString out = runScript(
        QStringLiteral(
            "[Sak.RawPartitionWrite]::IsPartitionPath('\\\\?\\GLOBALROOT\\Device\\Harddisk4\\"
            "Partition2')\n"
            "[Sak.RawPartitionWrite]::IsPartitionPath('\\\\?\\GLOBALROOT\\Device\\Harddisk4\\"
            "Partition0')\n"
            "[Sak.RawPartitionWrite]::IsPartitionPath(%1)\n"
            "$s = [Sak.RawPartitionWrite]::Open(%1)\n"
            "try { $s.Write([byte[]](1,2,3), 0, 3); 'IMAGE ' + $s.Length } finally { "
            "$s.Dispose() }\n")
            .arg(quoted(image)));
    const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QVERIFY2(lines.size() == 4, qPrintable(out));
    QCOMPARE(lines.at(0).trimmed(), QStringLiteral("True"));
    QCOMPARE(lines.at(1).trimmed(), QStringLiteral("False"));
    QCOMPARE(lines.at(2).trimmed(), QStringLiteral("False"));
    QCOMPARE(lines.at(3).trimmed(), QStringLiteral("IMAGE 4096"));
}

// Sak.SparseFiles: the HFS+ staging image is marked sparse and its written ranges listed by the
// file system itself (fsutil's labels are translated, so a non-English Windows could not be read).
// 64 KiB written at 16 MiB of a 64 MiB image: one range that covers it, far less than the image.
void ScriptTypesTests::sparseRangesAreReadFromTheFileSystem() {
    const QString image = m_root->filePath(QStringLiteral("sparse.img"));
    QFile file(image);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    const QString out = runScript(
        QStringLiteral(
            "[Sak.SparseFiles]::SetSparse(%1)\n"
            "'SPARSE ' + (([IO.File]::GetAttributes(%1) -band [IO.FileAttributes]::SparseFile) "
            "-ne 0)\n"
            "$s = [IO.File]::Open(%1, 'Open', 'ReadWrite', 'None')\n"
            "try { $s.SetLength(64MB); [void]$s.Seek(16MB, 'Begin'); $b = New-Object byte[] "
            "65536; for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = 0x5A }; $s.Write($b, 0, "
            "$b.Length) } finally { $s.Dispose() }\n"
            "$flat = [Sak.SparseFiles]::AllocatedRanges(%1)\n"
            "$covers = $false; $total = 0\n"
            "for ($i = 0; $i + 1 -lt $flat.Length; $i += 2) { $total += $flat[$i + 1]; if "
            "($flat[$i] -le 16MB -and ($flat[$i] + $flat[$i + 1]) -ge (16MB + 65536)) { $covers = "
            "$true } }\n"
            "'COVERS ' + $covers + ' SMALL ' + ($total -lt 64MB)\n"
            "try { [Sak.RawPartitionWrite]::Release(%1); 'RELEASED' } catch { 'NOT A PARTITION' "
            "}\n")
            .arg(quoted(image)));
    QVERIFY2(out.contains(QStringLiteral("SPARSE True")), qPrintable(out));
    QVERIFY2(out.contains(QStringLiteral("COVERS True SMALL True")), qPrintable(out));
    QVERIFY2(out.contains(QStringLiteral("NOT A PARTITION")), qPrintable(out));
}

QTEST_GUILESS_MAIN(ScriptTypesTests)
#include "test_script_types.moc"
