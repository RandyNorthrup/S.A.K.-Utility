// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_tool_planner.h"

#include <QDir>
#include <QJsonArray>
#include <QtTest/QtTest>

class AiCommandToolPlannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsPowerShellPlanWithPolicy();
    void buildsProcessPlanWithProgramPreview();
    void rejectsRelativeProcessProgram();
    void rejectsUnresolvableProcessProgram();
    void processPreviewQuotesAmbiguousArgs();
    void marksRiskyCommandAndPolicyDenial();
    void carriesPidMutationGuardBlock();
    void carriesChecksumBypassBlock();
    void rejectsNonCanonicalToolName();  // R5-G10-9
    void shellPreviewIsSanitizedBeforeItIsShownToAHuman();
    void validationFailureIsAlwaysRisky();
};

void AiCommandToolPlannerTests::buildsPowerShellPlanWithPolicy() {
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Get-Date");

    const auto plan =
        sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                 args,
                                                 sak::ai::AiToolPolicy::ReadOnlyPc,
                                                 sak::ai::AiCommandToolPlanner::Options{4096});

    QCOMPARE(plan.shell_label, QStringLiteral("PowerShell"));
    QCOMPARE(plan.preview, QStringLiteral("Get-Date"));
    QCOMPARE(plan.request.command, QStringLiteral("Get-Date"));
    QCOMPARE(plan.request.max_output_bytes, 4096);
    QVERIFY(plan.policy_decision.allowed);
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Read-only diagnostic shell command allowed"));
    QVERIFY(!plan.risky_change);
    QVERIFY(plan.guard_block_error.isEmpty());
}

void AiCommandToolPlannerTests::buildsProcessPlanWithProgramPreview() {
    QJsonObject args;
    // A bare name (cmd.exe is present under System32 on every Windows install).
    args[QStringLiteral("program")] = QStringLiteral("cmd.exe");
    args[QStringLiteral("arguments")] = QJsonArray{QStringLiteral("a.txt")};

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QCOMPARE(plan.shell_label, QStringLiteral("Process"));
    QCOMPARE(plan.request.arguments, QStringList{QStringLiteral("a.txt")});
#ifndef Q_OS_WIN
    QSKIP("bare-name resolution to an absolute path is Windows-specific");
#else
    // F43: the bare program name is resolved to an absolute, verified path AT PLAN TIME, and
    // both the stored request AND the preview (the text the human approves) name that resolved
    // binary -- not the bare string the broker would otherwise resolve against the process
    // search order only AFTER the user approved.
    QVERIFY(plan.guard_block_error.isEmpty());
    QVERIFY2(QDir::isAbsolutePath(plan.request.program), qPrintable(plan.request.program));
    QVERIFY2(plan.request.program.endsWith(QStringLiteral("cmd.exe"), Qt::CaseInsensitive),
             qPrintable(plan.request.program));
    QVERIFY(plan.preview.contains(QStringLiteral("cmd.exe"), Qt::CaseInsensitive));
    QVERIFY(plan.preview.contains(QStringLiteral("a.txt")));
    // A direct process launch can never be proven safe from its command line, so ReadOnlyPc
    // refuses it even after the program resolves cleanly. The plan is still built (request +
    // preview populated) for display.
    QVERIFY(!plan.policy_decision.allowed);
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Read-only PC policy allows only known read-only diagnostic shell "
                            "commands (ping, ipconfig, systeminfo, tasklist, netstat, whoami, "
                            "Get-*/Test-* reads, ...); this command is not on the read-only "
                            "allowlist"));
#endif
}

void AiCommandToolPlannerTests::rejectsRelativeProcessProgram() {
    QJsonObject args;
    // A working-directory-relative path has no defensible base and would launch CWD-relative;
    // it must be refused at plan time rather than resolved against the current directory.
    args[QStringLiteral("program")] = QStringLiteral("sub/planted.exe");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    // The refusal echoes the sanitized offending path -- the security-relevant part an injection
    // or regression would corrupt -- and guard_block_error is copied verbatim from it.
    const QString kRelativeError = QStringLiteral(
        "program must be an absolute path or a bare executable name, not a "
        "working-directory-relative path: sub/planted.exe");
    QCOMPARE(plan.request.validation_error, kRelativeError);
    QCOMPARE(plan.guard_block_error, kRelativeError);
    QVERIFY(!plan.policy_decision.allowed);
}

