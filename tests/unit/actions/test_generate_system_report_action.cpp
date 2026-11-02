// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_generate_system_report_action.cpp
/// @brief Unit tests for GenerateSystemReportAction pure seams (Codex B6-15):
///        a collector that fails to run must be surfaced, and a saved-but-empty
///        report must not be reported as a successful comprehensive report.

#include "sak/actions/generate_system_report_action.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <expected>

using sak::GenerateSystemReportAction;

class GenerateSystemReportActionTests : public QObject {
    Q_OBJECT

    using Action = GenerateSystemReportAction;

private Q_SLOTS:
    void collectorFailed_truthTable();
    void volumeSectionHasOnePercentSign();
    void collectorScriptsSurfaceUnavailableQueries();
    void reportGenerationSucceeded_requiresSaveAndCollectors();
    void saveReportCountsUtf8BytesNotUtf16Units();
    void activationSectionReportsTheLicenseStatus();
    void activationScriptRunsAndReportsAStatus();
    void saveReportCarriesTheWriteError();
    void failedSaveLogsItsReason();
};

void GenerateSystemReportActionTests::collectorScriptsSurfaceUnavailableQueries() {
    const QString storage = Action::buildStorageInfoScript();
    QVERIFY2(!storage.contains(QStringLiteral("SilentlyContinue")), qPrintable(storage));
    QVERIFY2(!storage.contains(QStringLiteral("catch {}")), qPrintable(storage));
    QVERIFY(storage.contains(QStringLiteral("Get-StorageReliabilityCounter -ErrorAction Stop")));
    QVERIFY(storage.contains(
        QStringLiteral("Storage reliability counters: unavailable (command not supported)")));
    QVERIFY(storage.contains(QStringLiteral("Storage reliability counter query failed:")));
    QVERIFY(storage.contains(QStringLiteral("Temperature: unavailable")));

    const QString network = Action::buildNetworkInfoScript();
    QVERIFY2(!network.contains(QStringLiteral("SilentlyContinue")), qPrintable(network));
    QVERIFY2(!network.contains(QStringLiteral("catch {}")), qPrintable(network));
    QVERIFY(network.contains(QStringLiteral("Get-NetIPAddress -InterfaceIndex ")));
    QVERIFY(network.contains(QStringLiteral("-ErrorAction Stop")));
    QVERIFY(
        network.contains(QStringLiteral("IP configuration: unavailable (command not supported)")));
    QVERIFY(network.contains(QStringLiteral("IP configuration query failed:")));
    QVERIFY(network.contains(QStringLiteral("IPv4 Address: unavailable")));
}

