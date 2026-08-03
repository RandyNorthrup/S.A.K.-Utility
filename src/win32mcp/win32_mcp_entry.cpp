// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// The win32 MCP server. Speaks MCP JSON-RPC 2.0 over stdio (one compact JSON object per
// line), the same transport the S.A.K. Utility client (AiMcpStdioSession /
// AiMcpSessionPool) already drives. Runs fully synchronously with no Qt event loop: read
// a line, route it, write the reply. Folded into the app binary: see win32_mcp_entry.h;
// the app's main() dispatches here before GUI startup.

#include "sak/win32mcp/win32_mcp_entry.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/win32mcp/browser_bridge_relay.h"
#include "sak/win32mcp/browser_control.h"
#include "sak/win32mcp/win32_mcp_dispatch.h"
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

namespace sak::win32mcp {
namespace {

// The provider gateway sets this in the spawned server's environment (providers.json).
constexpr char kWin32McpModeEnv[] = "SAK_WIN32_MCP_MODE";

// A single MCP request is one compact JSON object on its own line; a legitimate one is small. Cap
// the read so a peer that never sends a newline cannot force unbounded memory growth on this
// process. 4 MiB comfortably clears any real tool call while bounding a hostile/runaway stream.
constexpr std::size_t kMaxRequestLineBytes = 4u * 1024u * 1024u;

enum class LineRead {
    Ok,
    Eof,
    TooLong
};

// Read one newline-delimited line, failing closed (TooLong) once the ceiling is crossed instead of
// letting std::getline grow the string without bound. A trailing unterminated line at EOF is
// returned once as Ok, then Eof.
LineRead readBoundedLine(std::istream& in, std::string& line) {
    line.clear();
    char ch = 0;
    while (in.get(ch)) {
        if (ch == '\n') {
            return LineRead::Ok;
        }
        line.push_back(ch);
        if (line.size() > kMaxRequestLineBytes) {
            return LineRead::TooLong;
        }
    }
    return line.empty() ? LineRead::Eof : LineRead::Ok;
}

// Write one response line, returning false if the write or flush failed -- a lost response means
// the client can no longer be trusted to have received a mutating command's result, so the caller
// stops serving rather than silently continuing (and eventually exiting 0) with a dropped ack.
bool writeResponse(const QJsonObject& response) {
    const QByteArray out = sak::ai::mcp::jsonLine(response);
    const size_t want = static_cast<size_t>(out.size());
    return std::fwrite(out.constData(), 1, want, stdout) == want && std::fflush(stdout) == 0;
}

// Serve newline-delimited JSON-RPC until EOF, or until a line exceeds the size ceiling -- at which
// point we stop serving (the stream is desynchronized and cannot be trusted to re-frame).
void serveRequests(BrowserControl* browser_ptr, const Win32McpServerPolicy& policy) {
    std::string line;
    while (true) {
        const LineRead status = readBoundedLine(std::cin, line);
        if (status == LineRead::Eof) {
            break;
        }
        if (status == LineRead::TooLong) {
            std::fprintf(stderr,
                         "win32 mcp: request line exceeded %zu bytes; closing stream\n",
                         kMaxRequestLineBytes);
            break;
        }
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
        const std::optional<QJsonObject> response = handleRequest(request, browser_ptr, policy);
        if (response.has_value() && !writeResponse(response.value())) {
            std::fprintf(stderr, "win32 mcp: response write failed; closing stream\n");
            break;
        }
    }
}

// Chrome launches a native messaging host with the calling extension's origin
// (chrome-extension://<id>/) as an argument -- we never get to add our own flag to
// the manifest's `path`, so OUR extension's origin is the signal to run as the browser-control
// relay. The explicit --browser-relay flag is accepted too, for local testing.
bool wantsRelayMode(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--browser-relay") == 0 || isOurExtensionOrigin(argv[i])) {
            return true;
        }
    }
    return false;
}

// MCP server mode is selected by the provider gateway's spawned child. Require BOTH the
// exact env token AND a redirected-pipe stdin (QProcess always connects one). This
// guards against a stray SAK_WIN32_MCP_MODE in the user's persistent environment silently
// launching the GUI-subsystem app headless (EOF on stdin -> exit 0, no window, no error):
// a normal double-click has a console or no stdin, never a pipe.
bool wantsMcpServerMode() {
    if (qEnvironmentVariable(kWin32McpModeEnv) != QLatin1String("1")) {
        return false;
    }
    return GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_PIPE;
}

}  // namespace

bool isWin32McpHelperInvocation(int argc, char** argv) {
    return wantsRelayMode(argc, argv) || wantsMcpServerMode();
}

int runWin32McpProcess(int argc, char** argv) {
    // Per-monitor-v2 DPI awareness BEFORE any window/monitor query or input injection: without it
    // the OS virtualizes coordinates on scaled/multi-monitor hosts, so GetWindowRect bounds are
    // wrong and any future click/drag would land in the wrong place. Best-effort (older OSes lack
    // the API); the manifest is not used since this is a console child process.
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE) {
        // Best-effort: on a modern OS this succeeds; surface a failure so a virtualized-coordinate
        // session is diagnosable rather than silently wrong. (Older OSes lack the API entirely and
        // compile this branch out.)
        std::fprintf(stderr, "win32 mcp: could not set per-monitor DPI awareness\n");
    }
#endif

    // Binary stdio so message framing is byte-exact in both directions (newline for MCP JSON-RPC,
    // length-prefixed for native messaging). A failure here would corrupt every frame, so fail
    // closed rather than serve on a text-translated stream.
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::fprintf(stderr, "win32 mcp: could not set binary stdio mode; aborting\n");
        return 1;
    }

    // Relay mode (launched by Chrome for the browser-control extension): pump
    // length-prefixed native-messaging frames between Chrome and the bridge pipe of
    // the long-lived MCP process, instead of serving newline-delimited JSON-RPC.
    if (wantsRelayMode(argc, argv)) {
        return sak::win32mcp::runBrowserRelay();
    }

    // MCP server mode: own the browser authority (pipe server + session) so a browser_*
    // tool call drives the live extension through the relay. If the pipe fails to start
    // (e.g. squatted), keep serving the built-in win32 tools with browser control off.
    sak::win32mcp::BrowserControl browser;
    QString browser_error;
    const bool browser_ready = browser.start(&browser_error);
    if (!browser_ready) {
        std::fprintf(stderr,
                     "browser control unavailable: %s\n",
                     browser_error.toUtf8().constData());
    }
    sak::win32mcp::BrowserControl* const browser_ptr = browser_ready ? &browser : nullptr;

    // Enforce the security profile / output redaction the provider gateway set in this process's
    // environment. The gateway pools a distinct process per profile, so reading it once here
    // holds for the process lifetime.
    const sak::win32mcp::Win32McpServerPolicy policy =
        sak::win32mcp::Win32McpServerPolicy::fromEnvironment();

    serveRequests(browser_ptr, policy);
    browser.stop();
    return 0;
}

}  // namespace sak::win32mcp