void AiCommandToolPlannerTests::rejectsUnresolvableProcessProgram() {
    QJsonObject args;
    // A bare name that resolves nowhere under System32 or the absolute PATH entries fails
    // closed instead of being handed to the broker to resolve after approval.
    args[QStringLiteral("program")] = QStringLiteral("definitely-not-a-real-sak-test-binary.exe");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    // The refusal echoes the sanitized bare name it could not resolve; guard_block_error is the
    // same string. Pin both so a dropped/garbled echo cannot pass unnoticed.
    const QString kResolveError = QStringLiteral(
        "Cannot resolve program 'definitely-not-a-real-sak-test-binary.exe' to an absolute "
        "path; refusing to launch a bare name");
    QCOMPARE(plan.request.validation_error, kResolveError);
    QCOMPARE(plan.guard_block_error, kResolveError);
    QVERIFY(!plan.policy_decision.allowed);
}

void AiCommandToolPlannerTests::processPreviewQuotesAmbiguousArgs() {
    QJsonObject args;
    args[QStringLiteral("program")] = QStringLiteral("C:\\Program Files\\app.exe");
    args[QStringLiteral("arguments")] = QJsonArray{QStringLiteral("hello world"),
                                                   QStringLiteral("plain"),
                                                   QStringLiteral("tab\there")};

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    // The whole preview is deterministic: the space-bearing absolute program path and the
    // space-bearing arg are each quoted as one token, the plain arg is left bare, and the
    // embedded tab is escaped visibly (never emitted raw). Pin the exact rendered command -- the
    // security-relevant text the human approves -- which subsumes the per-token quoting, ordering,
    // and no-raw-tab checks in a single assertion.
    //
    // THE ESCAPE FORM CHANGED DELIBERATELY, from "\t" to "<TAB>". A backslash escape is ambiguous
    // the moment a value contains the literal characters '\' and 't', and disambiguating it would
    // mean doubling every backslash -- unreadable in a product whose commands are full of Windows
    // paths, as the program path in this very fixture shows. The bracketed form cannot be produced
    // by escaping, so the path below stays exactly as the operator would type it.
    QCOMPARE(plan.preview,
             QStringLiteral(
                 "\"C:\\Program Files\\app.exe\" \"hello world\" plain \"tab<TAB>here\""));
}

void AiCommandToolPlannerTests::marksRiskyCommandAndPolicyDenial() {
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Remove-Item C:\\temp\\x -Recurse");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QVERIFY(plan.risky_change);
    QVERIFY(!plan.policy_decision.allowed);
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Read-only PC policy blocked mutating command"));
    QVERIFY(plan.policy_decision.risky_change);
}

void AiCommandToolPlannerTests::carriesPidMutationGuardBlock() {
    QJsonObject args;
    args[QStringLiteral("command")] =
        QStringLiteral("$pid=0; [void][Win32]::GetWindowThreadProcessId($hWnd,[ref]$pid)");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QCOMPARE(plan.guard_block_error,
             QStringLiteral("Blocked PowerShell $PID mutation. $PID is a read-only automatic "
                            "variable; use a different variable such as $processId or "
                            "$windowProcessId."));
}

void AiCommandToolPlannerTests::carriesChecksumBypassBlock() {
    QJsonObject args;
    args[QStringLiteral("command")] =
        QStringLiteral("choco install superantispyware -y --ignore-checksums");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("run_powershell"), args, sak::ai::AiToolPolicy::ExclusiveMutatingExecutor);

    QCOMPARE(plan.guard_block_error,
             QStringLiteral("Blocked package checksum bypass. Do not pass --ignore-checksums, "
                            "substitute checksums, or run cached installers after a package "
                            "checksum mismatch."));
    QVERIFY(plan.guard_approval_reason.isEmpty());
    QVERIFY(plan.policy_decision.allowed);
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Known local tool allowed with exclusive mutation policy"));
    QVERIFY(plan.policy_decision.requires_exclusive_lease);
}

void AiCommandToolPlannerTests::rejectsNonCanonicalToolName() {
    // The command tools are a CLOSED set of EXACT names. The router matches names case- and
    // whitespace-insensitively, so a case-variant like "Run_PowerShell" is routed here as a
    // command tool; buildPlan must refuse the non-canonical spelling (default-denied) rather
    // than fall through to the run_process branch and launch a model-supplied executable.
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Get-Date");
    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("Run_PowerShell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);
    // The refusal echoes the sanitized non-canonical spelling that was rejected.
    QCOMPARE(plan.guard_block_error, QStringLiteral("Unsupported command tool: Run_PowerShell"));
    QCOMPARE(plan.request.validation_error,
             QStringLiteral("Unsupported command tool: Run_PowerShell"));
    QVERIFY(!plan.policy_decision.allowed);  // policy_decision stays default-denied
    QVERIFY(plan.risky_change);

    // Non-vacuity: the exact canonical name IS accepted (no guard block), so the refusal is
    // the non-canonical spelling, not a rejection of the command itself.
    const auto ok = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                             args,
                                                             sak::ai::AiToolPolicy::ReadOnlyPc);
    QVERIFY(ok.guard_block_error.isEmpty());
}

