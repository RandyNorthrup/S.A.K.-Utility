// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_named_properties.h
/// @brief MS-PST Name-to-ID map (2.4.7) and the named MAPI properties the app reads.
///
/// An Outlook store keeps most calendar, task and contact fields in NAMED properties: the property
/// id on disk (0x8000 and up) differs per store, and only the store's Name-to-ID map says which
/// id means "appointment start". Read as plain tags they are invisible, which left every calendar
/// event without a start, end or location, every task "Not Started", and every contact without an
/// email address.

#pragma once

#include "sak/email_types.h"

#include <QByteArray>
#include <QHash>

#include <cstdint>

namespace sak::pst {

/// The named-property sets the app understands. Anything else is Other and is ignored.
enum class NamedPropertySet : uint8_t {
    Other,
    Appointment,  ///< PSETID_Appointment
    Task,         ///< PSETID_Task
    Address,      ///< PSETID_Address
    Note,         ///< PSETID_Note
};

/// What a store's property id 0x8000 + N names.
struct NamedPropertyKey {
    NamedPropertySet set = NamedPropertySet::Other;
    uint32_t long_id = 0;
};

/// First property id that is a named property (MS-PST 2.4.7: NPID = 0x8000 + wPropIdx).
inline constexpr uint16_t kFirstNamedPropertyId = 0x8000;

/// Name-to-ID map property tags, on NID_NAME_TO_ID_MAP.
inline constexpr uint16_t kPropIdNameidStreamGuid = 0x0002;
inline constexpr uint16_t kPropIdNameidStreamEntry = 0x0003;

/// Named-property long ids the app reads. The values are from MS-OXPROPS.
inline constexpr uint32_t kLidTaskStatus = 0x8101;
inline constexpr uint32_t kLidTaskPercentComplete = 0x8102;
inline constexpr uint32_t kLidTaskStartDate = 0x8104;
inline constexpr uint32_t kLidTaskDueDate = 0x8105;
inline constexpr uint32_t kLidAppointmentBusyStatus = 0x8205;
inline constexpr uint32_t kLidAppointmentLocation = 0x8208;
inline constexpr uint32_t kLidAppointmentStartWhole = 0x820D;
inline constexpr uint32_t kLidAppointmentEndWhole = 0x820E;
inline constexpr uint32_t kLidAppointmentSubType = 0x8215;
inline constexpr uint32_t kLidRecurrencePattern = 0x8232;
inline constexpr uint32_t kLidEmail1DisplayName = 0x8080;
inline constexpr uint32_t kLidEmail1EmailAddress = 0x8083;
inline constexpr uint32_t kLidEmail2EmailAddress = 0x8093;
inline constexpr uint32_t kLidEmail3EmailAddress = 0x80A3;
inline constexpr uint32_t kLidNoteColor = 0x8B00;

/// Build property id -> name from a store's GUID and entry streams (the raw bytes of
/// PidTagNameidStreamGuid and PidTagNameidStreamEntry). An entry naming a string instead of a long
/// id, or a set the app does not read, is left out. A malformed stream yields what parsed cleanly:
/// a missing mapping only hides that one field, it cannot misname another.
[[nodiscard]] QHash<uint16_t, NamedPropertyKey> parseNameIdMap(const QByteArray& guid_stream,
                                                               const QByteArray& entry_stream);

/// Apply one named property to @p detail, if it is one the app reads.
void applyNamedProperty(sak::PstItemDetail& detail,
                        const NamedPropertyKey& key,
                        const sak::MapiProperty& prop);

}  // namespace sak::pst
