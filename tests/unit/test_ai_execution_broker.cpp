// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_execution_broker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSignalSpy>
#include <QStringList>
#include <QtTest/QtTest>

Q_DECLARE_METATYPE(sak::ai::AiCommandResult)

class AiExecutionBrokerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void runPowerShell_capturesStdoutAndEmitsFinished();
    void runPowerShell_streamsStdoutChunks();
    void runPowerShell_rejectsElevatedWithoutRunner();
    void runPowerShell_usesElevatedRunnerWhenRequested();
    void runPowerShell_cancelsRunningProcess();
    void runPowerShell_rejectsConcurrentStarts();
    void elevatedCancel_isInvokedWhenCancelCalledDuringElevatedRun();
    void startCmd_capturesStdoutFromCmd();
    void startCmd_rejectsElevation();
    void startProcess_launchesProgramDirectly();
    void startProcess_refusesUnresolvableBareName();
    void startProcess_reportsInvalidProgramAsAsyncFailure();
    void startProcess_rejectsEmptyProgram();
    void processRequestFromJson_parsesArguments();
    void processRequestFromJson_refusesMalformedArguments();
    void refusesEmbeddedNulThatTheLaunchWouldTruncate();
    void embeddedNulSurvivesJsonTextSoTheGuardIsModelReachable();
    void toJson_redactsSecretsInStdoutAndStderr();
    void runPowerShell_truncationKeepsTerminalError();
};

namespace {

[[nodiscard]] bool waitForFinish(QSignalSpy& spy, int timeout_ms = 20'000) {
    // qWaitFor re-checks the predicate, so it returns immediately when the broker already
    // emitted finished before the test reached this call. A bare spy.wait() latches the
    // count on entry and would block for a second emission that never comes.
    return QTest::qWaitFor([&spy]() { return spy.count() > 0; }, timeout_ms);
}

[[nodiscard]] sak::ai::AiCommandResult resultFromSpy(const QSignalSpy& spy) {
    if (spy.isEmpty()) {
        return {};
    }
    const QList<QVariant> args = spy.first();
    return args.at(1).value<sak::ai::AiCommandResult>();
}

}  // namespace

void AiExecutionBrokerTests::initTestCase() {
    qRegisterMetaType<sak::ai::AiCommandResult>("sak::ai::AiCommandResult");
}

void AiExecutionBrokerTests::runPowerShell_capturesStdoutAndEmitsFinished() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);
    QSignalSpy started_spy(&broker, &sak::ai::ExecutionBroker::started);

    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Write-Output 'sak-ai-ok'");
    request.timeout_seconds = 10;

    QVERIFY(broker.startPowerShell(request, QStringLiteral("cmd_001")));
    QVERIFY(waitForFinish(finished_spy));
    QCOMPARE(finished_spy.count(), 1);
    QCOMPARE(started_spy.count(), 1);
    QCOMPARE(started_spy.first().at(0).toString(), QStringLiteral("cmd_001"));

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(result.started);
    QVERIFY(!result.cancelled);
    QVERIFY(!result.timed_out);
    // Plain (non-admin) path: onProcessFinished (ai_execution_broker.cpp:737) builds the result
    // and never sets `elevated`, so an ordinary command MUST be reported unelevated. Nothing
    // else in the suite pins this false, so without it a flipped struct default
    // (ai_execution_broker.h:46) would audit every command as elevated with zero red.
    QVERIFY(!result.elevated);
    // Output is far under the cap, so fillCappedOutput (ai_execution_broker.cpp:727) must
    // report the text as the complete run.
    QVERIFY(!result.output_truncated);
    QCOMPARE(result.exit_code, 0);
    QCOMPARE(result.exit_status, 0);  // QProcess::NormalExit
    QVERIFY(result.error_message.isEmpty());
    // Measured at ai_execution_broker.cpp:742, not left at the {0} member default.
    QVERIFY(result.duration_ms > 0);
    // Exact output, not a substring: `Write-Output 'sak-ai-ok'` under -NoProfile emits exactly
    // one line, and cappedHeadTail returns a sub-cap string unchanged, so a leaked prologue or
    // a stray "...[N bytes truncated]..." marker has to show up here.
    QCOMPARE(result.stdout_text.trimmed(), QStringLiteral("sak-ai-ok"));
}

