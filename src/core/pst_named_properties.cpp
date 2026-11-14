// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_named_properties.cpp
/// @brief MS-PST Name-to-ID map parsing and named-property application.

#include "sak/pst_named_properties.h"

#include "sak/pst_recurrence.h"

#include <QByteArrayView>
#include <QStringList>
#include <QtEndian>

#include <array>
#include <cstring>
#include <limits>

namespace sak::pst {

namespace {

constexpr int kGuidBytes = 16;
constexpr int kNameIdEntryBytes = 8;
constexpr int kEntryGuidFieldOffset = 4;
constexpr int kEntryIndexFieldOffset = 6;
// wGuid values 0-2 are reserved (none, PS_MAPI, PS_PUBLIC_STRINGS); stream GUIDs start at 3.
constexpr int kFirstStreamGuidIndex = 3;
constexpr uint16_t kEntryIsStringBit = 0x0001;
constexpr int kEntryGuidIndexShift = 1;
constexpr int kPropertySetTagOffset = 0;

// The sets are all {000620xx-0000-0000-C000-000000000046}. On disk a GUID's first DWORD is
// little-endian, so xx is byte 0.
constexpr uint8_t kSetByteAppointment = 0x02;
constexpr uint8_t kSetByteTask = 0x03;
constexpr uint8_t kSetByteAddress = 0x04;
constexpr uint8_t kSetByteLog = 0x0A;
constexpr uint8_t kSetByteNote = 0x0E;

const std::array<uint8_t, kGuidBytes> kSetGuidTemplate = {
    0x00, 0x20, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

NamedPropertySet setFromGuid(const char* guid) {
    if (std::memcmp(guid + 1, kSetGuidTemplate.data() + 1, kGuidBytes - 1) != 0) {
        return NamedPropertySet::Other;
    }
    switch (static_cast<uint8_t>(guid[kPropertySetTagOffset])) {
    case kSetByteAppointment:
        return NamedPropertySet::Appointment;
    case kSetByteTask:
        return NamedPropertySet::Task;
    case kSetByteAddress:
        return NamedPropertySet::Address;
    case kSetByteLog:
        return NamedPropertySet::Log;
    case kSetByteNote:
        return NamedPropertySet::Note;
    default:
        return NamedPropertySet::Other;
    }
}

QDateTime parseUtcValue(const sak::MapiProperty& prop) {
    return QDateTime::fromString(prop.display_value, Qt::ISODate);
}

/// The appointment values read as a group; false when @p lid is not one of them.
bool applyAppointmentRawValue(AppointmentRawValues& raw,
                              uint32_t lid,
                              const sak::MapiProperty& prop) {
    switch (lid) {
    case kLidAppointmentRecur:
        raw.recurrence = prop.raw_value;
        return true;
    case kLidTimeZoneStruct:
        raw.zone_struct = prop.raw_value;
        return true;
    case kLidTimeZoneDescription:
        raw.zone_description = prop.display_value;
        return true;
    case kLidTimeZoneDefinitionRecur:
        raw.zone_definition_recurring = prop.raw_value;
        return true;
    case kLidTimeZoneDefinitionStart:
        raw.zone_definition_start = prop.raw_value;
        return true;
    default:
        return false;
    }
}

void applyAppointment(sak::PstItemDetail& detail, uint32_t lid, const sak::MapiProperty& prop) {
    switch (lid) {
    case kLidAppointmentStartWhole:
        detail.start_time = parseUtcValue(prop);
        break;
    case kLidAppointmentEndWhole:
        detail.end_time = parseUtcValue(prop);
        break;
    case kLidAppointmentLocation:
        detail.location = prop.display_value;
        break;
    case kLidAppointmentBusyStatus:
        detail.busy_status = prop.display_value.toInt();
        break;
    case kLidAppointmentSubType:
        detail.is_all_day = prop.display_value == QLatin1String("true");
        break;
    case kLidRecurrencePattern:
        detail.recurrence_description = prop.display_value;
        break;
    case kLidRecurring:
        detail.marked_recurring = prop.display_value == QLatin1String("true");
        break;
    default:
        break;
    }
}

void applyJournal(sak::PstItemDetail& detail, uint32_t lid, const sak::MapiProperty& prop) {
    switch (lid) {
    case kLidLogType:
        detail.journal_type = prop.display_value;
        break;
    case kLidLogStart:
        detail.start_time = parseUtcValue(prop);
        break;
    case kLidLogEnd:
        detail.end_time = parseUtcValue(prop);
        break;
    case kLidLogDuration:
        detail.journal_duration_minutes = prop.display_value.toInt();
        break;
    default:
        break;
    }
}

void applyTask(sak::PstItemDetail& detail, uint32_t lid, const sak::MapiProperty& prop) {
    switch (lid) {
    case kLidTaskStatus:
        detail.task_status = prop.display_value.toInt();
        break;
    case kLidTaskPercentComplete:
        detail.task_percent_complete = prop.display_value.toDouble();
        break;
    case kLidTaskStartDate:
        detail.task_start_date = parseUtcValue(prop);
        break;
    case kLidTaskDueDate:
        detail.task_due_date = parseUtcValue(prop);
        break;
    default:
        break;
    }
}

// MS-OXCDATA 2.2.5.1, a one-off EntryID: 4 flag bytes, the 16-byte provider UID, a 2-byte version,
// 2 bytes of flags, then the display name, address type and address, each NUL-terminated.
constexpr int kOneOffFlagsOffset = 22;
constexpr int kOneOffStringsOffset = 24;
constexpr uint16_t kOneOffUnicodeFlag = 0x8000;
constexpr int kOneOffStringCount = 3;
constexpr int kOneOffNameIndex = 0;
constexpr int kOneOffAddressIndex = 2;
constexpr int kWideCharBytes = 2;

QString oneOffString(const QByteArrayView text, bool wide) {
    return wide ? QString::fromUtf16(reinterpret_cast<const char16_t*>(text.data()),
                                     text.size() / kWideCharBytes)
                : QString::fromLatin1(text);
}

QStringList oneOffStrings(const QByteArrayView entry) {
    QStringList strings;
    if (entry.size() < kOneOffStringsOffset) {
        return strings;
    }
    const auto flags = qFromLittleEndian<uint16_t>(entry.data() + kOneOffFlagsOffset);
    const bool wide = (flags & kOneOffUnicodeFlag) != 0;
    const int unit = wide ? kWideCharBytes : 1;
    const auto endsHere = [&entry, wide](qsizetype where) {
        return entry[where] == 0 && (!wide || entry[where + 1] == 0);
    };
    qsizetype at = kOneOffStringsOffset;
    while (strings.size() < kOneOffStringCount) {
        qsizetype end = at;
        while (end + unit <= entry.size() && !endsHere(end)) {
            end += unit;
        }
        if (end + unit > entry.size()) {
            return {};  // A string with no terminator: this entry is damaged.
        }
        strings.append(oneOffString(entry.sliced(at, end - at), wide));
        at = end + unit;
    }
    return strings;
}

// MS-PST 2.3.3.4.2, a multi-valued binary: a count, one 32-bit offset per value, then the values.
QVector<sak::PstDistListMember> oneOffMembers(const QByteArray& raw) {
    constexpr int kFieldBytes = 4;
    QVector<sak::PstDistListMember> members;
    if (raw.size() < kFieldBytes) {
        return members;
    }
    const auto count = qFromLittleEndian<uint32_t>(raw.constData());
    if (count > static_cast<uint32_t>((raw.size() - kFieldBytes) / kFieldBytes)) {
        return members;
    }
    const auto offsetOf = [&raw](uint32_t index) {
        return qFromLittleEndian<uint32_t>(raw.constData() + kFieldBytes + index * kFieldBytes);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t start = offsetOf(i);
        const uint32_t end = i + 1 < count ? offsetOf(i + 1) : static_cast<uint32_t>(raw.size());
        if (start > end || end > static_cast<uint32_t>(raw.size())) {
            continue;
        }
        const QStringList strings = oneOffStrings(QByteArrayView(raw).sliced(start, end - start));
        if (strings.size() == kOneOffStringCount) {
            members.append({strings.at(kOneOffNameIndex), strings.at(kOneOffAddressIndex)});
        }
    }
    return members;
}

void applyAddress(sak::PstItemDetail& detail, uint32_t lid, const sak::MapiProperty& prop) {
    if (lid == kLidDistributionListOneOffMembers) {
        detail.distlist_members = oneOffMembers(prop.raw_value);
        return;
    }
    // A contact can carry up to three addresses. Email1 is the primary; the others only fill a
    // gap, so a contact with just a second address still shows one.
    if (lid == kLidEmail1EmailAddress) {
        detail.email_address = prop.display_value;
    } else if ((lid == kLidEmail2EmailAddress || lid == kLidEmail3EmailAddress) &&
               detail.email_address.isEmpty()) {
        detail.email_address = prop.display_value;
    }
}

}  // namespace

QHash<uint16_t, NamedPropertyKey> parseNameIdMap(const QByteArray& guid_stream,
                                                 const QByteArray& entry_stream) {
    QHash<uint16_t, NamedPropertyKey> map;
    const int entries = static_cast<int>(entry_stream.size() / kNameIdEntryBytes);
    for (int i = 0; i < entries; ++i) {
        const char* entry = entry_stream.constData() +
                            (static_cast<qsizetype>(i) * kNameIdEntryBytes);
        const uint16_t guid_field = qFromLittleEndian<uint16_t>(entry + kEntryGuidFieldOffset);
        if ((guid_field & kEntryIsStringBit) != 0) {
            continue;
        }
        const int guid_index = (guid_field >> kEntryGuidIndexShift) - kFirstStreamGuidIndex;
        const qsizetype guid_offset = static_cast<qsizetype>(guid_index) * kGuidBytes;
        if (guid_index < 0 || guid_offset + kGuidBytes > guid_stream.size()) {
            continue;
        }
        const NamedPropertySet set = setFromGuid(guid_stream.constData() + guid_offset);
        if (set == NamedPropertySet::Other) {
            continue;
        }
        const uint16_t prop_index = qFromLittleEndian<uint16_t>(entry + kEntryIndexFieldOffset);
        if (prop_index > std::numeric_limits<uint16_t>::max() - kFirstNamedPropertyId) {
            continue;
        }
        const auto prop_id = static_cast<uint16_t>(kFirstNamedPropertyId + prop_index);
        map.insert(prop_id,
                   NamedPropertyKey{.set = set, .long_id = qFromLittleEndian<uint32_t>(entry)});
    }
    return map;
}

void applyNamedProperty(sak::PstItemDetail& detail,
                        AppointmentRawValues& raw,
                        const NamedPropertyKey& key,
                        const sak::MapiProperty& prop) {
    switch (key.set) {
    case NamedPropertySet::Appointment:
        if (!applyAppointmentRawValue(raw, key.long_id, prop)) {
            applyAppointment(detail, key.long_id, prop);
        }
        break;
    case NamedPropertySet::Log:
        applyJournal(detail, key.long_id, prop);
        break;
    case NamedPropertySet::Task:
        applyTask(detail, key.long_id, prop);
        break;
    case NamedPropertySet::Address:
        applyAddress(detail, key.long_id, prop);
        break;
    case NamedPropertySet::Note:
        if (key.long_id == kLidNoteColor) {
            detail.note_color = prop.display_value.toInt();
        }
        break;
    case NamedPropertySet::Other:
        break;
    }
}

void finishAppointment(sak::PstItemDetail& detail, const AppointmentRawValues& raw) {
    // The zone the series was set up in, first; the zone its start was shown in only stands in.
    detail.time_zone = parseTimeZoneDefinition(raw.zone_definition_recurring);
    if (!detail.time_zone.known) {
        detail.time_zone = parseTimeZoneStruct(raw.zone_struct, raw.zone_description);
    }
    if (!detail.time_zone.known) {
        detail.time_zone = parseTimeZoneDefinition(raw.zone_definition_start);
    }
    if (!raw.recurrence.isEmpty()) {
        detail.recurrence =
            parseAppointmentRecurrence(raw.recurrence, detail.time_zone, detail.is_all_day);
    } else if (detail.marked_recurring) {
        // Marked as repeating with no pattern stored. It is shown as such, never as a one-off.
        detail.recurrence.state = RecurrenceState::Unreadable;
        detail.recurrence.problem = QStringLiteral("the store holds no pattern for it");
        detail.recurrence.description = QStringLiteral(
            "Repeats, but the pattern could not be read: the store holds no "
            "pattern for it");
    }
}

}  // namespace sak::pst
