// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace sak::win32mcp {

// The win32 MCP server + Chrome browser-control host are folded into the single app
// binary rather than shipped as a second exe. The app's main() calls these before any
// GUI initialization so one executable serves three roles selected at launch:
//
//   * GUI (default): neither signal present.
//   * MCP server: spawned by the assistant's provider gateway with the SAK_WIN32_MCP_MODE
//     environment variable set (providers.json). Serves newline-delimited MCP JSON-RPC on
//     stdio and owns the browser-control bridge.
//   * Browser relay: launched by Chrome as the native messaging host, which passes the
//     calling extension's chrome-extension://<id>/ origin (or --browser-relay for tests)
//     as an argument. Pumps native-messaging frames between Chrome and the bridge pipe.

// True when this process was launched as the MCP server or the browser relay.
[[nodiscard]] bool isWin32McpHelperInvocation(int argc, char** argv);

// Run the win32 MCP helper (MCP server or browser relay, auto-detected). Sets up binary
// stdio + DPI awareness and runs fully synchronously with no Qt event loop. The caller
// (app main()) must return this value and never continue into GUI startup.
[[nodiscard]] int runWin32McpProcess(int argc, char** argv);

}  // namespace sak::win32mcp
