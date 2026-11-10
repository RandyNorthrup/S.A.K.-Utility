// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file compress_fixture.h
/// @brief Real gzip, bzip2 and xz streams for tests, built with the same libraries the app decodes
///        them with. A test linking this also links ZLIB::ZLIB, BZip2::BZip2 and LibLZMA::LibLZMA.

#include <QByteArray>

#include <array>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>

namespace sak_test {

[[nodiscard]] inline QByteArray gzipCompress(const QByteArray& input) {
    z_stream zs{};
    // windowBits 15 + 16 selects the gzip wrapper.
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        return {};
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    zs.avail_in = static_cast<uInt>(input.size());

    QByteArray out;
    std::array<char, 16'384> buffer{};
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buffer.data());
        zs.avail_out = static_cast<uInt>(buffer.size());
        ret = deflate(&zs, Z_FINISH);
        out.append(buffer.data(), static_cast<qsizetype>(buffer.size() - zs.avail_out));
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return ret == Z_STREAM_END ? out : QByteArray();
}

[[nodiscard]] inline QByteArray bzip2Compress(const QByteArray& input) {
    // libbz2's documented one-shot output-buffer floor: sourceLen + sourceLen/100 + 600.
    auto destLen = static_cast<unsigned int>(input.size() + input.size() / 100 + 600);
    QByteArray out(static_cast<qsizetype>(destLen), Qt::Uninitialized);
    const int ret = BZ2_bzBuffToBuffCompress(out.data(),
                                             &destLen,
                                             const_cast<char*>(input.constData()),
                                             static_cast<unsigned int>(input.size()),
                                             9,
                                             0,
                                             0);
    if (ret != BZ_OK) {
        return {};
    }
    out.truncate(static_cast<qsizetype>(destLen));
    return out;
}

[[nodiscard]] inline QByteArray xzCompress(const QByteArray& input) {
    const size_t bound = lzma_stream_buffer_bound(static_cast<size_t>(input.size()));
    QByteArray out(static_cast<qsizetype>(bound), Qt::Uninitialized);
    size_t out_pos = 0;
    const lzma_ret ret =
        lzma_easy_buffer_encode(LZMA_PRESET_DEFAULT,
                                LZMA_CHECK_CRC64,
                                nullptr,
                                reinterpret_cast<const uint8_t*>(input.constData()),
                                static_cast<size_t>(input.size()),
                                reinterpret_cast<uint8_t*>(out.data()),
                                &out_pos,
                                bound);
    if (ret != LZMA_OK) {
        return {};
    }
    out.truncate(static_cast<qsizetype>(out_pos));
    return out;
}

}  // namespace sak_test