void AiExecutionBrokerTests::runPowerShell_streamsStdoutChunks() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy chunk_spy(&broker, &sak::ai::ExecutionBroker::stdoutChunk);
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Write-Output 'stream-marker'");
    request.timeout_seconds = 10;

    QVERIFY(broker.startPowerShell(request, QStringLiteral("cmd_stream")));
    QVERIFY(waitForFinish(finished_spy));

    // At least one chunk should arrive before the finished signal.
    bool seen_marker = false;
    for (const auto& call : chunk_spy) {
        if (call.at(1).toString().contains(QStringLiteral("stream-marker"))) {
            // Argument 0 is the command id the timeline labels the chunk with; an empty or
            // stale id makes the chunk unattributable, so pin it alongside the payload.
            QCOMPARE(call.at(0).toString(), QStringLiteral("cmd_stream"));
            seen_marker = true;
            break;
        }
    }
    QVERIFY(seen_marker);
}

void AiExecutionBrokerTests::runPowerShell_rejectsElevatedWithoutRunner() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Write-Output 'admin'");
    request.requires_admin = true;

    QVERIFY(!broker.startPowerShell(request, QStringLiteral("cmd_elev_missing")));
    QVERIFY(waitForFinish(finished_spy));
    const auto result = resultFromSpy(finished_spy);
    QVERIFY(!result.started);
    QVERIFY(result.elevated);  // the no-runner elevated branch still marks the result elevated
    QCOMPARE(result.error_message,
             QStringLiteral("Elevated AI command execution is not connected"));
}

