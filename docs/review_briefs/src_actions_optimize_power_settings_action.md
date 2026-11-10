- [HIGH] `src/actions/optimize_power_settings_action.cpp:22-25` -- English-only `powercfg` parsing breaks discovery and active-plan detection on non-English Windows.

- [HIGH] `src/actions/optimize_power_settings_action.cpp:184-205` -- Exact-name custom schemes can override canonical GUID fallback, activating wrong plan despite stated GUID-anchored contract.

- [MEDIUM] `src/actions/optimize_power_settings_action.cpp:393-416` -- Cancellation after discovery is ignored on already-optimized and activation/verification paths; cancelled status gets overwritten by completion.

- [MEDIUM] `src/actions/optimize_power_settings_action.cpp:287-300` -- Result claims processor boost/minimal restrictions and unchanged sleep/display settings without querying them; switching plans can change those effective settings.

- [MEDIUM] `src/actions/optimize_power_settings_action.cpp:120-136,229-235` -- Failed active-plan query becomes applicable `"Power plan detected"` scan success.

- [LOW] `src/actions/optimize_power_settings_action.cpp:161-173,302-308` -- Discovery failure is mislabeled as activation failure and emits unusable blank `powercfg -SETACTIVE` guidance.

- [LOW] `src/actions/optimize_power_settings_action.cpp:29,248-266` -- Report rows include newline before `leftJustified()`, placing padding and closing border on next line; configured width also exceeds border width.

- [LOW] `src/actions/optimize_power_settings_action.cpp:10,14,24,79-101` -- `queryPowerPlan()`, its regex, `QTextStream`, and `layout_constants.h` are dead/unused.

- [LOW] `src/actions/optimize_power_settings_action.cpp:32,205-221` -- High-performance GUID is duplicated; private helper's Balanced and Power Saver branches are unreachable.
