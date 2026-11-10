// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sak/win32mcp/browser_extension_installer.h"  // kBrowserExtensionId

#include <cstring>
#include <string>

namespace sak::win32mcp {

// True only when @p arg is the native-messaging origin of OUR pinned extension
// (chrome-extension://<our-id>[/...]), not merely any chrome-extension:// origin. Chrome's
// allowed_origins already restricts which extension can launch the host, but validating the id
// here is defense in depth so a stray/spoofed chrome-extension:// argument from another
// extension cannot drive this process as the browser-control relay. Header-inline so it is
// unit-testable without linking the entry translation unit.
[[nodiscard]] inline bool isOurExtensionOrigin(const char* arg) {
    if (arg == nullptr) {
        return false;
    }
    static const std::string prefix = std::string("chrome-extension://") + kBrowserExtensionId;
    if (std::strncmp(arg, prefix.c_str(), prefix.size()) != 0) {
        return false;
    }
    const char after = arg[prefix.size()];
    return after == '\0' || after == '/';  // the exact 32-char id, then end-of-arg or a path
}

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
