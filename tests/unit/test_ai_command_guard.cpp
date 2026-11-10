// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_guard.h"

#include <QtTest/QtTest>

#include <type_traits>

template <typename T>
concept PublicGuardStorage = requires(T value) {
    value.m_block_error;
    value.m_approval_reason;
};

static_assert(!std::is_default_constructible_v<sak::ai::AiCommandGuardResult>);
static_assert(!std::is_aggregate_v<sak::ai::AiCommandGuardResult>);
static_assert(!PublicGuardStorage<sak::ai::AiCommandGuardResult>);

class AiCommandGuardTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void blocksBinaryContentDump();
    void binaryReadCheckIsClauseAndQuoteScoped();
    void nestedAndDirectProcessBinaryReadsAreBlocked();
    void blocksBroadRegistryRecursion();
    void registryRecursionCheckIsClauseScoped();
    void scriptBlockBodyStaysWithTheValueItActsOn();
    void blocksPowerShellPidMutation();
    void blocksChecksumBypass();
    void checksumBypassCatchesEverySpellingAndSubstitution();
    void asksApprovalForCachedPackageInstallerRun();
    void asksApprovalForEveryChocolateyCacheInstaller();
    void cacheApprovalIsClauseScopedAndRequiresInstaller();
    void directProcessCacheInstallerRequiresApproval();
    void safeReadOnlyCommandPasses();
    void checksumFeatureDisableIsBlocked();
    void tmpEnvironmentCacheInstallerNeedsApproval();
    void registryHiveRootsAndWildcardsAreBroad();
};

void AiCommandGuardTests::blocksBinaryContentDump() {
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Get-Content 'C:\\Program Files\\App\\tool.exe'");

    const sak::ai::AiCommandGuardResult result = sak::ai::evaluateCommandGuard(request);

    QCOMPARE(result.blockError(),
             QStringLiteral("Blocked binary file dump. Use Get-Item, Get-FileHash, Authenticode "
                            "signature checks, or Format-Hex -Count for a small sample instead of "
                            "Get-Content/cat/type."));
    QVERIFY(result.approvalReason().isEmpty());
}

void AiCommandGuardTests::binaryReadCheckIsClauseAndQuoteScoped() {
    const auto blockError = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return sak::ai::evaluateCommandGuard(request).blockError();
    };

    QVERIFY(blockError(QStringLiteral("Get-Content notes.txt; Get-FileHash tool.exe")).isEmpty());
    QVERIFY(blockError(QStringLiteral("Get-Content notes.txt | Get-FileHash tool.exe")).isEmpty());
    QVERIFY(blockError(QStringLiteral("Write-Output 'safe; Get-Content tool.exe'")).isEmpty());
    QVERIFY(blockError(QStringLiteral("Write-Output \"Get-Content tool.exe\"")).isEmpty());
    QVERIFY(blockError(QStringLiteral("Write-Output '$(Get-Content tool.exe)'")).isEmpty());
    QVERIFY(blockError(QStringLiteral("Write-Output \"`$(Get-Content tool.exe)\"")).isEmpty());
    QVERIFY(!blockError(QStringLiteral("Get-Content tool.exe; Get-Date")).isEmpty());
    QVERIFY(!blockError(QStringLiteral("Get-Date; Get-Content 'tool.exe'")).isEmpty());
    QVERIFY(!blockError(QStringLiteral("& 'Get-Content' 'tool.exe'")).isEmpty());
    QVERIFY(!blockError(QStringLiteral("if ($true) { & 'Get-Content' 'tool.exe' }")).isEmpty());
    QVERIFY(!blockError(QStringLiteral("Write-Output \"$(Get-Content 'tool.exe')\"")).isEmpty());
}

void AiCommandGuardTests::nestedAndDirectProcessBinaryReadsAreBlocked() {
    sak::ai::AiCommandRequest control_clause;
    control_clause.command = QStringLiteral("if ($true) { Get-Content 'tool.exe' }");
    QVERIFY(!sak::ai::evaluateCommandGuard(control_clause).blockError().isEmpty());

    sak::ai::AiCommandRequest direct_process;
    direct_process.program =
        QStringLiteral("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    direct_process.arguments = {QStringLiteral("-NoProfile"),
                                QStringLiteral("-Command"),
                                QStringLiteral("Get-Content 'tool.exe'")};
    QVERIFY(!sak::ai::evaluateCommandGuard(direct_process).blockError().isEmpty());
}