void AiExecutionBrokerTests::runPowerShell_usesElevatedRunnerWhenRequested() {
    sak::ai::ExecutionBroker broker;
    broker.setElevatedRunner([](const sak::ai::AiCommandRequest& request) {
        sak::ai::AiCommandResult result;
        result.started = true;
        result.elevated = true;
        result.exit_code = 0;
        result.stdout_text = request.command;
        result.duration_ms = 987'654;  // the elevated worker measured this run itself
        return result;
    });
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);
    QSignalSpy started_spy(&broker, &sak::ai::ExecutionBroker::started);

    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Write-Output 'admin'");
    request.requires_admin = true;

    QVERIFY(broker.startPowerShell(request, QStringLiteral("cmd_elev")));
    QVERIFY(waitForFinish(finished_spy));
    QCOMPARE(started_spy.count(), 1);

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(result.started);
    QVERIFY(result.elevated);
    QCOMPARE(result.exit_code, 0);
    QCOMPARE(result.stdout_text, QStringLiteral("Write-Output 'admin'"));
    // The runner reported its own measurement, so the broker must keep it. Its fallback timer
    // (ai_execution_broker.cpp:403) fills in only when the runner reported 0 -- an unconditional
    // `duration_ms = timer.elapsed()` would report a UAC-gated ten-minute run as the microseconds
    // the synchronous call happened to sit on this stack.
    QCOMPARE(result.duration_ms, static_cast<qint64>(987'654));
}

void AiExecutionBrokerTests::runPowerShell_cancelsRunningProcess() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Start-Sleep -Seconds 30");
    request.timeout_seconds = 60;

    QVERIFY(broker.startPowerShell(request, QStringLiteral("cmd_cancel")));
    QVERIFY(broker.isRunning());
    // Give the process a moment to begin the sleep, then cancel.
    QTest::qWait(300);
    broker.cancel();
    QVERIFY(waitForFinish(finished_spy, 10'000));
    const auto result = resultFromSpy(finished_spy);
    QVERIFY(result.started);
    QVERIFY(result.cancelled);
    QCOMPARE(result.error_message, QStringLiteral("Command cancelled"));
    QVERIFY(!result.timed_out);
    QVERIFY(!broker.isRunning());
}

void AiExecutionBrokerTests::runPowerShell_rejectsConcurrentStarts() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest first;
    first.command = QStringLiteral("Start-Sleep -Seconds 10");
    first.timeout_seconds = 30;
    QVERIFY(broker.startPowerShell(first, QStringLiteral("cmd_first")));
    QVERIFY(broker.isRunning());

    sak::ai::AiCommandRequest second;
    second.command = QStringLiteral("Write-Output 'second'");
    second.timeout_seconds = 5;
    QVERIFY(!broker.startPowerShell(second, QStringLiteral("cmd_second")));

    broker.cancel();
    // Wait for both: deferred reject + actual cancel. The old loop called wait(10'000) once
    // per emission, so its real budget was up to 20s for the two signals; a single
    // 10'000 here would have halved that for no reason, so the timeout is doubled to match
    // what this test was actually allowed before.
    QTRY_COMPARE_WITH_TIMEOUT(finished_spy.count(), 2, 20'000);

    bool saw_first_cancelled = false;
    bool saw_second_rejected = false;
    for (const auto& call : finished_spy) {
        const QString id = call.at(0).toString();
        const auto result = call.at(1).value<sak::ai::AiCommandResult>();
        if (id == QStringLiteral("cmd_first")) {
            saw_first_cancelled = result.started && result.cancelled &&
                                  result.error_message == QStringLiteral("Command cancelled");
        } else if (id == QStringLiteral("cmd_second")) {
            saw_second_rejected = !result.started &&
                                  result.error_message ==
                                      QStringLiteral("Broker is already running a command");
        }
    }

    QVERIFY(saw_first_cancelled);
    QVERIFY(saw_second_rejected);
    QVERIFY(!broker.isRunning());
}

void AiExecutionBrokerTests::elevatedCancel_isInvokedWhenCancelCalledDuringElevatedRun() {
    sak::ai::ExecutionBroker broker;
    int cancel_invocations = 0;
    broker.setElevatedCancel([&cancel_invocations]() { ++cancel_invocations; });
    // The runner triggers cancel() mid-run; the broker should route the
    // cancel through the elevated-cancel hook and return the runner's
    // result on the same call.
    broker.setElevatedRunner([&broker](const sak::ai::AiCommandRequest&) {
        broker.cancel();
        sak::ai::AiCommandResult result;
        result.started = true;
        result.elevated = true;
        result.cancelled = true;
        result.exit_code = -1;
        result.error_message = QStringLiteral("Cancelled by user");
        return result;
    });

    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Write-Output 'admin'");
    request.requires_admin = true;

    QVERIFY(broker.startPowerShell(request, QStringLiteral("cmd_elev_cancel")));
    QVERIFY(waitForFinish(finished_spy));
    QCOMPARE(cancel_invocations, 1);

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(result.elevated);
    QVERIFY(result.cancelled);
    QCOMPARE(result.error_message, QStringLiteral("Cancelled by user"));
}

void AiExecutionBrokerTests::startCmd_capturesStdoutFromCmd() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("echo cmd-marker");
    request.timeout_seconds = 10;

    QVERIFY(broker.startCmd(request, QStringLiteral("cmd_cmd")));
    QVERIFY(waitForFinish(finished_spy));

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(result.started);
    QCOMPARE(result.exit_code, 0);
    QVERIFY(result.stdout_text.contains(QStringLiteral("cmd-marker")));
}

void AiExecutionBrokerTests::startCmd_rejectsElevation() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("whoami");
    request.requires_admin = true;

    QVERIFY(!broker.startCmd(request, QStringLiteral("cmd_cmd_admin")));
    QVERIFY(waitForFinish(finished_spy));

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(!result.started);
    QCOMPARE(result.error_message,
             QStringLiteral("Elevated cmd.exe launch is not supported; use run_powershell "
                            "for admin tasks."));
}

void AiExecutionBrokerTests::startProcess_launchesProgramDirectly() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    request.program = QStringLiteral("cmd.exe");
    request.arguments = QStringList{QStringLiteral("/c"), QStringLiteral("echo proc-marker")};
    request.timeout_seconds = 10;

    QVERIFY(broker.startProcess(request, QStringLiteral("cmd_proc")));
    QVERIFY(waitForFinish(finished_spy));

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(result.started);
    QCOMPARE(result.exit_code, 0);
    // `cmd /c echo proc-marker` is deterministic, so pin the exact line rather than a
    // substring: a leaked prologue or elision marker has to show up here.
    QCOMPARE(result.stdout_text.trimmed(), QStringLiteral("proc-marker"));
}

