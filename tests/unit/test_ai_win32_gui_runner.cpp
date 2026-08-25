// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_win32_gui_runner.h"

#include <QJsonArray>
#include <QStringList>
#include <QtTest/QtTest>

using sak::ai::executeWin32GuiSteps;
using sak::ai::Win32StepOutcome;
using sak::ai::win32WaitExpectationFailure;

namespace {

QJsonObject step(const QString& tool, bool optional = false) {
    QJsonObject object{{QStringLiteral("tool"), tool}};
    if (optional) {
        object[QStringLiteral("optional")] = true;
    }
    return object;
}

Win32StepOutcome ok(const QString& text = QStringLiteral("done")) {
    return Win32StepOutcome{.planned = true, .result_text = text};
}

Win32StepOutcome toolError(const QString& message) {
    return Win32StepOutcome{.planned = true, .tool_error = true, .error = message};
}

Win32StepOutcome planFailure(const QString& message) {
    return Win32StepOutcome{.planned = false, .error = message};
}

// An executor result that carries error text WITHOUT setting tool_error -- a contradictory
// outcome the runner must fail closed on rather than record as ok.
Win32StepOutcome errorTextOnly(const QString& message) {
    return Win32StepOutcome{.planned = true, .error = message};
}

Win32StepOutcome highRisk() {
    return Win32StepOutcome{.planned = true, .high_risk = true};
}

Win32StepOutcome disallowed(const QString& message) {
    return Win32StepOutcome{.planned = true, .disallowed = true, .error = message};
}

}  // namespace

class AiWin32GuiRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runsEveryStepOnSuccess();
    void stopsAtFirstNonOptionalToolError();
    void continuesPastOptionalToolError();
    void rejectsHighRiskStep();
    void rejectsDisallowedMiddleTierStep();
    void failsWhenAStepCannotBePlanned();
    void failsClosedWhenPlanFailureCarriesNoReason();
    void waitExpectationFailureFlags();
    void waitStepRequiresSatisfiedFlag();
};

void AiWin32GuiRunnerTests::runsEveryStepOnSuccess() {
    QStringList seen;
    const QJsonArray steps{step(QStringLiteral("focus_window")),
                           step(QStringLiteral("click_text")),
                           step(QStringLiteral("ocr_window"))};
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject& s) {
        seen << s.value(QStringLiteral("tool")).toString();
        return ok();
    });

    QVERIFY(result.value(QStringLiteral("success")).toBool());
    QCOMPARE(result.value(QStringLiteral("step_count")).toInt(), 3);
    QCOMPARE(seen,
             QStringList({QStringLiteral("focus_window"),
                          QStringLiteral("click_text"),
                          QStringLiteral("ocr_window")}));  // every step ran, in recipe order

    // The steps array is the evidence record handed back to the model verbatim
    // (runWin32GuiAction, ai_provider_gateway_tool_runner.cpp:443-448), so pin each
    // entry's contract fields -- index/tool/optional/ok plus the executor's own output.
    const QJsonArray recorded = result.value(QStringLiteral("steps")).toArray();
    QCOMPARE(recorded.size(), 3);
    const QStringList expected_tools{QStringLiteral("focus_window"),
                                     QStringLiteral("click_text"),
                                     QStringLiteral("ocr_window")};
    for (int i = 0; i < recorded.size(); ++i) {
        const QJsonObject entry = recorded.at(i).toObject();
        QCOMPARE(entry.value(QStringLiteral("index")).toInt(-1), i);
        QCOMPARE(entry.value(QStringLiteral("tool")).toString(), expected_tools.at(i));
        QCOMPARE(entry.value(QStringLiteral("optional")).toBool(true), false);
        QVERIFY(entry.value(QStringLiteral("ok")).toBool());
        // the tool's own output must survive into the record, not just the ok flag
        QCOMPARE(entry.value(QStringLiteral("result_text")).toString(), QStringLiteral("done"));
        QVERIFY(!entry.contains(QStringLiteral("error")));
    }
}

