// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonObject>

#include <optional>

/// @file win32_mcp_dispatch.h
/// @brief JSON-RPC 2.0 request routing for the native win32 MCP server, kept
/// separate from the stdio loop so it can be unit-tested by feeding request
/// objects and inspecting response objects with no process I/O.
namespace sak::win32mcp {

class BrowserControl;

/// Server-side enforcement of the security controls the provider gateway sets in the spawned
/// server's environment (WIN32_MCP_SECURITY_PROFILE, WIN32_MCP_REDACT_SENSITIVE_OUTPUT). The
/// gateway pools a distinct process per security profile, so a value read once at startup holds
/// for the whole process lifetime. Defaults are permissive/no-redaction so a server started
/// outside the gateway (and every pure unit test) behaves exactly as before.
struct Win32McpServerPolicy {
    // WIN32_MCP_SECURITY_PROFILE == "read_only": tools/list advertises only read-only tools and
    // tools/call refuses any non-read-only tool, so a read-only session cannot invoke a mutating
    // or input tool even if the client's own policy gate were bypassed.
    bool read_only_profile{false};
    // WIN32_MCP_REDACT_SENSITIVE_OUTPUT == "true": mask obvious secrets in tool result text
    // before it leaves the process, so raw passwords/tokens never reach the model.
    bool redact_sensitive_output{false};

    [[nodiscard]] static Win32McpServerPolicy fromEnvironment();
};

/// Whether a tool is read-only (no mutation, no input injection, no process control). Mirrors
/// the client-side AiProviderGateway::isWin32ReadOnlyTool; the server enforces it independently
/// so it never trusts the client. KEEP THE TWO LISTS IN SYNC when adding a read-only tool.
[[nodiscard]] bool win32McpToolIsReadOnly(const QString& tool_name);

/// Mask obvious secrets (password/token/secret/api key/bearer assignments) in @p text so raw
/// sensitive output never reaches the model. Exposed for unit testing.
[[nodiscard]] QString redactWin32McpSensitiveText(const QString& text);

/// Validate tool-call @p args against a native tool's advertised @p input_schema: returns an
/// empty string when the args conform, else a fail-closed rejection reason (missing required
/// argument, unknown key under additionalProperties:false, or a wrong-typed value). The server
/// enforces this independently of the client. Exposed for unit testing.
[[nodiscard]] QString win32McpValidateArgsAgainstSchema(const QJsonObject& input_schema,
                                                        const QJsonObject& args);

/// Shape a ToolResult into the MCP `tools/call` result object. A screenshot (image
/// present) becomes an `image` content block plus an optional text summary; a text
/// result stays a single `text` block. Exposed so the content shaping -- including the
/// image branch -- is unit-testable without a live browser.
[[nodiscard]] QJsonObject toolCallResult(const ToolResult& result);

/// Handle one parsed JSON-RPC request. Returns the response object to write back,
/// or std::nullopt for notifications (no `id`, e.g. notifications/initialized)
/// which the protocol says must not be answered.
///
/// When @p browser is non-null, its `browser_*` tools are merged into `tools/list`
/// and a matching `tools/call` is routed to it (live browser control). When null
/// (the default, and every pure unit test), only the built-in win32 tools are
/// advertised and a browser_* call falls through to the unknown-tool error.
[[nodiscard]] std::optional<QJsonObject> handleRequest(const QJsonObject& request,
                                                       BrowserControl* browser = nullptr,
                                                       const Win32McpServerPolicy& policy = {});

}  // namespace sak::win32mcp
