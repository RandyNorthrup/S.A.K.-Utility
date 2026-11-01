// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_microsoft_defender_action.cpp
/// @brief Microsoft Defender quick scan / full scan / signature update: MpCmdRun result mapping,
///        the platform-folder resolution rule, and the elevation route they share with the other
///        administrator quick actions.

#include "sak/action_constants.h"
#include "sak/actions/microsoft_defender_action.h"
#include "sak/process_runner.h"
#include "sak/quick_action_controller.h"

#include <QtTest/QtTest>

using sak::DefenderOperation;
using sak::DefenderOutcome;
using sak::MicrosoftDefenderAction;
using sak::ProcessResult;

namespace {

ProcessResult finished(int exit_code, QString std_out = {}, QString std_err = {}) {
    ProcessResult result;
    result.exit_code = exit_code;
    result.exit_status = 0;
    result.std_out = std::move(std_out);
    result.std_err = std::move(std_err);
    return result;
}

const QString kPlatformRoot = QStringLiteral("C:/ProgramData/Microsoft/Windows Defender/Platform");
const QString kProgramRoot = QStringLiteral("C:/Program Files/Windows Defender");

}  // namespace

class MicrosoftDefenderActionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void scanExitZeroIsClean();
    void scanExitTwoNeedsAttentionNeverClean();
    void otherScanExitCodesAreFailures();
    void signatureUpdateMapsZeroAndNonZero();
    void timeoutCancelAndCrashAreTheirOwnOutcomes();
    void argumentsSelectTheDocumentedScanTypes();
    void eachOperationHasItsOwnBoundedTimeout();
    void platformVersionFolderResolvesMpCmdRun();
    void inboxProgramFolderResolvesMpCmdRun();
    void foldersOutsideDefenderAreRefused();
    void runsThroughTheSharedElevatedHelperRoute();
};

void MicrosoftDefenderActionTests::scanExitZeroIsClean() {
    for (const auto operation : {DefenderOperation::QuickScan, DefenderOperation::FullScan}) {
        const auto verdict = MicrosoftDefenderAction::verdictFor(operation, finished(0));
        QCOMPARE(verdict.outcome, DefenderOutcome::Clean);
        QVERIFY(verdict.success);
        QVERIFY2(verdict.message.contains(QStringLiteral("no threats found")),
                 qPrintable(verdict.message));
    }
}

void MicrosoftDefenderActionTests::scanExitTwoNeedsAttentionNeverClean() {
    // MpCmdRun's documented exit 2: malware not remediated, user action needed, or scan errors.
    // It must be reported as exactly that -- not as clean, and not as a generic failure.
    for (const auto operation : {DefenderOperation::QuickScan, DefenderOperation::FullScan}) {
        const auto verdict = MicrosoftDefenderAction::verdictFor(operation, finished(2));
        QCOMPARE(verdict.outcome, DefenderOutcome::ThreatsNeedAttention);
        QVERIFY(!verdict.success);
        QVERIFY2(verdict.message.contains(QStringLiteral("exit code 2")),
                 qPrintable(verdict.message));
        QVERIFY(verdict.message.contains(QStringLiteral("Protection history")));
    }
}

