# GUI Startup and Teardown Field Notes

Field notes for the Qt GUI: how startup cost was actually located, how a destruction-order
crash was found, and what the non-interactive startup gates really cover.

The fixes themselves live in code, each with its rationale in a comment next to it. This file
does not repeat them -- it points at them and records what a code comment has no room for: the
false trails, the measurements that mislead, and the technique.

## Measure startup; never reason about it

Every startup phase logs its own elapsed milliseconds. `showMainWindow` in `src/main.cpp` times
main-window construction and show; `MainWindow::setupUi` in `src/gui/main_window.cpp` times
setupUi, window-state load, status bar, log window, panels, lazy-tab placeholders, help and
about. Read the newest log under `sak::app_paths::logsDirectory()` (portable `data/logs` beside
the exe when that is writable).

`MainWindow::createStatusBar` reports the family count and how long its forced
query waited for the font database. This is the first explicit startup family
query, so it exposes the long native stall. The W51 experiment removed the
query and eager warmup, but was reverted after inconclusive whole-startup
comparison; those logs are historical, not the current implementation.

**Offscreen timing is not desktop timing.** `QT_QPA_PLATFORM=offscreen` takes a different font
path, and the desktop-only font cost never reproduced headless. `scripts/check_startup_budget.ps1`
runs its budget gate offscreen, so treat a pass as a guard against gross regressions (hangs,
quadratic work) and not as evidence that the app starts quickly on a real desktop. A
desktop-only startup regression will still be noticed by a human first.

## Font database traps

The warmup lives in `startFontDatabaseWarmup()` in `src/main.cpp`. The W51
candidate removed it under the owner's conditional test approval, then restored
it byte-for-byte when the evidence did not meet that condition. The former
`forceGdiFontDatabase()` set an ineffective runtime variable and has been
removed; its historical comment was not evidence that GDI was selected.
The current startup failure and backend comparison are tracked in R5. What earlier probes missed:

| Trap | Why it misleads | Defence |
| --- | --- | --- |
| The alias-table cost migrates between probes | Building the family alias table is a *second* full walk of every installed family, separate from `QFontDatabase::families()`. It is triggered by the first font MATCH that misses -- a style-sheet fallback stack naming a family that is not installed -- so it attaches to whichever font operation happens next and looks like a different panel's cost on every run. | Never attribute it to the panel the timing log blames. Confirm by moving the probe. |
| An existence check does not trigger it | `QFontDatabase::hasFamily()` does not build the alias table; only a real match does. | The warmup constructs `QFontMetrics(QFont("sak-alias-warmup-probe")).height()` rather than a cheaper query. The W51 experiment temporarily removed this synthetic work; actual UI font matching remained. |
| A freshly built exe can pay an antivirus tax | A first-run delay alone does not establish its cause. Font backend choice and file/cache state can also differ. | Compare the same binary with explicit backend evidence; do not dismiss a startup failure as antivirus cost without measurements. |
| An unsupported environment variable is a no-op even at the right time | Qt 6.10.3's Windows integration chooses the font database from parsed platform options, not the runtime `QT_NO_DIRECTWRITE` variable formerly used by the app. | Verify the pinned Qt implementation and actual selected backend. Setting the variable before `QApplication` did not prove a GDI selection. |
| The warmup thread must be owned and joined | Detached, it touches font-database internals while `QApplication` is being destroyed. It presents as heap corruption at process exit, on a stack with nothing to do with fonts. | The current code uses `qAddPostRoutine(joinFontDatabaseWarmup)`; W51 removed the thread only for its reverted experiment. |

