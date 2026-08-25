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
    void detectOrphaned_remotePathsAreNeverProbed();
    void detectOrphaned_missingUninstallerIsOrphaned();
    void detectOrphaned_msiexecNotOrphaned();
    void markBloatware_emptyList();
    void markBloatware_matchesKnownPatterns();
    void markBloatware_normalProgramNotBloatware();
    void programInfo_defaults();
    void cancelAndReset();
    void enumerateAll_cancelledBeforeStart();
};

void TestProgramEnumerator::construction_default() {
    // `dynamic_cast<QObject*>(&enumerator) != nullptr` asserted nothing: QObject is an
    // unambiguous non-virtual base, so that is a compile-time upcast of a known-non-null stack
    // address and the compiler emits no runtime check at all. The claim is additionally gated by
    // a static_assert in the header -- the binary only links because it holds. Pin what the
    // constructor actually DOES with its argument instead: forward the parent to QObject.
    QObject parent;
    auto* child = new ProgramEnumerator(&parent);
    QCOMPARE(child->parent(), &parent);
    // ... and the default constructor leaves it parentless, so ownership is opt-in.
    ProgramEnumerator orphan;
    QCOMPARE(orphan.parent(), nullptr);
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
    ProgramEnumerator enumerator;
    const auto size = enumerator.calculateDirSize(QString());
    QCOMPARE(size, 0);
}

void TestProgramEnumerator::calculateDirSize_nonExistent() {
    ProgramEnumerator enumerator;
    const auto size = enumerator.calculateDirSize(QStringLiteral("C:\\NonExistent_Path_12345"));
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

    // NESTED, not flat. calculateDirSize walks with QDirIterator::Subdirectories, and a flat
    // fixture cannot distinguish a recursive walk from a non-recursive one -- there is nothing
    // nested to miss. Recursion is the whole point: every real install tree is nested, and losing
    // it silently under-reports disk usage for every program in the panel and every
    // totalSpaceRecovered figure derived from it.
    const QDir root(temp_dir.path());
    QVERIFY(root.mkpath(QStringLiteral("sub/deeper")));
    constexpr int kNestedFiles = 2;
    const QStringList nested{QStringLiteral("sub/nested.dat"),
                             QStringLiteral("sub/deeper/deep.dat")};
    for (const QString& relative : nested) {
        QFile file(root.filePath(relative));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(kBytesPerFile, 'B'));
        file.close();
    }

    ProgramEnumerator enumerator;
    const qint64 size = enumerator.calculateDirSize(temp_dir.path());
    QCOMPARE(size, static_cast<qint64>((kFileCount + kNestedFiles) * kBytesPerFile));

    // The cancel flag has a cheap public observable, and cancelAndReset() could not reach it: a
    // cancelled walk sizes nothing, and a reset one sizes the whole tree again.
    enumerator.requestCancel();
    QCOMPARE(enumerator.calculateDirSize(temp_dir.path()), static_cast<qint64>(0));
    enumerator.resetCancel();
    QCOMPARE(enumerator.calculateDirSize(temp_dir.path()),
             static_cast<qint64>((kFileCount + kNestedFiles) * kBytesPerFile));
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

    // The skip is `source == UWP || source == Provisioned`, and only UWP was ever fed -- so the
    // Provisioned arm was deletable in silence. It is a real arm: scanProvisionedPackages stamps
    // that source, and both the panel and the controller treat Provisioned as a package rather
    // than a Win32 entry, so a provisioned entry falling through into the filesystem probes would
    // be classified by a path it never has.
    ProgramInfo provisioned;
    provisioned.displayName = QStringLiteral("ProvisionedApp");
    provisioned.installLocation = QStringLiteral("C:\\NonExistent_Provisioned_99999");
    provisioned.source = ProgramInfo::Source::Provisioned;
    programs.append(provisioned);

    enumerator.detectOrphaned(programs);
    QVERIFY(!programs[0].isOrphaned);
    QVERIFY2(!programs[1].isOrphaned, "a Provisioned package must be skipped like a UWP one");
}

void TestProgramEnumerator::detectOrphaned_remotePathsAreNeverProbed() {
    // detectOrphaned refuses to touch the filesystem for remote/device paths in TWO places -- the
    // installLocation expression and the uninstaller branch -- and neither was reachable from any
    // fixture: every path in the file is a plain drive-letter path. These are the guards that stop
    // an attacker-writable Uninstall value from coercing SMB authentication and unbounded remote
    // I/O as the elevated caller, and they were silently deletable. With either gone, a
    // nonexistent UNC host resolves as "missing" and the entry flips to orphaned -- which is
    // exactly what makes them observable here.
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    const QStringList remote_locations{QStringLiteral("\\\\no-such-host-99999\\share\\App"),
                                       QStringLiteral("//no-such-host-99999/share/App"),
                                       QStringLiteral("\\\\?\\C:\\NonExistent_99999"),
                                       QStringLiteral("\\\\.\\PhysicalDrive9")};
    for (const QString& location : remote_locations) {
        ProgramInfo prog;
        prog.displayName = QStringLiteral("RemoteApp");
        prog.installLocation = location;
        prog.source = ProgramInfo::Source::RegistryHKLM;
        programs.append(prog);
    }
    // ... and the uninstaller-side guard, reached only when installLocation is empty.
    for (const QString& location : remote_locations) {
        ProgramInfo prog;
        prog.displayName = QStringLiteral("RemoteUninstaller");
        prog.uninstallString = location + QStringLiteral("\\unins000.exe");
        prog.source = ProgramInfo::Source::RegistryHKLM;
        programs.append(prog);
    }

    enumerator.detectOrphaned(programs);
    for (int i = 0; i < programs.size(); ++i) {
        QVERIFY2(!programs[i].isOrphaned,
                 qPrintable(QStringLiteral("entry %1 (%2) was probed and orphaned")
                                .arg(i)
                                .arg(programs[i].installLocation + programs[i].uninstallString)));
    }
}