void AiCommandGuardTests::blocksBroadRegistryRecursion() {
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Get-ChildItem HKLM:\\Software -Recurse");

    const QString error = sak::ai::evaluateCommandGuard(request).blockError();

    QCOMPARE(error,
             QStringLiteral("Blocked broad recursive registry scan. Query exact uninstall/vendor "
                            "keys instead and cap output with Select-Object -First."));
}

void AiCommandGuardTests::registryRecursionCheckIsClauseScoped() {
    const auto blockError = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return sak::ai::evaluateCommandGuard(request).blockError();
    };
    const QString broad = QStringLiteral("Get-ChildItem HKLM:\\Software -Recurse");
    const QString narrow = QStringLiteral(
        "Get-ChildItem HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall "
        "-Recurse");

    QVERIFY(!blockError(broad + QStringLiteral("; ") + narrow).isEmpty());
    QVERIFY(blockError(narrow).isEmpty());
    QVERIFY(blockError(QStringLiteral("Get-Item HKLM:\\Software; "
                                      "Get-ChildItem C:\\Windows -Recurse"))
                .isEmpty());
    QVERIFY(blockError(QStringLiteral("Get-ChildItem HKCU:\\Software -Recurse:$false")).isEmpty());
    QVERIFY(!blockError(QStringLiteral("Get-ChildItem HKCU:\\Software -Recurse:$true")).isEmpty());
    QVERIFY(!blockError(QStringLiteral("Get-ChildItem HKLM:\\Software\\ -Recurse")).isEmpty());
    QVERIFY(blockError(QStringLiteral("Get-ChildItem HKLM:\\Software\\Vendor -Recurse")).isEmpty());
    for (const QString& root : {
             QStringLiteral("Registry::HKEY_LOCAL_MACHINE\\Software"),
             QStringLiteral("Registry::HKEY_CURRENT_USER\\Software"),
         }) {
        QVERIFY2(!blockError(QStringLiteral("Get-ChildItem %1 -Recurse").arg(root)).isEmpty(),
                 qPrintable(root));
    }
    for (const QString& recurse : {
             QStringLiteral("-Rec"),
             QStringLiteral("-Recu"),
             QStringLiteral("-Recur"),
             QStringLiteral("-Recurs"),
             QStringLiteral("-Recurse"),
         }) {
        QVERIFY2(
            !blockError(QStringLiteral("Get-ChildItem HKLM:\\Software %1").arg(recurse)).isEmpty(),
            qPrintable(recurse));
    }
    QVERIFY(blockError(QStringLiteral("Get-ChildItem HKLM:\\Software -Rec:$false")).isEmpty());
}

void AiCommandGuardTests::scriptBlockBodyStaysWithTheValueItActsOn() {
    // GUARD-6 residual, kept on purpose. A `{ }` block is not a clause separator, so
    // `if (Test-Path HKLM:\Software) { gci C:\x -Recurse }` over-blocks. Splitting at braces --
    // even a split that keeps a `${env:TEMP}` variable whole -- moves a loop or script block's
    // body into a clause apart from the value it acts on, and the guards' own targets, written
    // through a loop variable or a script-block parameter, would then pass. Telling the `if`
    // apart from the `foreach` needs variable data flow, not a better tokenizer.
    const auto blockError = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return sak::ai::evaluateCommandGuard(request).blockError();
    };
    const QString registry_scan = QStringLiteral(
        "Blocked broad recursive registry scan. Query exact uninstall/vendor keys instead and cap "
        "output with Select-Object -First.");
    QCOMPARE(blockError(
                 QStringLiteral("foreach ($k in 'HKLM:\\Software') { Get-ChildItem $k -Recurse }")),
             registry_scan);
    QCOMPARE(blockError(QStringLiteral("Invoke-Command -ScriptBlock { param($p) Get-ChildItem $p "
                                       "-Recurse } -ArgumentList 'HKLM:\\Software'")),
             registry_scan);
    QVERIFY(blockError(QStringLiteral("foreach ($f in 'C:\\tools\\tool.exe') { Get-Content $f }"))
                .startsWith(QStringLiteral("Blocked binary file dump.")));
}