void AiExecutionBrokerTests::startProcess_refusesUnresolvableBareName() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);
    QSignalSpy started_spy(&broker, &sak::ai::ExecutionBroker::started);

    // A BARE name (no separator) is the input the System32/PATH resolver exists to harden:
    // CreateProcess would search the current directory ahead of PATH and let a planted
    // .\<name> win. A name that resolves under neither System32 nor an absolute PATH entry
    // must be REFUSED synchronously, not handed to CreateProcess to find on its own.
    // startProcess_launchesProgramDirectly cannot see this: cmd.exe resolves either way, and
    // AiCommandResult carries no program field, so only the refusal side is observable.
    sak::ai::AiCommandRequest request;
    request.program = QStringLiteral("definitely-not-a-real-sak-test-binary.exe");
    request.timeout_seconds = 10;

    // Refusal is synchronous (false), unlike the async "Process start error" an absolute
    // bad path produces -- delete the resolver and this flips to true.
    QVERIFY(!broker.startProcess(request, QStringLiteral("cmd_bare_unresolvable")));
    QVERIFY(waitForFinish(finished_spy));
    QCOMPARE(started_spy.count(), 0);

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(!result.started);
    QCOMPARE(result.error_message,
             QStringLiteral("Cannot resolve program 'definitely-not-a-real-sak-test-binary.exe' "
                            "to an absolute path; refusing to launch a bare name"));
}

void AiExecutionBrokerTests::startProcess_reportsInvalidProgramAsAsyncFailure() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);
    QSignalSpy started_spy(&broker, &sak::ai::ExecutionBroker::started);

    sak::ai::AiCommandRequest request;
    request.program = QStringLiteral("C:/definitely-not-a-real-sak-test-binary.exe");
    request.timeout_seconds = 10;

    QVERIFY(broker.startProcess(request, QStringLiteral("cmd_bad_proc")));
    QVERIFY(waitForFinish(finished_spy));
    QCOMPARE(started_spy.count(), 0);

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(!result.started);
    QVERIFY(result.error_message.contains(QStringLiteral("Process start error")));
}

void AiExecutionBrokerTests::startProcess_rejectsEmptyProgram() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    QVERIFY(!broker.startProcess(request, QStringLiteral("cmd_empty")));
    QVERIFY(waitForFinish(finished_spy));

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(!result.started);
    QCOMPARE(result.error_message, QStringLiteral("Program path is empty"));

    // A whitespace-only program is the malformed input the .trimmed() half of the guard at
    // ai_execution_broker.cpp:538 exists for (parseRequiredString does not trim, so a tool
    // call of {"program": "   "} arrives here verbatim). It must be refused with this same
    // contract message, not fall through to the bare-name resolver, which would report
    // "Cannot resolve program '' to an absolute path; refusing to launch a bare name".
    sak::ai::ExecutionBroker blank_broker;
    QSignalSpy blank_finished_spy(&blank_broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest blank_request;
    blank_request.program = QStringLiteral("   ");
    QVERIFY(!blank_broker.startProcess(blank_request, QStringLiteral("cmd_blank")));
    QVERIFY(waitForFinish(blank_finished_spy));

    const auto blank_result = resultFromSpy(blank_finished_spy);
    QVERIFY(!blank_result.started);
    QCOMPARE(blank_result.error_message, QStringLiteral("Program path is empty"));
}