void TestProgramEnumerator::detectOrphaned_missingUninstallerIsOrphaned() {
    // The final arm -- `isOrphaned = !QFileInfo::exists(exePath)` -- was never once allowed to
    // decide TRUE by any fixture: the missing-install-location test gets its true from the
    // earlier arm, and every other fixture asserts !isOrphaned, which is also the struct's
    // default. So "the uninstaller executable is gone", the feature's second reason to exist, was
    // entirely unconstrained.
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    const QDir root(temp_dir.path());

    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    // Missing uninstaller -> orphaned.
    ProgramInfo gone;
    gone.displayName = QStringLiteral("GoneApp");
    gone.uninstallString = root.filePath(QStringLiteral("no_such_unins.exe"));
    gone.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(gone);

    // Present uninstaller -> not orphaned. Without this control the arm could simply always
    // answer true.
    const QString real_exe = root.filePath(QStringLiteral("unins000.exe"));
    QFile exe(real_exe);
    QVERIFY(exe.open(QIODevice::WriteOnly));
    exe.write(QByteArrayLiteral("MZ"));
    exe.close();
    ProgramInfo present;
    present.displayName = QStringLiteral("PresentApp");
    present.uninstallString = real_exe;
    present.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(present);

    // The QUOTED registry form -- `"C:\path with spaces\unins000.exe" /SILENT` -- is the
    // overwhelmingly common one, and the branch that strips those quotes was reached by NO
    // fixture: the only non-empty uninstallString in the suite was the unquoted MsiExec one. With
    // the strip gone the quotes travel into QFileInfo::exists and every quoted-uninstaller program
    // on a real machine is reported orphaned, and offered for forced removal.
    ProgramInfo quoted;
    quoted.displayName = QStringLiteral("QuotedApp");
    quoted.uninstallString = QStringLiteral("\"") + real_exe + QStringLiteral("\" /SILENT");
    quoted.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(quoted);

    // ... and the unquoted-with-arguments form, which the sibling space-split branch handles.
    ProgramInfo spaced;
    spaced.displayName = QStringLiteral("SpacedApp");
    spaced.uninstallString = real_exe + QStringLiteral(" /SILENT");
    spaced.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(spaced);

    enumerator.detectOrphaned(programs);
    QVERIFY2(programs[0].isOrphaned, "a missing uninstaller executable must be orphaned");
    QVERIFY2(!programs[1].isOrphaned, "a present uninstaller executable must not be orphaned");
    QVERIFY2(!programs[2].isOrphaned, "a QUOTED uninstaller path must have its quotes stripped");
    QVERIFY2(!programs[3].isOrphaned, "an uninstaller with arguments must be split at the space");
}

