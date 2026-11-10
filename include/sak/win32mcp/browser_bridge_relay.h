// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/// @file browser_bridge_relay.h
/// @brief The browser-control bridge RELAY, hosted in the Chrome-spawned native
/// messaging process (the client end of the bridge pipe).
///
/// Topology (settled by the design panel): the long-lived MCP process owns the
/// browser authority (BrowserBridgeSession + BrowserBridgePipeServer); THIS process
/// is a thin, id-transparent relay. Chrome launches it as a native messaging host,
/// so it cannot receive the pipe name on argv -- it discovers the server through the
/// rendezvous record, connects to the hardened pipe as an IDENTIFICATION-level client,
/// verifies the SERVER by code identity (the record's pid must resolve to our own
/// executable, so a squatted record cannot redirect us to a foreign process), and
/// completes the token/protocol handshake. Thereafter it pumps whole native-messaging
/// frames between Chrome's stdio and the pipe with no parsing or correlation: a command
/// from the server goes to the extension over stdout, and the extension's reply comes
/// back over stdin to the server. All correlation, id minting, and ref_index state live
/// in the MCP process, so the relay can die and be relaunched by Chrome at any time
/// without losing browser state.
///
/// This phase is strictly request/reply (the extension speaks only in response to a
/// forwarded command), which is why a single-threaded alternating pump is correct and
/// matches the server's one-op-outstanding read-after-write model. An event channel
/// (for unsolicited detach/navigation notices) is a later unit.
namespace sak::win32mcp {

/// Read one whole native-messaging frame (a JSON object) from the browser side, or
/// return false at end-of-stream / on a framing error. Injected so the pump is
/// testable without real stdio.
using BrowserReadFn = std::function<bool(QJsonObject*)>;

/// Write one whole native-messaging frame (a JSON object) to the browser side, or
/// return false if the sink is closed.
using BrowserWriteFn = std::function<bool(const QJsonObject&)>;

/// Connect to the bridge server described by the rendezvous record at @p
/// rendezvous_path: read the record, verify the recorded pid resolves to OUR OWN
/// executable image (fail closed on a foreign or missing process, so a rewritten
/// record cannot redirect the relay), then open the pipe as an IDENTIFICATION-level
/// client. On success returns a valid handle and fills @p token_out / @p protocol_out
/// for the handshake; on failure returns INVALID_HANDLE_VALUE with @p error set. The
/// caller owns the returned handle and must CloseHandle it.
[[nodiscard]] HANDLE relayConnect(const QString& rendezvous_path,
                                  QString* token_out,
                                  int* protocol_out,
                                  QString* error);

/// Perform the client side of the hello/welcome handshake on an already-connected
/// pipe @p pipe: send `{type:hello, token, protocol}` and require a `welcome` reply
/// carrying the matching protocol. Returns false (with @p error set) if the server
/// rejects the token/protocol or drops the connection.
[[nodiscard]] bool relayHandshake(HANDLE pipe, const QString& token, int protocol, QString* error);

/// Pump exactly one command/reply exchange: block for a command frame from @p pipe
/// (sent by the server), forward it to the browser via @p browser_write, block for
/// the browser's reply via @p browser_read, and forward that reply back to @p pipe.
/// Returns false when either side closes (the caller stops the relay), true to
/// continue. Id-transparent: frames are moved verbatim, never parsed or correlated.
[[nodiscard]] bool relayPumpOnce(HANDLE pipe,
                                 const BrowserReadFn& browser_read,
                                 const BrowserWriteFn& browser_write);

/// Blocking whole-frame read/write helpers for a synchronous (non-overlapped) client
/// pipe handle. Exposed for the relay pump and its tests; each reads/writes the 4-byte
/// little-endian length prefix plus body, guarding an out-of-range prefix.
[[nodiscard]] bool relayReadPipeFrame(HANDLE pipe, QJsonObject* out);
[[nodiscard]] bool relayWritePipeFrame(HANDLE pipe, const QJsonObject& message);

/// Run the full relay against Chrome's stdio: discover + connect + verify + handshake,
/// announce readiness to the extension with a single `bridge_ready` frame (or a
/// `bridge_unavailable` frame then exit if the bridge is down), then loop relayPumpOnce
/// until either side closes. Returns a process exit code (0 on a clean shutdown).
/// Assumes stdin/stdout are already in binary mode.
[[nodiscard]] int runBrowserRelay();

}  // namespace sak::win32mcp
