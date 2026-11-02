// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_verify_system_files_action.cpp
/// @brief Unit tests for VerifySystemFilesAction pure seams (Codex B6-07/08):
///        DISM corruption detection must not fire on a clean machine, and the
///        SFC temp filename must be unique per run.

#include "sak/actions/verify_system_files_action.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QtTest/QtTest>

using sak::VerifySystemFilesAction;

class VerifySystemFilesActionTests : public QObject {
    Q_OBJECT

    using Action = VerifySystemFilesAction;

private Q_SLOTS:
    // B6-07: the healthy DISM verdict contains the substring "corruption"; it
    // must still be read as "no corruption" so RestoreHealth is not run needlessly.
    void dismReportsCorruption_cleanMachineIsNotCorrupt();
    void dismReportsCorruption_repairableAndDetectedAreCorrupt();
    // B6-08: SFC redirect target is unpredictable and PowerShell-safe.
    void makeUniqueSfcOutputName_uniqueAndWellFormed();
    void sfcOutputIsReadAsUtf16WithoutAByteOrderMark();
    void parseSfcOutput_readsEachVerdict();
    void parseSfcOutput_tellsAnEnglishRefusalFromAnotherLanguage();
    void healthyPcInAnotherLanguageIsNotToldItFailed();
    void unreadableSfcWithDismFailingIsStillAFailure();
    void englishRefusalIsStillAFailure();
    void cleanEnglishRunStillPasses();
    // One 60-minute bound for the whole run, and the registry waits exactly that long.
    void runIsBoundByOneSixtyMinuteTimeout();
    void stepTimeoutIsCutToTheRemainingBudget();
};

void VerifySystemFilesActionTests::runIsBoundByOneSixtyMinuteTimeout() {
    QCOMPARE(sak::kVerifySystemFilesTimeoutMs, 60 * 60 * 1000);
    const Action action;
    // The value the assistant's registry thunk waits on is the action's own bound, not the
    // 30-minute generic ceiling that used to cut SFC + DISM off part-way.
    QCOMPARE(action.executionTimeoutMs(), sak::kVerifySystemFilesTimeoutMs);
    QVERIFY(action.executionTimeoutMs() != sak::kDefaultQuickActionTimeoutMs);
}

void VerifySystemFilesActionTests::stepTimeoutIsCutToTheRemainingBudget() {
    constexpr int kStepCapMs = 1'800'000;
    // Plenty left: the step keeps its own cap.
    QCOMPARE(Action::boundedStepTimeoutMs(kStepCapMs, 3'600'000), kStepCapMs);
    // Less left than the cap: the step gets only what is left, so the steps' sum can never
    // run past the run's single bound.
    QCOMPARE(Action::boundedStepTimeoutMs(kStepCapMs, 120'000), 120'000);
    QCOMPARE(Action::boundedStepTimeoutMs(kStepCapMs, 1), 1);
    // Nothing left (or the deadline already passed): the step must not start at all.
    QCOMPARE(Action::boundedStepTimeoutMs(kStepCapMs, 0), 0);
    QCOMPARE(Action::boundedStepTimeoutMs(kStepCapMs, -5), 0);
    QCOMPARE(Action::boundedStepTimeoutMs(0, 3'600'000), 0);
}

void VerifySystemFilesActionTests::dismReportsCorruption_cleanMachineIsNotCorrupt() {
    // The exact DISM /CheckHealth and /ScanHealth healthy output.
    QVERIFY(
        !Action::dismReportsCorruption(QStringLiteral("No component store corruption detected.\n"
                                                      "The operation completed successfully.")));
    // Bare success line with no corruption vocabulary at all.
    QVERIFY(
        !Action::dismReportsCorruption(QStringLiteral("The operation completed successfully.")));
    QVERIFY(!Action::dismReportsCorruption(QString()));

    // Real DISM /CheckHealth prints a version banner BEFORE the verdict, so the healthy line is
    // not at offset 0: the exclusion must be a whole-string search, never an anchored
    // startsWith()/^regex. (B6-07: anchoring flips every clean machine to "corrupt" and forces a
    // needless /RestoreHealth.)
    QVERIFY(!Action::dismReportsCorruption(
        QStringLiteral("Deployment Image Servicing and Management tool\n"
                       "Version: 10.0.19041.844\n"
                       "\n"
                       "Image Version: 10.0.19041.844\n"
                       "\n"
                       "No component store corruption detected.\n"
                       "The operation completed successfully.")));
    // The exclusion is case-insensitive BY CONTRACT: a differently-cased healthy verdict must
    // still read as clean. Dropping Qt::CaseInsensitive from the exclusion would let the bare
    // "corruption" arm -- itself case-insensitive -- fire on this healthy output.
    QVERIFY(
        !Action::dismReportsCorruption(QStringLiteral("no component store CORRUPTION detected.\n"
                                                      "The operation completed successfully.")));
}

void VerifySystemFilesActionTests::dismReportsCorruption_repairableAndDetectedAreCorrupt() {
    // Repairable corruption (CheckHealth/ScanHealth phrasing).
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("The component store is repairable.\n"
                                                     "The operation completed successfully.")));
    // Explicit corruption-detected phrasing (not the "No ... corruption" verdict).
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("Component store corruption was detected.")));
    // The exclusion is the FULL "No component store corruption" verdict, not the
    // "corruption detected" tail it shares with the corrupt phrasing: the same tail without the
    // leading "No" is corruption.
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("Component store corruption detected.\n"
                                                     "The operation completed successfully.")));
}

