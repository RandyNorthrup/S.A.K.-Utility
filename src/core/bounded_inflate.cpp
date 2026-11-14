// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/bounded_inflate.h"

#include <limits>

#include <zlib.h>

namespace sak {

std::optional<QByteArray> inflateZlibExactly(QByteArrayView stream, qsizetype expected_bytes) {
    // One z_stream call takes uInt counts; the spare output byte must fit as well.
    constexpr auto kMaxZlibCount = static_cast<qsizetype>(std::numeric_limits<uInt>::max());
    if (expected_bytes < 0 || expected_bytes >= kMaxZlibCount || stream.isEmpty() ||
        stream.size() > kMaxZlibCount) {
        return std::nullopt;
    }
    // The spare byte is how an over-long stream shows itself: inflate fills it, the stream has
    // not ended, and decoding stops there instead of growing a buffer to the stream's real size.
    QByteArray out(expected_bytes + 1, Qt::Uninitialized);
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        return std::nullopt;
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(stream.data()));
    zs.avail_in = static_cast<uInt>(stream.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&zs, Z_FINISH);
    const uLong produced = zs.total_out;
    inflateEnd(&zs);
    if (result != Z_STREAM_END || produced != static_cast<uLong>(expected_bytes)) {
        return std::nullopt;
    }
    out.truncate(expected_bytes);
    return out;
}

}  // namespace sak
