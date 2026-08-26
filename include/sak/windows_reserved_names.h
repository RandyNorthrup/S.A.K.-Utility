// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows_reserved_names.h
/// @brief The DOS device names Windows reserves, in ONE place.
///
/// WHY THIS FILE EXISTS
/// This catalogue was written out five separate times (file_management_file_system,
/// file_recovery_engine, input_validator, mbox_writer, offline_deployment_worker), each with its
/// own spelling of the same rule. That is the duplicated-knowledge shape this campaign keeps
/// finding: one fact, several copies, and nothing that makes them move together -- so a fix to
/// one leaves the others wrong. A sixth copy was needed for the AI conversation store's artifact
/// directory naming; this is that copy promoted to the canonical one instead.
///
/// Header-only and Qt-only-by-QString on purpose: it has no link-time dependency, so any target
/// can include it without new wiring.
///
/// MIGRATION STATUS -- INCOMPLETE, NOT DEFERRED: ai_conversation_store uses this. The five
/// pre-existing private copies listed above still exist and are being migrated onto it.

#pragma once

#include <QChar>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

namespace sak {

/// @brief True when @p name is a reserved Windows DOS device name.
///
/// Reserved names are matched on the stem BEFORE the first dot, case-insensitively, because an
/// extension does not make them safe: "NUL.txt" still opens the device rather than creating a
/// file. Trailing dots and spaces are ignored for the same reason they are elsewhere -- Windows
/// strips them from a path component, so "CON." is CON.
///
/// Creating a file or directory with one of these names FAILS, so callers that build a path from
/// user- or model-supplied text must substitute a fallback rather than let the operation fail
/// somewhere that cannot explain why.
[[nodiscard]] inline bool isWindowsReservedName(const QString& name) {
    QString stem = name.section(QLatin1Char('.'), 0, 0);
    while (!stem.isEmpty() &&
           (stem.endsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char(' ')))) {
        stem.chop(1);
    }
    stem = stem.trimmed().toUpper();
    static const QStringList kSingletonNames = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL")};
    if (kSingletonNames.contains(stem)) {
        return true;
    }
    // "COM"/"LPT" followed by exactly one digit 1-9. COM0/LPT0 are not reserved, and a longer
    // run of digits ("COM12") is an ordinary name.
    constexpr int kNumberedDeviceStemLength = 4;
    constexpr int kDevicePrefixLength = 3;
    if (stem.size() != kNumberedDeviceStemLength) {
        return false;
    }
    const QString prefix = stem.left(kDevicePrefixLength);
    if (prefix != QStringLiteral("COM") && prefix != QStringLiteral("LPT")) {
        return false;
    }
    const QChar last = stem.at(kDevicePrefixLength);
    return last >= QLatin1Char('1') && last <= QLatin1Char('9');
}

}  // namespace sak