void AiExecutionBrokerTests::processRequestFromJson_parsesArguments() {
    const QJsonObject input = QJsonObject{
        {QStringLiteral("program"), QStringLiteral("C:/Windows/System32/cmd.exe")},
        {QStringLiteral("arguments"), QJsonArray{QStringLiteral("/c"), QStringLiteral("echo hi")}},
        {QStringLiteral("timeout_seconds"), 30},
        {QStringLiteral("requires_admin"), false},
    };
    const auto request = sak::ai::ExecutionBroker::processRequestFromJson(input);
    QCOMPARE(request.program, QStringLiteral("C:/Windows/System32/cmd.exe"));
    QCOMPARE(request.arguments.size(), 2);
    QCOMPARE(request.arguments.at(0), QStringLiteral("/c"));
    QCOMPARE(request.arguments.at(1), QStringLiteral("echo hi"));
    QCOMPARE(request.timeout_seconds, 30);
    QVERIFY(!request.requires_admin);
    QVERIFY(request.validation_error.isEmpty());  // accept path produces NO error

    // requires_admin is the ELEVATION CLASSIFICATION: a present-but-non-bool value must be
    // REJECTED, never coerced to false by QJsonValue::toBool()'s default. Without this,
    // dropping the isBool() guard in parseOptionalRequiresAdmin turns a plan reviewed as
    // elevated into an unelevated launch carrying an EMPTY validation_error.
    QJsonObject mistyped = input;
    mistyped[QStringLiteral("requires_admin")] = QStringLiteral("true");
    const auto rejected = sak::ai::ExecutionBroker::processRequestFromJson(mistyped);
    QCOMPARE(rejected.validation_error, QStringLiteral("requires_admin must be a boolean"));
    QVERIFY(!rejected.requires_admin);  // stays at the fail-closed default, never coerced

    // The accepted side must carry true through as well, not only false.
    QJsonObject elevated = input;
    elevated[QStringLiteral("requires_admin")] = true;
    const auto elevated_request = sak::ai::ExecutionBroker::processRequestFromJson(elevated);
    QVERIFY(elevated_request.validation_error.isEmpty());
    QVERIFY(elevated_request.requires_admin);

    // parseOptionalTimeoutSeconds fails closed on both an out-of-domain value and a wrong
    // type (src/ai/ai_execution_broker.cpp:103-123), so probe each arm by its OWN message.
    // A below-floor value must be REFUSED here, never accepted and silently substituted with
    // a default by launchProcess's std::clamp at ai_execution_broker.cpp:663; and a wrong-typed
    // value must speak the type message, not the range message that toDouble()'s 0 would give.
    QJsonObject below_floor = input;
    below_floor[QStringLiteral("timeout_seconds")] = 1;  // 1, not 0: 0 is caught by
                                                         // launchLimitsError instead
    const auto below = sak::ai::ExecutionBroker::processRequestFromJson(below_floor);
    QCOMPARE(below.validation_error, QStringLiteral("timeout_seconds must be between 5 and 3600"));

    QJsonObject above_ceiling = input;
    above_ceiling[QStringLiteral("timeout_seconds")] = 999'999;
    const auto above = sak::ai::ExecutionBroker::processRequestFromJson(above_ceiling);
    QCOMPARE(above.validation_error, QStringLiteral("timeout_seconds must be between 5 and 3600"));

    QJsonObject wrong_type = input;
    wrong_type[QStringLiteral("timeout_seconds")] = QStringLiteral("30");
    const auto typed = sak::ai::ExecutionBroker::processRequestFromJson(wrong_type);
    QCOMPARE(typed.validation_error, QStringLiteral("timeout_seconds must be a number"));
}

