# MCP stdio session review and remediation

The original review named three findings. Historical baseline, reconciled on
2026-09-19: sequential cross-thread close could delete the joined QThread
controller with the opening caller's QObject affinity; the null nextCursor
allegation was already fixed; public error sinks were mandatory references but
Worker/shared-parser nullable sinks and split result contracts still remained.
The original affinity allegation established an ownership-contract violation,
not a reproduced crash.

The isolated four-case probe remains diagnostic history only:
build/mcp-affinity-probe-7e349221/observations.json. Permanent implementation now
detaches the private controller on its creator, joins the managed worker, adopts
the controller on the closing caller, and observes actual destruction affinity in
the registered fixture. The existing thread-starter pattern is reused for real
allocation observations and deterministic startup failure. A completed join remains mandatory before destruction. The checked-close contract now permits a bounded join failure with retained ownership and a join-only retry; deleting a live worker is still forbidden.

Session, Worker, pool and shared framing parser now use expected results, and
actual callers are migrated without legacy overloads. Buffered no-match remains
an optional value inside expected; valid empty discovery is not failure. The
existing null-cursor, pagination, correlation and noise-policy guards remain.
The canonical Worker ID allocator now permits INT_MAX once, then refuses before
increment or serialization. A private test-class seed feeds that same Worker;
seven real fake-server rows inspect exact received IDs/methods/cursors, including
partial-pagination refusal and repeated exhaustion. No parallel allocator or
public counter override exists.

Native integration now uses the real sak_utility.exe built-in MCP mode, piped
stdin and child-only temporary LOCALAPPDATA. Ordinary bounded session/pool tests
replace the two obsolete opt-in slots and skip entries. Only health_check is
called; catalog inspection proves read-only/full policy differences without
executing desktop/browser/registry mutations. Full profile is represented by the
gateway's absent-profile contract. Held process-generation handles witness live
reuse and original-generation exit. This is not arbitrary descendant, gateway
end-to-end, full desktop/browser or whole-app certification.

Wave40 targeted build and three CTests passed with immutable Qt logs showing
session41, pool14 and framing6 passes, zero failures/skips. Evidence credit in
this integration is limited to R5-MCP-AC-NATIVE and R5-MCP-AC-IDS. Five native
production red triples, five counter-production triples and one separately
classified transcript-fixture guard triple use current frozen sources, actual
Qt failures and exact copied-source restoration. The first counter-harness
scalar-array failure is retained and earns no behavioral red credit.

Historical 33 API/lifetime reds are not promoted to current-source proof. Their
refresh, remaining aggregate/final gates and documentation reconciliation remain
required before task verification. The structured ledger is authoritative; no
archive or certification claim is made here, and tests/README.md is unchanged by
this evidence-only integration.

MCP startup uses the shared WindowsProcessJob owner and atomic JOB_LIST attachment,
not post-start assignment or PID-snapshot fallback. Checked close observes primary
completion and zero active Job members before release. Retained ownership permits
bounded join/retry without the former infinite-wait exception. Cleanup dispatch
must also obey the same deadline without stack-bound callbacks or unbounded retry
queues; R5-MCP-AC-CLEANUP owns that obligation and its current evidence.
Stderr accumulation and Qt-private buffering, current-source VM proof, full
Release/CTest and full-app E2E also remain open. Broader R5/hardware authority is
unchanged. Do not archive this brief while its related containment work is open.

Wave48 deadline correction, 2026-09-24: the first full Release build exited 0,
but full CTest ended 286/287 when cleanupDeadlineRetainsOwner caught a return
before its 30 ms deadline. The failure is preserved at
`build/wave48-mcp-deadline-failure.txt`; no green claim is made for that run.
Worker cleanup had narrowed QDeadlineTimer::remainingTime() to whole int
milliseconds before QSemaphore::tryAcquire, allowing an early dispatch timeout.
It now passes the same QDeadlineTimer to Qt's supported overload; the test
checks deadline.hasExpired() directly. Focused Release build, CTest and 20
repeated runs passed. A copied-source immediate-short-deadline mutant produced
the named early-deadline assertion RED with build exit 0; exact restoration
recompiled and passed. Evidence is
`build/wave48-mcp-deadline-drills/bounded-20260924/results.json`. The first
mutant attempt also fatally broke fixture recovery and is not credited. The
subsequent current-source full Release build and 287/287 CTests passed; local
ZIP/extraction and Windows 11 VM package startup/accessibility passed with
matching app hash. One other fresh local package of the same app hash timed
out at the unchanged 30-second native startup bound, so this is not reliable
cold-start certification. The broader MCP ledger acceptance and remaining
external/UI gates are still open; task status stays implemented, not verified.

