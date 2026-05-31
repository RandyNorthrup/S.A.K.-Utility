# S.A.K. Utility Production Grade Audit

Generated: 2026-05-27  
Last updated: 2026-05-31

Scope: source scan of `src/`, `include/`, `tests/`, `docs/`, and release scripts, followed by
Release build, CTest, release-readiness gates, runtime accessibility audit, global magic-number
scanning, and targeted GUI/style gates. Build outputs, virtual environments, and bundled
third-party source are excluded from source compliance claims. Changed GUI surfaces received
operator visual acceptance on 2026-05-31; clean-VM packaging QA remains a separate release step.

This document only makes claims backed by code inspection or passing automated checks.

## Verification Results

| Check | Result | Evidence |
|---|---:|---|
| Release build | Pass | `cmake --build build --config Release --parallel` completed successfully after final source edits. |
| Full CTest suite | Pass | `ctest --test-dir build -C Release --output-on-failure`: 129/129 passed. |
| Global magic-number gate | Pass | `python scripts\check_magic_numbers.py`: 0 violations. The check is now wired into `scripts/check_release_readiness.ps1`. |
| GUI raw color-token gate | Pass | `scripts/check_gui_style_tokens.ps1`: no raw hex/rgba tokens outside approved theme/report/color constants. |
| GUI raw stylesheet-literal gate | Pass | `scripts/check_gui_stylesheet_literals.ps1`: no raw CSS/rich-text style literals across `src/` and `include/` outside approved style/color/report constants. |
| GUI layout/sizing magic-number gate | Pass | `scripts/check_gui_magic_numbers.ps1`: no targeted raw layout/sizing numeric literals outside constants. |
| Blocking-pattern gate | Pass | `scripts/check_blocking_patterns.ps1`: passed. |
| Logged message-box gate | Pass | `scripts/check_logged_message_boxes.ps1`: no direct static production `QMessageBox` calls outside the logged helper. |
| Secrets/path regex scan | Pass | `scripts/scan_secrets.ps1 -SkipExternalTools`: clean. |
| Third-party license audit | Pass | `scripts/check_third_party_licenses.ps1`: passed. |
| Qt resource audit | Pass | `scripts/check_qrc_resources.ps1`: passed. |
| Lizard hard gates | Pass | `python scripts/run_lizard.py`: all functions within limits. |
| Lizard advisory scan | Pass | `python scripts/run_lizard.py`: no length advisories remain after splitting the report CSS and forced-uninstall dialog builders. |
| Runtime accessibility audit | Pass | `scripts/check_accessibility_patterns.ps1`: 394 explicit accessors, no fallback; runtime audit completes with `missing=0`. |
| Release readiness aggregate | Pass | `scripts/check_release_readiness.ps1`: all included release-readiness gates passed. |
| Targeted UI/AI regression pass | Pass | Fixed UUP ampersand text, email preview toggle sizing, Vulnerability Scanner bottom spacing, AI status-bar token details, and provider-gateway tool schema. Focused CTest and style/magic/Lizard gates passed. |
| Local CSS/HTML constants scan | Pass | In-app rich text, generated report CSS, and Windows theme QSS now resolve colors, font weights, spacing, borders, radii, and selector assets through shared design/report/style tokens instead of inline literals. |
| Dark-mode card surface scan | Pass | Backup/Restore and Image Flasher cards now use shared `sakCard` palette-backed style helpers instead of local light-only card styles. |
| Linux ISO catalog verification | Pass | Removed MemTest86+; refreshed Ubuntu, Fedora, Debian, Arch, Mint, Kali, SystemRescue, Clonezilla, GParted, ShredOS, and Ventoy entries. Direct ISO URLs returned HTTP 200; GitHub release APIs resolved ShredOS and Ventoy ISO assets. |
| Bundled/build stack version check | Pass | `smartctl --version` = 7.5, `iperf3 --version` = 3.21, bundled Chocolatey = 2.7.2, aria2 = 1.37.0, win32-mcp-server = 2.6.1; release CI Qt updated to 6.10.3 and CMake summary now reports the actual runner version. |
| GitHub Actions Node 24 readiness | Pass | Release and secret-scan workflows now use Node 24 action majors where available; Gitleaks runs as a verified CLI download, TruffleHog is pinned to `v3.95.3`, and no Node 20 Gitleaks action remains. Local Gitleaks 8.30.1 full-history scan reports no leaks after exact false-positive fingerprints were documented in `.gitleaksignore`. |
| PST/OST fixture regression pass | Pass | `test_pst_parser`, `test_email_search_worker`, `test_email_export_worker`, and `test_ost_integration` passed. The integration test was run with `SAK_TEST_PST_PATH=temp\my emails.pst` and `SAK_TEST_OST_PATH=temp\randy.northrup@outlook.com.ost`, verifying open, folder hierarchy, item list, and item detail for both files. |
| Email selection/export regression pass | Pass | Full CTest passed after adding checkbox email selection, checkbox attachment selection, and selected-message export to HTML, TXT, EML, and PDF. `test_ost_integration` now smoke-exports one real PST item and one real OST item in each selected format. |

