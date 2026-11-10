// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_ics_writer.h
/// @brief Writes calendar items as an RFC 5545 iCalendar file.
///
/// The one writer behind both ICS exports: the whole calendar and the single event in the Calendar
/// window's right-click menu. Those had grown apart, and only one of them wrote a UID.

#pragma once

#include "sak/email_types.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

namespace sak {

/// The iCalendar text for @p events, each stamped with @p stamp as its DTSTAMP. Lines end in CRLF
/// and are folded at 75 octets. A repeating event carries its rule in the zone it was made in, with
/// EXDATE for deleted occurrences and a RECURRENCE-ID event for each changed one.
[[nodiscard]] QByteArray buildIcsCalendar(const QVector<PstItemDetail>& events,
                                          const QDateTime& stamp);

/// What @p event's export cannot carry exactly, in words for the export log. Empty when it is
/// whole.
[[nodiscard]] QStringList icsEventNotes(const PstItemDetail& event);

/// An attendee's reply in plain words for the Calendar window, or empty when there is none to show.
[[nodiscard]] QString attendeeResponseWord(int response);

}  // namespace sak