```quality-ledger
{
  "schema_version": 1,
  "work": {
    "id": "R5-MCP-LIFETIME",
    "title": "MCP transport ownership, request integrity and process containment",
    "scope_revision": 8,
    "brief": null,
    "brief_reason": "Existing MCP session review identifies a cross-thread QObject lifetime violation and error-result API standards gap; preserve current Windows-only protocol and pooling behavior.",
    "rules": [
      {
        "path": "AGENTS.md",
        "revision": "2026-09-19-observed"
      },
      {
        "path": "docs/CODING_STANDARDS.md",
        "revision": "2026-09-19-observed"
      },
      {
        "path": "CONTRIBUTING.md",
        "revision": "2026-09-19-observed"
      }
    ],
    "inputs": [
      "CMakeLists.txt",
      "tests/CMakeLists.txt",
      ".pre-commit-config.yaml",
      "include/sak/layout_constants.h",
      "resources/ai/providers/providers.json",
      "src/win32mcp/win32_mcp_dispatch.cpp",
      "src/win32mcp/win32_mcp_tools.cpp",
      "src/win32mcp/browser_bridge_pipe.cpp",
      "src/win32mcp/browser_bridge_security.cpp"
    ],
    "environment": {
      "platform": "windows",
      "tools": {
        "python": "3.14.0",
        "git": "2.52.0.windows.1",
        "cmake": "3.31.3",
        "qt": "6.10.3.0",
        "msvc": "19.44.35222.0"
      }
    }
  },
  "requirements": [
    {
      "id": "R5-MCP-REQ-OWNER",
      "statement": "Preserve documented sequential cross-thread session use with safe QObject controller ownership and synchronous worker cleanup, including startup failure.",
      "priority": "critical",
      "acceptance": [
        "R5-MCP-AC-OWNER"
      ],
      "superseded_by": null
    },
    {
      "id": "R5-MCP-REQ-RESULT",
      "statement": "Replace split sentinel/error-output results throughout the MCP session, pool and shared framing parser with explicit expected values; migrate every caller without legacy overloads or wire behavior changes.",
      "priority": "normal",
      "acceptance": [
        "R5-MCP-AC-RESULT"
      ],
      "superseded_by": null
    },
    {
      "id": "R5-MCP-REQ-NATIVE",
      "statement": "Replace obsolete opt-in transport checks with ordinary hermetic integration against the actual built-in MCP server, preserving user browser authority and proving process reuse and policy isolation.",
      "priority": "critical",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "superseded_by": null
    },
    {
      "id": "R5-MCP-REQ-IDS",
      "statement": "Allocate unique positive integer MCP request IDs without signed overflow, wraparound, silent restart or traffic after exhaustion.",
      "priority": "critical",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "superseded_by": null
    },
    {
      "id": "R5-MCP-REQ-CONTAINMENT",
      "statement": "Contain every MCP server before startup code executes and preserve ownership until bounded, observed process-family cleanup succeeds; failures must never become successful shutdown or weaker process control.",
      "priority": "critical",
      "acceptance": [
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP"
      ],
      "superseded_by": null
    },
    {
      "id": "R5-MCP-REQ-IO-BOUNDS",
      "statement": "Bound actual Windows MCP stdio ingress while the Worker is idle and apply one finite request budget through writing and response parsing; hostile streams must never grow private buffers or monopolize the Worker beyond that budget.",
      "priority": "critical",
      "acceptance": [
        "R5-MCP-AC-PIPE-BOUNDS",
        "R5-MCP-AC-REQUEST-DEADLINE"
      ],
      "superseded_by": null
    }
  ],
  "acceptance": [
    {
      "id": "R5-MCP-AC-OWNER",
      "requirement": "R5-MCP-REQ-OWNER",
      "given": "A session creator has no Qt event loop and sequential operations may later run on another thread.",
      "when": "Opening, discovering, explicitly closing or destroying the session, including a missing executable or injected thread-start failure.",
      "then": "The actual QThread controller is destroyed on its current QObject affinity thread after managed-thread completion when started. Both discovery pages remain correct. Explicit close, destructor, same-thread control, repeated close and both startup failures are tested through the real allocation/lifetime path. Worker deletion remains tied to finished; no queued cleanup depends on the creator event loop. Audit the existing blocking suppression and retain only if bounded alternatives cannot preserve unconditional join safely.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-MCP-AC-RESULT",
      "requirement": "R5-MCP-REQ-RESULT",
      "given": "Session, Worker, pool and framing-parser operations can succeed with an empty tool catalog or fail with a specific reason.",
      "when": "Callers exercise valid replies, empty discovery, malformed framing/pagination, closed sessions, launch/dispatch failures, pool validation and transport failure.",
      "then": "Fallible APIs return std::expected with a value or nonempty diagnostic, never optional error sinks or success-shaped failure. Buffered no-match is a distinct optional value inside expected. All production/test callers migrate, no legacy overloads remain, pool drops failed sessions only after proved cleanup and retains ownership while cleanup is unresolved, and gateway preserves the existing result wrapper and exact failure propagation. Existing cursor/page/tool/byte bounds, environment isolation, request correlation and documented noise-line policy remain enforced.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-MCP-AC-NATIVE",
      "requirement": "R5-MCP-REQ-NATIVE",
      "given": "The real sak_utility executable is built beside the maintained session/pool tests and each spawned server receives its own test-owned LOCALAPPDATA.",
      "when": "Ordinary CTest execution opens the built-in server using its actual mode environment and piped stdin, discovers its catalog, and issues only health_check calls across repeated and differently profiled sessions.",
      "then": "No external helper path, opt-in flag or skip remains. The producing app is an explicit test build dependency and each CTest has a60-second deadline. Sandbox rendezvous creation is verified. Health fields status/server/native and positive pid are checked; same-key calls reuse the pid, distinct policy instances use different pids and slots. Read-only catalog includes health_check/list_windows but omits clipboard_write; full access uses the gateway's unset-profile contract and includes that mutator without executing it. Pure profile-only key controls prevent unrelated sandbox differences from hiding a keying defect. Real LOCALAPPDATA is never overridden in the parent, browser/registry/desktop/network mutations are not exercised, and both obsolete skip-ledger entries are removed. Held read-only process handles prove the original process generations stay alive across reuse/policy checks and become signaled after close; PID equality alone is insufficient.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-MCP-AC-IDS",
      "requirement": "R5-MCP-REQ-IDS",
      "given": "The real Worker has consumed all but a few IDs in its positive signed-int request space.",
      "when": "Initialize, tools/call and multi-page tools/list reach INT_MAX or attempt another request, including repeated attempts.",
      "then": "INT_MAX is transmitted exactly once when available. The canonical allocator refuses before increment or serialization after exhaustion with a nonempty stable expected error. No wrap, ID reuse, automatic retry or partial successful catalog occurs. A private friend-test seed initializes the real Worker; owned child transcripts independently prove exact wire IDs and absence of later traffic. Normal new sessions still begin at one.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-MCP-AC-LAUNCH",
      "requirement": "R5-MCP-REQ-CONTAINMENT",
      "given": "A server may spawn descendants immediately on entry, before the MCP handshake, and launch preparation/native APIs may fail.",
      "when": "The real QProcess is prepared and launched through the shared Windows Job owner.",
      "then": "An unnamed kill-on-close Job is attached atomically using the Windows startup JOB_LIST attribute. Attribute storage and handle values remain alive through creation. Qt startup fields, redirected standard handles and existing flags are preserved. An existing modifier or preparation failure is refused before child startup. No suspended launch, post-start assignment, PID walk or taskkill fallback remains in this session path. Actual startup descendants are contained, with an owned bounded fixture and independent generation observations. Existing Windows-only product scope is unchanged.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-MCP-AC-CLEANUP",
      "requirement": "R5-MCP-REQ-CONTAINMENT",
      "given": "A session can fail during startup, requests or cleanup, and a primary can exit while descendants remain.",
      "when": "Explicit session close, failed-open/request cleanup, pool replacement/closeAll or destruction runs.",
      "then": "close and closeAll return expected results. One bounded cleanup budget covers checked termination, primary completion and observed ActiveProcesses zero before handle release and Worker destruction. On cleanup failure retain the Worker, process, thread, Job and pool ownership, refuse reuse/new launches while pool cleanup remains unresolved, and preserve both primary and cleanup diagnostics. Failed close returns before thread quit. Only proved-clean sessions are reset/erased; partial pool cleanup retains failures. Destruction fails closed before unsafe member destruction if cleanup cannot be proved. Test recoverable API/deadline failures, stopped primary/live descendants, retained owners, failure propagation and isolated destructor refusal; do not suppress QProcess cleanup warnings. The same cleanup deadline includes pool mutex acquisition and QThread joining. After proved process cleanup, a timed join retains an explicit join-pending state; retry must not redispatch to a stopped/deleted Worker. The previous unbounded join marker is replaced by this direct bounded ownership-safe path, with delayed-finish/retry regression proof and a zero-exception blocking-gate result. Worker dispatch and its completion wait share that deadline. Queued cleanup owns its completion state and deadline by value, never caller-stack references. A dispatch timeout retains owners; repeated timed-out closes cannot enqueue unbounded duplicate work. Late completion remains safe, and retry observes its real result before deciding whether another cleanup attempt is needed.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-MCP-AC-PIPE-BOUNDS",
      "requirement": "R5-MCP-REQ-IO-BOUNDS",
      "given": "A contained MCP child can emit unbounded stdout or stderr after handshake while the Worker is idle.",
      "when": "The real QProcess is prepared with separate channels and launched through its one Windows Job modifier.",
      "then": "Before CreateProcess, the existing modifier configures positive 8 MiB stdout and 64 KiB stderr limits on the actual fresh Windows QProcess private pipe readers, after checking exact runtime Qt version and reader/channel shape. An unexpected shape fails closed before child code executes. There is no second uncapped MCP launch path or fallback. Independent owned child floods on each channel prove producer backpressure, bounded Qt-private bytes, resumed writes after bounded Worker drain, and checked Job cleanup. Separate copied-source cap removals each produce an intended assertion failure and exact restoration.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-MCP-AC-REQUEST-DEADLINE",
      "requirement": "R5-MCP-REQ-IO-BOUNDS",
      "given": "A server can stream complete id-less or malformed JSONL forever, delay a valid matching result, or block request writes; elapsed time is qint64.",
      "when": "The Worker writes one request and drains complete response lines under its configured request timeout.",
      "then": "One QDeadlineTimer bounds write and read together. Check the deadline before every line and before accepting a matching reply, and check the stdout byte cap before every readLine. Clamp positive qint64 remaining time only when invoking Qt int-timeout operations; never narrow elapsed time before subtraction. Keep the existing strict JSON-RPC, correlation, notification and noise behavior. Real finite child noise with a late matching reply and a controlled production line-reader seam prove timely refusal, with separate compiled red reversals for inner and outer deadline guards.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    }
  ],
  "tasks": [
    {
      "id": "R5-MCP-TASK-API-LIFETIME",
      "purpose": "Enhance canonical session/Worker and pool, migrate shared parser and every direct consumer, and extend existing real-process tests. Reuse the existing per-instance thread-starter test seam pattern to observe actual QObject destruction and force startup failure without replacing the production controller. Repair the actual built-in-server integration route, isolate child browser authority in owned temporary roots, and retire two unjustified skips with explicit build dependencies and bounded normal CTest coverage. Consolidate duplicated native fixture/parsing oracles in one shared test-only header with non-terminating process-generation observation. Bound the canonical Worker request-ID allocator and prove real-transport exhaustion through a private friend-test seed and owned subprocess transcripts. Replace the unsafe MCP post-start Job/PID cleanup with a shared startup Job owner and checked retained-ownership teardown, migrating all direct callers and tests. Re-audit and replace the now-unnecessary infinite join with deadline-bound joining and retained join-only retry state. Bound Worker cleanup dispatch with retained completion ownership and prove timeout, late completion and retry without callback accumulation. Apply positive Qt-private stdout/stderr caps through the existing atomic Windows Job launch modifier, and use one request deadline through actual write and JSONL read paths with independently sensitive child/line-reader tests.",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS",
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP",
        "R5-MCP-AC-PIPE-BOUNDS",
        "R5-MCP-AC-REQUEST-DEADLINE"
      ],
      "depends_on": [],
      "changes": [
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "action": "modify"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "action": "modify"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "action": "modify"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "action": "modify"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "action": "modify"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "action": "modify"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "action": "modify"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "action": "modify"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "action": "modify"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "action": "modify"
        },
        {
          "path": "tests/CMakeLists.txt",
          "action": "modify"
        },
        {
          "path": "tests/skip_baseline.txt",
          "action": "modify"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "action": "modify"
        },
        {
          "path": "tests/README.md",
          "action": "modify"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "action": "create"
        },
        {
          "action": "create",
          "path": "include/sak/windows_process_job.h"
        },
        {
          "action": "create",
          "path": "src/core/windows_process_job.cpp"
        },
        {
          "action": "create",
          "path": "tests/unit/test_windows_process_job.cpp"
        },
        {
          "action": "modify",
          "path": "CMakeLists.txt"
        }
      ],
      "status": "implemented",
      "evidence": [
        "R5-MCP-READY-W39",
        "R5-MCP-READY-W39-FINAL",
        "R5-MCP-TARGETED-W39",
        "R5-MCP-READY-NATIVE-W39",
        "R5-MCP-READY-FIXTURE-W39",
        "R5-MCP-READY-IDS-W40",
        "R5-MCP-READY-CURRENT-W40",
        "R5-MCP-TARGETED-NATIVE-W40",
        "R5-MCP-TARGETED-IDS-W40",
        "R5-MCP-NATIVE-BATCH-W40",
        "R5-MCP-COUNTER-BATCH-W40",
        "R5-MCP-NATIVE-RED-W40-01",
        "R5-MCP-NATIVE-RED-W40-02",
        "R5-MCP-NATIVE-RED-W40-03",
        "R5-MCP-NATIVE-RED-W40-04",
        "R5-MCP-NATIVE-RED-W40-05",
        "R5-MCP-COUNTER-RED-W40-01",
        "R5-MCP-COUNTER-RED-W40-02",
        "R5-MCP-COUNTER-RED-W40-03",
        "R5-MCP-COUNTER-RED-W40-04",
        "R5-MCP-COUNTER-RED-W40-05",
        "R5-MCP-FIXTURE-RED-W40-01",
        "R5-MCP-IDS-HARNESS-FAIL-W40",
        "R5-MCP-READY-CONTAINMENT-W41",
        "R5-MCP-READY-BOUNDED-JOIN-W41",
        "R5-MCP-READY-DISPATCH-W42",
        "R5-MCP-DISPATCH-REPRO-W42",
        "R5-MCP-READY-PIPE-BOUNDS-W43"
      ],
      "blocker": null,
      "superseded_by": null
    }
  ],
  "evidence": [
    {
      "id": "R5-MCP-READY-W39",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Live canonical source/caller/test review reconciled the original findings, preserved protocol boundaries, and separated the confirmed process containment follow-up. The actual worker/controller lifetime probe informs a real-allocation regression seam; it is not claimed as a production fix.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave39-mcp-readiness.md",
        "sha256": "58e1eabc55ac7b2e62f9cced5a2d1e9a6492a64d619b6d9afcef1264daf6a8ab"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        }
      ],
      "scope_sha256": "c661a044a904b51bd3f034b6ac42bd3279da15db963c03ae5d3805a17e44364e",
      "red": null,
      "reason": "Candidate editor normalized one trailing narrative newline; semantic scope digest changed without requirements or code changes. Rechecked exact current requirements and caller map; use R5-MCP-READY-W39-FINAL."
    },
    {
      "id": "R5-MCP-READY-W39-FINAL",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Live canonical source/caller/test review reconciled the original findings, preserved protocol boundaries, and separated the confirmed process containment follow-up. The actual worker/controller lifetime probe informs a real-allocation regression seam; it is not claimed as a production fix. Rechecked the complete current scope after EOF normalization; obligations and source remain identical.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave39-mcp-readiness.md",
        "sha256": "58e1eabc55ac7b2e62f9cced5a2d1e9a6492a64d619b6d9afcef1264daf6a8ab"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        }
      ],
      "scope_sha256": "b9431bd59f8e22c5742467f375fb38a3c62f87ae71abd86b5cc270c83cf13ab8",
      "red": null,
      "reason": "Native integration acceptance, canonical launch inputs and task scope were added after discovering stale external-helper routing and incorrectly represented full profile. Original phase results/artifacts are preserved; fresh readiness and final-scope verification are required."
    },
    {
      "id": "R5-MCP-TARGETED-W39",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT"
      ],
      "kind": "behavior",
      "status": "stale",
      "command": [
        "ctest",
        "--test-dir",
        "build",
        "-C",
        "Release",
        "--output-on-failure",
        "-R",
        "^(test_ai_mcp_stdio_session|test_ai_mcp_session_pool|test_fuzz_mcp_framing)$"
      ],
      "method": "Actual targeted build exited0 before CTest ran; all3 selected registered suites passed. Qt logs retained as build/wave39-test_ai_mcp_stdio_session-targeted.txt (32passed/1existing live-optin skip), corresponding pool log (13passed/1existing live-optin skip), and framing log (6passed). Real self-hosted servers exercised thread affinity, typed failure/empty success, correlation and failed-session retries. This does not build or certify the gateway/server callers, skipped live integration, source-bound VM cohort, or whole app; complete Release/CTest and fresh controller evidence remain open.",
      "environment": {
        "platform": "windows",
        "tools": {
          "cmake": "3.31.3",
          "git": "2.52.0.windows.1",
          "msvc": "19.44.35222.0",
          "python": "3.14.0",
          "qt": "6.10.3.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave39-mcp-targeted-ctest.log",
        "sha256": "87b67c69be4b944666f191a3448b7595724ffd3f38115894d8981f79777b05bf"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "0a2e451bf5fc5de9532db9edb4057c9a4bc122d94859eef5f8635d5796081719"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "074d9cd6127873000a7ba032e177068f3857b520a9cb6f75f3c5c2590d3838a8"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "9d859f77243a3ab5506456b40cbce172b0f6d4b1a1b59dd3f8d40f8bdac18a44"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "3b5fddd5809f3d823631fff3f733aff756bd6b0b886e9b3f4e8b8189ba7c11b5"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "b9431bd59f8e22c5742467f375fb38a3c62f87ae71abd86b5cc270c83cf13ab8",
      "red": null,
      "reason": "Native integration acceptance, canonical launch inputs and task scope were added after discovering stale external-helper routing and incorrectly represented full profile. Original phase results/artifacts are preserved; fresh readiness and final-scope verification are required."
    },
    {
      "id": "R5-MCP-READY-NATIVE-W39",
      "acceptance": [
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Reviewed current native launch/env contract and browser startup effects, identified stale opt-in paths and incorrect full-profile token, and defined isolated real-process proof with explicit app build dependencies and bounded CTest execution. Existing Windows tool coverage remains; health-only transport checks do not substitute for full desktop/browser certification.",
      "environment": {
        "platform": "windows",
        "tools": {
          "cmake": "3.31.3",
          "git": "2.52.0.windows.1",
          "msvc": "19.44.35222.0",
          "python": "3.14.0",
          "qt": "6.10.3.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave39-mcp-native-readiness.md",
        "sha256": "2215a9ad8b5ee55f333c856b3034733073ff46ad63ab23db87aadca1791f60ff"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        }
      ],
      "scope_sha256": "3778e607fde809b42b7b8564ff3126ba66b1bf3877aa14faa4b46f5a7453f323",
      "red": null,
      "reason": "Shared fixture ownership and process-generation observations were added during review, and the declared CTest configuration changed during implementation. Preserve original readiness; re-review current scope before this follow-up."
    },
    {
      "id": "R5-MCP-READY-FIXTURE-W39",
      "acceptance": [
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Reviewed shared-oracle duplication, actual Qt isFile signature, CTest app dependencies/timeouts and removed skip entries. Approved one test-only fixture with held non-terminating process handles, bounded wait/error semantics and child-only sandboxing. This remains process-transport scope, not descendant or whole-app certification.",
      "environment": {
        "platform": "windows",
        "tools": {
          "cmake": "3.31.3",
          "git": "2.52.0.windows.1",
          "msvc": "19.44.35222.0",
          "python": "3.14.0",
          "qt": "6.10.3.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave39-mcp-fixture-readiness.md",
        "sha256": "b41cf8e197f0889922f2b79c241223305a4bcac40dd7a1d8091899562de69666"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "98c9499be119b8108c1e8826383715fe2d8a47ab56dca75aa4d70b5529f32848"
        }
      ],
      "scope_sha256": "62a17e00a69c0ea011b3425c38e9caf03f2029fa2832aef5d1a70c7d6863e334",
      "red": null,
      "reason": "Current fixture/configuration and newly declared request-ID boundary require fresh scoped evidence; original artifacts and fingerprints are preserved."
    },
    {
      "id": "R5-MCP-READY-IDS-W40",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Reviewed the live canonical allocator and all three consumers, existing friend-test patterns, actual fake-process transport and native integration proof. Existing source is reused; boundary behavior and separate meaningful red drills are specified in the bound review.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "sha256": "18a96c01487e50f9d6f40fa6d2655ed4552ef59cd4e7f3b4416a226c8c78d2d2",
        "path": "build/wave40-mcp-counter-readiness.md"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        }
      ],
      "scope_sha256": "ac9b6c15fd07739cd381285f12deac89f4ba70e4c1a1f9aa99935ad72bf0bed6",
      "red": null,
      "reason": "Narrative now distinguishes historical baseline from implemented code and current bounded proof. Original artifact, inputs and scope are preserved; current reconciliation has a new readiness receipt."
    },
    {
      "id": "R5-MCP-READY-CURRENT-W40",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Reviewed current canonical scope, implementation and completed evidence; requirements are unchanged. Corrected historical/current narrative, preserved old source-bound proofs, and retained implemented status and explicit open gates.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave40-mcp-evidence-81e06a43/readiness.md",
        "sha256": "2457943b781fe8df7b62e7d1fc9d23b3fa72e7bd05116127f584a6de67b7d100"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": null,
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-TARGETED-NATIVE-W40",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "kind": "behavior",
      "status": "stale",
      "command": [
        "ctest",
        "--test-dir",
        "build",
        "-C",
        "Release",
        "--output-on-failure",
        "-V",
        "-R",
        "^(test_ai_mcp_stdio_session|test_ai_mcp_session_pool|test_fuzz_mcp_framing)$"
      ],
      "method": "Current registered native session/pool integration, generation observation and policy isolation. Root session12721 observed immediate targeted build exit0 followed by CTest exit0,3/3. Immutable session41/pool14/framing6 logs have zero failures/skips; all supporting hashes are independently audited. This receipt does not claim gateway/full-app behavior, current API/lifetime red refresh or the complete Release gate. Supporting build: build/wave40-counter-final-build.log; Qt logs: build/wave40-counter-final-test_ai_mcp_stdio_session.txt, build/wave40-counter-final-test_ai_mcp_session_pool.txt and build/wave40-counter-final-test_fuzz_mcp_framing.txt.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-final-ctest.log",
        "sha256": "384f9f521bbb0edeb430f6684a19d374b7f6737c95ffb69155f268608b0457f7"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": null,
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-TARGETED-IDS-W40",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "behavior",
      "status": "stale",
      "command": [
        "ctest",
        "--test-dir",
        "build",
        "-C",
        "Release",
        "--output-on-failure",
        "-V",
        "-R",
        "^(test_ai_mcp_stdio_session|test_ai_mcp_session_pool|test_fuzz_mcp_framing)$"
      ],
      "method": "Current seven request-ID boundary rows and separately bounded transcript fixture. Root session12721 observed immediate targeted build exit0 followed by CTest exit0,3/3. Immutable session41/pool14/framing6 logs have zero failures/skips; all supporting hashes are independently audited. This receipt does not claim gateway/full-app behavior, current API/lifetime red refresh or the complete Release gate. Supporting build: build/wave40-counter-final-build.log; Qt logs: build/wave40-counter-final-test_ai_mcp_stdio_session.txt, build/wave40-counter-final-test_ai_mcp_session_pool.txt and build/wave40-counter-final-test_fuzz_mcp_framing.txt.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-final-ctest.log",
        "sha256": "384f9f521bbb0edeb430f6684a19d374b7f6737c95ffb69155f268608b0457f7"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": null,
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-NATIVE-BATCH-W40",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "kind": "behavior",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave39-native-mcp-drills/run.ps1",
        "-RunName",
        "counter-current"
      ],
      "method": "Root session41805 terminal0; five independently restored production-only native triples. The test oracles and live inputs remained exact; real authority was absent before and after. This is bounded native transport/policy proof, not desktop mutation or descendant certification.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave39-native-mcp-drills/counter-current/results.json",
        "sha256": "64d6e69c426f069588ad1d6b13c7022330d6ae255409fc3cec4eb162acaaee1f"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": null,
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-COUNTER-BATCH-W40",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "behavior",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-array-current",
        "-Execute"
      ],
      "method": "Session42764 terminal0; seven real Worker/transport rows pass before/after with9 Qt passes including init/cleanup. Five production triples retain all test bytes; the sixth is separately classified as a test-fixture writer guard. Every phase log/source fingerprint was audited; no timeout/crash was accepted as red.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-array-current/results.json",
        "sha256": "274efda948130dbd18a9208edb0a2438a574d044e54457a30cb70ee0ffceb696"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": null,
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-NATIVE-RED-W40-01",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave39-native-mcp-drills/run.ps1",
        "-RunName",
        "counter-current"
      ],
      "method": "Native guard session-persistence-lifetime: if (!invoked) { return std::unexpected( QStringLiteral(\"Could not dispatch tools/list to the MCP session worker\")); } close(); return result; Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave39-native-mcp-drills/counter-current/session-persistence-lifetime-mutant-qt.txt",
        "sha256": "3b0350f7562272b84f6a06e04b695e5c67256330d9671d3fab144314934fd5a9"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Native guard session-persistence-lifetime: if (!invoked) { return std::unexpected( QStringLiteral(\"Could not dispatch tools/list to the MCP session worker\")); } close(); return result;",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::builtInMcpPersistentSession() 'first_result.has_value()' returned FALSE. (MCP session is not open)",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::builtInMcpPersistentSession() 'first_result.has_value()' returned FALSE. (MCP session is not open)",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "3a2b6426b38ac73a0f2da6f9cdfd6a1fdd1cdbed018d8aff33373357a14632ac"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-NATIVE-RED-W40-02",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave39-native-mcp-drills/run.ps1",
        "-RunName",
        "counter-current"
      ],
      "method": "Native guard session-read-only-catalog: tools.append(AiMcpToolDescriptor{ .name = QStringLiteral(\"clipboard_write\"), .description = {}, .input_schema = {}}); return tools; Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave39-native-mcp-drills/counter-current/session-read-only-catalog-mutant-qt.txt",
        "sha256": "16bcf259df1201a0c4b56b3162c59f6a4a905370f0304f3eb0d225c52e50c0ce"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Native guard session-read-only-catalog: tools.append(AiMcpToolDescriptor{ .name = QStringLiteral(\"clipboard_write\"), .description = {}, .input_schema = {}}); return tools;",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::builtInMcpPersistentSession() '!native_mcp::containsTool(*tools, QStringLiteral(\"clipboard_write\"))' returned FALSE. ()",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::builtInMcpPersistentSession() '!native_mcp::containsTool(*tools, QStringLiteral(\"clipboard_write\"))' returned FALSE. ()",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "5219dcf8710b997f835f338e9c696b2f90fbc25b748a7441386a8b95085613f6"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-NATIVE-RED-W40-03",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave39-native-mcp-drills/run.ps1",
        "-RunName",
        "counter-current"
      ],
      "method": "Native guard pool-same-key-reuse: const QMutexLocker lock(&m_mutex); const QString key = sessionKey(request) + QString::number(m_sessions.size()); auto it = m_sessions.find(key); Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave39-native-mcp-drills/counter-current/pool-same-key-reuse-mutant-qt.txt",
        "sha256": "0b91c4096082f82283747837af6a9921586c643501b22d02837644514339016a"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Native guard pool-same-key-reuse: const QMutexLocker lock(&m_mutex); const QString key = sessionKey(request) + QString::number(m_sessions.size()); auto it = m_sessions.find(key);",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpSessionPoolTests::builtInMcpReuseAndPolicyIsolation() Compared values are not the same",
        "observed_diagnostic": "FAIL!  : AiMcpSessionPoolTests::builtInMcpReuseAndPolicyIsolation() Compared values are not the same",
        "before": [
          {
            "path": "src/ai/ai_mcp_session_pool.cpp",
            "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_session_pool.cpp",
            "sha256": "e0c9da73d8d6e564750ada98c811a61b2738616816400258a46a7eae3cf8d8aa"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_session_pool.cpp",
            "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-NATIVE-RED-W40-04",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave39-native-mcp-drills/run.ps1",
        "-RunName",
        "counter-current"
      ],
      "method": "Native guard profile-only-key-isolation: QStringLiteral(\"mutated-environment-key\") Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave39-native-mcp-drills/counter-current/profile-only-key-isolation-mutant-qt.txt",
        "sha256": "3c69fb759a1bc2ae0ac51fc11d876aeed4fe18212df3d010a8efb861a9d6b8aa"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Native guard profile-only-key-isolation: QStringLiteral(\"mutated-environment-key\")",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpSessionPoolTests::keyIsolatesCommandAndEnvironment() 'Pool::sessionKeyForTesting(read_only) != Pool::sessionKeyForTesting(full)' returned FALSE. ()",
        "observed_diagnostic": "FAIL!  : AiMcpSessionPoolTests::keyIsolatesCommandAndEnvironment() 'Pool::sessionKeyForTesting(read_only) != Pool::sessionKeyForTesting(full)' returned FALSE. ()",
        "before": [
          {
            "path": "src/ai/ai_mcp_session_pool.cpp",
            "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_session_pool.cpp",
            "sha256": "76251203804f400ec5e72d4aa27a6889570bd4cb4fe1997667b166c88960eb56"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_session_pool.cpp",
            "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-NATIVE-RED-W40-05",
      "acceptance": [
        "R5-MCP-AC-NATIVE"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave39-native-mcp-drills/run.ps1",
        "-RunName",
        "counter-current"
      ],
      "method": "Native guard pool-full-catalog: tools.removeIf([](const AiMcpToolDescriptor& tool) { return tool.name == QLatin1String(\"clipboard_write\"); }); return tools; Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave39-native-mcp-drills/counter-current/pool-full-catalog-mutant-qt.txt",
        "sha256": "4b7a8973bfbb8bde506efdf2182628be47ea334fa847c0e6ddf43a7718b70ccd"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Native guard pool-full-catalog: tools.removeIf([](const AiMcpToolDescriptor& tool) { return tool.name == QLatin1String(\"clipboard_write\"); }); return tools;",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpSessionPoolTests::builtInMcpReuseAndPolicyIsolation() 'native_mcp::containsTool(*full_tools, QStringLiteral(\"clipboard_write\"))' returned FALSE. ()",
        "observed_diagnostic": "FAIL!  : AiMcpSessionPoolTests::builtInMcpReuseAndPolicyIsolation() 'native_mcp::containsTool(*full_tools, QStringLiteral(\"clipboard_write\"))' returned FALSE. ()",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "431a490f70558ac9fa16445b46ed8eaee6a3aa97bc44c8d0c3ec0f63c89b7f78"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-COUNTER-RED-W40-01",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-array-current",
        "-Execute"
      ],
      "method": "Production request-ID guard: reuse-max-on-exhaustion. Exact edit: [{\"before\": \" if (m_next_id == std::numeric_limits<int>::max()) {\\n return std::unexpected(QStringLiteral(\\\"MCP request ID space exhausted\\\"));\\n }\", \"after\": \" if (m_next_id == std::numeric_limits<int>::max()) {\\n return m_next_id;\\n }\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}] Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-array-current/reuse-max-on-exhaustion-mutant-qt.txt",
        "sha256": "e9c0663486027ac91f45661da556a99b6d1e0e3bd10a9e34ff7b24acc065128f"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Production request-ID guard: reuse-max-on-exhaustion. Exact edit: [{\"before\": \" if (m_next_id == std::numeric_limits<int>::max()) {\\n return std::unexpected(QStringLiteral(\\\"MCP request ID space exhausted\\\"));\\n }\", \"after\": \" if (m_next_id == std::numeric_limits<int>::max()) {\\n return m_next_id;\\n }\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}]",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(exhausted-initialize) Compared values are not the same",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(exhausted-initialize) Compared values are not the same",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "43f22d4bad74969adf83a7a47df944e26b822e6f5045488d704ded56156874c8"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-COUNTER-RED-W40-02",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-array-current",
        "-Execute"
      ],
      "method": "Production request-ID guard: refuse-before-last-valid-id. Exact edit: [{\"before\": \"m_next_id == std::numeric_limits<int>::max()\", \"after\": \"m_next_id >= std::numeric_limits<int>::max() - 1\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}] Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-array-current/refuse-before-last-valid-id-mutant-qt.txt",
        "sha256": "69b83a6847a43832dde2e0a3c7e30cc4a3b4483dcf7c562bd36e9e67fae988ab"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Production request-ID guard: refuse-before-last-valid-id. Exact edit: [{\"before\": \"m_next_id == std::numeric_limits<int>::max()\", \"after\": \"m_next_id >= std::numeric_limits<int>::max() - 1\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}]",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-initialize) Compared values are not the same",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-initialize) Compared values are not the same",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "d5914b9fd0b89c74547cfc20cf9681f767c694a072e2138bc2bcd28b1725f280"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-COUNTER-RED-W40-03",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-array-current",
        "-Execute"
      ],
      "method": "Production request-ID guard: initialize-wire-id-bypass. Exact edit: [{\"before\": \"mcp::initializePayload(*initialize_id)\", \"after\": \"mcp::initializePayload(1)\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}, {\"before\": \"*initialize_id);\", \"after\": \"1);\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}] Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-array-current/initialize-wire-id-bypass-mutant-qt.txt",
        "sha256": "ba2a645c989fd99af4eb4e92326523bd7c1e7546e8e20b4fb689e16d5e04ab3b"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Production request-ID guard: initialize-wire-id-bypass. Exact edit: [{\"before\": \"mcp::initializePayload(*initialize_id)\", \"after\": \"mcp::initializePayload(1)\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}, {\"before\": \"*initialize_id);\", \"after\": \"1);\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}]",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-initialize) Compared values are not the same",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-initialize) Compared values are not the same",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "9bb916a8a06ecd9355d3a57af51342d6a2f99f73f15332465861f1897062df92"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-COUNTER-RED-W40-04",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-array-current",
        "-Execute"
      ],
      "method": "Production request-ID guard: list-wire-id-bypass. Exact edit: [{\"before\": \"request(mcp::toolsListPayload(*id, cursor), *id)\", \"after\": \"request(mcp::toolsListPayload(1, cursor), 1)\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}] Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-array-current/list-wire-id-bypass-mutant-qt.txt",
        "sha256": "28d331820dde16ff9eb929451243f2ee2314643985412f05aa22fbdeb2c57e20"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Production request-ID guard: list-wire-id-bypass. Exact edit: [{\"before\": \"request(mcp::toolsListPayload(*id, cursor), *id)\", \"after\": \"request(mcp::toolsListPayload(1, cursor), 1)\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}]",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-pagination) Compared values are not the same",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-pagination) Compared values are not the same",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "6055da4719622474ec604d188f067873c6a46efb593aeb0ee3a150d24f4caa14"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-COUNTER-RED-W40-05",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-array-current",
        "-Execute"
      ],
      "method": "Production request-ID guard: call-wire-id-bypass. Exact edit: [{\"before\": \"request(mcp::toolCallPayload(*id, tool_name, arguments), *id)\", \"after\": \"request(mcp::toolCallPayload(1, tool_name, arguments), 1)\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}] Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-array-current/call-wire-id-bypass-mutant-qt.txt",
        "sha256": "c155d525c6e86b8e33b9a947a2379092940d9e00ab0420f27418c2d3c65aee36"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Production request-ID guard: call-wire-id-bypass. Exact edit: [{\"before\": \"request(mcp::toolCallPayload(*id, tool_name, arguments), *id)\", \"after\": \"request(mcp::toolCallPayload(1, tool_name, arguments), 1)\", \"file\": \"src/ai/ai_mcp_stdio_session.cpp\"}]",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-call) Compared values are not the same",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::requestIdBoundary(last-call) Compared values are not the same",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "49ab957fd4edb6bf4ac22d3914cb48aa97f0914d7f560221c57590fc6993d785"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-FIXTURE-RED-W40-01",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "red",
      "status": "stale",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-array-current",
        "-Execute"
      ],
      "method": "Test-fixture sensitivity only; production behavior is not mutated: fixture-transcript-projected-limit. Exact edit: [{\"before\": \"line.size() > kMaxFakeTranscriptBytes - transcript.size()\", \"after\": \"transcript.size() > kMaxFakeTranscriptBytes\", \"file\": \"tests/unit/test_ai_mcp_stdio_session.cpp\"}] Independent copied-source0/1/0 was audited against actual Qt assertion text and exact restoration. The canonical path identifies the copied subject; no tracked source mutation occurred. Detailed child argv remain in the unchanged batch artifact.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-array-current/fixture-transcript-projected-limit-mutant-qt.txt",
        "sha256": "ea6257b3d9fb59ea94b7bd9d69381fef16ee46decffca5713002030387cce8c8"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": {
        "mutation": "Test-fixture sensitivity only; production behavior is not mutated: fixture-transcript-projected-limit. Exact edit: [{\"before\": \"line.size() > kMaxFakeTranscriptBytes - transcript.size()\", \"after\": \"transcript.size() > kMaxFakeTranscriptBytes\", \"file\": \"tests/unit/test_ai_mcp_stdio_session.cpp\"}]",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "FAIL!  : AiMcpStdioSessionTests::fakeTranscriptRespectsBound() '!recordFakeRequest(path, request)' returned FALSE. ()",
        "observed_diagnostic": "FAIL!  : AiMcpStdioSessionTests::fakeTranscriptRespectsBound() '!recordFakeRequest(path, request)' returned FALSE. ()",
        "before": [
          {
            "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
            "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
          }
        ],
        "mutated": [
          {
            "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
            "sha256": "edcb95ceab6735b7bd4003309b471ecd32d83a3afca5fe5378d1349849bbec73"
          }
        ],
        "after": [
          {
            "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
            "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
          }
        ]
      },
      "reason": "Scope5 adds startup containment, checked close contracts and helper/registration inputs. Preserve the exact Wave40 artifacts; refresh affected evidence after implementation rather than rebinding old passes."
    },
    {
      "id": "R5-MCP-IDS-HARNESS-FAIL-W40",
      "acceptance": [
        "R5-MCP-AC-IDS"
      ],
      "kind": "behavior",
      "status": "fail",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave40-counter-drills/run.ps1",
        "-RunName",
        "counter-current-5d7309ab",
        "-Execute"
      ],
      "method": "Historical infrastructure failure before the first mutation: one selected row unrolled to a scalar and Count failed. Aggregate and first baseline tests passed; no behavioral red is credited. Original runner bytes are preserved beside results as runner-before-array-fix.ps1.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 1,
      "artifact": {
        "path": "build/wave40-counter-drills/counter-current-5d7309ab/results.json",
        "sha256": "00c058189d79af7b6a22e7150fcc343ad2d1547769f645bdaa4561eac3c881d4"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c72c275e30e68da779376dee04f44ac95712791d85cdb8c93dc6464fd96e542"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "05bd2b619c6c798e08067a1799a84a1b6e2540cec75fc97d05c7476a89e5013a"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "f100b67b54f45eddeb8e4bb6be233fa9ba48f735d1df3481c826294039e3d62b"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "00e8c408ff216ba04c087c42ee27379b1f88ab6dc46611dc5e6436d00c3e82b3"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        },
        {
          "path": "tests/README.md",
          "sha256": "0baeb4d7a6697ffb623fdc6d837cc16949e7ca9ad1bfd4b5b444f4ea6a269e85"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "bb444014a28c9c67fecb28d22cfc8e2600db45dfd1b8dbea08ac83e65d34e33f"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "8d6de477b3e38c426ed3d620998f3594aad185ab0018e3de0bc21b852821e0cd"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "64e4af45349b0d11a75dbd85bff71ddaabe7d1eb968ff6a2c3fdcacf5dc90aa3"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        }
      ],
      "scope_sha256": "277c27cdc81e91f572825211b072c616236aed3c4a4800e0d806d0479b45b110",
      "red": null,
      "reason": "Harness scalar-array defect; later array-wrapped runner completed the separate counter-array-current proof. Original artifacts remain unchanged."
    },
    {
      "id": "R5-MCP-READY-CONTAINMENT-W41",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS",
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Reviewed current process owners, all four build consumers, callback/native API lifetime requirements, ownership-preserving close propagation and existing real-process fixtures. The bounded actual JOB_LIST probe resolves current-host accounting while an external handle is held; it is design evidence only. Reuse and failure/acceptance boundaries are recorded in the attached review.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave41-mcp-containment-readiness.md",
        "sha256": "a5388068ed7699b69ea77cb83ea57f00bb15448690379756afa7aa39f5b97f32"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "6d6f560c1a1d71c62099aac0729922b3c6d693e58d8d6b65dc659f47c24527ae"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "28a7a1b7b23033cc75e0ff3e57bb8e56145d2d98893e2ac0c724597defaef91d"
        }
      ],
      "scope_sha256": "ff2ae61d6dccf1aa10b68d1ef0f1acc078bdb4949e45296d5ff4fbe552073042",
      "red": null,
      "reason": "Scope6 includes the ownership-safe bounded join made possible by checked close; preserve earlier artifacts and refresh affected proof on final sources."
    },
    {
      "id": "R5-MCP-READY-BOUNDED-JOIN-W41",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS",
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Re-audited the existing wait exception after checked-close ownership retention was implemented. Reviewed the installed Qt deadline-wait API, retry-state lifecycle and a bounded real finished-callback test. The attached review explains why a direct fix is now possible without weakening destruction safety.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave41-bounded-join-readiness.md",
        "sha256": "8f69a6f17534d3403c9294e7d33132f2391b24cd0bd26cf3cbc2099b5c73be0b"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "f13ae22417a6f787d3e8f7800bb50690101e0fab00c8ba434a35e1ec1ebdcbed"
        }
      ],
      "scope_sha256": "8f680e2ed917fcf205974a94db962f0fb90cecc23febf12ad1f17d6ea80b508f",
      "red": null,
      "reason": "Scope7 makes the existing one-cleanup-deadline requirement explicit at Worker dispatch and reconciles proved-clean removal versus retained ownership. Original evidence is preserved; refresh affected checks after implementation."
    },
    {
      "id": "R5-MCP-READY-DISPATCH-W42",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS",
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Reviewed all existing obligations, actual blocking dispatch, Qt6.5 deadline condition support, retained owner lifecycle, retry accumulation and test/reuse boundaries. This review is not dispatch behavior proof.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "sha256": "8bda95daa76c09e94dd6d59ccd848da9b606773d5fa9617e6b4bd3e8fd5a36a2",
        "path": "build/wave42-dispatch-readiness.md"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "f13ae22417a6f787d3e8f7800bb50690101e0fab00c8ba434a35e1ec1ebdcbed"
        }
      ],
      "scope_sha256": "ae26ce7c5bdbda11d98c1d1363eda3390d5729cf1f4d8e0d05502a2474b765fd",
      "red": null,
      "reason": "Scope8 adds real private-pipe caps and total request write/read deadline. Original artifacts and hashes stay historical; rerun applicable behavior and red checks on final code."
    },
    {
      "id": "R5-MCP-DISPATCH-REPRO-W42",
      "acceptance": [
        "R5-MCP-AC-CLEANUP"
      ],
      "kind": "behavior",
      "status": "fail",
      "command": [
        "cmake",
        "-E",
        "env",
        "--modify",
        "PATH=path_list_prepend:C:/Qt/6.10.3/msvc2022_64/bin",
        "build/Release/test_ai_mcp_stdio_session.exe",
        "cleanupDeadlineIncludesWorkerDispatch",
        "-o",
        "build/wave42-dispatch-before-qt.txt,txt"
      ],
      "method": "Actual maintained-test probe built successfully against unchanged production. A real Worker callback held dispatch for an independently bounded500ms; close with30ms deadline returned only after the hold, with the join-timeout diagnostic instead of the required dispatch-timeout diagnostic. Qt reported2pass/1fail in516ms, not a harness timeout. Probe source retained at build/wave42-dispatch-regression-test.cpp; newly added test removed exactly for owner-requested pause. Original test SHA restored and matching binary rebuilt; five targeted suites then passed. This is defect reproduction, not a green/red/restored behavior certification.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 1,
      "artifact": {
        "sha256": "9cbaf8bd0236f37d0dcddc9edb3626f0d18af216a25ea57ad3b27d6d47e94db4",
        "path": "build/wave42-dispatch-before-qt.txt"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c7b789c2c225072f455cc5d887fb94398f359fff6186962e311f4e3d26f41e4"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "925875b72f1b0b2f901c1bf8fa84ac9d351e60590b29b87f3fafbe126e57480f"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "include/sak/windows_process_job.h",
          "sha256": "80551ee72108d0ccbe8c9731cd31e8ce5bc16435d2840b98e9ac1536b2994a2d"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "8f5ad67d09ed76c041cb94f5bd9c8aff7fc64c496c782df8ea305ea43c87c0fe"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "b5dd774eda7ec6f9dc6a4f5b16378587d0118ca6e5dd37433971c9b0de6aa0a6"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/core/windows_process_job.cpp",
          "sha256": "88feb0f0c0975f5cac731d3dbd7327acb1515696bb7368b4c795c0bacf155fc5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "f13ae22417a6f787d3e8f7800bb50690101e0fab00c8ba434a35e1ec1ebdcbed"
        },
        {
          "path": "tests/README.md",
          "sha256": "3954123c91d523c46082ea00d72d447a0076ebabbb4ac7829e3c27131e4d4e76"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "6a5957cd615612fbfc16dda35e3f65578331fed8e6398a40eb9a1e49f0df67da"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "a985968289adb950d4c08d2cabeb1bbffad773aa52dc401fdafa96809fad46a8"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "096900fb16c4cd8d3ab58c56c326ca6cf5fc169bd3bce37651088332b8fd0cbb"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        },
        {
          "path": "tests/unit/test_windows_process_job.cpp",
          "sha256": "3a9f6472d852126077011755e6aa081e620118011018aa627aa04bbfded0c99c"
        }
      ],
      "scope_sha256": "ae26ce7c5bdbda11d98c1d1363eda3390d5729cf1f4d8e0d05502a2474b765fd",
      "red": null,
      "reason": "INCOMPLETE: BlockingQueuedConnection exceeds the one-cleanup deadline before joining. On authorized resume, implement a retained shared completion request, copied deadline and bounded wait; preserve exact pending request across repeated timeouts, propagate late errors, and prove recovery without duplicate queued cleanup."
    },
    {
      "id": "R5-MCP-READY-PIPE-BOUNDS-W43",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS",
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP",
        "R5-MCP-AC-PIPE-BOUNDS",
        "R5-MCP-AC-REQUEST-DEADLINE"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Reviewed canonical Worker/WindowsProcessJob, Qt6.5.3 and pinned6.10.3 private pipe reader/launch ordering, call sites, actual buffer behavior, one-request timer conversion, owned child and controlled line-reader oracles. Review records ABI, failure and test boundaries; it is not implementation or certification proof.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave43-mcp-pipe-readiness.md",
        "sha256": "964d227f974249052d00243344f112de27100b6cdd482af9a830c5861c95c7db"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "2be5c40b06cc52aeeae471588f208739639a4f827d925de097fd8d1b607f0d22"
        }
      ],
      "scope_sha256": "ac206a65d38a3aac77c1a157e78f8f5e3d5c89914c5ff2fc0ea302f3168b0bb5",
      "red": null,
      "reason": "Scope8 acceptance is unchanged, but tests/CMake and the implementation/test source changed after this pre-implementation review. Preserve its original artifact and perform a current readiness review before crediting it again."
    },
    {
      "id": "R5-MCP-READY-CURRENT-W44",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS",
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP",
        "R5-MCP-AC-PIPE-BOUNDS",
        "R5-MCP-AC-REQUEST-DEADLINE"
      ],
      "kind": "readiness",
      "status": "pass",
      "command": [],
      "method": "Re-reviewed all eight scope8 criteria against the live Windows-only session/Job, pool/gateway callers, Qt reader behavior, registered tests and project rules. Corrected the false capped-reader-implies-capped-QProcess assumption without weakening acceptance. This is requirements readiness, not behavioral certification.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave44-mcp-readiness.md",
        "sha256": "fe2f44d5dd1b9c8c392d1a46ef993b509c50c551647e0302c929ec47c517fc49"
      },
      "inputs": [
        {"path": ".pre-commit-config.yaml", "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"},
        {"path": "AGENTS.md", "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"},
        {"path": "CMakeLists.txt", "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"},
        {"path": "CONTRIBUTING.md", "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"},
        {"path": "docs/CODING_STANDARDS.md", "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"},
        {"path": "include/sak/layout_constants.h", "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"},
        {"path": "resources/ai/providers/providers.json", "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"},
        {"path": "src/win32mcp/browser_bridge_pipe.cpp", "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"},
        {"path": "src/win32mcp/browser_bridge_security.cpp", "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"},
        {"path": "src/win32mcp/win32_mcp_dispatch.cpp", "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"},
        {"path": "src/win32mcp/win32_mcp_tools.cpp", "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"},
        {"path": "tests/CMakeLists.txt", "sha256": "b31365040e0863de80ef01a235ad2c2d85a19639795cc96743ef2028a2432d91"}
      ],
      "scope_sha256": "ac206a65d38a3aac77c1a157e78f8f5e3d5c89914c5ff2fc0ea302f3168b0bb5",
      "red": null,
      "reason": null
    },
    {
      "id": "R5-MCP-FULL-CTEST-W44",
      "acceptance": [
        "R5-MCP-AC-OWNER",
        "R5-MCP-AC-RESULT",
        "R5-MCP-AC-NATIVE",
        "R5-MCP-AC-IDS",
        "R5-MCP-AC-LAUNCH",
        "R5-MCP-AC-CLEANUP",
        "R5-MCP-AC-PIPE-BOUNDS",
        "R5-MCP-AC-REQUEST-DEADLINE"
      ],
      "kind": "behavior",
      "status": "pass",
      "command": [
        "ctest",
        "--test-dir",
        "build/wave42-release-vs2022",
        "-C",
        "Release",
        "--output-on-failure",
        "-V",
        "--timeout",
        "300"
      ],
      "method": "Complete current-source Release build exited 0 before CTest. All 287 registered CTests passed, including native MCP session/Job, finite stdout/stderr flood backpressure, deadline and cleanup cases. The separate skip audit found 25 reviewed skips, not proof of those external paths.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave44-full-ctest.log",
        "sha256": "8768ff9eb65ef664e332b05522127af6289b33e8e1deb06edd6712d2830daaba"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c7b789c2c225072f455cc5d887fb94398f359fff6186962e311f4e3d26f41e4"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "6f3f92f77b16ad3f4feaa4c122c77d32006f28dd904f448d621debcaeffb1b33"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "include/sak/windows_process_job.h",
          "sha256": "2dfe276afb418754b73ce9108c0fd50f18685efc84fb96c285619ca81741223f"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "8f5ad67d09ed76c041cb94f5bd9c8aff7fc64c496c782df8ea305ea43c87c0fe"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "7a8e75b519aba6dbc954e3b41917b66c363dc1f4c0ca7ea969f80be50e033b57"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/core/windows_process_job.cpp",
          "sha256": "c6ea77ea6d563f6cb82bb4103050a2f75fbd0ac141b19197c0cc5a822408bf04"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "b31365040e0863de80ef01a235ad2c2d85a19639795cc96743ef2028a2432d91"
        },
        {
          "path": "tests/README.md",
          "sha256": "e9c98eb79586784a1ac69a35a70c09d972ce6e8d49bd22542caef3bcd8449162"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "6a5957cd615612fbfc16dda35e3f65578331fed8e6398a40eb9a1e49f0df67da"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "a985968289adb950d4c08d2cabeb1bbffad773aa52dc401fdafa96809fad46a8"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "981c16fa069600d0861ae21814366f3e54d4247e4056bd39741c3e998928bf8c"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        },
        {
          "path": "tests/unit/test_windows_process_job.cpp",
          "sha256": "65e6a1c3907461fb3d748b932b8612785885dd33eca74c698ecf2ca5d9114d12"
        }
      ],
      "scope_sha256": "ac206a65d38a3aac77c1a157e78f8f5e3d5c89914c5ff2fc0ea302f3168b0bb5",
      "red": null,
      "reason": null
    },
    {
      "id": "R5-MCP-PIPE-STDOUT-RED-W44",
      "acceptance": [
        "R5-MCP-AC-PIPE-BOUNDS"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave43-pipe-cap-drills/run.ps1",
        "-RunName",
        "guard-final-v2",
        "-Execute"
      ],
      "method": "Copied WindowsJob source with stdout reader cap disabled and matching setup assertion changed so the real owned flood reached the intended backpressure assertion. Baseline and byte-restored source built and passed; mutant built and failed exactly one named Qt row. The same batch separately drills stderr.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave43-pipe-cap-drills/guard-final-v2/stdout-reader-cap-disabled-mutant-qt.txt",
        "sha256": "2e42a035b520be8f6a747b214fb1bdb7055bcb8d861970a58748d88c27d19ff1"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c7b789c2c225072f455cc5d887fb94398f359fff6186962e311f4e3d26f41e4"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "6f3f92f77b16ad3f4feaa4c122c77d32006f28dd904f448d621debcaeffb1b33"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "include/sak/windows_process_job.h",
          "sha256": "2dfe276afb418754b73ce9108c0fd50f18685efc84fb96c285619ca81741223f"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "8f5ad67d09ed76c041cb94f5bd9c8aff7fc64c496c782df8ea305ea43c87c0fe"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "7a8e75b519aba6dbc954e3b41917b66c363dc1f4c0ca7ea969f80be50e033b57"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/core/windows_process_job.cpp",
          "sha256": "c6ea77ea6d563f6cb82bb4103050a2f75fbd0ac141b19197c0cc5a822408bf04"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "b31365040e0863de80ef01a235ad2c2d85a19639795cc96743ef2028a2432d91"
        },
        {
          "path": "tests/README.md",
          "sha256": "e9c98eb79586784a1ac69a35a70c09d972ce6e8d49bd22542caef3bcd8449162"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "6a5957cd615612fbfc16dda35e3f65578331fed8e6398a40eb9a1e49f0df67da"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "a985968289adb950d4c08d2cabeb1bbffad773aa52dc401fdafa96809fad46a8"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "981c16fa069600d0861ae21814366f3e54d4247e4056bd39741c3e998928bf8c"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        },
        {
          "path": "tests/unit/test_windows_process_job.cpp",
          "sha256": "65e6a1c3907461fb3d748b932b8612785885dd33eca74c698ecf2ca5d9114d12"
        }
      ],
      "scope_sha256": "ac206a65d38a3aac77c1a157e78f8f5e3d5c89914c5ff2fc0ea302f3168b0bb5",
      "red": {
        "mutation": "Disable the actual stdout QWindowsPipeReader cap while preserving launch; the owned producer reaches its target without backpressure.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "MCP stdout producer reached target without backpressure",
        "observed_diagnostic": "MCP stdout producer reached target without backpressure",
        "before": [
          {
            "path": "src/core/windows_process_job.cpp",
            "sha256": "c6ea77ea6d563f6cb82bb4103050a2f75fbd0ac141b19197c0cc5a822408bf04"
          }
        ],
        "mutated": [
          {
            "path": "src/core/windows_process_job.cpp",
            "sha256": "efba77bac82be7734829ab7c9bbc309fd39a6ee843dad9255c6c7ebfa3fdd09f"
          }
        ],
        "after": [
          {
            "path": "src/core/windows_process_job.cpp",
            "sha256": "c6ea77ea6d563f6cb82bb4103050a2f75fbd0ac141b19197c0cc5a822408bf04"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-MCP-LATE-MATCH-RED-W44",
      "acceptance": [
        "R5-MCP-AC-REQUEST-DEADLINE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave43-bounds-drills/run.ps1",
        "-RunName",
        "guard-final-v2",
        "-Execute"
      ],
      "method": "Copied session source with both post-read and post-parse deadline checks bypassed; a delayed matching line was incorrectly accepted. The named test was green before, red only for this built mutant, and green after exact byte restoration. Four other current-source boundary mutants are retained in the same batch; the two post-line checks are not yet separately mutation-isolated.",
      "environment": {
        "platform": "windows",
        "tools": {
          "python": "3.14.0",
          "git": "2.52.0.windows.1",
          "cmake": "3.31.3",
          "qt": "6.10.3.0",
          "msvc": "19.44.35222.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave43-bounds-drills/guard-final-v2/late-matching-line-deadline-bypass-mutant-qt.txt",
        "sha256": "ef5fd9ba4e882c3891391e82f66cc890bd080a20826cecd8c1a27412e14d9cd0"
      },
      "inputs": [
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
        },
        {
          "path": "CMakeLists.txt",
          "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
        },
        {
          "path": "CONTRIBUTING.md",
          "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
        },
        {
          "path": "docs/CODING_STANDARDS.md",
          "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
        },
        {
          "path": "include/sak/ai/ai_mcp_jsonrpc.h",
          "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
        },
        {
          "path": "include/sak/ai/ai_mcp_session_pool.h",
          "sha256": "0c7b789c2c225072f455cc5d887fb94398f359fff6186962e311f4e3d26f41e4"
        },
        {
          "path": "include/sak/ai/ai_mcp_stdio_session.h",
          "sha256": "6f3f92f77b16ad3f4feaa4c122c77d32006f28dd904f448d621debcaeffb1b33"
        },
        {
          "path": "include/sak/layout_constants.h",
          "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
        },
        {
          "path": "include/sak/windows_process_job.h",
          "sha256": "2dfe276afb418754b73ce9108c0fd50f18685efc84fb96c285619ca81741223f"
        },
        {
          "path": "resources/ai/providers/providers.json",
          "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
        },
        {
          "path": "src/ai/ai_mcp_session_pool.cpp",
          "sha256": "8f5ad67d09ed76c041cb94f5bd9c8aff7fc64c496c782df8ea305ea43c87c0fe"
        },
        {
          "path": "src/ai/ai_mcp_stdio_session.cpp",
          "sha256": "7a8e75b519aba6dbc954e3b41917b66c363dc1f4c0ca7ea969f80be50e033b57"
        },
        {
          "path": "src/ai/ai_provider_gateway.cpp",
          "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
        },
        {
          "path": "src/core/windows_process_job.cpp",
          "sha256": "c6ea77ea6d563f6cb82bb4103050a2f75fbd0ac141b19197c0cc5a822408bf04"
        },
        {
          "path": "src/win32mcp/browser_bridge_pipe.cpp",
          "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
        },
        {
          "path": "src/win32mcp/browser_bridge_security.cpp",
          "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
        },
        {
          "path": "src/win32mcp/win32_mcp_dispatch.cpp",
          "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
        },
        {
          "path": "src/win32mcp/win32_mcp_entry.cpp",
          "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
        },
        {
          "path": "src/win32mcp/win32_mcp_tools.cpp",
          "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "b31365040e0863de80ef01a235ad2c2d85a19639795cc96743ef2028a2432d91"
        },
        {
          "path": "tests/README.md",
          "sha256": "e9c98eb79586784a1ac69a35a70c09d972ce6e8d49bd22542caef3bcd8449162"
        },
        {
          "path": "tests/skip_baseline.txt",
          "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
        },
        {
          "path": "tests/skip_site_inventory.txt",
          "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
        },
        {
          "path": "tests/support/ai_mcp_native_fixture.h",
          "sha256": "6a5957cd615612fbfc16dda35e3f65578331fed8e6398a40eb9a1e49f0df67da"
        },
        {
          "path": "tests/unit/test_ai_mcp_session_pool.cpp",
          "sha256": "a985968289adb950d4c08d2cabeb1bbffad773aa52dc401fdafa96809fad46a8"
        },
        {
          "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
          "sha256": "981c16fa069600d0861ae21814366f3e54d4247e4056bd39741c3e998928bf8c"
        },
        {
          "path": "tests/unit/test_fuzz_mcp_framing.cpp",
          "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
        },
        {
          "path": "tests/unit/test_windows_process_job.cpp",
          "sha256": "65e6a1c3907461fb3d748b932b8612785885dd33eca74c698ecf2ca5d9114d12"
        }
      ],
      "scope_sha256": "ac206a65d38a3aac77c1a157e78f8f5e3d5c89914c5ff2fc0ea302f3168b0bb5",
      "red": {
        "mutation": "Bypass both deadline checks surrounding parseMatchingLine after bounded line read.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "matchingLineCompletedAfterDeadlineIsRejected() '!timed_out.has_value()' returned FALSE",
        "observed_diagnostic": "matchingLineCompletedAfterDeadlineIsRejected() '!timed_out.has_value()' returned FALSE",
        "before": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "7a8e75b519aba6dbc954e3b41917b66c363dc1f4c0ca7ea969f80be50e033b57"
          }
        ],
        "mutated": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "8b18f4e856398becf7fb1dee78c3696c9b133c76a19492283f3185c1e69e690b"
          }
        ],
        "after": [
          {
            "path": "src/ai/ai_mcp_stdio_session.cpp",
            "sha256": "7a8e75b519aba6dbc954e3b41917b66c363dc1f4c0ca7ea969f80be50e033b57"
          }
        ]
      },
      "reason": null
    }
  ],
  "checkpoint": {
    "scope_sha256": "ac206a65d38a3aac77c1a157e78f8f5e3d5c89914c5ff2fc0ea302f3168b0bb5",
    "inputs": [
      {
        "path": ".pre-commit-config.yaml",
        "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
      },
      {
        "path": "AGENTS.md",
        "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
      },
      {
        "path": "CMakeLists.txt",
        "sha256": "2b70e9ae4e0b8bc12d70ca15e6fbb0f277970b99f5a33ec1cc5dbb9904a2d4f0"
      },
      {
        "path": "CONTRIBUTING.md",
        "sha256": "a5b92c7b33b1985929ea6a708b64a56f923ecd6068a81dfd4608aae8a426f922"
      },
      {
        "path": "docs/CODING_STANDARDS.md",
        "sha256": "632d1a20a2b36032a474bd1deed4c15d29bd8a1a4270bfd813bba24dbf4bb599"
      },
      {
        "path": "include/sak/ai/ai_mcp_jsonrpc.h",
        "sha256": "10664f3e6fb26b59ebc0bb8bbbf7fb96d32abfd6bdac368576941baa7123752c"
      },
      {
        "path": "include/sak/ai/ai_mcp_session_pool.h",
        "sha256": "0c7b789c2c225072f455cc5d887fb94398f359fff6186962e311f4e3d26f41e4"
      },
      {
        "path": "include/sak/ai/ai_mcp_stdio_session.h",
        "sha256": "6f3f92f77b16ad3f4feaa4c122c77d32006f28dd904f448d621debcaeffb1b33"
      },
      {
        "path": "include/sak/layout_constants.h",
        "sha256": "a777c9ab709c1fdb540dde30e650533146039272317143efb6fb18371b6c3294"
      },
      {
        "path": "include/sak/windows_process_job.h",
        "sha256": "2dfe276afb418754b73ce9108c0fd50f18685efc84fb96c285619ca81741223f"
      },
      {
        "path": "resources/ai/providers/providers.json",
        "sha256": "99b4fb64967d4a2c310532075a11915d97768ff61da8a93120a18f9464e03db5"
      },
      {
        "path": "src/ai/ai_mcp_session_pool.cpp",
        "sha256": "8f5ad67d09ed76c041cb94f5bd9c8aff7fc64c496c782df8ea305ea43c87c0fe"
      },
      {
        "path": "src/ai/ai_mcp_stdio_session.cpp",
        "sha256": "7a8e75b519aba6dbc954e3b41917b66c363dc1f4c0ca7ea969f80be50e033b57"
      },
      {
        "path": "src/ai/ai_provider_gateway.cpp",
        "sha256": "8e343d56e8af556e80bc6080e4d0d313c53ae55bd6e65dd4186cf2375eccdbcc"
      },
      {
        "path": "src/core/windows_process_job.cpp",
        "sha256": "c6ea77ea6d563f6cb82bb4103050a2f75fbd0ac141b19197c0cc5a822408bf04"
      },
      {
        "path": "src/win32mcp/browser_bridge_pipe.cpp",
        "sha256": "e57028d35175d2327f1513c6e76fafb9dd43bb00e0ed8d47580a02bc341308f9"
      },
      {
        "path": "src/win32mcp/browser_bridge_security.cpp",
        "sha256": "2a691d2a8cc6310aa0b956b5141083d778edd91ce5e11e6d77c01889fe1f0a0a"
      },
      {
        "path": "src/win32mcp/win32_mcp_dispatch.cpp",
        "sha256": "0260ee853a1551d7154618bbb01d1b0635335460460536cadc7a25a95d235b58"
      },
      {
        "path": "src/win32mcp/win32_mcp_entry.cpp",
        "sha256": "35f15f4ecb8149752b643bb3dd3bd3296ab10fbd39d59f690717abd1ed96575c"
      },
      {
        "path": "src/win32mcp/win32_mcp_tools.cpp",
        "sha256": "f0b9f4966017aab835dce767737c88eea5498283506772c6272ca49bf1bb1d0f"
      },
      {
        "path": "tests/CMakeLists.txt",
        "sha256": "b31365040e0863de80ef01a235ad2c2d85a19639795cc96743ef2028a2432d91"
      },
      {
        "path": "tests/README.md",
        "sha256": "e9c98eb79586784a1ac69a35a70c09d972ce6e8d49bd22542caef3bcd8449162"
      },
      {
        "path": "tests/skip_baseline.txt",
        "sha256": "447c4de4e637675fee0bd27f9f2b6d73b5c6933ed5e4c8d0f0cb9b1367d9a307"
      },
      {
        "path": "tests/skip_site_inventory.txt",
        "sha256": "aeca9d06faedd254f4a85b38c591e164ec0d5e852804e97445854becd784b627"
      },
      {
        "path": "tests/support/ai_mcp_native_fixture.h",
        "sha256": "6a5957cd615612fbfc16dda35e3f65578331fed8e6398a40eb9a1e49f0df67da"
      },
      {
        "path": "tests/unit/test_ai_mcp_session_pool.cpp",
        "sha256": "a985968289adb950d4c08d2cabeb1bbffad773aa52dc401fdafa96809fad46a8"
      },
      {
        "path": "tests/unit/test_ai_mcp_stdio_session.cpp",
        "sha256": "981c16fa069600d0861ae21814366f3e54d4247e4056bd39741c3e998928bf8c"
      },
      {
        "path": "tests/unit/test_fuzz_mcp_framing.cpp",
        "sha256": "b3f1c664f2cbc0921f2395b096cb83a16de7cf02ac22247e1054a38981f39f00"
      },
      {
        "path": "tests/unit/test_windows_process_job.cpp",
        "sha256": "65e6a1c3907461fb3d748b932b8612785885dd33eca74c698ecf2ca5d9114d12"
      }
    ],
    "environment": {
      "platform": "windows",
      "tools": {
        "python": "3.14.0",
        "git": "2.52.0.windows.1",
        "cmake": "3.31.3",
        "qt": "6.10.3.0",
        "msvc": "19.44.35222.0"
      }
    },
    "verified_tasks": [],
    "pending_operations": [],
    "next_action": "Scope8 code is implemented: atomic Windows Job launch, retained bounded cleanup dispatch/join, direct capped stdout/stderr readers, and one write/read deadline. Current nested VS2022 Release build exited0 and complete CTest passed287/287 in 1340.69s; the skip audit passed25 reviewed skips. Copied current-source drills passed five response/deadline cases, two channel-cap cases and four cleanup-dispatch cases with intended red and exact restoration. These are local proofs, not full app, VM, hardware or release certification. The task remains implemented, not verified: refresh stale owner/result/native/ID/launch red evidence on this scope, independently drill post-read versus post-parse deadline checks, and reconcile all acceptance with a current readiness review. Seven vendor LZFSE D9025 overrides, ProcessRunner descendant cleanup, staging path-swap risk and other R5 programs remain open. No archive move, commit or push. Future plans parked.",
    "source_revision": "65fa4c58f251f4b5949797a74e71a80f5e7e4e60"
  }
}
```
