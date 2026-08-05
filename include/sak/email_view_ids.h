// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_view_ids.h
/// @brief Fail-closed decoding of the folder/message ids that the email
///        inspector stores on its view items.

#pragma once

#include <QMetaType>
#include <QVariant>

#include <cstdint>
#include <optional>

namespace sak {

/// Decode a folder or message id previously stored in a view item's
/// `Qt::UserRole`.
///
/// Fails closed. An unset role, a value of any non-integral type, or a negative
/// number yields nullopt rather than an id. Absence is never reported as 0,
/// because 0 is a legitimate id here: it is the MBOX root folder, and it is the
/// message index of the first message in an MBOX file. A negative value is
/// refused outright rather than widened, since widening turns it into a huge
/// unsigned id that names an arbitrary node.
///
/// Callers must treat nullopt as "this item cannot be acted on" and surface the
/// refusal, never as "use whatever was selected before".
[[nodiscard]] inline std::optional<uint64_t> decodeEmailViewId(const QVariant& role_value) {
    const int type_id = role_value.typeId();
    const bool is_unsigned = (type_id == QMetaType::UInt || type_id == QMetaType::ULongLong);
    const bool is_signed = (type_id == QMetaType::Int || type_id == QMetaType::LongLong);
    if (!is_unsigned && !is_signed) {
        // A string, a double, an object or an unset role: refused rather than
        // coerced, because a rounded or defaulted id names a different item than
        // the one the user pointed at.
        return std::nullopt;
    }
    if (is_signed && role_value.toLongLong() < 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(role_value.toULongLong());
}

}  // namespace sak