## Completed Fixes

### Stability

- Reworked `VulnerabilityPanel` scan shutdown around `QFutureWatcher` and shared cancellation state.
- Removed UI-thread `waitForFinished()` from vulnerability panel teardown.
- Fixed accessibility audit startup so it runs on the Qt event loop and exits cleanly instead of using a no-event-loop path.
- Hardened accessibility audit process handling: raw `argv` output-path parsing, unique audit output per run, explicit failure when requested output cannot be written, and no hidden-window early quit before the audit timer runs.
- Suppressed external/persistent side effects during accessibility audit: drive scan startup, AI credential/session/workflow/Chocolatey initialization, MainWindow state load/save, OST settings load/save, and App Installation runtime signal/external setup.
- Stabilized App Installation audit construction by attaching the scroll area after layout construction and using interactive result-table headers instead of hidden `ResizeToContents` sizing work.
- Fixed PST/OST inspector parsing for legacy Unicode PST files: 512-byte NDB pages are used for `wVer=23`, compressible-encrypted PST blocks decode with the MS-PST permutative decode table, and large archives no longer drop BBT/NBT entries at a fixed 50k cache cap.

### Accessibility

- Removed theme-level accessibility fallback/auto-fill behavior.
- Added strict explicit accessibility gate and runtime audit mode.
- Verified all first-party interactive widgets in the runtime audit have explicit accessible names.
- Added runtime audit status output so timeouts identify the construction or scan phase instead of failing silently.
- Fixed the runtime audit timeout path by removing unnecessary headless startup work, skipping splash probing and startup banner work in audit mode, and passing parsed runtime options directly instead of reparsing Qt arguments.
- Fixed AI provider-gateway function schema compatibility by sending provider-specific arguments as a strict string field and normalizing JSON-object strings before tool execution.

### Themed UI Constants