void AiWin32GuiRunnerTests::stopsAtFirstNonOptionalToolError() {
    int calls = 0;
    const QJsonArray steps{step(QStringLiteral("focus_window")),
                           step(QStringLiteral("click_text")),   // this one fails, not optional
                           step(QStringLiteral("ocr_window"))};  // must NOT run
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject& s) {
        ++calls;
        return s.value(QStringLiteral("tool")).toString() == QLatin1String("click_text")
                   ? toolError(QStringLiteral("Text not found on screen: X"))
                   : ok();
    });

    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(calls, 2);  // stopped after the failing step; third never ran
    QCOMPARE(result.value(QStringLiteral("error")).toString(),
             QStringLiteral("Step 1 (click_text) failed"));
    // 'error_message' is the key every consumer actually reads (ai_tool_dispatcher.cpp
    // resultErrorMessage falls back to 'message', never to 'error'), so the recipe failure
    // reason must also reach the orchestrator/health ledger under that key.
    QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Step 1 (click_text) failed"));

    // Second arm of the fail-closed test at ai_win32_gui_runner.cpp:82: an outcome whose error
    // text is set while tool_error is UNSET is contradictory and must still fail the step, so
    // the recipe stops instead of marching on and reporting success.
    int text_only_calls = 0;
    const QJsonObject text_only = executeWin32GuiSteps(steps, [&](const QJsonObject& s) {
        ++text_only_calls;
        return s.value(QStringLiteral("tool")).toString() == QLatin1String("click_text")
                   ? errorTextOnly(QStringLiteral("Text not found on screen: X"))
                   : ok();
    });
    QVERIFY(!text_only.value(QStringLiteral("success")).toBool());
    QCOMPARE(text_only_calls, 2);  // stopped there too; third never ran
    QCOMPARE(text_only.value(QStringLiteral("error")).toString(),
             QStringLiteral("Step 1 (click_text) failed"));
    QVERIFY(!text_only.value(QStringLiteral("steps"))
                 .toArray()
                 .at(1)
                 .toObject()
                 .value(QStringLiteral("ok"))
                 .toBool());  // recorded as failed, not ok
}

void AiWin32GuiRunnerTests::continuesPastOptionalToolError() {
    int calls = 0;
    const QJsonArray steps{step(QStringLiteral("focus_window")),
                           step(QStringLiteral("malware database"),
                                /*optional=*/true),  // may be absent
                           step(QStringLiteral("Quick Scan"))};
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject& s) {
        ++calls;
        return s.value(QStringLiteral("tool")).toString() == QLatin1String("malware database")
                   ? toolError(QStringLiteral("Text not found on screen: malware database"))
                   : ok();
    });

    QVERIFY(result.value(QStringLiteral("success")).toBool());  // optional failure tolerated
    QCOMPARE(calls, 3);                                         // all three still ran
    const QJsonArray recorded = result.value(QStringLiteral("steps")).toArray();
    QCOMPARE(recorded.size(), 3);  // every step, failed one included, is recorded
    const QJsonObject failed_step = recorded.at(1).toObject();
    // Read the flag as a PRESENT bool: value().toBool() alone is false-by-default, so an
    // ok key dropped entirely would look identical to ok:false.
    QVERIFY(failed_step.value(QStringLiteral("ok")).isBool());
    QVERIFY(!failed_step.value(QStringLiteral("ok")).toBool());  // recorded as failed
    // The surrounding steps carry ok:true, so dropping the flag cannot pass as "all failed".
    QVERIFY(recorded.at(0).toObject().value(QStringLiteral("ok")).isBool());
    QVERIFY(recorded.at(0).toObject().value(QStringLiteral("ok")).toBool());
    QVERIFY(recorded.at(2).toObject().value(QStringLiteral("ok")).toBool());
    QVERIFY(!recorded.at(1).toObject().value(QStringLiteral("ok")).toBool());  // recorded as failed
}

void AiWin32GuiRunnerTests::rejectsHighRiskStep() {
    const QJsonArray steps{step(QStringLiteral("focus_window")),
                           step(QStringLiteral("kill_process"))};
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject& s) {
        return s.value(QStringLiteral("tool")).toString() == QLatin1String("kill_process")
                   ? highRisk()
                   : ok();
    });

    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(result.value(QStringLiteral("error")).toString(),
             QStringLiteral(
                 "Step 1 (kill_process) uses a high-risk tool, which a win32_gui recipe may not "
                 "call"));

    // A recipe cannot opt out of the high-risk gate by marking the step optional: the
    // rejection at ai_win32_gui_runner.cpp:157 is fatal regardless of `optional` (only the
    // ordinary step-failure check at :167 honours it), per the header contract that a
    // high-risk tool is "disallowed inside a GUI recipe" unconditionally. Recipes are
    // model-authored, so optional:true on a high-risk step is exactly what this gate exists
    // for. Also pins the per-step rejection record, which nothing else covers.
    int optional_calls = 0;
    const QJsonArray optional_steps{step(QStringLiteral("focus_window")),
                                    step(QStringLiteral("kill_process"), /*optional=*/true),
                                    step(QStringLiteral("ocr_window"))};  // must NOT run
    const QJsonObject optional_result =
        executeWin32GuiSteps(optional_steps, [&](const QJsonObject& s) {
            ++optional_calls;
            return s.value(QStringLiteral("tool")).toString() == QLatin1String("kill_process")
                       ? highRisk()
                       : ok();
        });

    QVERIFY(!optional_result.value(QStringLiteral("success")).toBool());
    QCOMPARE(optional_calls, 2);  // rejected at the high-risk step; third never ran
    QCOMPARE(optional_result.value(QStringLiteral("error")).toString(),
             QStringLiteral(
                 "Step 1 (kill_process) uses a high-risk tool, which a win32_gui recipe may not "
                 "call"));
    const QJsonArray optional_recorded = optional_result.value(QStringLiteral("steps")).toArray();
    QCOMPARE(optional_recorded.size(), 2);  // the rejected step IS appended, not dropped
    const QJsonObject rejected_entry = optional_recorded.at(1).toObject();
    QVERIFY(rejected_entry.value(QStringLiteral("optional")).toBool());  // it really was optional
    QVERIFY(!rejected_entry.value(QStringLiteral("ok")).toBool());
    QCOMPARE(rejected_entry.value(QStringLiteral("error")).toString(),
             QStringLiteral("high-risk tool not allowed in recipe"));
}

