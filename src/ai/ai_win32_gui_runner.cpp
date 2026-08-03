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

// A fatal pre-execution rejection reason (unplannable tool, high-risk tool, or a disallowed
// middle-tier tool), else empty when the step may run. .short_error goes on the step record;
// .detail is the recipe-level failure message.
struct Rejection {
    QString short_error;
    QString detail;
    bool fatal() const { return !short_error.isEmpty(); }
};

Rejection rejectionFor(const Win32StepOutcome& outcome, int index, const QString& tool) {
    if (!outcome.planned) {
        return {outcome.error,
                QStringLiteral("Step %1 (%2) could not be planned: %3")
                    .arg(index)
                    .arg(tool, outcome.error)};
    }
    if (outcome.high_risk) {
        return {QStringLiteral("high-risk tool not allowed in recipe"),
                QStringLiteral("Step %1 (%2) uses a high-risk tool, which a win32_gui recipe may "
                               "not call")
                    .arg(index)
                    .arg(tool)};
    }
    if (outcome.disallowed) {
        return {outcome.error.isEmpty() ? QStringLiteral("tool not permitted in recipe")
                                        : outcome.error,
                QStringLiteral("Step %1 (%2) uses a tool that is not permitted in a win32_gui "
                               "recipe (only read-only and input-tier desktop tools are)")
                    .arg(index)
                    .arg(tool)};
    }
    return {};
}

}  // namespace

std::optional<QString> win32WaitExpectationFailure(const QJsonObject& payload,
                                                   bool require_satisfied) {
    struct WaitFlag {
        QLatin1String key;
        QLatin1String message;
    };
    bool any_satisfied = false;
    for (const WaitFlag& flag :
         {WaitFlag{QLatin1String("found"), QLatin1String("awaited text did not appear")},
          WaitFlag{QLatin1String("satisfied"),
                   QLatin1String("awaited window state was not reached")},
          WaitFlag{QLatin1String("idle"), QLatin1String("window did not settle")}}) {
        const QJsonValue value = payload.value(flag.key);
        if (value.isUndefined()) {
            continue;
        }
        if (!value.isBool()) {
            return QStringLiteral("%1 (result flag was malformed)").arg(flag.message);
        }
        if (!value.toBool()) {
            return QStringLiteral("%1 before the timeout").arg(flag.message);
        }
        any_satisfied = true;
    }
    if (require_satisfied && !any_satisfied) {
        return QStringLiteral("wait step produced no verifiable found/satisfied/idle result");
    }
    return std::nullopt;
}

QJsonObject executeWin32GuiSteps(const QJsonArray& steps, const Win32StepExecutor& exec) {
    QJsonArray results;
    for (int index = 0; index < steps.size(); ++index) {
        const QJsonObject step = steps.at(index).toObject();
        const QString tool = step.value(QStringLiteral("tool")).toString().trimmed();
        const bool optional = step.value(QStringLiteral("optional")).toBool(false);
        const Win32StepOutcome outcome = exec(step);

        QJsonObject entry = stepEntry(index, tool, optional);
        if (const Rejection rejection = rejectionFor(outcome, index, tool); rejection.fatal()) {
            entry[QStringLiteral("ok")] = false;
            entry[QStringLiteral("error")] = rejection.short_error;
            results.append(entry);
            return finish(results, false, rejection.detail);
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