void AiExecutionBrokerTests::refusesEmbeddedNulThatTheLaunchWouldTruncate() {
    // QProcess hands the program path and the assembled command line to CreateProcessW as C
    // strings, which end at the first NUL. A value carrying one is therefore TRUNCATED at
    // launch, while the approval preview -- built from the whole QString -- renders the NUL as
    // a visible escape and shows the reader everything after it.
    //
    // The mismatch runs in the dangerous direction, which is why this is refused rather than
    // merely escaped for display: the fixture below reads as a dry run and would delete for
    // real, because the trailing flag the reviewer's eye lands on is exactly the part that
    // never reaches the process.
    const QString nul = QString(QChar(QChar::Null));
    const QString deceptive = QStringLiteral("Remove-Item C:/temp -Recurse") + nul +
                              QStringLiteral(" -WhatIf");

    // NON-VACUITY, and it is the assertion this whole slot rests on: if the fixture did not
    // actually carry a NUL, every rejection below would be testing nothing. QString is
    // length-counted, so it holds one -- but assert it rather than assume it.
    QCOMPARE(deceptive.size(), 29 + 1 + 7);
    QVERIFY(deceptive.contains(QChar::Null));

    const auto shell = sak::ai::ExecutionBroker::requestFromJson(
        QJsonObject{{QStringLiteral("command"), deceptive}});
    QCOMPARE(shell.validation_error,
             QStringLiteral("command must not contain an embedded NUL character"));
    // The value is CLEARED as well as rejected, matching the length-limit arm beside it: a
    // caller that ignored validation_error must not find a usable truncated command waiting.
    QVERIFY(shell.command.isEmpty());

    const auto program = sak::ai::ExecutionBroker::processRequestFromJson(QJsonObject{
        {QStringLiteral("program"),
         QStringLiteral("C:/Windows/System32/cmd.exe") + nul + QStringLiteral("evil.exe")}});
    QCOMPARE(program.validation_error,
             QStringLiteral("program must not contain an embedded NUL character"));
    QVERIFY(program.program.isEmpty());

    // argv is where the truncation bites hardest: the approver reads one argument and the
    // process receives a shorter prefix of it.
    const auto argument = sak::ai::ExecutionBroker::processRequestFromJson(QJsonObject{
        {QStringLiteral("program"), QStringLiteral("C:/Windows/System32/cmd.exe")},
        {QStringLiteral("arguments"),
         QJsonArray{QStringLiteral("/c"), QStringLiteral("dir") + nul + QStringLiteral(" /s")}}});
    QCOMPARE(argument.validation_error,
             QStringLiteral("an argument must not contain an embedded NUL character"));

    // THE OTHER HALF OF THE BRACKET: the same fixtures WITHOUT the NUL are accepted, so a
    // guard that refused every command, or one that keyed on "Remove-Item" or on length,
    // turns this red while the rejections above stay green.
    const auto clean_shell = sak::ai::ExecutionBroker::requestFromJson(
        QJsonObject{{QStringLiteral("command"),
                     QStringLiteral("Remove-Item C:/temp -Recurse") + QStringLiteral(" -WhatIf")}});
    QVERIFY2(clean_shell.validation_error.isEmpty(), qPrintable(clean_shell.validation_error));
    const auto clean_process = sak::ai::ExecutionBroker::processRequestFromJson(QJsonObject{
        {QStringLiteral("program"), QStringLiteral("C:/Windows/System32/cmd.exe")},
        {QStringLiteral("arguments"),
         QJsonArray{QStringLiteral("/c"), QStringLiteral("dir") + QStringLiteral(" /s")}}});
    QVERIFY2(clean_process.validation_error.isEmpty(), qPrintable(clean_process.validation_error));
}

void AiExecutionBrokerTests::embeddedNulSurvivesJsonTextSoTheGuardIsModelReachable() {
    // THE REAL DELIVERY PATH, pinned rather than assumed. A model's tool call arrives as JSON
    // TEXT, so whether a \u0000 escape survives QJsonDocument parsing is what decides whether a
    // model can reach the guard at all -- i.e. whether the sibling slot above defends a live
    // path or a merely theoretical one. Recording the answer in a test rather than in someone's
    // memory means a future Qt that changes it says so out loud instead of quietly turning that
    // slot into a test of an unreachable branch.
    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(R"({"command":"Remove-Item C:/temp -Recurse\u0000 -WhatIf"})"), &parse_error);
    QCOMPARE(parse_error.error, QJsonParseError::NoError);
    const QString from_json = doc.object().value(QStringLiteral("command")).toString();
    QVERIFY2(from_json.contains(QChar::Null),
             "JSON text parsing must preserve \\u0000 for the NUL guard to be model-reachable");
    const auto via_json = sak::ai::ExecutionBroker::requestFromJson(doc.object());
    QCOMPARE(via_json.validation_error,
             QStringLiteral("command must not contain an embedded NUL character"));
}

