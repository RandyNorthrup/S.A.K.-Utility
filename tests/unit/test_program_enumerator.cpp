// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_program_enumerator.cpp
/// @brief Unit tests for ProgramEnumerator

#include "sak/advanced_uninstall_types.h"
#include "sak/program_enumerator.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;

class TestProgramEnumerator : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void programs_emptyInitially();
    void calculateDirSize_emptyPath();
    void calculateDirSize_nonExistent();
    void calculateDirSize_realDirectory();
    void detectOrphaned_emptyList();
    void detectOrphaned_missingInstallLocation();
    void detectOrphaned_existingInstallLocation();
    void detectOrphaned_uwpSkipped();
    void detectOrphaned_msiexecNotOrphaned();
    void markBloatware_emptyList();
    void markBloatware_matchesKnownPatterns();
    void markBloatware_normalProgramNotBloatware();
    void programInfo_defaults();
    void cancelAndReset();
    void enumerateAll_cancelledBeforeStart();
};

void TestProgramEnumerator::construction_default() {
    ProgramEnumerator enumerator;
    QVERIFY(dynamic_cast<QObject*>(&enumerator) != nullptr);
}

void TestProgramEnumerator::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<ProgramEnumerator>);
    QVERIFY(!std::is_move_constructible_v<ProgramEnumerator>);
}

void TestProgramEnumerator::programs_emptyInitially() {
    ProgramEnumerator enumerator;
    const auto programs = enumerator.programs();
    QVERIFY(programs.isEmpty());
}

void TestProgramEnumerator::calculateDirSize_emptyPath() {
    const auto size = ProgramEnumerator::calculateDirSize(QString());
    QCOMPARE(size, 0);
}

void TestProgramEnumerator::calculateDirSize_nonExistent() {
    const auto size =
        ProgramEnumerator::calculateDirSize(QStringLiteral("C:\\NonExistent_Path_12345"));
    QCOMPARE(size, 0);
}

void TestProgramEnumerator::calculateDirSize_realDirectory() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    constexpr int kFileCount = 3;
    constexpr int kBytesPerFile = 100;
    for (int i = 0; i < kFileCount; ++i) {
        QFile file(temp_dir.filePath(QString("file_%1.dat").arg(i)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(kBytesPerFile, 'A'));
        file.close();
    }

    const qint64 size = ProgramEnumerator::calculateDirSize(temp_dir.path());
    QCOMPARE(size, static_cast<qint64>(kFileCount * kBytesPerFile));
}

void TestProgramEnumerator::detectOrphaned_emptyList() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> empty_list;
    enumerator.detectOrphaned(empty_list);
    QVERIFY(empty_list.isEmpty());
}

void TestProgramEnumerator::detectOrphaned_missingInstallLocation() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("FakeApp");
    prog.installLocation = QStringLiteral("C:\\NonExistent_Dir_99999");
    prog.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(prog);

    enumerator.detectOrphaned(programs);
    QVERIFY(programs[0].isOrphaned);
}

void TestProgramEnumerator::detectOrphaned_existingInstallLocation() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("RealApp");
    prog.installLocation = temp_dir.path();
    prog.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(prog);

    enumerator.detectOrphaned(programs);
    QVERIFY(!programs[0].isOrphaned);
}

void TestProgramEnumerator::detectOrphaned_uwpSkipped() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("UWPApp");
    prog.installLocation = QStringLiteral("C:\\NonExistent_UWP_99999");
    prog.source = ProgramInfo::Source::UWP;
    programs.append(prog);

    enumerator.detectOrphaned(programs);
    QVERIFY(!programs[0].isOrphaned);
}

void TestProgramEnumerator::detectOrphaned_msiexecNotOrphaned() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("MSI App");
    prog.uninstallString = QStringLiteral("MsiExec.exe /X{GUID}");
    prog.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(prog);

    enumerator.detectOrphaned(programs);
    QVERIFY(!programs[0].isOrphaned);
}

void TestProgramEnumerator::markBloatware_emptyList() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> empty_list;
    enumerator.markBloatware(empty_list);
    QVERIFY(empty_list.isEmpty());
}

void TestProgramEnumerator::markBloatware_matchesKnownPatterns() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    const QStringList bloatware_names = {QStringLiteral("CandyCrush Saga"),
                                         QStringLiteral("Xbox Game Bar"),
                                         QStringLiteral("Microsoft Solitaire Collection"),
                                         QStringLiteral("BingNews"),
                                         QStringLiteral("Facebook App")};

    for (const auto& name : bloatware_names) {
        ProgramInfo prog;
        prog.displayName = name;
        programs.append(prog);
    }

    enumerator.markBloatware(programs);

    for (int i = 0; i < programs.size(); ++i) {
        QVERIFY2(programs[i].isBloatware,
                 qPrintable(QString("Expected bloatware: %1").arg(programs[i].displayName)));
    }
}

void TestProgramEnumerator::markBloatware_normalProgramNotBloatware() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("Visual Studio Code");
    programs.append(prog);

    ProgramInfo prog2;
    prog2.displayName = QStringLiteral("CMake 3.28");
    programs.append(prog2);

    enumerator.markBloatware(programs);

    QVERIFY(!programs[0].isBloatware);
    QVERIFY(!programs[1].isBloatware);
}

void TestProgramEnumerator::programInfo_defaults() {
    ProgramInfo info;
    QVERIFY(info.displayName.isEmpty());
    QVERIFY(info.publisher.isEmpty());
    QVERIFY(info.displayVersion.isEmpty());
    QCOMPARE(info.estimatedSizeKB, static_cast<qint64>(0));
    QCOMPARE(info.actualSizeBytes, static_cast<qint64>(0));
    QVERIFY(!info.isSystemComponent);
    QVERIFY(!info.isOrphaned);
    QVERIFY(!info.isBloatware);
    QCOMPARE(info.source, ProgramInfo::Source::RegistryHKLM);
}

void TestProgramEnumerator::cancelAndReset() {
    ProgramEnumerator enumerator;
    enumerator.requestCancel();
    enumerator.resetCancel();
    QVERIFY(enumerator.programs().isEmpty());
}

void TestProgramEnumerator::enumerateAll_cancelledBeforeStart() {
    ProgramEnumerator enumerator;
    enumerator.requestCancel();

    QSignalSpy failed_spy(&enumerator, &ProgramEnumerator::enumerationFailed);
    enumerator.enumerateAll();

    QCOMPARE(failed_spy.count(), 1);
    const auto args = failed_spy.takeFirst();
    QVERIFY(args.at(0).toString().contains("cancelled"));
}

QTEST_MAIN(TestProgramEnumerator)
#include "test_program_enumerator.moc"