void AiCommandGuardTests::blocksPowerShellPidMutation() {
    sak::ai::AiCommandRequest request;
    request.command =
        QStringLiteral("$pid=0; [void][Win32]::GetWindowThreadProcessId($hWnd,[ref]$pid)");

    const QString error = sak::ai::evaluateCommandGuard(request).blockError();

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
        return !sak::ai::evaluateCommandGuard(request).blockError().isEmpty();
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

    const sak::ai::AiCommandGuardResult result = sak::ai::evaluateCommandGuard(request);

    QCOMPARE(result.blockError(),
             QStringLiteral("Blocked package checksum bypass. Do not pass --ignore-checksums, "
                            "substitute checksums, or run cached installers after a package "
                            "checksum mismatch."));
    QVERIFY(result.approvalReason().isEmpty());
}

void AiCommandGuardTests::asksApprovalForCachedPackageInstallerRun() {
    sak::ai::AiCommandRequest request;
    request.command =
        QStringLiteral("Start-Process 'C:/x/data/temp/chocolatey/pkg/1.0/setup.exe' -Wait");

    const QString reason = sak::ai::evaluateCommandGuard(request).approvalReason();

    QCOMPARE(reason,
             QStringLiteral("Cached package installer execution requested after package-manager "
                            "handling. Continue only with explicit user approval and verification "
                            "evidence."));
}

void AiCommandGuardTests::asksApprovalForEveryChocolateyCacheInstaller() {
    const auto approvalReason = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return sak::ai::evaluateCommandGuard(request).approvalReason();
    };
    const QStringList commands = {
        QStringLiteral("& '$env:TEMP\\chocolatey\\pkg\\setup.exe'"),
        QStringLiteral("& '${env:TEMP}\\chocolatey\\pkg\\setup.msi'"),
        QStringLiteral("%TEMP%\\chocolatey\\pkg\\setup.msix"),
        QStringLiteral("$env:ChocolateyInstall\\lib-bad\\pkg\\setup.exe"),
        QStringLiteral("${env:ChocolateyInstall}\\cache\\pkg\\setup.msi"),
        QStringLiteral("%ChocolateyInstall%\\temp\\pkg\\setup.appx"),
        QStringLiteral("C:\\SAK\\data\\temp\\chocolatey\\pkg\\setup.msixbundle"),
        QStringLiteral("C:\\Users\\Username\\AppData\\Local\\Temp\\chocolatey\\pkg\\setup.exe"),
        QStringLiteral("C:\\ProgramData\\chocolatey\\lib-bad\\pkg\\setup.exe"),
        QStringLiteral("C:/ProgramData/chocolatey/cache/pkg/setup.exe"),
        QStringLiteral("C:\\ProgramData\\chocolatey\\temp\\pkg\\setup.exe"),
    };
    for (const QString& command : commands) {
        QVERIFY2(!approvalReason(command).isEmpty(), qPrintable(command));
    }
}

void AiCommandGuardTests::cacheApprovalIsClauseScopedAndRequiresInstaller() {
    const auto approvalReason = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return sak::ai::evaluateCommandGuard(request).approvalReason();
    };
    const QStringList benign = {
        QStringLiteral("Get-Content C:\\ProgramData\\chocolatey\\cache\\notes.txt"),
        QStringLiteral("C:\\ProgramData\\chocolatey\\lib\\pkg\\tools\\setup.exe"),
        QStringLiteral("C:\\notchocolatey\\cache\\setup.exe"),
        QStringLiteral("Get-Item C:\\ProgramData\\chocolatey\\cache\\notes.txt; "
                       "Start-Process C:\\Verified\\setup.exe"),
    };
    for (const QString& command : benign) {
        QVERIFY2(approvalReason(command).isEmpty(), qPrintable(command));
    }
}

void AiCommandGuardTests::directProcessCacheInstallerRequiresApproval() {
    sak::ai::AiCommandRequest request;
    request.program = QStringLiteral("C:\\ProgramData\\chocolatey\\cache\\pkg\\setup.exe");
    request.arguments = {QStringLiteral("/quiet")};

    const auto result = sak::ai::evaluateCommandGuard(request);
    QVERIFY(!result.allowed());
    QVERIFY(!result.blocked());
    QVERIFY(result.requiresApproval());
    QVERIFY(result.blockError().isEmpty());
    QVERIFY(!result.approvalReason().isEmpty());
}

