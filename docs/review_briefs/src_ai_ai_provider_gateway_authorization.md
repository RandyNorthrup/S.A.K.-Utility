- **Medium** - Every `requires_confirmation` call is labeled "Browser input action," misrepresenting desktop input, clipboard, extension, and unclassified tools. [src/ai/ai_provider_gateway_authorization.cpp:25](src/ai/ai_provider_gateway_authorization.cpp:25)
- **Low** - `toolError`, `assisted`, and `unattended` duplicate identical helpers in `ai_provider_gateway_tool_runner.cpp`. [src/ai/ai_provider_gateway_authorization.cpp:10](src/ai/ai_provider_gateway_authorization.cpp:10)

Wave 30 resolution: both findings are closed. Confirmation uses the neutral Win32 MCP action label;
`toolError` is shared, and the access predicates are one enum-owned constexpr implementation used
by authorization and execution.
