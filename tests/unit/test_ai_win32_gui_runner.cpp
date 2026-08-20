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
    QCOMPARE(seen.size(), 3);  // every step ran
    QCOMPARE(result.value(QStringLiteral("steps")).toArray().size(), 3);
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
}

QTEST_GUILESS_MAIN(AiWin32GuiRunnerTests)
#include "test_ai_win32_gui_runner.moc"
