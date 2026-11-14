// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file link_following_calls.h
/// @brief Finds the Qt copy and rename calls in a source file: each follows a link planted at its
/// target name.
///
/// Security brief 4b: as administrator, a copy or rename that followed a link planted at the name
/// it writes would write wherever the link points. The profile workers copy and rename through
/// sak::no_follow instead. Their own link checks refuse a planted link before the copy, so a test
/// that plants one never reaches the copy; only the moment between that check and the copy is
/// left, which no test can time. This scan is what pins the copy itself.

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace sak::test {

/// The lines of @p source_path (numbered) that call QFile::copy or QFile::rename, or copy or
/// rename on a QFile or QDir object; text after "//" is left out. A file that cannot be read is
/// reported as a finding, so the check never passes for want of input.
inline QStringList linkFollowingFileCalls(const QString& source_path) {
    QFile file(source_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {QStringLiteral("cannot read ") + source_path};
    }
    static const QRegularExpression call(
        QStringLiteral(R"((\bQFile::(copy|rename)|\.(copy|rename))\s*\()"));
    QStringList found;
    int line_number = 0;
    for (const QString& line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        ++line_number;
        if (call.match(line.section(QStringLiteral("//"), 0, 0)).hasMatch()) {
            found.append(QStringLiteral("%1: %2").arg(line_number).arg(line.trimmed()));
        }
    }
    return found;
}

}  // namespace sak::test
