// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

/// @file native_messaging.h
/// @brief Chrome Native Messaging wire framing for the browser-control bridge.
///
/// Chrome talks to a native messaging host over the host's stdio using a simple
/// framing: each message is a 32-bit length in NATIVE byte order (little-endian on
/// the x86/x64 Windows targets) followed by that many bytes of UTF-8 JSON. This is
/// a different framing from the newline-delimited MCP JSON-RPC the same executable
/// speaks to the app; keeping the codec here, pure and header-light, lets it be
/// unit-tested with byte buffers and reused by the host loop.
namespace sak::win32mcp {

/// Hard upper bound on a single decoded frame. Chrome caps host->browser messages
/// at 1 MiB, but browser->host messages (which may later carry a screenshot) can be
/// far larger, so the decoder guard is generous; it exists only to stop a corrupt
/// or hostile length prefix from triggering a multi-gigabyte allocation.
inline constexpr int kMaxNativeMessageBytes = 64 * 1024 * 1024;

/// Version of the browser-control bridge protocol spoken over native messaging.
/// Bump when the message shapes change so both ends can detect a mismatch.
inline constexpr int kBrowserBridgeProtocol = 1;

/// One attempt to decode a frame from the front of a byte buffer.
struct NativeFrame {
    enum class Status {
        Ok,        ///< A complete, valid frame was decoded into `message`.
        NeedMore,  ///< The buffer does not yet hold a full frame; read more bytes.
        Error      ///< The frame is malformed (bad length or non-object JSON).
    };

    Status status{Status::NeedMore};
    QJsonObject message;  ///< Decoded object (valid only when status == Ok).
    int consumed{0};      ///< Bytes to drop from the buffer once handled (Ok only).
    QString error;        ///< Human-readable reason when status == Error.
};

/// Encode one JSON object as a native-messaging frame: 4-byte little-endian length
/// prefix followed by the compact UTF-8 JSON body.
[[nodiscard]] QByteArray encodeFrame(const QJsonObject& message);

/// Try to decode one frame from the front of @p buffer. Returns NeedMore when the
/// buffer is short of a full frame (caller reads more and retries), Error on a bad
/// length or non-object body (caller should treat the stream as unrecoverable), or
/// Ok with the message and the byte count to erase from the buffer.
[[nodiscard]] NativeFrame parseFrame(const QByteArray& buffer);

}  // namespace sak::win32mcp
