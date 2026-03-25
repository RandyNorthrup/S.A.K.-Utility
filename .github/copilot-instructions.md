# S.A.K. Utility — Copilot Instructions

## Project Identity

**S.A.K. Utility** (Swiss Army Knife Utility) is a portable Windows toolkit for PC
technicians, IT pros, and sysadmins. Single-EXE deployment — no installer, no
runtime dependencies on the target machine.

- **Language**: C++23 (MSVC 19.44+, `/std:c++latest`)
- **Framework**: Qt 6.5.3 (Core, Widgets, Concurrent, Network)
- **Build**: CMake 3.28+, Visual Studio 2022 generator
- **Target**: Windows 10/11 x64
- **Package Manager**: vcpkg (for non-Qt dependencies)
- **Tests**: Qt Test framework, CTest runner

---

## TigerStyle — The Quality Philosophy

This project follows **TigerStyle**, a coding discipline from
[TigerBeetle](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md)
that prioritizes **safety → performance → developer experience**, in that order.

> "The design is not just what it looks like and feels like.
> The design is how it works." — Steve Jobs

### Core Tenets

1. **Zero technical debt** — Do it right the first time. What we ship is solid.
   No workarounds, no placeholders, no stubs, no "fix later" comments.

2. **Assertions are a force multiplier** — Use assertions to validate meaningful
   preconditions and postconditions. Every assertion should catch a real bug —
   never add assertions just to hit a density target. Use `Q_ASSERT` for
   debug-mode checks, `static_assert` for compile-time invariants.

3. **Simplicity is the hardest revision** — The first attempt is never the
   simplest. Refactor until the code reads as obviously correct.

4. **Put a limit on everything** — All loops, queues, buffers, and timeouts have
   bounded sizes expressed as named constants.

5. **Fail fast** — Detect violations sooner rather than later. Never silently
   swallow errors. Every user-visible error must also be logged via `sak::logError`
   or `sak::logWarning`.

### TigerStyle Hard Limits

| Rule | Limit | Enforcement |
|---|---|---|
| Function body length | ≤70 lines | Code review, Lizard |
| Nesting depth | ≤3 levels | Early returns, helper extraction |
| Line length | ≤100 columns | `.clang-format`, code review |
| Assertions | Meaningful preconditions/postconditions | Code review |
| `catch(...)` | Must have explanatory comment | Only in logger (exempt) |
| Magic numbers | Named `constexpr` constants | Only 0, 1, −1 are acceptable bare literals |
| Single-letter variables | Forbidden | Except tiny lambda predicates (`c` in `\[\](QChar c)`) |
| `else` after `return` | Forbidden | Use early-return guard clauses |
| Nested ternary | Forbidden | Use `if`/`else` or helper function |
| TODO / FIXME / HACK in code | Forbidden | Track in issue tracker instead |
| Commented-out code | Forbidden | Delete it; Git has the history |

### TigerStyle Adaptations for C++/Qt

| TigerStyle (Zig) | S.A.K. (C++23/Qt6) |
|---|---|
| `std.debug.assert()` | `Q_ASSERT()` / `Q_ASSERT_X()` |
| `comptime assert` | `static_assert()` |
| `zig fmt` | `.clang-format` (100-col limit) |
| `snake_case` everywhere | `snake_case` for vars/functions, `PascalCase` for types (Qt convention) |
| No dynamic allocation | Not applicable — Qt widgets require `new` with parent ownership |
| No dependencies | Qt6 is the framework; vcpkg for utilities |

---

## Coding Standards

### Compiler Strictness

All code must compile cleanly under:
```
/W4 /WX /permissive- /utf-8 /std:c++latest
```
Zero warnings. Zero errors. No exceptions.

### Naming Conventions

```cpp
class PascalCaseClass {};            // Classes, structs, enums
void camelCaseFunction();            // Free functions and methods
int m_camelCaseMember;               // Private members (m_ prefix)
constexpr int kPascalCaseConstant;   // Constants (k prefix)
enum class PascalCase { Value };     // Enum values are PascalCase
QString local_variable;              // Local variables are snake_case
```

### File Organization

- Headers: `include/sak/*.h` — `#pragma once`, public API with `///` Doxygen docs
- Sources: `src/core/`, `src/gui/`, `src/actions/`, `src/threading/`
- Tests: `tests/test_*.cpp` — one test file per unit
- Project headers before Qt headers before STL headers

### Qt-Specific Rules

- Use `Q_EMIT` not `emit` (compiled with `QT_NO_KEYWORDS`)
- Use `Q_SIGNALS` and `Q_SLOTS` section markers
- Always pass `this` as parent for heap-allocated widgets
- Prefer `QString`, `QList`, `QVector` in Qt-facing code
- Use `std::expected<T, ErrorCode>` for fallible operations in core logic

### Error Handling

- **Never silence errors.** Every `QMessageBox::warning` / `::critical` must have
  a corresponding `sak::logError()` or `sak::logWarning()` call.
- **Logger API uses `std::string`** — convert with `.toStdString()`:
  ```cpp
  sak::logError("Failed to open: {}", path.toStdString());
  ```
- **Use `std::expected`** for functions that can fail — not exceptions for control flow.
- **Typed catches only** — `catch (const std::exception&)` or
  `catch (const std::filesystem::filesystem_error&)`. Never bare `catch(...)` without
  a comment explaining why.
