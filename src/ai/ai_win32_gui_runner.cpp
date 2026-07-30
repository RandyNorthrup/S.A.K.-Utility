// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_win32_gui_runner.h"

namespace sak::ai {

namespace {

QJsonObject finish(QJsonArray results, bool success, const QString& error) {
    QJsonObject out;
    out[QStringLiteral("steps")] = results;
    out[QStringLiteral("step_count")] = results.size();
    out[QStringLiteral("success")] = success;
    if (!error.isEmpty()) {
        out[QStringLiteral("error")] = error;
    }
    return out;
}

// Build the per-step record; ok/error are filled by the caller from the outcome.
QJsonObject stepEntry(int index, const QString& tool, bool optional) {
    return QJsonObject{{QStringLiteral("index"), index},
                       {QStringLiteral("tool"), tool},
                       {QStringLiteral("optional"), optional}};
}

}  // namespace

QJsonObject executeWin32GuiSteps(const QJsonArray& steps, const Win32StepExecutor& exec) {
    QJsonArray results;
    for (int index = 0; index < steps.size(); ++index) {
        const QJsonObject step = steps.at(index).toObject();
        const QString tool = step.value(QStringLiteral("tool")).toString().trimmed();
        const bool optional = step.value(QStringLiteral("optional")).toBool(false);
        const Win32StepOutcome outcome = exec(step);

        QJsonObject entry = stepEntry(index, tool, optional);
        if (!outcome.planned) {
            entry[QStringLiteral("ok")] = false;
            entry[QStringLiteral("error")] = outcome.error;
            results.append(entry);
            return finish(results,
                          false,
                          QStringLiteral("Step %1 (%2) could not be planned: %3")
                              .arg(index)
                              .arg(tool, outcome.error));
        }
        if (outcome.high_risk) {
            entry[QStringLiteral("ok")] = false;
            entry[QStringLiteral("error")] = QStringLiteral("high-risk tool not allowed in recipe");
            results.append(entry);
            return finish(results,
                          false,
                          QStringLiteral("Step %1 (%2) uses a high-risk tool, which a win32_gui "
                                         "recipe may not call")
                              .arg(index)
                              .arg(tool));
        }

        entry[QStringLiteral("ok")] = !outcome.tool_error;
        if (!outcome.result_text.isEmpty()) {
            entry[QStringLiteral("result_text")] = outcome.result_text;
        }
        if (outcome.tool_error) {
            entry[QStringLiteral("error")] = outcome.error.isEmpty() ? outcome.result_text
                                                                     : outcome.error;
        }
        results.append(entry);

        if (outcome.tool_error && !optional) {
            return finish(results,
                          false,
                          QStringLiteral("Step %1 (%2) failed").arg(index).arg(tool));
        }
    }
    return finish(results, true, QString());
}

}  // namespace sak::ai