void AiCommandToolPlannerTests::shellPreviewIsSanitizedBeforeItIsShownToAHuman() {
    // The approval dialog renders its text in a QPlainTextEdit with NoWrap and a COLLAPSED
    // maximum height. A raw newline therefore does not merely look untidy -- it pushes whatever
    // follows out of the visible box, so the operator approves the first line and the rest runs
    // unseen. U+2028 does the same (QTextDocument treats it as a line separator), and a bidi
    // override reverses the visual order of everything after it.
    //
    // The process branch was always safe because buildProcessPreview quotes each argument through
    // the sanitiser. The two SHELL branches -- the ones a model actually uses -- passed the raw
    // string straight to the dialog.
    struct Case {
        QString label;
        QString injected;
    };
    const QList<Case> cases = {
        {QStringLiteral("newline"), QStringLiteral("\n")},
        {QStringLiteral("carriage return"), QStringLiteral("\r")},
        {QStringLiteral("NEL"), QString(QChar(0x0085))},
        {QStringLiteral("line separator"), QString(QChar(0x2028))},
        {QStringLiteral("paragraph separator"), QString(QChar(0x2029))},
        {QStringLiteral("RLO"), QString(QChar(0x202E))},
        {QStringLiteral("LRI"), QString(QChar(0x2066))},
        {QStringLiteral("zero width space"), QString(QChar(0x200B))},
        {QStringLiteral("BOM"), QString(QChar(0xFEFF))},
    };

    for (const QString& tool : {QStringLiteral("run_powershell"), QStringLiteral("run_cmd")}) {
        for (const Case& c : cases) {
            QJsonObject args;
            args[QStringLiteral("command")] = QStringLiteral("Get-Date") + c.injected +
                                              QStringLiteral("Remove-Item -Recurse C:/");

            const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(
                tool, args, sak::ai::AiToolPolicy::ExclusiveMutatingExecutor);

            const QString context = tool + QStringLiteral(" / ") + c.label;
            // The RAW preview keeps the character: the guard and the risk classifier must judge
            // the text that will actually run, not a display rendering of it.
            QVERIFY2(plan.preview.contains(c.injected), qPrintable(context));
            // The DISPLAYED preview must not.
            QVERIFY2(!plan.display_preview.contains(c.injected), qPrintable(context));
            // And nothing is lost: the dangerous tail is still visible to the reader.
            QVERIFY2(plan.display_preview.contains(QStringLiteral("Remove-Item")),
                     qPrintable(context + QStringLiteral(" -> ") + plan.display_preview));
        }
    }

    // Ordinary Windows commands must survive UNCHANGED. A sanitiser that doubled backslashes to
    // disambiguate its own escapes would mangle every path in the product, which is why the
    // escape form is bracketed instead.
    QJsonObject plain;
    plain[QStringLiteral("command")] =
        QStringLiteral(R"(Copy-Item C:\Windows\System32\drivers\etc\hosts D:\backup)");
    const auto plain_plan = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("run_powershell"), plain, sak::ai::AiToolPolicy::ExclusiveMutatingExecutor);
    QCOMPARE(plain_plan.display_preview, plain_plan.preview);
}

void AiCommandToolPlannerTests::validationFailureIsAlwaysRisky() {
    // buildPlan's contract says any call whose arguments fail the broker's typed validation comes
    // back with risky_change SET, and the unsupported-tool branch honours it. The shell branches
    // did not: a malformed request whose text did not look destructive returned
    // risky_change == false, so a caller following the documented contract would skip the
    // risky-command presentation and the restore-point offer for a request nothing could parse.
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Get-Date");
    // A wrong-typed field the broker rejects; the command text itself is entirely benign.
    args[QStringLiteral("timeout_seconds")] = QStringLiteral("not-a-number");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("run_powershell"), args, sak::ai::AiToolPolicy::ExclusiveMutatingExecutor);

    QVERIFY2(!plan.request.validation_error.isEmpty(),
             "fixture must actually fail typed validation");
    QVERIFY2(!plan.guard_block_error.isEmpty(), qPrintable(plan.guard_block_error));
    QVERIFY2(plan.risky_change, "a call that failed validation must be marked risky");
    QVERIFY(!plan.policy_decision.allowed);
    // The unsupported-tool sibling has always done this; pin it too so the two paths cannot
    // drift apart again.
    const auto unsupported = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("RUN_POWERSHELL"), args, sak::ai::AiToolPolicy::ExclusiveMutatingExecutor);
    QVERIFY(unsupported.risky_change);
}

QTEST_GUILESS_MAIN(AiCommandToolPlannerTests)
#include "test_ai_command_tool_planner.moc"
