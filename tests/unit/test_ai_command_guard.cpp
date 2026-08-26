// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_guard.h"

#include <QtTest/QtTest>

class AiCommandGuardTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void blocksBinaryContentDump();
    void blocksBroadRegistryRecursion();
    void blocksPowerShellPidMutation();
    void blocksChecksumBypass();
    void checksumBypassCatchesEverySpellingAndSubstitution();
    void asksApprovalForCachedPackageInstallerRun();
    void safeReadOnlyCommandPasses();
};

void AiCommandGuardTests::blocksBinaryContentDump() {
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Get-Content 'C:\\Program Files\\App\\tool.exe'");

    const sak::ai::AiCommandGuardResult result = sak::ai::evaluateCommandGuard(request,
                                                                               request.command);

    QCOMPARE(result.block_error,
             QStringLiteral("Blocked binary file dump. Use Get-Item, Get-FileHash, Authenticode "
                            "signature checks, or Format-Hex -Count for a small sample instead of "
                            "Get-Content/cat/type."));
    QVERIFY(result.approval_reason.isEmpty());
}

void AiCommandGuardTests::blocksBroadRegistryRecursion() {
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Get-ChildItem HKLM:\\Software -Recurse");

    const QString error = sak::ai::commandGuardBlockError(request, request.command);

    QCOMPARE(error,
             QStringLiteral("Blocked broad recursive registry scan. Query exact uninstall/vendor "
                            "keys instead and cap output with Select-Object -First."));
}

void AiCommandGuardTests::blocksPowerShellPidMutation() {
    sak::ai::AiCommandRequest request;
    request.command =
        QStringLiteral("$pid=0; [void][Win32]::GetWindowThreadProcessId($hWnd,[ref]$pid)");

    const QString error = sak::ai::commandGuardBlockError(request, request.command);

    QCOMPARE(error,
             QStringLiteral("Blocked PowerShell $PID mutation. $PID is a read-only automatic "
                            "variable; use a different variable such as $processId or "
                            "$windowProcessId."));
}

void AiCommandGuardTests::checksumBypassCatchesEverySpellingAndSubstitution() {
    // R5-LEDGER. This guard is the thing standing between a model-issued command and an
    // unverified package install, and it had two holes:
    //
    //   1. It listed FIXED spellings and claimed "the singular spellings also catch the plural
    //      switches". That only held for the unhyphenated singular. `--ignore-checksum` --
    //      hyphenated singular, which Chocolatey accepts -- matched nothing and passed.
    //   2. Its refusal message has always claimed to stop "substitute checksums", but nothing
    //      checked for them. Supplying --checksum/--download-checksum verifies the download
    //      against a value the CALLER chose, defeating verification just as completely.
    const auto blocked = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return !sak::ai::commandGuardBlockError(request, QString()).isEmpty();
    };

    // Every hyphenation and plurality of each bypass switch.
    for (const QString& command : {
             QStringLiteral("choco install x -y --ignore-checksums"),
             QStringLiteral("choco install x -y --ignore-checksum"),  // was NOT blocked
             QStringLiteral("choco install x -y --ignorechecksums"),
             QStringLiteral("choco install x -y --ignorechecksum"),
             QStringLiteral("choco install x -y --allow-empty-checksums"),
             QStringLiteral("choco install x -y --allow-empty-checksum"),  // was NOT blocked
             QStringLiteral("choco install x -y --allowemptychecksum"),
             QStringLiteral("choco install x -y --skip-checksums"),
             QStringLiteral("choco install x -y --skip-checksum"),  // was NOT blocked
             QStringLiteral("choco install x -y --skipchecksum"),
         }) {
        QVERIFY2(blocked(command), qPrintable(command));
    }

    // Checksum SUBSTITUTION, which the message promised to stop and the code never checked.
    for (const QString& command : {
             QStringLiteral("choco install x -y --checksum=DEADBEEF"),
             QStringLiteral("choco install x -y --checksum64 DEADBEEF"),
             QStringLiteral("choco install x -y --download-checksum=DEADBEEF"),
             QStringLiteral("choco install x -y --download-checksum-x64 DEADBEEF"),
             QStringLiteral("choco install x -y --downloadchecksum DEADBEEF"),
             QStringLiteral("choco install x -y --checksumtype sha1"),
             QStringLiteral("choco install x -y --checksum-type sha1"),
         }) {
        QVERIFY2(blocked(command), qPrintable(command));
    }

    // A single-dash form and a slash form are the same switch.
    QVERIFY(blocked(QStringLiteral("choco install x -ignore-checksum")));
    QVERIFY(blocked(QStringLiteral("choco install x /ignore-checksum")));

    // The environment / config spellings carry no switch prefix and must still be caught.
    QVERIFY(blocked(QStringLiteral("$env:ChocolateyIgnoreChecksums='true'; choco install x -y")));
    QVERIFY(blocked(QStringLiteral("$env:ChocolateyAllowEmptyChecksums='true'; choco install x")));

    // ...and the guard must NOT block ordinary text that merely mentions a checksum, or it
    // becomes noise the operator learns to click through. A switch prefix is required.
    for (const QString& benign : {
             QStringLiteral("Get-FileHash installer.exe  # compare the checksum by hand"),
             QStringLiteral("choco install x -y"),
             QStringLiteral("Write-Output 'checksum verified'"),
             QStringLiteral("Get-Content checksums.txt"),
         }) {
        QVERIFY2(!blocked(benign), qPrintable(benign));
    }
}

void AiCommandGuardTests::blocksChecksumBypass() {
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("choco install superantispyware -y --ignore-checksums");

    const sak::ai::AiCommandGuardResult result = sak::ai::evaluateCommandGuard(request,
                                                                               request.command);

    QCOMPARE(result.block_error,
             QStringLiteral("Blocked package checksum bypass. Do not pass --ignore-checksums, "
                            "substitute checksums, or run cached installers after a package "
                            "checksum mismatch."));
    QVERIFY(result.approval_reason.isEmpty());
}

void AiCommandGuardTests::asksApprovalForCachedPackageInstallerRun() {
    sak::ai::AiCommandRequest request;
    request.command =
        QStringLiteral("Start-Process 'C:/x/data/temp/chocolatey/pkg/1.0/setup.exe' -Wait");

    const QString reason = sak::ai::commandGuardApprovalReason(request, request.command);

    QCOMPARE(reason,
             QStringLiteral("Cached package installer execution requested after package-manager "
                            "handling. Continue only with explicit user approval and verification "
                            "evidence."));
}

void AiCommandGuardTests::safeReadOnlyCommandPasses() {
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral(
        "Get-Item 'C:\\Program Files\\App\\tool.exe'; Get-FileHash 'C:\\Program "
        "Files\\App\\tool.exe'");

    const sak::ai::AiCommandGuardResult result = sak::ai::evaluateCommandGuard(request,
                                                                               request.command);

    // isEmpty() alone is also satisfied by a default (un-run) result; pin the derived contract so
    // a guard that returned an un-evaluated default would fail.
    QVERIFY(result.evaluated);
    QVERIFY(result.allowed());
    QVERIFY(!result.requiresApproval());
    QVERIFY(result.block_error.isEmpty());
    QVERIFY(result.approval_reason.isEmpty());
}

QTEST_GUILESS_MAIN(AiCommandGuardTests)
#include "test_ai_command_guard.moc"