void TestProgramEnumerator::detectOrphaned_msiexecNotOrphaned() {
    ProgramEnumerator enumerator;
    QVector<ProgramInfo> programs;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("MSI App");
    prog.uninstallString = QStringLiteral("MsiExec.exe /X{GUID}");
    prog.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(prog);

    // The exemption reads the NORMALIZED exePath, not the raw uninstallString, and the fixture
    // above makes them agree (both contain "msiexec"), so no assertion could tell which source it
    // consulted. This entry separates them: the quoted path holds no "msiexec", while the raw
    // string does -- so it must be treated as an ordinary missing uninstaller, not exempted.
    ProgramInfo disguised;
    disguised.displayName = QStringLiteral("Disguised");
    disguised.uninstallString =
        QStringLiteral("\"C:\\NonExistent_99999\\setup.exe\" /uninstall msiexec");
    disguised.source = ProgramInfo::Source::RegistryHKLM;
    programs.append(disguised);

    enumerator.detectOrphaned(programs);
    QVERIFY(!programs[0].isOrphaned);
    QVERIFY2(programs[1].isOrphaned,
             "the msiexec exemption must read the normalized exe path, not the raw string");
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

    // The match is a two-arm disjunction over displayName OR packageFamilyName, and NO fixture
    // anywhere in this file set packageFamilyName -- so the second arm was unreachable and
    // deletable with the suite green. It is not a decorative arm: several shipped patterns are
    // package-family tokens that never appear in a UWP DisplayName ("king.com" matches only a
    // family name like king.comCandyCrushSaga_kgqvnymyfvs32), and packageFamilyName is exactly
    // what scanUwpPackages populates. With the arm gone, UWP bloatware whose Name is innocuous
    // stops being flagged and never surfaces in the BloatwareOnly view.
    ProgramInfo uwp_family;
    uwp_family.displayName = QStringLiteral("Puzzle Game");  // matches no pattern on its own
    uwp_family.packageFamilyName = QStringLiteral("king.comCandyCrushSaga_kgqvnymyfvs32");
    uwp_family.source = ProgramInfo::Source::UWP;
    programs.append(uwp_family);

    enumerator.markBloatware(programs);

    for (int i = 0; i < programs.size(); ++i) {
        QVERIFY2(programs[i].isBloatware,
                 qPrintable(QString("Expected bloatware: %1 / %2")
                                .arg(programs[i].displayName, programs[i].packageFamilyName)));
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

    // NEAR MISSES. The two names above share nothing with any shipped pattern, so the negative
    // side of the classifier was proved only from arm's length -- and !isBloatware is also the
    // struct's default, so those assertions held even if markBloatware never ran. Any loosening
    // of the substring compare (a prefix match, or matching on a TRUNCATED pattern) leaves both
    // green while flagging real software: four patterns begin with "Windows", "Xbox" truncates to
    // "box", "Solitaire" to "Solit". Flagging a program as bloatware steers the BloatwareOnly view
    // and bulk removal, so a false positive is a delete-the-wrong-thing risk.
    const QStringList near_misses{QStringLiteral("Windows Terminal"),
                                  QStringLiteral("Windows SDK"),
                                  QStringLiteral("Dropbox"),
                                  QStringLiteral("Sandboxie"),
                                  QStringLiteral("Solitude Editor"),
                                  QStringLiteral("Bingo Card Maker"),
                                  QStringLiteral("Skypearl Utilities")};
    for (const QString& name : near_misses) {
        ProgramInfo near_miss;  // not `near`: windows.h still defines that as a legacy macro
        near_miss.displayName = name;
        programs.append(near_miss);
    }

    enumerator.markBloatware(programs);

    for (int i = 0; i < programs.size(); ++i) {
        QVERIFY2(!programs[i].isBloatware,
                 qPrintable(QStringLiteral("'%1' was wrongly flagged as bloatware")
                                .arg(programs[i].displayName)));
    }
    // Control: markBloatware really did run over this batch, so the negatives above are a verdict
    // rather than the struct default surviving an early return.
    ProgramInfo control;
    control.displayName = QStringLiteral("Xbox Game Bar");
    programs.append(control);
    enumerator.markBloatware(programs);
    QVERIFY2(programs.last().isBloatware, "control: a known pattern must still be flagged");
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
    // programs() returns the cache, which is written only at the very END of a successful
    // enumerateAll: it is empty BEFORE requestCancel() is called and stays empty regardless of
    // what these two calls do, so the assertion was already satisfied by the fixture's own
    // pre-state and the only two calls under test were entirely unobserved. The flag's real
    // observable is the directory walk, which breaks on it -- pinned in dirSize_countsFiles.
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    QFile file(QDir(temp_dir.path()).filePath(QStringLiteral("a.dat")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(64, 'A'));
    file.close();

    ProgramEnumerator enumerator;
    QCOMPARE(enumerator.calculateDirSize(temp_dir.path()), static_cast<qint64>(64));
    enumerator.requestCancel();
    QVERIFY2(enumerator.calculateDirSize(temp_dir.path()) == 0,
             "requestCancel() must stop the directory walk");
    enumerator.resetCancel();
    QVERIFY2(enumerator.calculateDirSize(temp_dir.path()) == 64,
             "resetCancel() must let the walk run again");
    QVERIFY(enumerator.programs().isEmpty());
}

void TestProgramEnumerator::enumerateAll_cancelledBeforeStart() {
    ProgramEnumerator enumerator;
    enumerator.requestCancel();

    QSignalSpy failed_spy(&enumerator, &ProgramEnumerator::enumerationFailed);
    // The test is named cancelledBeforeStart, and nothing asserted that the run was not STARTED.
    // The guard's whole contract is to refuse before enumerationStarted() is emitted: a driver
    // puts the UI into a scanning/busy state on that signal, so a start announced for a run that
    // never scans is exactly the state this early return exists to avoid -- and spying only the
    // failure signal cannot see it.
    QSignalSpy started_spy(&enumerator, &ProgramEnumerator::enumerationStarted);
    enumerator.enumerateAll(42);  // generation is echoed back so a stale run can be dropped

    QCOMPARE(failed_spy.count(), 1);
    QCOMPARE(started_spy.count(), 0);
    const auto args = failed_spy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 42);  // generation echoed
    QCOMPARE(args.at(1).toString(), QStringLiteral("Enumeration cancelled."));
    // No results were cached either: a refused run must not leave a partial catalogue behind.
    QVERIFY(enumerator.programs().isEmpty());
}

QTEST_MAIN(TestProgramEnumerator)
#include "test_program_enumerator.moc"
