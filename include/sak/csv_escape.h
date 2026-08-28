// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QChar>
#include <QLatin1Char>
#include <QString>

/// @file csv_escape.h
/// @brief The one way this program writes a value into a CSV cell.
///
/// There were four writers and they did not agree. Two (diagnostic_report_generator,
/// email_export_worker) carried the formula guard below; two (the vulnerability panel and the
/// network diagnostic panel's table export) carried quoting ONLY. Quoting alone stops a value
/// from breaking out of its cell -- it does not stop the cell from being EXECUTED, because a
/// spreadsheet evaluates a leading '=' inside a quoted cell exactly as it does outside one.
///
/// That mattered on both of the paths that lacked it, because both export text this program did
/// not author: vulnerability findings come from external advisory feeds, and the network panel
/// exports WiFi SSIDs, remote hosts, share names and firewall rule names. A network named
/// `=HYPERLINK("http://attacker/","Click")` is a legal SSID, and it becomes a live formula the
/// moment the exported CSV is opened.
namespace sak {

/// @brief True when a value leads with a character a spreadsheet treats as the start of a
///        formula (CWE-1236, "Improper Neutralization of Formula Elements in a CSV File").
/// @note Checked on the TRIMMED value: leading whitespace does not stop the evaluation, so it
///       must not be allowed to hide the character that triggers it. Tab and CR are included
///       because they are stripped by some importers before the first real character is read.
[[nodiscard]] inline bool startsWithFormulaChar(const QString& value) {
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const QChar first = trimmed.at(0);
    return first == QLatin1Char('=') || first == QLatin1Char('+') || first == QLatin1Char('-') ||
           first == QLatin1Char('@') || first == QLatin1Char('\t') || first == QLatin1Char('\r');
}

/// @brief Escape @p value for one CSV cell, RFC 4180 plus the formula guard.
/// @param delimiter The field separator in use, so a cell is quoted whenever it contains one.
/// @return A cell that is safe to join with @p delimiter and safe to open in a spreadsheet.
///
/// Two independent jobs, in this order:
///   1. A leading apostrophe when the value would read as a formula. This forces the cell to
///      plain text and must be applied BEFORE the quoting decision, so the apostrophe is inside
///      the quotes rather than stranded outside them.
///   2. RFC 4180 quoting when the value contains the delimiter, a double quote, CR or LF:
///      embedded quotes are doubled and the whole cell is wrapped.
/// A caller that wraps the result in its own quotes on top of this would produce a broken cell;
/// use the returned string as-is.
[[nodiscard]] inline QString csvEscape(const QString& value, QChar delimiter = QLatin1Char(',')) {
    QString sanitized = value;
    if (startsWithFormulaChar(sanitized)) {
        sanitized.prepend(QLatin1Char('\''));
    }
    if (sanitized.contains(delimiter) || sanitized.contains(QLatin1Char('"')) ||
        sanitized.contains(QLatin1Char('\n')) || sanitized.contains(QLatin1Char('\r'))) {
        sanitized.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + sanitized + QLatin1Char('"');
    }
    return sanitized;
}

}  // namespace sak