void AiExecutionBrokerTests::processRequestFromJson_refusesMalformedArguments() {
    // parseOptionalArguments has four rejection arms and the accept-path fixture reaches
    // none of them (src/ai/ai_execution_broker.cpp:162-188). A wrong shape must be REFUSED,
    // never silently repaired into a DIFFERENT command line than the one that was reviewed.
    const QJsonObject program_only{
        {QStringLiteral("program"), QStringLiteral("C:/Windows/System32/cmd.exe")}};
    const auto parse_with = [&program_only](const QJsonValue& value) {
        QJsonObject obj = program_only;
        obj.insert(QStringLiteral("arguments"), value);
        return sak::ai::ExecutionBroker::processRequestFromJson(obj);
    };

    // Absent / null is accepted and contributes no arguments.
    const auto absent = sak::ai::ExecutionBroker::processRequestFromJson(program_only);
    QVERIFY(absent.validation_error.isEmpty());
    QVERIFY(absent.arguments.isEmpty());
    const auto null_args = parse_with(QJsonValue(QJsonValue::Null));
    QVERIFY(null_args.validation_error.isEmpty());
    QVERIFY(null_args.arguments.isEmpty());

    // A non-array must be refused, not degraded into an empty argument list. The
    // validation_error compare is the killer here: toArray() on a string already yields an
    // empty array, so the isEmpty() line alone would not catch a deleted guard.
    const auto not_array = parse_with(QStringLiteral("/c echo hi"));
    QCOMPARE(not_array.validation_error, QStringLiteral("arguments must be an array of strings"));
    QVERIFY(not_array.arguments.isEmpty());

    // A non-string entry must be refused, not turned into a blank argument. Parsing stops at
    // the offending entry and keeps what it had already appended, so {"/c"} is the contract.
    const auto bad_entry = parse_with(QJsonArray{QStringLiteral("/c"), QJsonValue(7)});
    QCOMPARE(bad_entry.validation_error,
             QStringLiteral("every entry of arguments must be a string"));
    QCOMPARE(bad_entry.arguments, QStringList{QStringLiteral("/c")});  // no blank appended

    // 256 entries accepted, 257 refused.
    QJsonArray at_cap;
    for (int i = 0; i < 256; ++i) {
        at_cap.append(QStringLiteral("a"));
    }
    const auto at_count_limit = parse_with(at_cap);
    QVERIFY(at_count_limit.validation_error.isEmpty());
    QCOMPARE(at_count_limit.arguments.size(), 256);
    QJsonArray over_cap = at_cap;
    over_cap.append(QStringLiteral("a"));
    QCOMPARE(parse_with(over_cap).validation_error,
             QStringLiteral("arguments exceeds the 256 entry limit"));

    // A 30000-char argument accepted, 30001 refused.
    const auto at_char_limit = parse_with(QJsonArray{QString(30'000, QLatin1Char('a'))});
    QVERIFY(at_char_limit.validation_error.isEmpty());
    QCOMPARE(parse_with(QJsonArray{QString(30'001, QLatin1Char('a'))}).validation_error,
             QStringLiteral("an argument exceeds the 30000 character limit"));
}

void AiExecutionBrokerTests::toJson_redactsSecretsInStdoutAndStderr() {
    sak::ai::AiCommandResult result;
    result.started = true;
    result.exit_code = 0;
    const QString openai_redaction_sample = QStringLiteral("sk-") +
                                            QStringLiteral("abcdefghijklmnopqrstuvwxyz");
    const QString github_redaction_sample = QStringLiteral("ghp_") +
                                            QStringLiteral("abcdefghijklmnopqrstuvwxyz012345");
    result.stdout_text =
        QStringLiteral("token=%1 %2").arg(openai_redaction_sample, github_redaction_sample);
    const QString password_redaction_sample = QStringLiteral("hunter") + QStringLiteral("2") +
                                              QStringLiteral("hunter");
    result.stderr_text = QStringLiteral("password=%1 on stderr").arg(password_redaction_sample);
    // toJson() routes THREE strings through redactSecrets (ai_execution_broker.cpp:202-204).
    // error_message is the third and is not innocuous: it carries QProcess start errors
    // (:756-757), the model-supplied program path (:558-561) and whatever text an elevated
    // runner returns, straight into the AI transcript. Without pinning it, deleting the
    // redactSecrets call on :204 leaves every assertion in this file green.
    result.error_message = QStringLiteral("Process start error: %1").arg(github_redaction_sample);

    const QJsonObject json = result.toJson();
    const QString stdout_field = json.value(QStringLiteral("stdout")).toString();
    const QString stderr_field = json.value(QStringLiteral("stderr")).toString();
    QVERIFY(!stdout_field.contains(openai_redaction_sample));
    QVERIFY(!stdout_field.contains(github_redaction_sample));
    QVERIFY(!stderr_field.contains(password_redaction_sample));
    // Exact post-redaction output: the sk- token is collapsed by the assignment-secret rule
    // (token=[redacted]) and the ghp_ token by the github rule; the earlier "sk-...[redacted]"
    // disjunct was dead. Pins the whole redacted format, not just presence of a marker.
    QCOMPARE(stdout_field, QStringLiteral("token=[redacted] [redacted-github-token]"));
    QCOMPARE(stderr_field, QStringLiteral("password=[redacted] on stderr"));

    const QString error_field = json.value(QStringLiteral("error_message")).toString();
    QVERIFY(!error_field.contains(github_redaction_sample));
    QCOMPARE(error_field, QStringLiteral("Process start error: [redacted-github-token]"));
}

void AiExecutionBrokerTests::runPowerShell_truncationKeepsTerminalError() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    // Emit a large head of filler followed by a terminal marker line, with a cap small
    // enough to force truncation but larger than the streaming buffer keeps, so the
    // final-output-carrying tail must survive. Before the fix, only the head was kept.
    sak::ai::AiCommandRequest request;
    request.command = QStringLiteral("Write-Output ('A' * 20000); Write-Output 'TERMINAL-ERR-ZZZ'");
    request.timeout_seconds = 20;
    request.max_output_bytes = 8192;

    QVERIFY(broker.startPowerShell(request, QStringLiteral("cmd_trunc")));
    QVERIFY(waitForFinish(finished_spy));

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(result.started);
    QVERIFY(result.output_truncated);  // the dedicated flag, not just the appended marker text
    QVERIFY(result.stdout_text.contains(QStringLiteral("truncated")));
    QVERIFY(result.stdout_text.contains(QStringLiteral("TERMINAL-ERR-ZZZ")));

    // Same output at the MINIMUM cap: appendCapped's rolling window drops ~19 KB, but leaves the
    // buffer at exactly 1024 bytes, so half_cap == max(512, kMinOutputCap) == 1024 and the final
    // cappedHeadTail never fires. output_truncated can then only be true through the
    // m_output_dropped_bytes term (ai_execution_broker.cpp:727) -- the arm the run above
    // short-circuits past. Without that term a 1 KB caller gets the last kilobyte of a 20 KB run
    // presented as the COMPLETE output.
    sak::ai::ExecutionBroker min_cap_broker;
    QSignalSpy min_cap_spy(&min_cap_broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest min_cap_request;
    min_cap_request.command = request.command;
    min_cap_request.timeout_seconds = 20;
    min_cap_request.max_output_bytes = 1024;

    QVERIFY(min_cap_broker.startPowerShell(min_cap_request, QStringLiteral("cmd_trunc_min")));
    QVERIFY(waitForFinish(min_cap_spy));

    const auto min_cap_result = resultFromSpy(min_cap_spy);
    QVERIFY(min_cap_result.started);
    // Neither stream carries the elision marker, so both cappedHeadTail truncation flags were
    // false: the dropped-bytes term is the sole reason the run is reported as partial.
    QVERIFY(!min_cap_result.stdout_text.contains(QStringLiteral("bytes truncated")));
    QVERIFY(!min_cap_result.stderr_text.contains(QStringLiteral("bytes truncated")));
    QVERIFY(min_cap_result.stdout_text.toUtf8().size() <= 1024);
    QVERIFY(min_cap_result.output_truncated);
}

QTEST_GUILESS_MAIN(AiExecutionBrokerTests)
#include "test_ai_execution_broker.moc"