- Added centralized color, style, report, layout, and widget-helper constants.
- Migrated touched GUI styles to theme helpers or named style constants.
- Preserved original action-button colors through named button-tone and solid-button constants instead of changing button palettes.
- Added release gates for raw color tokens, raw stylesheet literals, and targeted GUI layout/sizing magic numbers.
- Moved remaining search-highlight colors into `include/sak/color_constants.h` so GUI color tokens are no longer inline.
- Fixed reported UI regressions in the Windows ISO/UUP progress label, email preview toggles, and Vulnerability Scanner bottom control spacing without adding raw style tokens.
- Replaced the Email Inspector message-list icon column with explicit checkboxes and added checkbox-backed attachment selection so multi-select does not depend on keyboard modifiers.
- Added named dark-theme color tokens, a global theme switcher, and synchronized `Dark` slider toggles immediately beside each panel `Log` slider.
- Styled Qt spinbox/date/time steppers with shared themed metrics so up/down controls fit the input box instead of rendering cramped native rollers.
- Added `include/sak/design_token_constants.h` for core-safe shared font-weight, border, radius, and padding tokens used by both GUI style builders and generated HTML report CSS.
- Refactored generated report CSS in `include/sak/report_style_constants.h` so HTML reports use named metrics and color tokens without pulling GUI-only Qt headers into core/test targets.
- Consolidated action-card styling behind shared palette-backed card helpers and updated Backup/Restore plus Image Flasher cards so dark mode cannot retain light-only local card backgrounds.
- Restored the existing dark secondary button tone on Backup/Restore wizard card actions and applied explicit shared action styling with hover states to missed Settings, OST converter, Organizer category, Duplicate Finder, Benchmark panel, Application Management, WiFi, migration wizard, ISO/download, settings, log, and dialog buttons so active controls do not look disabled.
- Replaced the Email Inspector select-column header text with an Icons8 check icon and centralized combo-box, spin-box, date-picker, calendar-popup, and tab-chevron selector styling behind shared constants/helpers using Icons8 SVG chevrons and calendar icons so selector controls do not fall back to native dark corner buttons or missing glyph boxes.
- Moved Windows theme QSS templates and remaining rich-text/report style fragments behind shared constants, then tightened `scripts/check_gui_stylesheet_literals.ps1` so raw style literals fail outside approved constants.
- Updated Image Flasher ISO cards to share the same slightly lighter `sakCard` surface and restored the original Linux ISO Tux icon asset.

### Email Tools

- Added a `Save Selected Email` action beside the preview `Images` toggle, backed by checked-message export from the Email Inspector to HTML, TXT, EML, and PDF.
- Preserved attachments with their source email during selected-message export: EML embeds payloads, HTML writes files beside the generated page, and TXT/PDF write per-message attachment sidecar folders.
- Kept PST and OST behavior on the native archive path while extending the same selected-message export flow to MBOX where message payload data is available.

### Linux ISO Catalog

- Removed MemTest86+ from the ISO downloader catalog and tests because the downloadable asset path was not reliable in this app.
- Added Arch Linux to the downloader and added Fedora Workstation plus Debian Live GNOME so existing UI copy matches real catalog coverage.
- Refreshed current known catalog versions: Ubuntu 26.04 LTS, Kali 2026.1, SystemRescue 13.00, GParted Live 1.8.1-3, ShredOS 0.41, and Ventoy 1.1.12.
- Removed GitHub release URL fallback behavior; GitHub-backed ISO tools must resolve a matching asset from the latest release API response before download starts.
- Removed recursive/PATH tool lookup for Linux ISO and UUP downloads; required bundled tool paths now fail loudly when missing.
- Hardened checksum verification so checksum fetch errors or missing checksum entries fail the download instead of silently completing as verified.

### Bundled Tools

- Updated bundled smartmontools to 7.5 and iPerf3 to 3.21.
- Updated bundled portable Chocolatey to 2.7.2 and adjusted `.gitignore` so the required Chocolatey package payload is not silently ignored.
- Verified existing aria2 1.37.0 and win32-mcp-server 2.6.1 binaries remain current for this repo's configured bundle sources.
- Updated release CI from Qt 6.5.3/MSVC 2019 Qt package to Qt 6.10.3/MSVC 2022 Qt package after checking aqt-installable Windows desktop packages; Qt 6.11.x exists in the release index but the current CI installer metadata did not expose an installable Windows package during verification.
- Removed the stale fixed CMake version from workflow summaries; the workflow now prints the runner's actual `cmake --version` output.
- Added explicit CI installation for ripgrep so strict release gates run with their required scanner instead of relying on runner image contents.
- Fixed Qt 6.10/MSVC strict-build compatibility issues: deprecated Qt overloads, explicit parse-result handling, QByteArrayView hashing, and nodiscard test assertions.
- Replaced deprecated checkbox `stateChanged` connections with `toggled` signal wiring so strict Qt 6.10 builds do not depend on deprecated API or version fallbacks.
- Disabled vcpkg's implicit MSBuild app-local post-build copy in release CI; runtime DLL copying now stays in the explicit bundle step and CMake's named runtime copy commands.
- Made the CMake bundled-tools output copy deterministic by clearing the stale build output tools directory before copying the verified source `tools/` tree.
- Removed the release workflow CMake build-output cache so release packages are built from current checked-out sources and freshly bundled tools instead of stale `build/Release` state.
- Installed pinned `lizard` 1.21.2 in release CI so package-root readiness runs the same complexity gate used by local and pre-commit checks.
- Updated GitHub-hosted workflow actions to Node 24-ready major versions: checkout v6, setup-python v6, cache v5, setup-msbuild v3, upload-artifact v7, action-gh-release v3, and Azure artifact signing v2.
- Replaced the Node 20 `gitleaks/gitleaks-action@v2` workflow step with pinned Gitleaks CLI 8.30.1 installed from the upstream release archive and verified by SHA-256 before execution.
- Pinned TruffleHog to the current stable `v3.95.3` tag instead of tracking `main`.
- Added `.gitleaksignore` entries for two verified historical false positives so the full-history Gitleaks CLI gate stays strict without failing on non-secret enum/test strings.

