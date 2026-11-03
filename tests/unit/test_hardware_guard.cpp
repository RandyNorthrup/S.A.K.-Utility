// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_hardware_guard.cpp
/// @brief A test binary is refused a real drive, by each route there is.
///
/// Each route below is harmless if the guard has been broken: it names a drive number no PC has,
/// or a volume that does not exist, or a command that only prints a word. So the suite can be
/// drilled on a person's PC, and a broken guard shows up as a child that was not refused.

#include "../support/test_hardware_guard.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtTest/QtTest>

#include <windows.h>

#include <shellapi.h>

namespace {

constexpr auto kRouteVariable = "SAK_HARDWARE_GUARD_ROUTE";
constexpr int kChildTimeoutMs = 60'000;
// A route reports back through its exit code.
constexpr int kReachedIt = 0;
constexpr int kRefused = 10;
constexpr int kUnknownRoute = 2;
const QString kGuardLine = QStringLiteral("SAK TEST HARDWARE GUARD");
// Below the guard's floor for "no PC has this many drives", and in practice no PC does either.
constexpr auto kAbsentDrivePath = L"\\\\.\\PhysicalDrive31";
const QString kPrintsAWord = QStringLiteral("Write-Output Get-PhysicalDisk");

int openWithWin32(const wchar_t* path) {
    HANDLE handle = CreateFileW(
        path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD error = GetLastError();
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return kReachedIt;
    }
    // Windows says "not found" for a drive that does not exist. Only the guard says this.
    return error == ERROR_ACCESS_DENIED ? kRefused : kReachedIt;
}

int openWithQt() {
    QFile drive(QString::fromWCharArray(kAbsentDrivePath));
    return drive.open(QIODevice::ReadOnly) ? kReachedIt : kRefused;
}

int startWithQProcess(const QString& script) {
    QProcess process;
    process.start(QStringLiteral("powershell.exe"),
                  {QStringLiteral("-NoProfile"),
                   QStringLiteral("-NonInteractive"),
                   QStringLiteral("-Command"),
                   script});
    (void)process.waitForFinished(kChildTimeoutMs);
    return process.error() == QProcess::FailedToStart ? kRefused : kReachedIt;
}

// A file that is not there, so a broken guard shows no prompt: Windows says "not found".
int askForElevation() {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = L"C:\\no_such_file_for_the_hardware_guard_test.exe";
    info.nShow = SW_HIDE;
    info.fMask = SEE_MASK_FLAG_NO_UI;
    const bool started = ShellExecuteExW(&info) != 0;
    return (!started && GetLastError() == ERROR_ACCESS_DENIED) ? kRefused : kReachedIt;
}

int openThisFile() {
    QFile self(QCoreApplication::applicationFilePath());
    return self.open(QIODevice::ReadOnly) ? kReachedIt : kRefused;
}

// The routes a child process takes.
int runRoute(const QByteArray& route) {
    if (route == "win32-drive") {
        return openWithWin32(kAbsentDrivePath);
    }
    if (route == "win32-volume") {
        return openWithWin32(L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\");
    }
    if (route == "qt-drive") {
        return openWithQt();
    }
    if (route == "qprocess-drive-query") {
        return startWithQProcess(kPrintsAWord);
    }
    if (route == "elevation") {
        return askForElevation();
    }
    if (route == "ordinary-file") {
        return openThisFile();
    }
    if (route == "ordinary-process") {
        return startWithQProcess(QStringLiteral("Write-Output hello"));
    }
    return kUnknownRoute;
}

struct ChildResult {
    int exit_code{-1};
    QString std_err;
};

ChildResult runChild(const QByteArray& route, bool allow_real_hardware) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(kRouteVariable), QString::fromLatin1(route));
    environment.remove(QStringLiteral("SAK_TEST_ALLOW_REAL_HARDWARE"));
    if (allow_real_hardware) {
        environment.insert(QStringLiteral("SAK_TEST_ALLOW_REAL_HARDWARE"), QStringLiteral("1"));
    }
    QProcess child;
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath());
    if (!child.waitForFinished(kChildTimeoutMs)) {
        child.kill();
        child.waitForFinished();
        return {};
    }
    return {child.exitCode(), QString::fromUtf8(child.readAllStandardError())};
}

}  // namespace

class HardwareGuardTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void stopsEveryRoute_data();
    void stopsEveryRoute();
    void leavesOrdinaryWorkAlone_data();
    void leavesOrdinaryWorkAlone();
    void standsDownOnlyWhenTheMachineSaysSo();
    void devicePaths_data();
    void devicePaths();
    void commands_data();
    void commands();
};

void HardwareGuardTests::stopsEveryRoute_data() {
    QTest::addColumn<QByteArray>("route");
    QTest::newRow("CreateFileW on a physical drive") << QByteArray("win32-drive");
    QTest::newRow("CreateFileW on a volume") << QByteArray("win32-volume");
    QTest::newRow("QFile on a physical drive") << QByteArray("qt-drive");
    QTest::newRow("QProcess running a drive query") << QByteArray("qprocess-drive-query");
    QTest::newRow("ShellExecuteEx asking for elevation") << QByteArray("elevation");
}

void HardwareGuardTests::stopsEveryRoute() {
    QFETCH(QByteArray, route);
    const ChildResult child = runChild(route, false);
    QCOMPARE(child.exit_code, kRefused);
    QVERIFY2(child.std_err.contains(kGuardLine), qPrintable(child.std_err));
}