void VerifySystemFilesActionTests::makeUniqueSfcOutputName_uniqueAndWellFormed() {
    const QString a = Action::makeUniqueSfcOutputName(1234, 999'999, 0);
    const QString b = Action::makeUniqueSfcOutputName(1234, 999'999, 1);

    QCOMPARE(a, QStringLiteral("sak_sfc_output_1234_999999_0.txt"));
    // Production feeds a 63-bit OS-CSPRNG token and a ~1.76e12 epoch-ms stamp. Pin those at full
    // width: any narrowing of the leading field (int truncation, a "keep the name short" modulo)
    // would collapse the unpredictability that makes a planted-reparse-point hijack of the
    // elevated SFC write impractical, and small toy inputs render byte-identically under such a
    // bug.
    QCOMPARE(Action::makeUniqueSfcOutputName(
                 0x7F'FF'FF'FF'FF'FF'FF'FFLL, 1'766'000'000'000LL, 4'294'967'295u),
             QStringLiteral("sak_sfc_output_9223372036854775807_1766000000000_4294967295.txt"));
    // Every counter value is rendered VERBATIM as its own field -- it is not folded
    // into a tie-break bit -- so N runs inside one millisecond yield N distinct
    // redirect targets (verify_system_files_action.cpp:61,71).
    QCOMPARE(b, QStringLiteral("sak_sfc_output_1234_999999_1.txt"));
    QCOMPARE(Action::makeUniqueSfcOutputName(1234, 999'999, 2),
             QStringLiteral("sak_sfc_output_1234_999999_2.txt"));
    QCOMPARE(Action::makeUniqueSfcOutputName(1234, 999'999, 4'294'967'295u),
             QStringLiteral("sak_sfc_output_1234_999999_4294967295.txt"));
    QVERIFY(a != b);  // same pid+ms, different counter -> distinct
    QVERIFY(a.endsWith(QStringLiteral(".txt")));
    QVERIFY(a != b);  // same pid+ms, different counter -> distinct
    QVERIFY(a.endsWith(QStringLiteral(".txt")));
    // Must be safe to embed in a single-quoted PowerShell string.
    QVERIFY(!a.contains(QLatin1Char('\'')));
    QVERIFY(!a.contains(QLatin1Char(' ')));
}

// The real thing, end to end: real PowerShell reads a file holding SFC's real bytes. The refusal
// below was captured from sfc.exe on Windows 11 with its output redirected, the same way the action
// does it: UTF-16 little endian, no byte order mark. With the Get-Content left at its default this
// comes back with a NUL after every character, and no verdict phrase can match.
void VerifySystemFilesActionTests::sfcOutputIsReadAsUtf16WithoutAByteOrderMark() {
    const QString refusal = QStringLiteral(
        "\r\r\nYou must be an administrator running a console session in order to \r\r\n"
        "use the sfc utility.\r\r\n");
    const QString clean = QStringLiteral(
        "\r\nBeginning system scan.  This process will take some time.\r\n\r\n"
        "Beginning verification phase of system scan.\r\nVerification 100% complete.\r\n\r\n"
        "Windows Resource Protection did not find any integrity violations.\r\n");
    const auto runThroughPowerShell = [](const QString& text) {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sfc_output.txt"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return QString();
        }
        // No BOM, just the code units, exactly what sfc.exe writes.
        file.write(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
        file.close();
        const sak::ProcessResult proc = sak::runPowerShell(
            QStringLiteral("$sfcOutput = '%1'; %2").arg(path, Action::sfcOutputReadCommand()),
            sak::kTimeoutProcessLongMs);
        return proc.completedSuccessfully() ? proc.std_out : QStringLiteral("<powershell failed>");
    };

    const QString refused_output = runThroughPowerShell(refusal);
    QVERIFY2(!refused_output.contains(QChar(0)),
             "a NUL means the file was read as the wrong encoding");
    const auto refused = Action::parseSfcOutput(refused_output);
    QVERIFY(!refused.ran);
    QCOMPARE(refused.detail,
             QStringLiteral("You must be an administrator running a console session in order to "
                            "use the sfc utility."));

    const auto scanned = Action::parseSfcOutput(runThroughPowerShell(clean));
    QVERIFY2(scanned.ran, "a clean scan must be read as a completed one");
    QVERIFY(!scanned.found_issues);
}

namespace {
const QString kGermanHealthy = QStringLiteral(
    "Die Systemsuche wird gestartet. Dieser Vorgang kann einige Zeit dauern.\r\n"
    "Die \u00DCberpr\u00FCfung ist zu 100 % abgeschlossen.\r\n"
    "Der Windows-Ressourcenschutz hat keine Integrit\u00E4tsverletzungen gefunden.\r\n"
    "CBS_LOG_PATH:C:\\Windows\\Logs\\CBS\\CBS.log");
const QString kDismHealthy = QStringLiteral(
    "No component store corruption detected.\nThe operation completed successfully.");

/// Runs the action's real logic against fixed step output. Nothing reaches this Windows install:
/// no SFC, no DISM, not even PowerShell.
class ScriptedVerifyAction : public VerifySystemFilesAction {
public:
    QString sfc_output;
    QString dism_output;

protected:
    sak::ProcessResult runStep(const QString& script, int) override {
        sak::ProcessResult proc;
        proc.exit_code = 0;
        proc.std_out = script.contains(QStringLiteral("sfc.exe")) ? sfc_output : dism_output;
        return proc;
    }
};
}  // namespace

void VerifySystemFilesActionTests::parseSfcOutput_tellsAnEnglishRefusalFromAnotherLanguage() {
    const auto german = Action::parseSfcOutput(kGermanHealthy);
    QVERIFY(!german.ran);
    QVERIFY(german.unreadable);
    QVERIFY2(german.detail.contains(QStringLiteral("Integrit")), qPrintable(german.detail));
    QVERIFY(!german.detail.contains(QStringLiteral("CBS_LOG_PATH")));

    for (const char* refusal :
         {"You must be an administrator running a console session in order\r\n"
          "to use the sfc utility.",
          "Windows Resource Protection could not start the repair service.",
          "Windows Resource Protection could not perform the requested "
          "operation.",
          "There is a system repair pending which requires reboot to "
          "complete."}) {
        const auto verdict = Action::parseSfcOutput(QString::fromLatin1(refusal));
        QVERIFY2(!verdict.ran && !verdict.unreadable, refusal);
        QVERIFY2(!verdict.detail.isEmpty(), refusal);
    }

    // After a repair the verdict is not the last line: both have to reach the person.
    const auto repaired = Action::parseSfcOutput(
        QStringLiteral("Le Verdict.\r\nLes d\u00E9tails sont dans CBS.log.\r\n"));
    QVERIFY2(repaired.detail.contains(QStringLiteral("Le Verdict.")), qPrintable(repaired.detail));

    // Nothing at all from SFC is not a language problem: it did not run.
    QVERIFY(!Action::parseSfcOutput(QString()).unreadable);
    QVERIFY(!Action::parseSfcOutput(QStringLiteral("CBS_LOG_PATH:C:\\x")).unreadable);
}

void VerifySystemFilesActionTests::healthyPcInAnotherLanguageIsNotToldItFailed() {
    ScriptedVerifyAction action;
    action.sfc_output = kGermanHealthy;
    action.dism_output = kDismHealthy;

    action.execute();

    const auto& result = action.lastExecutionResult();
    QVERIFY2(!result.success, "a verdict nobody could read is not a pass");
    QVERIFY(result.inconclusive);
    QVERIFY2(result.message.contains(QStringLiteral("This is not a failure")),
             qPrintable(result.message));
    QVERIFY2(result.message.contains(QStringLiteral("Integrit")), qPrintable(result.message));
    QVERIFY2(result.message.contains(QStringLiteral("DISM found no issues")),
             qPrintable(result.message));
}

void VerifySystemFilesActionTests::unreadableSfcWithDismFailingIsStillAFailure() {
    ScriptedVerifyAction action;
    action.sfc_output = kGermanHealthy;
    action.dism_output = QStringLiteral("Error: 740\nElevated permissions are required.");

    action.execute();

    QVERIFY(!action.lastExecutionResult().success);
    QVERIFY(!action.lastExecutionResult().inconclusive);
}

void VerifySystemFilesActionTests::englishRefusalIsStillAFailure() {
    ScriptedVerifyAction action;
    action.sfc_output =
        QStringLiteral("Windows Resource Protection could not start the repair service.");
    action.dism_output = kDismHealthy;

    action.execute();

    const auto& result = action.lastExecutionResult();
    QVERIFY(!result.success);
    QVERIFY(!result.inconclusive);
    QVERIFY2(result.message.contains(QStringLiteral("did not complete a verification run")),
             qPrintable(result.message));
}

void VerifySystemFilesActionTests::cleanEnglishRunStillPasses() {
    ScriptedVerifyAction action;
    action.sfc_output =
        QStringLiteral("Windows Resource Protection did not find any integrity violations.");
    action.dism_output = kDismHealthy;

    action.execute();

    QVERIFY(action.lastExecutionResult().success);
    QVERIFY(!action.lastExecutionResult().inconclusive);
}

void VerifySystemFilesActionTests::parseSfcOutput_readsEachVerdict() {
    auto verdict = Action::parseSfcOutput(QStringLiteral(
        "Windows Resource Protection found corrupt files and successfully repaired them."));
    QVERIFY(verdict.ran && verdict.found_issues && verdict.repaired);

    verdict = Action::parseSfcOutput(QStringLiteral(
        "Windows Resource Protection found corrupt files but was unable to fix some of them."));
    QVERIFY(verdict.ran && verdict.found_issues && !verdict.repaired);

    // No verdict: not a pass, and SFC's own last line is kept. The CBS marker the script appends
    // is ours, not SFC's, so it is never what gets quoted.
    verdict = Action::parseSfcOutput(
        QStringLiteral("Windows Resource Protection could not perform the requested operation.\n"
                       "CBS_LOG_PATH:C:\\Windows\\Logs\\CBS\\CBS.log\n"));
    QVERIFY(!verdict.ran && !verdict.found_issues);
    QCOMPARE(verdict.detail,
             QStringLiteral(
                 "Windows Resource Protection could not perform the requested operation."));

    // Nothing at all.
    verdict = Action::parseSfcOutput(QString());
    QVERIFY(!verdict.ran);
    QVERIFY(verdict.detail.isEmpty());
}

QTEST_GUILESS_MAIN(VerifySystemFilesActionTests)
#include "test_verify_system_files_action.moc"
