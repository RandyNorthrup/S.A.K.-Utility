// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file reused_line_buffer.h
/// @brief Reads a file line by line through one buffer.

#pragma once

#include <QByteArray>
#include <QIODevice>

namespace sak {

/// A buffer for readLineIntoBuffer that can hold a line of @p longest_line bytes. readLine keeps
/// one byte for its terminator. Its pages are not touched until a line needs them.
[[nodiscard]] inline QByteArray makeLineBuffer(qint64 longest_line) {
    return QByteArray(static_cast<qsizetype>(longest_line) + 1, Qt::Uninitialized);
}

/// The next line of @p device, read into @p buffer, which the caller keeps between calls. A line
/// longer than the buffer comes back in pieces, and only one that ends in a newline ends the line.
/// Empty at the end of the device and on a read error.
///
/// QIODevice::readLine(max) allocates all of max for every line, which took minutes on a large
/// mailbox. The result borrows @p buffer, so it is good until the next call.
[[nodiscard]] inline QByteArray readLineIntoBuffer(QIODevice& device, QByteArray& buffer) {
    const qint64 length = device.readLine(buffer.data(), buffer.size());
    return length > 0 ? QByteArray::fromRawData(buffer.constData(), static_cast<qsizetype>(length))
                      : QByteArray();
}

}  // namespace sak
