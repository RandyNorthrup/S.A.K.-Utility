// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_drive_unmounter.cpp
/// @brief Unit tests for DriveUnmounter

#include "sak/drive_unmounter.h"

#include <QtTest/QtTest>

class TestDriveUnmounter : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void lastError_emptyInitially();
    void getVolumesOnDrive_invalidDrive();
    void getVolumesOnDrive_negativeDriveFailsClosed();  // R5-G10-9
    void ejectDrive_rejectsNegativeDriveNumber();
    void lockVolume_failedLockRegistersNothing();       // P11-18 sibling: P11-13
};

void TestDriveUnmounter::construction_default() {
    DriveUnmounter unmounter;
    // The vacuous upcast can never be null; pin the moc metaobject name instead (proves Q_OBJECT).
    QCOMPARE(QString(unmounter.metaObject()->className()), QStringLiteral("DriveUnmounter"));
}

void TestDriveUnmounter::lastError_emptyInitially() {
    DriveUnmounter unmounter;
    QVERIFY(unmounter.lastError().isEmpty());
}

void TestDriveUnmounter::getVolumesOnDrive_invalidDrive() {
    DriveUnmounter unmounter;
    // Drive 999 should not exist on any system
    const auto volumes = unmounter.getVolumesOnDrive(999);
    QVERIFY(volumes.isEmpty());
}

void TestDriveUnmounter::getVolumesOnDrive_negativeDriveFailsClosed() {
    // A negative drive number cannot name a physical disk and aliases the volume-probe
    // sentinels. getVolumesOnDrive must REFUSE it -- reporting enumerationOk=false -- and
    // never hand back an authoritative empty list: a caller gating a raw disk write on "no
    // volumes mounted on this drive" would read an unflagged empty list as "safe to
    // overwrite". The empty list alone is NOT the signal; enumerationOk is.
    DriveUnmounter unmounter;
    bool negativeOk = true;
    const auto negativeVolumes = unmounter.getVolumesOnDrive(-1, &negativeOk);
    QVERIFY(negativeVolumes.isEmpty());
    QVERIFY(!negativeOk);  // refused -- fail closed, NOT an authoritative empty

    // Non-vacuity control: a valid, non-negative drive number is NOT refused by this guard.
    // Enumeration runs and reports SUCCESS (enumerationOk stays true) even though drive 999
    // matches nothing -- so the false above is the negative-refusal signal, not a blanket
    // "always not-ok" and not merely "the list came back empty". Were the negative guard
    // removed, -1 would fall through to this same successful-but-empty enumeration and
    // report ok=true, which is exactly the fail-open the assertion above forbids.
    bool validOk = false;
    const auto validVolumes = unmounter.getVolumesOnDrive(999, &validOk);
    QVERIFY(validVolumes.isEmpty());
    QVERIFY(validOk);
}

void TestDriveUnmounter::ejectDrive_rejectsNegativeDriveNumber() {
    // A negative drive number cannot name a physical disk, and it also aliases the
    // volume-probe sentinels used during enumeration. Refuse it before any device is
    // opened, and say why -- a bare false with no lastError would be reported to the
    // user as an unexplained eject failure.
    DriveUnmounter unmounter;
    QVERIFY(!unmounter.ejectDrive(-1));
    QCOMPARE(unmounter.lastError(), QStringLiteral("Refusing to eject invalid drive number -1"));
}

// P11-13. lockVolume hands back an EXCLUSIVELY locked volume handle. It now registers
// that handle so ~DriveUnmounter closes it, because the caller borrowing it had no way to
// release it: the lock survived the operation for the life of the process, and Windows
// could not remount the drive the user had just been told was ejected.
//
// COVERAGE LIMIT, stated rather than papered over: the success path cannot be unit-tested
// here. Proving the handle is registered requires actually taking an exclusive lock on a
// real volume, and the only volumes present on a test machine are ones in use -- locking
// one is exactly the destructive act this guard exists to avoid. What IS provable without
// touching a volume is the other half of the contract: a lock that FAILS must register
// nothing (a registered INVALID_HANDLE_VALUE would be closed at destruction) and must say
// why. The success path is held by the ownership contract documented on the declaration.
void TestDriveUnmounter::lockVolume_failedLockRegistersNothing() {
    DriveUnmounter unmounter;
    // A volume path that cannot resolve: CreateFileW fails, so no handle exists to track.
    const HANDLE handle = unmounter.lockVolume(QStringLiteral("\\\\.\\NoSuchVolume999:"));
    QCOMPARE(handle, INVALID_HANDLE_VALUE);
    // And it reports the real reason -- an empty lastError would surface to the user as an
    // unexplained failure, and would also pass a bare "returned invalid handle" assertion.
    QVERIFY(unmounter.lastError().startsWith(QStringLiteral("CreateFile failed: error ")));
}

QTEST_MAIN(TestDriveUnmounter)
#include "test_drive_unmounter.moc"