- **Check all process results** — `waitForStarted()`, `waitForFinished()` return
  values must always be checked. Kill on timeout.

### Constants & Magic Numbers

- All timeouts, buffer sizes, retry counts → named `constexpr` in the appropriate
  constants header (`style_constants.h`, `layout_constants.h`, etc.)
- Column indices → `enum` in the panel header
- Colors → `style_constants.h` or `windows11_theme.cpp`
- Acceptable bare literals: `0`, `1`, `-1`, `nullptr`, `true`, `false`

### Code Shape

- **≤70 lines per function.** Data-only initializers (e.g., distro catalog entries)
  may be slightly over if splitting harms readability, but logic functions must not
  exceed 70 lines.
- **≤3 levels of nesting.** Use early returns (guard clauses) to flatten.
- **≤100 characters per line.** Break long strings, connect calls, and signatures.
- **One declaration per line.** No `int a, b, c;`.

---

## Testing Requirements

### Every Feature Gets Tests

When adding a new feature, panel, action, or core component, you **must** add
corresponding tests in `tests/`:

1. **Unit tests for core logic** — Test the controller/worker/manager, not the GUI.
2. **Happy path** — The normal successful case.
3. **Error/edge cases** — Invalid input, empty input, boundary values.
4. **Resource cleanup** — Verify no leaks via RAII; test cancel/abort paths.

### Test File Naming

```
tests/test_<component_name>.cpp
```

Each test file uses Qt Test:
```cpp
#include <QTest>

class TestComponentName : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void testHappyPath();
    void testInvalidInput();
    void testEdgeCase();
};
```

### Running Tests

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

All tests must pass before any commit. No exceptions.

### Test Coverage Expectations

- Every public function in `src/core/` and `src/threading/` should have test coverage.
- Action workers (`src/actions/`) should have at minimum: valid input, invalid input,
  and cancellation tests.
- GUI panels are tested indirectly through their controllers.

---

## Build & CI Requirements

### Build Commands

```powershell
# Configure (first time)
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Release

# Test
ctest --test-dir build -C Release --output-on-failure
```

### CI Gate

Every PR must:
- [ ] Build with zero warnings (`/W4 /WX`)
- [ ] Pass all tests (100% pass rate)
- [ ] Include tests for new features
- [ ] Have no `TODO`, `FIXME`, `HACK`, or commented-out code

---

## Architecture Quick Reference

### Directory Structure

```
include/sak/          — Public headers (one .h per class, 134 headers)
src/core/             — Core business logic, workers, parsers, managers (89 files)
src/gui/              — Qt widget panels, dialogs, and themes (35 files)
src/actions/          — Quick action workers (one file per action, 7 files)
src/threading/        — Thread workers (backup, scan, hash, flash, 4 files)
src/third_party/      — Bundled third-party source (qrcodegen)
tests/unit/           — Qt Test unit tests (96 files)
tests/unit/actions/   — Action validation tests (2 files)
tests/integration/    — End-to-end workflow tests (2 files)
resources/            — QRC files, icons, themes
scripts/              — Build/lint/utility scripts
docs/                 — Project documentation
cmake/                — CMake modules and build config
```

### Key Patterns

- **Panel + Controller** — GUI panels delegate logic to controllers.
  `FooPanel` (UI) → `FooController` (logic). Keep panels thin.
- **Worker threads** — Long operations use `WorkerBase` subclasses moved to
  `QThread`. Communicate via signals/slots only. Never touch GUI from a worker.
- **Modal dialog isolation** — Modal dialogs that share a controller with the
  parent panel must disconnect overlapping signals before `dialog.exec()` and
  reconnect after. Use `disconnectDialogSignals()` / `reconnectDialogSignals()`
  helpers plus a `m_dialog_active` guard flag for lambdas.
- **Action system** — Quick actions inherit `QuickAction` and implement `execute()`.
  Actions are registered with `QuickActionController` in the panels that host them.
- **Logging** — `sak::logInfo`, `sak::logWarning`, `sak::logError` write to
  `_logs/` with rotation. Uses `std::vformat` — all args must be `std::string`.

### Important Conventions

- **Portable mode** — Detected by `portable.ini` in the exe directory.
  All paths relative to exe, no registry writes.
- **Signal naming** — `Q_SIGNALS` use past tense: `scanFinished`, `errorOccurred`,
  `progressUpdated`.
- **Slot naming** — Private slots use `on` prefix: `onScanClicked`, `onTimerExpired`.
- **Section separators** — Use `// ======` comment blocks to visually divide
  major sections within source files.

---

## What NOT To Do

- Do not add features without tests.
- Do not silence errors or swallow exceptions.
- Do not use `catch(...)` without a justifying comment.
- Do not leave `TODO`/`FIXME` in committed code — file an issue instead.
- Do not commit commented-out code — Git preserves history.
- Do not use abbreviated or single-letter variable names.
- Do not introduce functions longer than 70 lines.
- Do not nest logic deeper than 3 levels.
- Do not use magic numbers — extract to named constants.
- Do not bypass build warnings with pragmas or casts.
- Do not use legacy C-style casts — use `static_cast`, `dynamic_cast`, etc.
- Do not use `emit` — use `Q_EMIT` (project uses `QT_NO_KEYWORDS`).

---