### Named Constants And Magic Numbers

- Drove `scripts/check_magic_numbers.py` to 0 violations across `src/` and `include/`.
- Converted public default parameters, protocol sizes, score thresholds, parse group indexes, timeout values, progress scales, byte sizes, and display precision literals to named constants.
- Added the global magic-number scanner to `scripts/check_release_readiness.ps1` so future regressions fail readiness.
- Fixed compile issues exposed by the constant migration, including namespace collisions with shared timing constants and recursive placeholder constants in vulnerability scanning.

### Dialog Logging

- Added `include/sak/message_box_helpers.h`.
- Migrated production GUI static message boxes to logged wrappers.
- Added release gate so new direct static `QMessageBox::warning`, `critical`, `information`, or `question` calls fail unless they are inside the helper.

### Reports And Status

- Moved report CSS through shared report constants.
- Split long report CSS builders, AI guardrail prompt assembly, diagnostic/email JSON report serializers, Linux distro metadata factories, and BitLocker PowerShell script templates into named helpers/constants.
- Reduced Lizard advisories to 0 while keeping hard CCN/parameter gates passing.
- Split the final long benchmark functions into semantic phases and refactored `main.cpp` startup into focused helpers.
- Split the forced-uninstall dialog into semantic widget builders and moved warning color HTML through `ui::htmlColor()`.
- Scoped AI token/run status to the status bar only while the AI Assistant tab is active, avoiding cross-panel status-bar noise.
- Kept vulnerability scanner summary as the right-side status item only when its sub-tab is active.

### Documentation

- Updated this audit with current pass/fail evidence.
- `tests/README.md` reflects 129 registered tests.
- `README.md` now documents the current distro catalog, dark theme support, and bundled tool versions.
- `README.md` and `CHANGELOG.md` now document checkbox email/attachment selection, selected email export formats, and attachment preservation behavior.
- `README.md` now documents portable config paths, Qt minimum versus release CI version, and the accurate bundled dependency table.
- Workflow release notes and `.github/copilot-instructions.md` no longer claim a single-EXE/no-runtime-dependency package.
- `THIRD_PARTY_LICENSES.md` now reflects smartmontools 7.5, iPerf3 3.21, and Chocolatey 2.7.2.

## Current Limits

Automated code compliance is at 100% for the gates run in this pass:

- Global magic-number scan: 0 violations.
- GUI style-token, stylesheet-literal, and GUI layout/sizing scans: 0 violations.
- Accessibility fallback ban and runtime audit: passing.
- Lizard hard and advisory scans: 0 findings.
- Release readiness aggregate: passing.

Remaining non-certified area:

- Clean-VM package QA and hosted release signing, because those require the published release package.

## Next Steps

1. For release packaging, run readiness again with `-PackageRoot` and signature verification after staging a package.
2. Verify hosted GitHub release assets, signatures, and `SHA256SUMS.txt` after the `v0.9.1.7` tag workflow publishes.
3. Consider promoting the current Lizard advisory limits to blocking policy now that the tree is clean.
