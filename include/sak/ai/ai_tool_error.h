// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace sak::ai {

/// Build the canonical failure envelope returned by built-in AI tool runners.
///
/// The schema is exactly a boolean `success:false` plus a non-empty string `error_message`.
/// Whitespace-only input fails closed to a stable diagnostic instead of emitting an ambiguous
/// failure that downstream dispatch, health, workflow, and model consumers cannot explain.
[[nodiscard]] inline QJsonObject toolError(const QString& message) {
    const QString error_message = message.trimmed().isEmpty()
                                      ? QStringLiteral("AI tool failed without an error message")
                                      : message;
    return QJsonObject{{QStringLiteral("success"), QJsonValue(false)},
                       {QStringLiteral("error_message"), QJsonValue(error_message)}};
}

}  // namespace sak::ai
