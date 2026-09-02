- [HIGH] [include/sak/ai/ai_mcp_stdio_session.h:58](include/sak/ai/ai_mcp_stdio_session.h:58) - Sequential cross-thread calls are declared safe, but later `close()`/destruction can delete caller-affine `QThread` from another thread.

- [MEDIUM] [include/sak/ai/ai_mcp_stdio_session.h:99](include/sak/ai/ai_mcp_stdio_session.h:99) - Parser promises rejection of non-string `nextCursor`, but implementation accepts JSON `null` as successful pagination completion.

- [LOW] [include/sak/ai/ai_mcp_stdio_session.h:70](include/sak/ai/ai_mcp_stdio_session.h:70) - Fallible APIs split results across sentinel return values and mutable error outputs instead of `std::expected`; valid empty discovery and failure both return an empty vector.