void MicrosoftDefenderActionTests::otherScanExitCodesAreFailures() {
    // 0x800106BA: the Defender service is disabled. Any code other than 0 and 2 is a failure,
    // reported with its value (decimal and hex, so an HRESULT reads as Microsoft prints it).
    const auto verdict = MicrosoftDefenderAction::verdictFor(
        DefenderOperation::QuickScan,
        finished(static_cast<int>(0x80'01'06'BAu),
                 QStringLiteral("CmdTool: Failed with hr = ...")));
    QCOMPARE(verdict.outcome, DefenderOutcome::Failed);
    QVERIFY(!verdict.success);
    QVERIFY2(verdict.message.contains(QStringLiteral("0x800106ba")), qPrintable(verdict.message));
    QVERIFY(verdict.message.contains(QStringLiteral("CmdTool: Failed with hr")));

    const auto one = MicrosoftDefenderAction::verdictFor(DefenderOperation::FullScan, finished(1));
    QCOMPARE(one.outcome, DefenderOutcome::Failed);
    QVERIFY(!one.success);
}

void MicrosoftDefenderActionTests::signatureUpdateMapsZeroAndNonZero() {
    const auto updated = MicrosoftDefenderAction::verdictFor(DefenderOperation::SignatureUpdate,
                                                             finished(0));
    QCOMPARE(updated.outcome, DefenderOutcome::SignaturesUpdated);
    QVERIFY(updated.success);
    QVERIFY(updated.message.contains(QStringLiteral("up to date")));

    // Exit 2 means "threats" only for a scan; for an update it is an ordinary failure.
    const auto failed = MicrosoftDefenderAction::verdictFor(DefenderOperation::SignatureUpdate,
                                                            finished(2));
    QCOMPARE(failed.outcome, DefenderOutcome::Failed);
    QVERIFY(!failed.success);
}

void MicrosoftDefenderActionTests::timeoutCancelAndCrashAreTheirOwnOutcomes() {
    ProcessResult timed_out = finished(0);
    timed_out.timed_out = true;
    const auto timeout_verdict = MicrosoftDefenderAction::verdictFor(DefenderOperation::FullScan,
                                                                     timed_out);
    QCOMPARE(timeout_verdict.outcome, DefenderOutcome::TimedOut);
    QVERIFY(!timeout_verdict.success);
    QVERIFY2(timeout_verdict.message.contains(QStringLiteral("240 minutes")),
             qPrintable(timeout_verdict.message));

    ProcessResult cancelled = finished(0);
    cancelled.cancelled = true;
    const auto cancel_verdict = MicrosoftDefenderAction::verdictFor(DefenderOperation::QuickScan,
                                                                    cancelled);
    QCOMPARE(cancel_verdict.outcome, DefenderOutcome::Cancelled);
    QVERIFY(!cancel_verdict.success);

    // A crash that still reports exit code 0 is not a clean scan.
    ProcessResult crashed = finished(0);
    crashed.exit_status = 1;
    const auto crash_verdict = MicrosoftDefenderAction::verdictFor(DefenderOperation::QuickScan,
                                                                   crashed);
    QCOMPARE(crash_verdict.outcome, DefenderOutcome::Failed);
    QVERIFY(!crash_verdict.success);
}

void MicrosoftDefenderActionTests::argumentsSelectTheDocumentedScanTypes() {
    QCOMPARE(
        MicrosoftDefenderAction::mpCmdRunArguments(DefenderOperation::QuickScan),
        (QStringList{QStringLiteral("-Scan"), QStringLiteral("-ScanType"), QStringLiteral("1")}));
    QCOMPARE(
        MicrosoftDefenderAction::mpCmdRunArguments(DefenderOperation::FullScan),
        (QStringList{QStringLiteral("-Scan"), QStringLiteral("-ScanType"), QStringLiteral("2")}));
    QCOMPARE(MicrosoftDefenderAction::mpCmdRunArguments(DefenderOperation::SignatureUpdate),
             QStringList{QStringLiteral("-SignatureUpdate")});
}

void MicrosoftDefenderActionTests::eachOperationHasItsOwnBoundedTimeout() {
    QCOMPARE(MicrosoftDefenderAction::timeoutMs(DefenderOperation::QuickScan), 2 * 60 * 60 * 1000);
    QCOMPARE(MicrosoftDefenderAction::timeoutMs(DefenderOperation::FullScan), 4 * 60 * 60 * 1000);
    QCOMPARE(MicrosoftDefenderAction::timeoutMs(DefenderOperation::SignatureUpdate),
             30 * 60 * 1000);
    // The assistant's registry thunk waits on executionTimeoutMs(), so it must be the same bound.
    for (const auto operation : {DefenderOperation::QuickScan,
                                 DefenderOperation::FullScan,
                                 DefenderOperation::SignatureUpdate}) {
        const MicrosoftDefenderAction action(operation);
        QCOMPARE(action.executionTimeoutMs(), MicrosoftDefenderAction::timeoutMs(operation));
        QVERIFY(action.requiresAdmin());
        QVERIFY(!action.isReadOnly());
    }
}

void MicrosoftDefenderActionTests::platformVersionFolderResolvesMpCmdRun() {
    // The shape Defender registers: a version folder under Platform, with a trailing separator.
    const auto path = MicrosoftDefenderAction::mpCmdRunPathFromInstallLocation(
        QStringLiteral("C:\\ProgramData\\Microsoft\\Windows Defender\\Platform\\4.18.25070.5-0\\"),
        kPlatformRoot,
        kProgramRoot);
    QVERIFY2(path.has_value(), qPrintable(path.error()));
    QCOMPARE(*path,
             QStringLiteral("C:\\ProgramData\\Microsoft\\Windows Defender\\Platform\\"
                            "4.18.25070.5-0\\MpCmdRun.exe"));
    // Case differences in the registry value do not matter on Windows.
    QVERIFY(MicrosoftDefenderAction::mpCmdRunPathFromInstallLocation(
                QStringLiteral("c:\\programdata\\microsoft\\windows defender\\platform\\4.18.1-0"),
                kPlatformRoot,
                kProgramRoot)
                .has_value());
}

void MicrosoftDefenderActionTests::inboxProgramFolderResolvesMpCmdRun() {
    const auto path = MicrosoftDefenderAction::mpCmdRunPathFromInstallLocation(
        QStringLiteral("C:\\Program Files\\Windows Defender\\"), kPlatformRoot, kProgramRoot);
    QVERIFY2(path.has_value(), qPrintable(path.error()));
    QCOMPARE(*path, QStringLiteral("C:\\Program Files\\Windows Defender\\MpCmdRun.exe"));
}

void MicrosoftDefenderActionTests::foldersOutsideDefenderAreRefused() {
    const QStringList refused{
        QString(),
        QStringLiteral("   "),
        QStringLiteral("Platform\\4.18.1-0"),                                      // relative
        QStringLiteral("\\\\server\\share\\Windows Defender"),                     // UNC
        QStringLiteral("C:\\Users\\Public\\Defender"),                             // elsewhere
        QStringLiteral("C:\\ProgramData\\Microsoft\\Windows Defender\\Platform"),  // no version
        QStringLiteral("C:\\ProgramData\\Microsoft\\Windows Defender\\Platform\\4.18\\nested"),
        QStringLiteral("C:\\ProgramData\\Microsoft\\Windows Defender\\Platform\\..\\..\\evil"),
        QStringLiteral("C:\\Program Files\\Windows Defender Evil"),  // prefix, not folder
        QStringLiteral("C:\\Program Files\\Windows Defender\\sub"),
    };
    for (const QString& install_location : refused) {
        const auto path = MicrosoftDefenderAction::mpCmdRunPathFromInstallLocation(install_location,
                                                                                   kPlatformRoot,
                                                                                   kProgramRoot);
        QVERIFY2(!path.has_value(),
                 qPrintable(QStringLiteral("accepted: '%1'").arg(install_location)));
        QVERIFY(!path.error().isEmpty());
    }
    // An unresolvable root accepts nothing rather than everything.
    QVERIFY(!MicrosoftDefenderAction::mpCmdRunPathFromInstallLocation(
                 QStringLiteral("C:\\Program Files\\Windows Defender"), QString(), QString())
                 .has_value());
}

void MicrosoftDefenderActionTests::runsThroughTheSharedElevatedHelperRoute() {
    // A non-elevated app runs administrator quick actions in sak_elevated_helper through
    // QuickActionController::startElevated; each Defender action must map to its own
    // helper task (registered in elevated_helper_main.cpp), never fall to "no task registered".
    using namespace sak::quick_action_elevation;
    const QList<QPair<DefenderOperation, QString>> expected{
        {DefenderOperation::QuickScan, QString::fromLatin1(kDefenderQuickScanTaskId)},
        {DefenderOperation::FullScan, QString::fromLatin1(kDefenderFullScanTaskId)},
        {DefenderOperation::SignatureUpdate, QString::fromLatin1(kDefenderSignatureUpdateTaskId)},
    };
    QStringList task_ids;
    for (const auto& [operation, task_id] : expected) {
        const MicrosoftDefenderAction action(operation);
        QCOMPARE(sak::elevatedTaskIdForQuickAction(action.name()), task_id);
        task_ids << task_id;
    }
    task_ids.removeDuplicates();
    QCOMPARE(task_ids.size(), 3);
}

QTEST_GUILESS_MAIN(MicrosoftDefenderActionTests)
#include "test_microsoft_defender_action.moc"