void AiCommandGuardTests::safeReadOnlyCommandPasses() {
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral(
        "Get-Item 'C:\\Program Files\\App\\tool.exe'; Get-FileHash 'C:\\Program "
        "Files\\App\\tool.exe'");

    const sak::ai::AiCommandGuardResult result = sak::ai::evaluateCommandGuard(request);

    QVERIFY(result.allowed());
    QVERIFY(!result.blocked());
    QVERIFY(!result.requiresApproval());
    QVERIFY(result.blockError().isEmpty());
    QVERIFY(result.approvalReason().isEmpty());
}

void AiCommandGuardTests::checksumFeatureDisableIsBlocked() {
    // NEW-3. Turning Chocolatey's checksumFiles feature off disables verification for EVERY later
    // install. The switch rule needs '-' or '/' directly before "checksum", which a feature NAME
    // never has, so this passed -- and in Unattended mode ran without a prompt.
    const auto blockError = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return sak::ai::evaluateCommandGuard(request).blockError();
    };
    for (const QString& command : {
             QStringLiteral("choco feature disable -n checksumFiles"),
             QStringLiteral("choco feature disable --name=\"checksumFiles\""),
             QStringLiteral("choco feature disable -n=checksumFiles -y"),
             QStringLiteral("& 'C:\\ProgramData\\chocolatey\\bin\\choco.exe' feature disable "
                            "--name 'checksumFiles'"),
             QStringLiteral("choco feature -n checksumFiles disable"),
             // Enabling empty checksums was already blocked; it stays blocked.
             QStringLiteral("choco feature enable -n allowEmptyChecksums"),
         }) {
        QVERIFY2(blockError(command).startsWith(QStringLiteral("Blocked package checksum bypass")),
                 qPrintable(command));
    }
    // Re-enabling verification, and disabling an unrelated feature, are ordinary.
    QVERIFY(blockError(QStringLiteral("choco feature enable -n checksumFiles")).isEmpty());
    QVERIFY(blockError(QStringLiteral("choco feature disable -n showDownloadProgress")).isEmpty());
    QVERIFY(blockError(QStringLiteral("choco feature list")).isEmpty());
}

void AiCommandGuardTests::tmpEnvironmentCacheInstallerNeedsApproval() {
    // NEW-4. TMP names the temp folder as TEMP does, and the cache rule knew only TEMP.
    for (const QString& command : {
             QStringLiteral("& \"$env:TMP\\chocolatey\\pkg\\1.0\\setup.exe\" /S"),
             QStringLiteral("Start-Process \"${env:TMP}\\chocolatey\\pkg\\1.0\\setup.msi\""),
             QStringLiteral("%TMP%\\chocolatey\\pkg\\1.0\\setup.exe /quiet"),
         }) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        const auto result = sak::ai::evaluateCommandGuard(request);
        QVERIFY2(result.blockError().isEmpty(), qPrintable(command));
        QVERIFY2(!result.approvalReason().isEmpty(), qPrintable(command));
    }
}

void AiCommandGuardTests::registryHiveRootsAndWildcardsAreBroad() {
    // NEW-5. Only HKLM:\Software and the long provider form were recognised as a broad root, so
    // a wildcard, a bare hive root and the short provider form all recursed the whole hive.
    const auto blockError = [](const QString& command) {
        sak::ai::AiCommandRequest request;
        request.command = command;
        return sak::ai::evaluateCommandGuard(request).blockError();
    };
    for (const QString& root : {
             QStringLiteral("HKLM:\\Software\\*"),
             QStringLiteral("HKLM:\\"),
             QStringLiteral("HKLM:"),
             QStringLiteral("HKCU:Software"),
             QStringLiteral("Registry::HKLM\\Software"),
             QStringLiteral("Registry::HKEY_LOCAL_MACHINE"),
             QStringLiteral("'HKCU:\\Software\\*'"),
         }) {
        QVERIFY2(blockError(QStringLiteral("Get-ChildItem %1 -Recurse").arg(root))
                     .startsWith(QStringLiteral("Blocked broad recursive registry scan")),
                 qPrintable(root));
    }
    // An exact vendor or uninstall key stays allowed, wildcard included.
    QVERIFY(blockError(QStringLiteral("Get-ChildItem "
                                      "HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\"
                                      "Uninstall\\* -Recurse"))
                .isEmpty());
    QVERIFY(blockError(QStringLiteral("Get-ChildItem Registry::HKLM\\Software\\Vendor -Recurse"))
                .isEmpty());
}

QTEST_GUILESS_MAIN(AiCommandGuardTests)
#include "test_ai_command_guard.moc"
