// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_win32_gui_runner.h"

#include <QJsonArray>
#include <QStringList>
#include <QtTest/QtTest>

using sak::ai::executeWin32GuiSteps;
using sak::ai::Win32StepOutcome;

namespace {

QJsonObject step(const QString& tool, bool optional = false) {
    QJsonObject object{{QStringLiteral("tool"), tool}};
    if (optional) {
        object[QStringLiteral("optional")] = true;
    }
    return object;
}

Win32StepOutcome ok(const QString& text = QStringLiteral("done")) {
    return Win32StepOutcome{true, false, false, text, QString()};
}

Win32StepOutcome toolError(const QString& message) {
    return Win32StepOutcome{true, false, true, QString(), message};
}

Win32StepOutcome planFailure(const QString& message) {
    return Win32StepOutcome{false, false, false, QString(), message};
}

Win32StepOutcome highRisk() {
    return Win32StepOutcome{true, true, false, QString(), QString()};
}

}  // namespace

class AiWin32GuiRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runsEveryStepOnSuccess();
    void stopsAtFirstNonOptionalToolError();
    void continuesPastOptionalToolError();
    void rejectsHighRiskStep();
    void failsWhenAStepCannotBePlanned();
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
    QVERIFY(
        result.value(QStringLiteral("error")).toString().contains(QStringLiteral("click_text")));
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
    QVERIFY(result.value(QStringLiteral("error")).toString().contains(QStringLiteral("high-risk")));
}

void AiWin32GuiRunnerTests::failsWhenAStepCannotBePlanned() {
    const QJsonArray steps{step(QStringLiteral("not_a_real_tool"))};
    const QJsonObject result = executeWin32GuiSteps(steps, [&](const QJsonObject&) {
        return planFailure(
            QStringLiteral("tool 'not_a_real_tool' is not in bundled provider manifest"));
    });

    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QVERIFY(result.value(QStringLiteral("error")).toString().contains(QStringLiteral("planned")));
}

QTEST_GUILESS_MAIN(AiWin32GuiRunnerTests)
#include "test_ai_win32_gui_runner.moc"
