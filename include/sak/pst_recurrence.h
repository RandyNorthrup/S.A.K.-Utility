// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_recurrence.h
/// @brief Reads a repeating appointment's pattern and time zone from an Outlook store.
///
/// A repeating event keeps its rule in one binary property (MS-OXOCAL 2.2.1.44), in the wall-clock
/// time of the zone it was created in, with a separate list of the occurrences that were deleted
/// or changed. Anything here that cannot be read exactly leaves the event marked Unreadable: a
/// wrong rule would put meetings on the wrong days in another calendar without anyone noticing.

#pragma once

#include "sak/email_types.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace sak::pst {

/// A zone from PidLidTimeZoneStruct's 48 bytes; not known when the bytes are the wrong size.
[[nodiscard]] PstTimeZone parseTimeZoneStruct(const QByteArray& raw, const QString& description);

/// A zone from a TZDEFINITION (PidLidAppointmentTimeZoneDefinition*), using the rule Outlook marks
/// as in effect. Not known when the bytes do not parse.
[[nodiscard]] PstTimeZone parseTimeZoneDefinition(const QByteArray& raw);

/// The instant a wall-clock time in @p zone names. @p wall is held in a UTC QDateTime so no other
/// zone's rules touch it; the result is a true UTC time. An unknown zone returns it unchanged.
[[nodiscard]] QDateTime zoneWallClockToUtc(const PstTimeZone& zone, const QDateTime& wall);

/// The wall-clock time @p utc falls on in @p zone, held in a UTC QDateTime.
[[nodiscard]] QDateTime zoneUtcToWallClock(const PstTimeZone& zone, const QDateTime& utc);

/// The date an all-day event's start or end falls on. Outlook stores those as midnight where the
/// event was made, in UTC, so on a PC east of UTC the UTC date is the day before. Rounds to the
/// nearest midnight, in the event's own zone when @p zone is known and otherwise from UTC itself,
/// which is right for any zone from 11 hours behind UTC to 12 ahead.
[[nodiscard]] QDate allDayDate(const PstTimeZone& zone, const QDateTime& utc);

/// Read an AppointmentRecur blob. Never fails: what could not be read comes back Unreadable, with
/// the reason in plain words.
[[nodiscard]] PstRecurrence parseAppointmentRecurrence(const QByteArray& blob,
                                                       const PstTimeZone& zone,
                                                       bool all_day);

}  // namespace sak::pst
