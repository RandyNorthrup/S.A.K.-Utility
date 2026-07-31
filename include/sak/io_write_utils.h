// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file io_write_utils.h
/// @brief Small helper for complete, short-write-safe I/O.

#pragma once

#include <QByteArray>
#include <QIODevice>

namespace sak {

/// Write every byte of @p data to @p device, looping over partial writes.
///
/// QIODevice::write() may accept fewer bytes than requested without signalling
/// an error, so a single call whose return value is only checked for `< 0` can
/// silently truncate a report, message, or attachment. This loops until all
/// bytes are written or a write makes no progress. Returns true iff the entire
/// payload reached the device; an empty payload is a successful no-op.
[[nodiscard]] inline bool writeFully(QIODevice& device, const QByteArray& data) {
    qint64 total = 0;
    const qint64 size = data.size();
    while (total < size) {
        const qint64 n = device.write(data.constData() + total, size - total);
        if (n <= 0) {
            return false;  // error, or a stalled write that would spin forever
        }
        total += n;
    }
    return true;
}

}  // namespace sak
