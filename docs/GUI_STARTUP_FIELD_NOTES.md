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

The load-bearing line is the one in `MainWindow::createStatusBar`: it reports the family count
and how long the GUI thread still had to wait for the font database. That is the first point in
startup that needs fonts, so it is where a font-database regression surfaces.

**Offscreen timing is not desktop timing.** `QT_QPA_PLATFORM=offscreen` takes a different font
path, and the desktop-only font cost never reproduced headless. `scripts/check_startup_budget.ps1`
runs its budget gate offscreen, so treat a pass as a guard against gross regressions (hangs,
quadratic work) and not as evidence that the app starts quickly on a real desktop. A
desktop-only startup regression will still be noticed by a human first.

## Font database traps

The mechanism and its two fixes are documented where they live: `forceGdiFontDatabase()` and
`startFontDatabaseWarmup()` in `src/main.cpp`. What those comments do not say:

| Trap | Why it misleads | Defence |
| --- | --- | --- |
| The alias-table cost migrates between probes | Building the family alias table is a *second* full walk of every installed family, separate from `QFontDatabase::families()`. It is triggered by the first font MATCH that misses -- a style-sheet fallback stack naming a family that is not installed -- so it attaches to whichever font operation happens next and looks like a different panel's cost on every run. | Never attribute it to the panel the timing log blames. Confirm by moving the probe. |
| An existence check does not trigger it | `QFontDatabase::hasFamily()` does not build the alias table; only a real match does. | This is why the warmup constructs `QFontMetrics(QFont("sak-alias-warmup-probe")).height()` rather than a cheaper query. A cheaper query moves the cost back onto the GUI thread. |
| A freshly built exe pays an antivirus tax | Real-time AV deep-scans a new binary's font-file reads, so the first run after a build is slower for reasons unrelated to the change. | Not app-fixable (code signing is the lever). Re-run the *same* binary before calling anything a regression. |
| Environment-variable timing | `QT_NO_DIRECTWRITE` is read by the platform plugin during `QApplication` init. Setting it afterwards is silently a no-op -- no warning, just the slow path. | Set it before the `QApplication` constructor, as `initializeApp` does. |
| The warmup thread must be owned and joined | Detached, it touches font-database internals while `QApplication` is being destroyed. It presents as heap corruption at process exit, on a stack with nothing to do with fonts. | `qAddPostRoutine(joinFontDatabaseWarmup)`. |

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
4. **Run non-headless.** Headless smoke calls `std::_Exit(0)` on success, which skips every
   destructor and hides teardown faults completely. `scripts/run_portable_e2e_smoke.ps1` sets
   `SAK_STARTUP_SMOKE_HEADLESS` only under CI and explicitly clears it otherwise, so a local run
   does exercise teardown. Keep it that way.

## What the startup gates actually cover

- `--smoke-test` materializes **every** lazy panel (`isStartupSmokeMode` in
  `src/gui/main_window.cpp`), so a crash in any panel *constructor* is covered in both headless
  and desktop runs.
- Headless smoke skips `show()` (`prepareMainWindowForStartup`, `src/main.cpp`). Nothing is ever
  exposed: no `showEvent`, no paint, no realized geometry. Deferred-on-show work, layout, and
  truncation bugs are invisible to it and need a desktop run or an exposed-widget test.
- The accessibility audit also exits through `_Exit(0)` to avoid teardown crashes in automation,
  so it is not a teardown gate either.

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
