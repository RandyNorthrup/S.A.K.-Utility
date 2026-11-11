// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file bounded_inflate.h
/// @brief Inflate a zlib stream whose uncompressed size is declared beside it, never past it.
///
/// qUncompress takes the size prefix only as an allocation hint: it grows its buffer until the
/// stream ends, so a small crafted stream that declares a few KiB can inflate to gigabytes before
/// any caller sees the size. The APFS and HFS+ decmpfs decoders and the PST 4K block reader each
/// checked the size only after that. This decodes into a buffer one byte longer than declared and
/// stops there, so a stream that runs past its declared size is refused having produced at most
/// that one extra byte.

#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include <optional>

namespace sak {

/// @brief Inflate the zlib stream @p stream to exactly @p expected_bytes bytes.
/// @return The bytes, or nullopt when the stream is not valid zlib, ends short, would produce more
///         than @p expected_bytes, or @p expected_bytes is negative or too large for one zlib call.
///         Bytes after the end of the zlib stream are ignored, as qUncompress ignores them.
[[nodiscard]] std::optional<QByteArray> inflateZlibExactly(QByteArrayView stream,
                                                           qsizetype expected_bytes);

}  // namespace sak
