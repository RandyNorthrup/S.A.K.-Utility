- **HIGH** - Provider-gateway health key uses irrelevant, unbounded caller fields while ignoring JSON-string nested arguments and `tool` alias. Same operation can evade breaker with new keys; unrelated tools can share suppression. [src/ai/ai_tool_dispatcher.cpp:54](src/ai/ai_tool_dispatcher.cpp:54)

- **HIGH** - `success:true` always records health success, even when result also carries `failure_class` or denial markers. Contradictory/hostile results bypass circuit breaker. [src/ai/ai_tool_dispatcher.cpp:39](src/ai/ai_tool_dispatcher.cpp:39) [src/ai/ai_tool_dispatcher.cpp:348](src/ai/ai_tool_dispatcher.cpp:348)

- **MEDIUM** - Every availability or handler failure becomes health failure. Invalid requests, cancellations, user refusals, and normal business failures can disable healthy tools. [src/ai/ai_tool_dispatcher.cpp:253](src/ai/ai_tool_dispatcher.cpp:253) [src/ai/ai_tool_dispatcher.cpp:348](src/ai/ai_tool_dispatcher.cpp:348)

- **MEDIUM** - Non-empty handler results with missing or mistyped `success` remain unchanged, producing unstructured failures without guaranteed `success:false` or error message. [src/ai/ai_tool_dispatcher.cpp:166](src/ai/ai_tool_dispatcher.cpp:166)

- **LOW** - Handler and availability-checker exceptions become user-visible failures without error logging. [src/ai/ai_tool_dispatcher.cpp:153](src/ai/ai_tool_dispatcher.cpp:153) [src/ai/ai_tool_dispatcher.cpp:237](src/ai/ai_tool_dispatcher.cpp:237)

- **LOW** - `handlerMissingResult` omits `operation`, unlike other structured dispatcher failures; duplicated payload construction has drifted. [src/ai/ai_tool_dispatcher.cpp:390](src/ai/ai_tool_dispatcher.cpp:390)

- **LOW** - `clearHandlers()` also clears availability checkers, making name/API behavior misleading. [src/ai/ai_tool_dispatcher.cpp:126](src/ai/ai_tool_dispatcher.cpp:126)
