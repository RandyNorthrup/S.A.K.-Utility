// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Entry point for the native win32 MCP server. Speaks MCP JSON-RPC 2.0 over stdio
// (one compact JSON object per line), the same transport the S.A.K. Utility client
// (AiMcpStdioSession / AiMcpSessionPool) already drives. Runs fully synchronously
// with no Qt event loop: read a line, route it, write the reply.

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/win32mcp/win32_mcp_dispatch.h"
#include "sak/win32mcp/win32_mcp_native_host.h"
#include "sak/win32mcp/win32_mcp_tools.h"

#include <QByteArray>
#include <QJsonObject>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <io.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

void writeResponse(const QJsonObject& response) {
    const QByteArray out = sak::ai::mcp::jsonLine(response);
    std::fwrite(out.constData(), 1, static_cast<size_t>(out.size()), stdout);
    std::fflush(stdout);
}

// Chrome launches a native messaging host with the calling extension's origin
// (chrome-extension://<id>/) as an argument -- we never get to add our own flag to
// the manifest's `path`, so that origin is the signal to run in host mode. The
// explicit --native-host flag is accepted too, for local testing.
bool wantsNativeHostMode(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--native-host") == 0 ||
            std::strncmp(argv[i], "chrome-extension://", 19) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    // Per-monitor-v2 DPI awareness BEFORE any window/monitor query or input injection: without it
    // the OS virtualizes coordinates on scaled/multi-monitor hosts, so GetWindowRect bounds are
    // wrong and any future click/drag would land in the wrong place. Best-effort (older OSes lack
    // the API); the manifest is not used since this is a console child process.
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    // Binary stdio so message framing is byte-exact in both directions (newline for
    // MCP JSON-RPC, length-prefixed for native messaging).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // Native messaging host mode (launched by Chrome for the browser-control
    // extension) speaks length-prefixed frames, not newline-delimited JSON-RPC.
    if (wantsNativeHostMode(argc, argv)) {
        return sak::win32mcp::runNativeHostLoop(sak::win32mcp::serverName(),
                                                sak::win32mcp::serverVersion());
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        QString parse_error;
        const QJsonObject request = sak::ai::mcp::parseJsonLine(QByteArray::fromStdString(line),
                                                                &parse_error);
        if (!parse_error.isEmpty()) {
            // A malformed line carries no reliable id to answer against; drop it and
            // keep serving rather than desynchronizing the stream.
            continue;
        }
        const std::optional<QJsonObject> response = sak::win32mcp::handleRequest(request);
        if (response.has_value()) {
            writeResponse(response.value());
        }
    }
    return 0;
}
