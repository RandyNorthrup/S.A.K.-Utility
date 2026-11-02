// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_named_properties.cpp
/// @brief MS-PST Name-to-ID map parsing and named-property application.

#include "sak/pst_named_properties.h"

#include <QtEndian>

#include <array>
#include <cstring>

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
constexpr uint16_t kPropertySetTagBytes = 2;

// The sets are all {000620xx-0000-0000-C000-000000000046}. On disk a GUID's first DWORD is
// little-endian, so xx is byte 0.
constexpr uint8_t kSetByteAppointment = 0x02;
constexpr uint8_t kSetByteTask = 0x03;
constexpr uint8_t kSetByteAddress = 0x04;
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
    case kSetByteNote:
        return NamedPropertySet::Note;
    default:
        return NamedPropertySet::Other;
    }
}

QDateTime parseUtcValue(const sak::MapiProperty& prop) {
    return QDateTime::fromString(prop.display_value, Qt::ISODate);
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

void applyAddress(sak::PstItemDetail& detail, uint32_t lid, const sak::MapiProperty& prop) {
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
        if (prop_index > 0xFFFF - kFirstNamedPropertyId) {
            continue;
        }
        const auto prop_id = static_cast<uint16_t>(kFirstNamedPropertyId + prop_index);
        map.insert(prop_id,
                   NamedPropertyKey{.set = set, .long_id = qFromLittleEndian<uint32_t>(entry)});
    }
    return map;
}

void applyNamedProperty(sak::PstItemDetail& detail,
                        const NamedPropertyKey& key,
                        const sak::MapiProperty& prop) {
    switch (key.set) {
    case NamedPropertySet::Appointment:
        applyAppointment(detail, key.long_id, prop);
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

}  // namespace sak::pst