Pinned source: [Qt 6.10.3 Windows integration](https://raw.githubusercontent.com/qt/qtbase/v6.10.3/src/plugins/platforms/windows/qwindowsintegration.cpp)
parses `fontengine=gdi` and `nodirectwrite` as platform options before choosing its database.
The supported argument forms are documented by
[QGuiApplication](https://doc.qt.io/qt-6.10/qguiapplication.html#QGuiApplication).
No backend change or timeout relaxation is certified by this source inspection alone.

The 2026-09-06 same-binary native A/B/A comparison did not justify a backend switch. After the
original 40.433-second font wait, unchanged default-backend runs completed the startup gate in
5.436 and 5.176 seconds. The explicit GDI arm completed in 8.996 seconds: its families query was
faster, but status-bar work was slower. Keep the original failed first-run evidence; warm passes
do not prove that its cause is fixed. All arms retained the same 45-second deadline and real
teardown, with no service, antivirus or font-cache change. Evidence is under
build/wave32-font-backend-2fd6351917f140fc9076a216c9479873.

## Font-startup experiment -- reverted after inconclusive comparison

The owner approved testing removal of eager font enumeration/warmup on 2026-09-06, keeping the
change only if verified. The fresh EA3EF304...1231089A package failed the existing 30-second native
startup gate: its forced family census consumed 26.405 seconds, and status-bar construction took
29.084 seconds. That failure remains under build/pk32-final-bc5bbc12; a later warm pass would not
erase it. Baseline source bytes are retained under build/wave32-font-lazy-experiment-e67bf3a4.

The experiment removed the census, the synthetic missing-family match and their owned warmup
thread. It left font rendering/theming, the current backend choice and real startup/status-bar
timing intact. Qt's own initialization may still be expensive: the pinned
[family-list implementation](https://raw.githubusercontent.com/qt/qtbase/v6.10.3/src/gui/text/qfontdatabase.cpp)
locks and initializes its database, and the pinned
[DirectWrite initializer](https://raw.githubusercontent.com/qt/qtbase/v6.10.3/src/gui/text/windows/qwindowsdirectwritefontdatabase.cpp)
walks the system font collection and additional names. Removing manual calls is therefore an
experiment, not proof that Qt avoids enumeration.

The candidate passed fresh native package tests in 16.239 seconds on Windows PowerShell 5.1 and
6.213 seconds on PowerShell 7, both under the unchanged 30-second cap, restoring each exact package
tree. Those first-run timings alone do not establish causation. A matched Windows PowerShell 5.1
ABBA comparison measured baseline 6.717/9.060 seconds versus candidate 8.303/7.305 seconds. Means
of 7.888 and 7.804 seconds are within the observed variation, not a demonstrated improvement.
Per the owner's conditional authorization, the experiment was reverted. Both source files match
their exact saved baseline hashes. Evidence: build/font-abba-9c16c6b0/results.json and
build/pk32-final-d7ef82e7/proof.json. The intermittent first-start delay remains INCOMPLETE;
neither antivirus attribution nor a backend change is established by these observations.

Wave48 current-source observation, 2026-09-24: a fresh 157-file native Windows
stage timed out under the unchanged 30-second startup smoke bound, with no
success marker and a zero-byte application log. A second fresh stage with the
identical app SHA256 passed, as did fresh ZIP extraction and Windows 11 VM
startup. Those passes do not erase the first failure or establish an antivirus,
font-backend, or MCP cause. The on-timeout phase is not observable in the
zero-byte log; capture a bounded live phase trace on a future cold run before
changing font initialization or relaxing the startup gate. Evidence and exact
hashes: `build/wave48-transaction-package-vm-evidence.md`.

Current diagnostic follow-up: smoke-mode startup now emits three flushed stderr
milestones around Qt application construction and app initialization. It also
flushes the application log before main-window construction and on both sides
of the first font database query. The portable smoke gate requires the stderr
milestones in order; its 41 mocked contracts and copied-guard red drill pass.
This is observability, not a font fix. Current compiled-package/native proof
for these new source bytes was attempted: the fresh W49 native package timed
out at the unchanged 30-second limit while the complete CTest suite was
running. Stderr shows Qt app construction and app initialization both finished;
the flushed log ends at `entering font database query`, before `font database
ready`. This narrows the stall to the first GUI-thread family query or its
shared font initialization, but does not distinguish warmup, backend, cache,
antivirus, or concurrent load. After the full 287/287 CTest run, independent
fresh stages with the same binary passed native smoke under PowerShell 7 and
Windows PowerShell 5.1 without test load. Those passes do not erase the timed
out first run. See `build/wave49-startup-phase-evidence.md`.

An uncontended fresh default-backend extraction then independently timed out
at the same first font query, while a fresh explicit-GDI arm completed in
12.557 seconds. A following same-binary fresh-extract A/B/B/A comparison had
all four arms pass within 8.324 seconds or less. That mixed result is evidence
to keep investigating, not evidence to switch the product backend or relax the
deadline. The no-op `QT_NO_DIRECTWRITE` override and its escape hatch were
then removed with a source-contract red/green drill under PowerShell 7 and
5.1; this changes no Qt backend on the pinned build. Full Release/package
proof for those final source bytes is still required. The actual font-query
timeout remains open.

W50 confirms the no-op override cleanup did not fix startup: full Release and
287/287 CTests passed, as did Windows 11 VM native/accessibility checks, but
the first fresh local ZIP extraction timed out at the unchanged 30-second
native gate. Its forced GUI-thread family query eventually returned 2491
families after 28839 ms, leaving no time for clean shutdown. Evidence:
`build/wave50-baseline-evidence.md`. The approved lazy-font change must be
tested against this real failure, not assumed effective from a warm pass.
The W51 source contract was red on W50's eager census and green after the
candidate removal on PowerShell 7 and 5.1. Its targeted build and 12-arm
fresh-ZIP A/B/B/A run passed, but mean whole-startup times differed by only
0.043 seconds and neither arm reproduced the rare timeout. The removal was
reverted to exact W50 source hashes, along with the candidate-only contract.
See `build/wave51-lazy-font-experiment.md`. Startup reliability remains open.

W67c recurrence, 2026-09-25: after a complete 300/300 local CTest gate and a
fresh single-use 157-file portable stage, the default 30-second native smoke
timed out. The staged app log stopped at `MainWindow startup: entering font
database query`, before its forced `QFontDatabase::families()` census returned.
The captured stderr reached `phase=app-initialized`; no `sak_utility` process
remained after the smoke, but one log and empty temp/crash directories did.
The source still performs a background families query plus alias-font match
in `main.cpp` and a forced GUI-thread families census in `main_window.cpp`.
W51's 12-arm mean comparison did not verify a benefit from removing them,
and this fresh failure does not alone prove which Qt/font-cache stage stalls.
Do not raise the 30-second gate or claim native startup reliability. Evidence:
`build/wave67c-package-native.log`.

## Wave 68 native startup diagnosis -- not a certified fix

The owner allowed removing the eager font warmup/census only if verified.
Removing both passed a targeted build but failed a fresh native extraction;
the experiment was restored. A narrower trial removed only the synthetic
missing-family alias match while retaining the family census. Its native
30-second smoke also failed and the match was restored. The currently
live source therefore retains both warmup and census. Targeted builds
do not replace a full Release build, complete CTest or current-binary package
and VM proof.

Several widget tree changes reduced local construction costs: the AI Assistant,
Benchmark and Diagnostics, App Installation, and Application Management use
empty attached shells before populated children. Native smoke displays the
initial shell before materializing all lazy tabs; it requires the all-panels
marker before success. Headless smoke and accessibility audit still construct
all tabs before their checks. The portable runner's 42 mocked contracts and
its copied missing-phase and missing-all-panels red drills passed. These are
contract checks, not a native package pass.

The default 30-second native gate remains red. A temporary diagnostic Qt
`notify()` probe measured a 31,328 ms `QEvent::MetaCall` while building tabs
and a 47,563 ms `QEvent::LayoutRequest` on MainWindow afterward. Both event
numbers were checked against pinned Qt 6.10.3 `qcoreevent.h`. Pinned Qt
[QTabWidget source](https://raw.githubusercontent.com/qt/qtbase/v6.10.3/src/widgets/widgets/qtabwidget.cpp)
queries each tab's size hint, and
[QStackedLayout source](https://raw.githubusercontent.com/qt/qtbase/v6.10.3/src/widgets/kernel/qstackedlayout.cpp)
also walks its pages. A temporary per-tab probe measured the first File
Management `sizeHint()` at 51,334 ms; its Advanced Search sub-tab then stalled
in a separate run. A deeper probe stopped at the Regex Patterns button's
first size hint. Replacing that text chevron with an existing SVG icon did not
make whole native startup pass and was restored. The slow call can migrate
between widgets; do not call that button the root cause without a controlled
same-binary comparison. All temporary event/hint probe source was restored
byte-for-byte. The associated raw logs are under the active Release tree's
`data/logs` with timestamps 12-00-12, 12-08-53, 12-13-16 and 12-24-23 on
2026-09-25; they are diagnostic, not portable-package receipts.

The same native campaign also revealed `scripts/process_capture.ps1` failing
closed when its `taskkill /T /F` utility exceeded the unchanged 5-second
cleanup budget. A test-owned app process sometimes remained alive briefly.
This reopens R5-IDX-39 despite its earlier scoped fixture passes. Windows
Performance Recorder's CPU profile could not start on this host because
system performance profiling policy returned `0xc5585011`; the in-process
event probe provided narrower evidence without changing host policy.

## QApplication argument ownership

Qt retains `argc` by reference and requires both argument inputs to stay valid through application
destruction: [Qt 6.10.3 constructor contract](https://doc.qt.io/qt-6.10/qguiapplication.html#QGuiApplication).
The old `initializeApp(int argc, ...)` constructed a static QApplication from its local copy;
returning left that reference dangling. `runApplication` now owns a local QApplication alongside
its argument-count parameter, and `initializeApp(QApplication&, ...)` only configures it.

The existing startup-budget regression script includes a narrow source-ownership contract. It
rejects the original source before launching anything and accepts the fixed structure on Windows
PowerShell 5.1 and PowerShell 7. Its two positive controls and seventeen exact-reason negative
controls are structural evidence, not a C++ semantic analyzer or an undefined-behavior-dependent
test. The fixed app builds successfully. The later Wave 32 teardown cleanup removes both
`std::_Exit` success shortcuts and their ignored flush results. Headless startup and accessibility
audit now return normally through MainWindow and QApplication destruction. Their separate runtime
gates pass with the same app hash, unchanged limits and no remaining app process; evidence is under
build/wave32-teardown-runtime-c2448695c39744bb9da1639a1dfde11f. Native desktop and full-suite proof
remain separate requirements.

## Fatal reporting after application destruction

The lifetime correction also changes the exception boundary: `runApplication` destroys its
QApplication before its caller handles a failure. The former `main` catches then tried to create
a Qt message box without a live application. Fatal reporting now uses a private Qt-free `noexcept`
boundary in `src/main.cpp`. It writes and checks stderr, invokes the Qt-independent critical logger,
and uses a fixed native Windows notice only for interactive launches. Native notice failure retains
the immediate Windows error and reports it through the checked diagnostic path. If every output
channel is unavailable, the process still fails; no visible or durable artifact is promised.

The existing startup-budget regression extracts the actual production boundary and its helpers
into an isolated CMake executable. Only diagnostic outputs and the application runner are injected.
The runner's local ownership sentinel proves reporting follows stack unwinding; native notice
callbacks are nonmodal, so this does not prove a rendered dialog or real QApplication teardown.
Each explicit diagnostic mode stays noninteractive even when the runner rewrites argv before
throwing. Standard and unknown Release exceptions, exact argument matching, stderr write/flush
errors, immediate last-error capture, sink bindings and bounded failure reporting are checked.

Run the narrow compiled probe under either Windows PowerShell 5.1 or PowerShell 7:

```powershell
& scripts/test_startup_budget.ps1 -FatalRuntimeOnly
& scripts/test_startup_budget.ps1 -FatalRuntimeOnly -FatalMutationDrill
```

The second invocation independently reverses production guards in extracted source copies. Each
mutant must compile under `/W4 /WX` and fail its own runtime assertion; compile failures earn no
coverage credit. Separate lexer-based checks reject Qt use in each fatal helper and missing catches.
The live source is never planted with a mutant. These checks augment the existing complete Release,
CTest, real-app startup, accessibility and native-package gates; they do not replace those gates.
Remaining release status belongs to `RELEASE_READINESS.md`; the archived R5 campaign history is `archive/CODEX_REVIEW_5_REMEDIATION.md`.

## Disproven -- do not re-chase

- **Pre-matching the real UI fonts off-thread.** The warmup thread and the GUI thread serialize
  on Qt's shared font mutex, and there is too little other startup work to overlap with, so the
  cost only moves.
- **`QGraphicsDropShadowEffect`** as a startup cost.
- **Application-wide stylesheet matching** as a startup cost.
- **Deferring widget polish to `show()`**.

Two related questions are already settled in code: a splash shown with `show()` alone never
paints before `app.exec()` (see `SplashScreen::showCentered`), and the splash close must be
queued so it outlives the first panel's construction (see the single-shot in `showMainWindow`).

## Keep synchronous I/O out of panel constructors

A constructor that expands a tree root performs a synchronous directory listing for a tab nobody
has looked at yet. `AdvancedSearchPanel` defers that to `showEvent` -- see
`populateFileExplorerRoot` in `src/gui/advanced_search_panel.cpp`. Look for the same shape in any
new panel: filesystem, registry, or adapter enumeration reachable from a constructor is startup
cost paid for a hidden widget.

Panels must also not start worker threads under the non-interactive gates. See the
`non_interactive_gate` check in `src/gui/network_diagnostic_panel.cpp`: the gate tears the panel
down moments later and would race the scan.

## The destruction-order crash in ~MainWindow

Why the flag exists is documented beside `m_shutting_down` in `include/sak/main_window.h`. Three
things that comment cannot convey:

**A null check is not a defence.** The tool panels are `unique_ptr` members freed in the
member-destruction phase, but their raw pointers are not cleared, so a handler reached from the
later base-destructor teardown sees a pointer that is non-null *and* dangling. Only an explicit
"teardown has begun" flag works, and it must be set first in `~MainWindow`, before anything else
in the destructor body can run.

**Every handler reachable from `currentChanged` needs the guard, not just the obvious one.** In
tree that is `onTabChanged`, the lazy `materializeTab` lambda, `updateVulnerabilityStatusBarVisibility`,
`updateAiStatusBarVisibility`, and `appendLogIfActive` -- anything that resolves a panel pointer
through `findPanelTabIndex`, or writes a per-tab container that is itself destroyed early because
of member declaration order.

**The crash happens after the app looks like it succeeded.** `runMainEventLoop` logs
`Application shutting down with exit code: 0` and prints `SAK_STARTUP_SMOKE_OK` before
`MainWindow` is destroyed. A log that ends in a clean exit code proves nothing about teardown.

## Technique: finding a teardown or timing-dependent crash

1. **Build Debug.** `/Zi` is Debug-only (`CMakeLists.txt`), so a Release run has no PDBs and its
   stack is not worth reading.
2. **Collect the fault from the shipped reporter.** `sak::CrashReporter`
   (`src/core/crash_reporter.cpp`, installed from `main()`) writes a minidump plus a text summary
   to `sak::app_paths::crashesDirectory()` on any unhandled SEH fault. Open the `.dmp` against the
   Debug PDBs. This replaces the ad-hoc `SetUnhandledExceptionFilter` + DbgHelp `StackWalk64` probe
   used to find the `~MainWindow` crash; there is no reason to re-add one.
3. **Bisect on crash RATE, not on a run.** Destruction-order and heap-timing faults reproduce
   intermittently, so single runs lie in both directions -- a clean run is not a fix and a crash is
   not a reproduction. Drive the same build many times per arm and compare failure rates, with the
   suspected component behind a temporary environment variable so the two arms differ by exactly
   one thing.
4. **Compare native and headless runs.** Both now return through normal destruction. Before
   Wave 32, headless success used `std::_Exit(0)` and skipped every destructor, so those historical
   green runs are not teardown evidence. Headless still skips native exposure and painting; it
   cannot replace a desktop run for those behaviors.

## What the startup gates actually cover

- `--smoke-test` materializes **every** lazy panel (`isStartupSmokeMode` in
  `src/gui/main_window.cpp`), so a crash in any panel *constructor* is covered in both headless
  and desktop runs.
- Headless smoke skips `show()` (`prepareMainWindowForStartup`, `src/main.cpp`). Nothing is ever
  exposed: no `showEvent`, no paint, no realized geometry. Deferred-on-show work, layout, and
  truncation bugs are invisible to it and need a desktop run or an exposed-widget test.
- The accessibility audit now returns normally after reporting its result. Its real process exit
  is part of the gate; the written accessibility result alone still does not prove teardown.

## Widget sizing: hand-computed widths always undercount

The `windows11_theme` stylesheet adds padding to every `QToolButton` and `QPushButton`, and
stylesheet padding participates in `QStyleSheetStyle::sizeFromContents`. A width computed from
`QFontMetrics` plus a guessed padding constant is therefore always too small, and the caption
elides.

- **Ask the style, do not compute.** `sak::ui::RibbonToolButton`
  (`include/sak/ribbon_tool_button.h`).
- **Never `setFixedWidth` or `setFixedSize` on a text-bearing widget.** `setMinimumWidth` leaves
  room for a longer caption, a larger font, or a translated string.
- **Test under the production stylesheet and assert the realized geometry.** A sizing test that
  runs unstyled tests nothing, because the padding under test only exists once the stylesheet is
  applied. `sak::ui::actionButtonStyle` is a header-inline function in
  `include/sak/style_constants.h`, so a test can apply the real button style without linking the
  theme. Assert `minimumSizeHint == sizeHint`, `sizeHint` wider than the bare caption advance, and
  realized width and height `>=` `sizeHint` -- see
  `PartitionManagerPanelTests::ribbonButtonsUseIcons8SvgSources`.

## Deliberate shapes, not oversights

- Tooltip shortcut hints are **derived** from the index each tab actually occupies
  (`updateTabShortcutHints`, re-applied after every lazy materialization). Never hardcode a
  `Ctrl+N` into a tooltip: an optional tab shifts every later index.
- Sites that configure a table by hand instead of calling `sak::configureStandardTable`
  (`include/sak/widget_helpers.h`) are intentional deviations, not a missed sweep. Do not unify
  them without checking what each one changes.

See `docs/COVERAGE_BASELINE.md` for which of these paths are unit-covered and which are
manual-cert only.
