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

    QVERIFY(result.block_error.isEmpty());
    QVERIFY(result.approval_reason.isEmpty());
}

QTEST_GUILESS_MAIN(AiCommandGuardTests)
#include "test_ai_command_guard.moc"