void AiWin32GuiRunnerTests::rejectsDisallowedMiddleTierStep() {
    // A middle-tier tool (planned, not high-risk, but demands a per-call confirmation on the direct
    // path -- e.g. clipboard_write) must not auto-run inside a recipe: the runner rejects it and
    // the following step never runs.
    int calls = 0;
    const QJsonArray steps{step(QStringLiteral("focus_window")),
                           step(QStringLiteral("clipboard_write")),
                           step(QStringLiteral("ocr_window"))};
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject& s) {
        ++calls;
        return s.value(QStringLiteral("tool")).toString() == QLatin1String("clipboard_write")
                   ? disallowed(QStringLiteral("tool 'clipboard_write' is not permitted in a "
                                               "win32_gui recipe"))
                   : ok();
    });

    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(calls, 2);  // stopped after the disallowed step; third never ran
    QCOMPARE(result.value(QStringLiteral("error")).toString(),
             QStringLiteral("Step 1 (clipboard_write) uses a tool that is not permitted in a "
                            "win32_gui recipe (only read-only and input-tier desktop tools are)"));

    // The executor's own message is what lands on the step record (short_error).
    QCOMPARE(result.value(QStringLiteral("steps"))
                 .toArray()
                 .at(1)
                 .toObject()
                 .value(QStringLiteral("error"))
                 .toString(),
             QStringLiteral("tool 'clipboard_write' is not permitted in a win32_gui recipe"));

    // Fail-closed fallback: an executor that flags disallowed WITHOUT filling in an error must
    // still be rejected. Rejection::fatal() keys on a non-empty short_error, so dropping the
    // fallback reason would demote the step to a plain (empty-error, no-tool_error) outcome that
    // records ok:true and lets the recipe report success with the disallowed tool marked good.
    int blank_calls = 0;
    const QJsonObject blank = executeWin32GuiSteps(steps, [&](const QJsonObject& s) {
        ++blank_calls;
        return s.value(QStringLiteral("tool")).toString() == QLatin1String("clipboard_write")
                   ? disallowed(QString())
                   : ok();
    });

    QVERIFY(!blank.value(QStringLiteral("success")).toBool());
    QCOMPARE(blank_calls, 2);  // still stopped at the disallowed step; third never ran
    const QJsonObject blank_step = blank.value(QStringLiteral("steps")).toArray().at(1).toObject();
    QVERIFY(!blank_step.value(QStringLiteral("ok")).toBool());
    QCOMPARE(blank_step.value(QStringLiteral("error")).toString(),
             QStringLiteral("tool not permitted in recipe"));
    QCOMPARE(blank.value(QStringLiteral("error")).toString(),
             QStringLiteral("Step 1 (clipboard_write) uses a tool that is not permitted in a "
                            "win32_gui recipe (only read-only and input-tier desktop tools are)"));
}

