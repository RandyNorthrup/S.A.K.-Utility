// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_execution_broker.h"

#include <QJsonArray>
#include <QJsonObject>
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
    void startProcess_rejectsEmptyProgram();
    void processRequestFromJson_parsesArguments();
    void toJson_redactsSecretsInStdoutAndStderr();
};

namespace {

[[nodiscard]] bool waitForFinish(QSignalSpy& spy, int timeout_ms = 20'000) {
    return spy.count() > 0 || spy.wait(timeout_ms);
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
    QCOMPARE(result.exit_code, 0);
    QVERIFY(result.stdout_text.contains(QStringLiteral("sak-ai-ok")));
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
    QVERIFY(result.error_message.contains(QStringLiteral("Elevated")));
}

void AiExecutionBrokerTests::runPowerShell_usesElevatedRunnerWhenRequested() {
    sak::ai::ExecutionBroker broker;
    broker.setElevatedRunner([](const sak::ai::AiCommandRequest& request) {
        sak::ai::AiCommandResult result;
        result.started = true;
        result.elevated = true;
        result.exit_code = 0;
        result.stdout_text = request.command;
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
    QVERIFY(result.stdout_text.contains(QStringLiteral("admin")));
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
    // Wait for both: deferred reject + actual cancel.
    while (finished_spy.count() < 2) {
        QVERIFY(finished_spy.wait(10'000));
    }

    bool saw_first_cancelled = false;
    bool saw_second_rejected = false;
    for (const auto& call : finished_spy) {
        const QString id = call.at(0).toString();
        const auto result = call.at(1).value<sak::ai::AiCommandResult>();
        if (id == QStringLiteral("cmd_first")) {
            saw_first_cancelled = result.started && result.cancelled;
        } else if (id == QStringLiteral("cmd_second")) {
            saw_second_rejected = !result.started &&
                                  result.error_message.contains(QStringLiteral("already running"));
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
    QVERIFY(result.error_message.contains(QStringLiteral("Elevated")));
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
    QVERIFY(result.stdout_text.contains(QStringLiteral("proc-marker")));
}

void AiExecutionBrokerTests::startProcess_rejectsEmptyProgram() {
    sak::ai::ExecutionBroker broker;
    QSignalSpy finished_spy(&broker, &sak::ai::ExecutionBroker::finished);

    sak::ai::AiCommandRequest request;
    QVERIFY(!broker.startProcess(request, QStringLiteral("cmd_empty")));
    QVERIFY(waitForFinish(finished_spy));

    const auto result = resultFromSpy(finished_spy);
    QVERIFY(!result.started);
    QVERIFY(result.error_message.contains(QStringLiteral("Program path is empty")));
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

    const QJsonObject json = result.toJson();
    const QString stdout_field = json.value(QStringLiteral("stdout")).toString();
    const QString stderr_field = json.value(QStringLiteral("stderr")).toString();
    QVERIFY(!stdout_field.contains(openai_redaction_sample));
    QVERIFY(!stdout_field.contains(github_redaction_sample));
    QVERIFY(!stderr_field.contains(password_redaction_sample));
    QVERIFY(stdout_field.contains(QStringLiteral("[redacted")) ||
            stdout_field.contains(QStringLiteral("sk-...[redacted]")));
    QVERIFY(stderr_field.contains(QStringLiteral("[redacted]")));
}

QTEST_GUILESS_MAIN(AiExecutionBrokerTests)
#include "test_ai_execution_broker.moc"
