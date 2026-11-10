// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

/// @file win32_mcp_native_host.h
/// @brief Native-messaging host mode for the win32 MCP executable.
///
/// When the same executable is launched by Chrome as a native messaging host (the
/// Chrome-passed `chrome-extension://<id>/` origin argument selects this mode, or
/// the explicit `--native-host` flag for testing) it runs a length-prefixed
/// stdio loop instead of the MCP JSON-RPC loop, bridging the S.A.K. browser-control
/// extension to the assistant. This unit is the scaffold + handshake: it answers a
/// `ping` with server identity so the extension can confirm the host is alive. The
/// message dispatcher is split out as a pure function so it is unit-testable with
/// no process I/O.
namespace sak::win32mcp {

/// Handle one decoded native-messaging request and produce the reply object.
/// Pure: no stdio, no globals. A `ping` yields a `pong` carrying the server name,
/// version, protocol, and pid; any other type yields a typed error the extension
/// can log and recover from. A present `id` is echoed back for correlation.
[[nodiscard]] QJsonObject handleNativeMessage(const QJsonObject& request,
                                              const QString& server_name,
                                              const QString& server_version);

/// Run the blocking native-messaging host loop over stdio until the peer closes
/// the pipe. Returns 0 on a clean end-of-stream, non-zero if the framing desyncs
/// (an out-of-range length or a truncated body), which Chrome recovers from by
/// relaunching the host on the next connection. Assumes stdin/stdout are already
/// in binary mode.
[[nodiscard]] int runNativeHostLoop(const QString& server_name, const QString& server_version);

}  // namespace sak::win32mcp