void AiWin32GuiRunnerTests::failsWhenAStepCannotBePlanned() {
    const QJsonArray steps{step(QStringLiteral("not_a_real_tool"))};
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject&) {
        return planFailure(
            QStringLiteral("tool 'not_a_real_tool' is not in bundled provider manifest"));
    });

    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(result.value(QStringLiteral("error")).toString(),
             QStringLiteral("Step 0 (not_a_real_tool) could not be planned: tool 'not_a_real_tool' "
                            "is not in bundled provider manifest"));
}
void AiWin32GuiRunnerTests::failsClosedWhenPlanFailureCarriesNoReason() {
    // A bare Win32StepOutcome{} is planned=false with an EMPTY error. The runner must still
    // reject it: without the fallback reason, Rejection::fatal() (keyed on a non-empty
    // short_error) reads false and the unplannable step is recorded ok:true -> whole-recipe
    // success. Fail closed instead, and do not run the following step.
    const QJsonArray steps{step(QStringLiteral("mystery_tool")),
                           step(QStringLiteral("ocr_window"))};
    int calls = 0;
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject&) {
        ++calls;
        return Win32StepOutcome{};  // planned=false, no reason supplied
    });

    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(calls, 1);  // stopped at the unplannable step; the second never ran
    QCOMPARE(result.value(QStringLiteral("error")).toString(),
             QStringLiteral("Step 0 (mystery_tool) could not be planned: tool could not be "
                            "planned"));
    const QJsonObject entry = result.value(QStringLiteral("steps")).toArray().at(0).toObject();
    QVERIFY(!entry.value(QStringLiteral("ok")).toBool());
    QCOMPARE(entry.value(QStringLiteral("error")).toString(),
             QStringLiteral("tool could not be planned"));
}


void AiWin32GuiRunnerTests::waitExpectationFailureFlags() {
    // All-true / absent flags -> no failure; any false found/satisfied/idle -> a failure message.
    QVERIFY(!win32WaitExpectationFailure(QJsonObject{}).has_value());
    QVERIFY(!win32WaitExpectationFailure(QJsonObject{{QStringLiteral("found"), true}}).has_value());
    QVERIFY(!win32WaitExpectationFailure(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}})
                 .has_value());  // non-flag payload ignored
    QCOMPARE(win32WaitExpectationFailure(QJsonObject{{QStringLiteral("found"), false}})
                 .value_or(QString()),
             QStringLiteral("awaited text did not appear before the timeout"));
    QCOMPARE(win32WaitExpectationFailure(QJsonObject{{QStringLiteral("satisfied"), false}})
                 .value_or(QString()),
             QStringLiteral("awaited window state was not reached before the timeout"));
    const auto idle = win32WaitExpectationFailure(QJsonObject{{QStringLiteral("idle"), false}});
    QVERIFY(idle.has_value());
    QCOMPARE(*idle, QStringLiteral("window did not settle before the timeout"));
    // A non-bool flag value is MALFORMED and must fail closed (previously treated as satisfied).
    QCOMPARE(win32WaitExpectationFailure(
                 QJsonObject{{QStringLiteral("found"), QStringLiteral("no")}})
                 .value_or(QString()),
             QStringLiteral("awaited text did not appear (result flag was malformed)"));

    // The scan must not stop at the first true flag: a later false flag in the SAME payload is
    // still the failing one (production keeps looping past any_satisfied, runner .cpp:120).
    QCOMPARE(win32WaitExpectationFailure(
                 QJsonObject{{QStringLiteral("found"), true}, {QStringLiteral("satisfied"), false}})
                 .value_or(QString()),
             QStringLiteral("awaited window state was not reached before the timeout"));
    QCOMPARE(win32WaitExpectationFailure(QJsonObject{{QStringLiteral("found"), true},
                                                     {QStringLiteral("satisfied"), true},
                                                     {QStringLiteral("idle"), false}})
                 .value_or(QString()),
             QStringLiteral("window did not settle before the timeout"));
}

void AiWin32GuiRunnerTests::waitStepRequiresSatisfiedFlag() {
    // require_satisfied=true (the step IS a wait_for_* tool): a truncated/empty result_text
    // parses to {} and must fail closed instead of silently passing.
    QCOMPARE(
        win32WaitExpectationFailure(QJsonObject{}, /*require_satisfied=*/true).value_or(QString()),
        QStringLiteral("wait step produced no verifiable found/satisfied/idle result"));
    // A present-and-true flag satisfies the requirement.
    QVERIFY(!win32WaitExpectationFailure(QJsonObject{{QStringLiteral("found"), true}},
                                         /*require_satisfied=*/true)
                 .has_value());
    // A non-wait step (require_satisfied=false) with no flags is still fine.
    QVERIFY(!win32WaitExpectationFailure(QJsonObject{}, /*require_satisfied=*/false).has_value());
    // A NON-EMPTY wait result that carries none of found/satisfied/idle (a garbled parse, or a
    // wait_for_* whose payload uses other keys) is unverified too -- the requirement is "at least
    // one flag present and true", not merely "payload non-empty".
    QCOMPARE(win32WaitExpectationFailure(QJsonObject{{QStringLiteral("waited_ms"), 5000},
                                                     {QStringLiteral("text"), QStringLiteral("x")}},
                                         /*require_satisfied=*/true)
                 .value_or(QString()),
             QStringLiteral("wait step produced no verifiable found/satisfied/idle result"));
}

QTEST_GUILESS_MAIN(AiWin32GuiRunnerTests)
#include "test_ai_win32_gui_runner.moc"