void HardwareGuardTests::leavesOrdinaryWorkAlone_data() {
    QTest::addColumn<QByteArray>("route");
    QTest::newRow("a file") << QByteArray("ordinary-file");
    QTest::newRow("a process") << QByteArray("ordinary-process");
}

void HardwareGuardTests::leavesOrdinaryWorkAlone() {
    QFETCH(QByteArray, route);
    const ChildResult child = runChild(route, false);
    QCOMPARE(child.exit_code, kReachedIt);
    QVERIFY2(child.std_err.isEmpty(), qPrintable(child.std_err));
}

// CI sets the variable, so the suite there keeps running what it always ran. The route is one that
// reaches nothing even when it is let through.
void HardwareGuardTests::standsDownOnlyWhenTheMachineSaysSo() {
    for (const QByteArray route : {QByteArray("win32-drive"), QByteArray("qprocess-drive-query")}) {
        const ChildResult child = runChild(route, true);
        QCOMPARE(child.exit_code, kReachedIt);
        QVERIFY2(!child.std_err.contains(kGuardLine), qPrintable(child.std_err));
    }
}

void HardwareGuardTests::devicePaths_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("blocked");
    QTest::newRow("first drive") << QStringLiteral("\\\\.\\PhysicalDrive0") << true;
    QTest::newRow("other prefix") << QStringLiteral("\\\\?\\PHYSICALDRIVE1") << true;
    QTest::newRow("forward slashes") << QStringLiteral("//./PhysicalDrive2") << true;
    QTest::newRow("no number") << QStringLiteral("\\\\.\\PhysicalDrive") << true;
    QTest::newRow("highest real") << QStringLiteral("\\\\.\\PhysicalDrive31") << true;
    QTest::newRow("no PC has it") << QStringLiteral("\\\\.\\PhysicalDrive32") << false;
    QTest::newRow("the flash suites' absent drive")
        << QStringLiteral("\\\\.\\PhysicalDrive1023") << false;
    QTest::newRow("a USB drive's letter") << QStringLiteral("\\\\.\\Q:") << true;
    QTest::newRow("a letter, slash") << QStringLiteral("\\\\.\\Q:\\") << true;
    QTest::newRow("a file on it") << QStringLiteral("\\\\?\\Q:\\folder\\file.txt") << false;
    QTest::newRow("a volume") << QStringLiteral("\\\\?\\Volume{1234}\\") << true;
    QTest::newRow("a partition") << QStringLiteral("\\\\.\\Harddisk1\\Partition1") << true;
    QTest::newRow("a volume number") << QStringLiteral("\\\\.\\HarddiskVolume3") << true;
    QTest::newRow("globalroot") << QStringLiteral("\\\\?\\GLOBALROOT\\Device\\X") << true;
    QTest::newRow("a pipe") << QStringLiteral("\\\\.\\pipe\\sak") << false;
    QTest::newRow("a plain file") << QStringLiteral("C:\\Temp\\disk.img") << false;
    QTest::newRow("a UNC share") << QStringLiteral("\\\\server\\share\\file") << false;
}

void HardwareGuardTests::devicePaths() {
    QFETCH(QString, path);
    QFETCH(bool, blocked);
    QCOMPARE(!sak::test::blockedDevicePath(path.toStdWString()).empty(), blocked);
}

void HardwareGuardTests::commands_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<bool>("blocked");
    const auto powershell = [](const char* script) {
        return QStringLiteral("powershell.exe -NoProfile -Command \"%1\"")
            .arg(QString::fromLatin1(script));
    };
    QTest::newRow("reliability") << powershell("$_|Get-StorageReliabilityCounter") << true;
    QTest::newRow("disks") << powershell("Get-PhysicalDisk") << true;
    QTest::newRow("disk") << powershell("get-disk -Number 1") << true;
    QTest::newRow("volumes") << powershell("Get-Volume") << true;
    QTest::newRow("WMI disks") << powershell("Get-CimInstance Win32_DiskDrive") << true;
    QTest::newRow("Repair-Volume") << powershell("Repair-Volume -DriveLetter C -Scan") << true;
    QTest::newRow("SMART") << QStringLiteral("C:\\Tools\\smartctl.exe -a /dev/sda") << true;
    QTest::newRow("sfc") << QStringLiteral("C:\\Windows\\System32\\sfc.exe /scannow") << true;
    QTest::newRow("dism") << QStringLiteral("dism /online /cleanup-image") << true;
    QTest::newRow("chkdsk") << QStringLiteral("chkdsk C:") << true;
    QTest::newRow("the application") << QStringLiteral("D:\\b\\sak_utility.exe --flag") << true;
    QTest::newRow("flush DNS") << QStringLiteral("ipconfig.exe /flushdns") << true;
    QTest::newRow("an echo") << powershell("Write-Output hello") << false;
    QTest::newRow("a word inside a name") << QStringLiteral("tool.exe sfcdata dismiss") << false;
    QTest::newRow("CPU heat") << powershell("Get-CimInstance MSAcpi_ThermalZone") << false;
    QTest::newRow("the test itself") << QStringLiteral("test_thermal_monitor.exe") << false;
}

void HardwareGuardTests::commands() {
    QFETCH(QString, command);
    QFETCH(bool, blocked);
    QCOMPARE(!sak::test::blockedCommand(command.toStdWString()).empty(), blocked);
}

int main(int argc, char** argv) {
    const QByteArray route = qgetenv(kRouteVariable);
    if (!route.isEmpty()) {
        const QCoreApplication app(argc, argv);
        return runRoute(route);
    }
    const QCoreApplication app(argc, argv);
    HardwareGuardTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_hardware_guard.moc"