void GenerateSystemReportActionTests::collectorFailed_truthTable() {
    QVERIFY(!Action::collectorFailed(false, 0));  // clean run
    QVERIFY(Action::collectorFailed(true, 0));    // timed out
    QVERIFY(Action::collectorFailed(false, 1));   // non-zero exit
    // NEGATIVE exit codes: -1 is the never-run default of a ProcessResult, and a crash arrives
    // as an NTSTATUS reinterpreted as a negative int. A guard written `exit_code > 0` calls both
    // a clean collector and reports a system report built from nothing.
    QVERIFY(Action::collectorFailed(false, -1));
    QVERIFY(Action::collectorFailed(false, -1'073'741'819));
    // THIRD ARM, which the two-argument calls above never reach: a collector that exits 0, on
    // time, but hands back blank or ceiling-truncated stdout is still a failure.
    QVERIFY(Action::collectorFailed(false, 0, true));
    QVERIFY(!Action::collectorFailed(false, 0, false));  // explicit false == the 2-arg default
    QVERIFY(Action::collectorFailed(true, 1, true));     // all three arms together
}

void GenerateSystemReportActionTests::reportGenerationSucceeded_requiresSaveAndCollectors() {
    QVERIFY(Action::reportGenerationSucceeded(true, true));  // wrote + all collectors ran
    // Saved, but a collector failed -> must NOT be a success (the B6-15 bug).
    QVERIFY(!Action::reportGenerationSucceeded(true, false));
    // Write failed -> failure regardless.
    QVERIFY(!Action::reportGenerationSucceeded(false, true));
    QVERIFY(!Action::reportGenerationSucceeded(false, false));
}

void GenerateSystemReportActionTests::saveReportCountsUtf8BytesNotUtf16Units() {
    // The report is WRITTEN as UTF-8, but its size was reported with QString::size(), which
    // counts UTF-16 code units. On a localized Windows -- where device names, user names and
    // service descriptions are not ASCII -- the ExecutionResult's bytes_processed and the
    // "Size: N KB" line shown to the technician both disagreed with the file on disk.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("report.txt"));

    // Cyrillic: one QString character each, TWO UTF-8 bytes each, so the two units cannot agree.
    const QString cyrillic = QString::fromUtf8("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    const QString report = QStringLiteral("System Report\n") + cyrillic.repeated(100);

    Action action(temp.path());
    const std::expected<qint64, QString> saved = action.saveReport(report, path);
    QVERIFY2(saved.has_value(), qPrintable(saved ? QString() : saved.error()));
    const qint64 bytes_written = *saved;

    // The reported count is the SIZE OF THE FILE ON DISK -- the number the technician can check
    // in Explorer.
    QCOMPARE(bytes_written, QFileInfo(path).size());
    // It is genuinely larger than the UTF-16 unit count, so this fixture actually distinguishes
    // the two units rather than passing on ASCII where they coincide.
    QVERIFY2(bytes_written > report.size(),
             qPrintable(QStringLiteral("bytes=%1 units=%2").arg(bytes_written).arg(report.size())));
    // And it is at least the encoded payload: the device is opened with QIODevice::Text, so on
    // Windows each '\n' becomes "\r\n" and the file is LARGER than the bytes handed to write().
    // Reporting the payload size would have been a subtler version of the same unit mistake.
    QVERIFY2(
        bytes_written >= static_cast<qint64>(report.toUtf8().size()),
        qPrintable(
            QStringLiteral("file=%1 payload=%2").arg(bytes_written).arg(report.toUtf8().size())));
}

void GenerateSystemReportActionTests::activationSectionReportsTheLicenseStatus() {
    // SYSREP-8. The section titled WINDOWS ACTIVATION printed product name, ID, edition and
    // owner, and no activation state, so an unactivated machine looked fine in the report.
    const QString hardware = Action::buildHardwareInfoScript();
    QVERIFY(hardware.contains(QStringLiteral("=== WINDOWS ACTIVATION ===")));
    const QString activation = Action::buildActivationInfoScript();
    // The Windows entry of SoftwareLicensingProduct that carries a product key -- the record
    // slmgr /dli reports.
    QVERIFY(activation.contains(QStringLiteral("SoftwareLicensingProduct")));
    QVERIFY(activation.contains(QStringLiteral("55c92734-d682-4d71-983e-d6ec3f16059f")));
    QVERIFY(activation.contains(QStringLiteral("PartialProductKey IS NOT NULL")));
    QVERIFY(activation.contains(QStringLiteral("$license.LicenseStatus")));
    // Every documented LicenseStatus value is named.
    for (const char* status : {"Unlicensed",
                               "Licensed",
                               "Out-of-box grace period",
                               "Out-of-tolerance grace period",
                               "Non-genuine grace period",
                               "Notification (not activated)",
                               "Extended grace period"}) {
        QVERIFY2(activation.contains(QLatin1String(status)), status);
    }
    // A failed query is reported in the section, never swallowed and never thrown -- the script
    // runs under $ErrorActionPreference = 'Stop', where an uncaught error drops the whole
    // OS/hardware collector.
    QVERIFY(activation.contains(QStringLiteral("License Status: query failed: ")));
    QVERIFY(!activation.contains(QStringLiteral("SilentlyContinue")));
    QVERIFY(!activation.contains(QStringLiteral("catch {}")));
}

void GenerateSystemReportActionTests::activationScriptRunsAndReportsAStatus() {
    // The licensing query runs inside the OS/hardware collector, so a PowerShell error in it
    // would cost the report its whole OS and hardware section. Run it as the collector does
    // (Windows PowerShell, Stop preference) and require the documented line shape.
    const QString powershell =
        QDir(qEnvironmentVariable("SystemRoot"))
            .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    QProcess process;
    process.start(powershell,
                  {QStringLiteral("-NoProfile"),
                   QStringLiteral("-NonInteractive"),
                   QStringLiteral("-Command"),
                   QStringLiteral("$ErrorActionPreference = 'Stop'\n") +
                       Action::buildActivationInfoScript()});
    constexpr int kLicenseQueryBudgetMs = 60'000;
    QVERIFY(process.waitForFinished(kLicenseQueryBudgetMs));
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
             qPrintable(QString::fromLocal8Bit(process.readAllStandardError())));
    static const QRegularExpression kStatusLine(
        QStringLiteral(R"(^License Status: (Unlicensed|Licensed|Out-of-box grace period|)"
                       R"(Out-of-tolerance grace period|Non-genuine grace period|)"
                       R"(Notification \(not activated\)|Extended grace period|)"
                       R"(Unknown status code \d+|query failed: .+|)"
                       R"(no Windows license with an installed product key)\r?$)"),
        QRegularExpression::MultilineOption);
    QVERIFY2(kStatusLine.match(output).hasMatch(), qPrintable(output));
}

void GenerateSystemReportActionTests::saveReportCarriesTheWriteError() {
    // SYSREP-11. Every save failure returned a bare false, so disk-full, access-denied and a
    // missing folder all reached the technician as "Could not write to: <path>" alone.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("missing-folder/report.txt"));
    QSaveFile probe(path);
    QVERIFY(!probe.open(QIODevice::WriteOnly | QIODevice::Text));
    const QString reason = probe.errorString();
    QVERIFY(!reason.isEmpty());

    Action action(temp.path());
    const auto saved = action.saveReport(QStringLiteral("report"), path);
    QVERIFY(!saved.has_value());
    QVERIFY2(saved.error().contains(reason), qPrintable(saved.error()));
}

void GenerateSystemReportActionTests::failedSaveLogsItsReason() {
    // ... and the reason has to reach the result the technician reads, with the folder that
    // could not be created when that is what happened.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("missing-folder/report.txt"));
    QSaveFile probe(path);
    QVERIFY(!probe.open(QIODevice::WriteOnly | QIODevice::Text));
    const QString reason = probe.errorString();

    Action action(temp.path());
    QString log;
    QString message;
    QObject::connect(&action,
                     &sak::QuickAction::executionComplete,
                     &action,
                     [&log, &message](const sak::QuickAction::ExecutionResult& result) {
                         log = result.log;
                         message = result.message;
                     });
    action.saveReportAndFinish(QStringLiteral("report"),
                               path,
                               QDateTime::currentDateTime(),
                               QStringLiteral("The report folder could not be created: X"));
    QVERIFY2(log.contains(path), qPrintable(log));
    QVERIFY2(log.contains(reason), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("could not be created")), qPrintable(log));
    QVERIFY2(message.contains(reason), qPrintable(message));
    // The message is the one-line summary, not a copy of the log.
    QVERIFY2(message.startsWith(QStringLiteral("Failed to save system inventory report: ")),
             qPrintable(message));
    QVERIFY2(!message.contains(path), qPrintable(message));
}

void GenerateSystemReportActionTests::volumeSectionHasOnePercentSign() {
    GenerateSystemReportAction action(QDir::tempPath());
    const QString section = action.gatherQtAndVolumeInfo();
    QVERIFY2(section.contains(QStringLiteral("  Used: ")), qPrintable(section));
    QVERIFY2(!section.contains(QStringLiteral("%%")), qPrintable(section));
}

QTEST_GUILESS_MAIN(GenerateSystemReportActionTests)
#include "test_generate_system_report_action.moc"
