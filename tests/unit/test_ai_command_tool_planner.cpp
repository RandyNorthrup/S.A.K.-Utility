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
    void brokerPreconditionsAreRefusedAtPlanTime();
    void planTimePreconditionsDoNotRefuseLegalPlans();
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

namespace {

// A request that PARSES cleanly -- typed validation has nothing to say about it -- and that the
// broker nonetheless refuses at launch.
struct PreconditionCase {
    QString label;
    QString tool;
    QJsonObject args;
    int max_output_bytes;
    QString expected;
};

// A budget well inside the broker's domain, so a case that is not ABOUT the budget cannot be
// refused for one.
constexpr int kOkOutputBudget = 4096;

// The expected strings are the BROKER's own refusal text, character for character (two of them
// are independently pinned in test_ai_execution_broker against a real broker). A plan-time
// refusal that reworded the launch-time one would leave a reader comparing a plan against a log
// unable to tell whether they were looking at one rule or two.
QList<PreconditionCase> preconditionCases() {
    const QString ceiling = QString::number(sak::ai::kAiCommandOutputBytesCeiling);
    return {
        // Whitespace-only is not "a command with odd spacing": the broker trims before testing,
        // so this arrives at the door as nothing at all.
        {QStringLiteral("blank powershell"),
         QStringLiteral("run_powershell"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("   \t  ")}},
         kOkOutputBudget,
         QStringLiteral("PowerShell command is empty")},
        {QStringLiteral("blank cmd"),
         QStringLiteral("run_cmd"),
         QJsonObject{{QStringLiteral("command"), QString{}}},
         kOkOutputBudget,
         QStringLiteral("CMD command is empty")},
        // Elevation is supported ONLY on the PowerShell entry point. The other two are refused
        // outright, and a plan that exists only to be refused is exactly what a human should
        // never be asked to approve.
        {QStringLiteral("elevated cmd"),
         QStringLiteral("run_cmd"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("dir")},
                     {QStringLiteral("requires_admin"), true}},
         kOkOutputBudget,
         QStringLiteral(
             "Elevated cmd.exe launch is not supported; use run_powershell for admin tasks.")},
        {QStringLiteral("elevated process"),
         QStringLiteral("run_process"),
         QJsonObject{{QStringLiteral("program"), QStringLiteral("cmd.exe")},
                     {QStringLiteral("requires_admin"), true}},
         kOkOutputBudget,
         QStringLiteral("Elevated direct-process launch is not supported; use run_powershell for "
                        "admin tasks.")},
        // An output budget outside the broker's domain, supplied by the CALLER through Options
        // rather than by the model, so no amount of argument validation reaches it.
        {QStringLiteral("zero output budget"),
         QStringLiteral("run_powershell"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("Get-Date")}},
         0,
         QStringLiteral("max_output_bytes 0 is outside the range 1-") + ceiling},
        {QStringLiteral("over-ceiling output budget"),
         QStringLiteral("run_powershell"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("Get-Date")}},
         sak::ai::kAiCommandOutputBytesCeiling + 1,
         QStringLiteral("max_output_bytes %1 is outside the range 1-%2")
             .arg(sak::ai::kAiCommandOutputBytesCeiling + 1)
             .arg(ceiling)},
    };
}

}  // namespace

void AiCommandToolPlannerTests::brokerPreconditionsAreRefusedAtPlanTime() {
    // Before this, each of these was rendered into a preview, risk-classified, put in front of a
    // human for approval and granted an execution lease, and only then declined at the broker's
    // door. The approval was spent on a decision that had no effect, which is how an operator
    // learns to click through the dialog.
    for (const PreconditionCase& c : preconditionCases()) {
        const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(
            c.tool,
            c.args,
            sak::ai::AiToolPolicy::ExclusiveMutatingExecutor,
            sak::ai::AiCommandToolPlanner::Options{c.max_output_bytes});

        const QString context = c.label + QStringLiteral(" -> ") + plan.guard_block_error;
        QCOMPARE(plan.request.validation_error, c.expected);
        // The refusal must also reach a caller that reads only guard_block_error, and the
        // documented contract requires risky_change SET and a default-denied policy here.
        QCOMPARE(plan.guard_block_error, c.expected);
        QVERIFY2(plan.risky_change, qPrintable(context));
        QVERIFY2(!plan.policy_decision.allowed, qPrintable(context));
    }
}

void AiCommandToolPlannerTests::planTimePreconditionsDoNotRefuseLegalPlans() {
    // THE OTHER HALF OF THE BRACKET around brokerPreconditionsAreRefusedAtPlanTime, and
    // load-bearing rather than ceremonial. Every fixture in that slot differs from a working
    // plan by exactly ONE field, so dropping the precondition check entirely turns it red while
    // this one stays green; making the check too broad -- refusing elevation on all three
    // targets, or rejecting a budget that is legal -- turns THIS one red while that one stays
    // green. Neither mistake can pass both.
    const auto elevated_powershell = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("run_powershell"),
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Get-Date")},
                    {QStringLiteral("requires_admin"), true}},
        sak::ai::AiToolPolicy::ExclusiveMutatingExecutor,
        sak::ai::AiCommandToolPlanner::Options{kOkOutputBudget});
    QVERIFY2(elevated_powershell.request.validation_error.isEmpty(),
             qPrintable(elevated_powershell.request.validation_error));

    // The ceiling itself is a legal budget -- the domain is inclusive, so an off-by-one in the
    // comparison turns this red.
    const auto at_ceiling = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("run_powershell"),
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Get-Date")}},
        sak::ai::AiToolPolicy::ExclusiveMutatingExecutor,
        sak::ai::AiCommandToolPlanner::Options{sak::ai::kAiCommandOutputBytesCeiling});
    QVERIFY2(at_ceiling.request.validation_error.isEmpty(),
             qPrintable(at_ceiling.request.validation_error));
}

QTEST_GUILESS_MAIN(AiCommandToolPlannerTests)
#include "test_ai_command_tool_planner.moc"
