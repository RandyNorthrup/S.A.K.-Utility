# S.A.K. Utility -- Coding Standards

> **Read [AGENTS.md](../AGENTS.md) first.** This file covers HOW to write code here (naming, Qt
> rules, error handling, testing, build). AGENTS.md covers how to WORK here: the non-negotiable
> owner rulings, the gate that every code commit must pass, break-every-fix drill discipline, the
> ways a green run has meant nothing, where authoritative status lives, and the pre-push secret
> scan. Neither file restates the other.
>
> If `AGENTS.local.md` exists at the repo root, read that too -- it is gitignored and holds this
> machine's certification rigs and the owner's working preferences.

## Project Identity

**S.A.K. Utility** (Swiss Army Knife Utility) is a portable Windows toolkit for PC
technicians, IT pros, and sysadmins. Release artifacts are portable ZIP packages
with the app, Qt runtime files, plugins, and bundled technician tools.

- **Language**: C++23 (MSVC 19.44+, `/std:c++latest`)
- **Framework**: Qt 6.5+ minimum; release CI uses Qt 6.10.3 (Core, Widgets, Concurrent, Network, Xml)
- **Build**: CMake 3.28+, Visual Studio 2022 generator
- **Target**: Windows 10/11 x64
- **Package Manager**: vcpkg (for non-Qt dependencies)
- **Tests**: Qt Test framework, CTest runner

---

## Code Quality Rules

Safety first, then performance, then developer experience.

Some of what follows is enforced by a hook and some is not; each rule below says which.
The hard gates are **zero build warnings, zero build errors, and all tests passing**
(see *Build & CI Requirements* below).

### Core Tenets

1. **Zero technical debt** -- Do it right the first time. What we ship is solid.
   No workarounds, no placeholders, no stubs, no "fix later" comments.

2. **Assertions are a force multiplier** -- Use assertions to validate meaningful
   preconditions and postconditions. Every assertion should catch a real bug --
   never add assertions just to hit a density target. Use `Q_ASSERT` for
   debug-mode checks, `static_assert` for compile-time invariants.

3. **Simplicity is the hardest revision** -- The first attempt is never the
   simplest. Refactor until the code reads as obviously correct.

4. **Put a limit on everything** -- All loops, queues, buffers, and timeouts have
   bounded sizes expressed as named constants.

5. **Fail fast** -- Detect violations sooner rather than later. Never silently
   swallow errors. Every user-visible error must also be logged via `sak::logError`
   or `sak::logWarning`.

### Quality Rules and What Enforces Each

BLOCKING rows are pre-commit hooks: the commit is rejected. Review rows are not checked by
any tool, so they hold only if a human notices.

| Guideline | Target | Enforced by |
|---|---|---|
| Function body length | <=70 lines | `lizard-complexity` -- BLOCKING (`MAX_FUNC_LENGTH` in `scripts/run_lizard.py`) |
| Cyclomatic complexity | CCN <=10 | `lizard-complexity` -- BLOCKING (`MAX_CCN`) |
| Magic numbers | Named `constexpr` constants | `magic-numbers` -- BLOCKING (`scripts/check_magic_numbers.py`); `0`, `1`, `-1` stay bare |
| Line length | <=100 columns | `clang-format` -- BLOCKING |
| Single-letter variables | Avoid, except tiny lambda predicates | `readability-identifier-naming`, run in CI on every build |
| Nesting depth | <=3 levels | Review -- lizard measures complexity, not nesting depth |
| Assertions | Meaningful preconditions/postconditions | Review -- every assertion should catch a real bug |
| `catch(...)` | Should have explanatory comment | Review -- only the logger is exempt |
| `else` after `return` | Avoid | Review -- prefer early-return guard clauses |
| Nested ternary | Avoid | Review -- prefer `if`/`else` or a helper |
| TODO / FIXME / HACK in code | Avoid in committed code | Review -- no gate checks this |
| Commented-out code | Delete it; Git has the history | Review |

When lizard rejects a function, fix it STRUCTURALLY by extracting a seam -- never by deleting
the comments that explain why the code is shaped the way it is. A function over the length
limit at CCN 2 is a comment-density signal, not a complexity problem.

Data-only initializers may exceed the length target where splitting hurts readability; they
still have to pass the hook, so extract the data rather than arguing with it.

---

## Coding Standards

### Compiler Strictness (Hard Gate)

All code **must** compile cleanly under:
```
/W4 /WX /permissive- /utf-8 /std:c++latest
```
Zero warnings. Zero errors. **This is a release hook -- it must always pass.**

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

- Headers: `include/sak/*.h` -- `#pragma once`, public API with `///` Doxygen docs
- Sources: `src/core/`, `src/gui/`, `src/actions/`, `src/threading/`
- Tests: `tests/test_*.cpp` -- one test file per unit
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
- **Logger API uses `std::string`** -- convert with `.toStdString()`:
  ```cpp
  sak::logError("Failed to open: {}", path.toStdString());
  ```
- **Use `std::expected`** for functions that can fail -- not exceptions for control flow.
- **Typed catches only** -- `catch (const std::exception&)` or
  `catch (const std::filesystem::filesystem_error&)`. Never bare `catch(...)` without
  a comment explaining why.
- **Check all process results** -- `waitForStarted()`, `waitForFinished()` return
  values must always be checked. Kill on timeout.

### Constants & Magic Numbers

- All timeouts, buffer sizes, retry counts -> named `constexpr` in the appropriate
  constants header (`style_constants.h`, `layout_constants.h`, etc.)
- Column indices -> `enum` in the panel header
- Colors -> `style_constants.h` or `windows11_theme.cpp`
- Acceptable bare literals: `0`, `1`, `-1`, `nullptr`, `true`, `false`

### Code Shape

Function length, complexity and line length are in the quality table above, with the hook
that enforces each. Lizard rejects at 71 lines; there is no "slightly over".

- **<=3 levels of nesting.** Use early returns (guard clauses) to flatten. Review only.
- **One declaration per line.** No `int a, b, c;`. Review only.

---

## Testing Requirements

### Every Feature Gets Tests

When adding a new feature, panel, action, or core component, you **must** add
corresponding tests in `tests/`:

1. **Unit tests for core logic** -- Test the controller/worker/manager, not the GUI.
2. **Happy path** -- The normal successful case.
3. **Error/edge cases** -- Invalid input, empty input, boundary values.
4. **Resource cleanup** -- Verify no leaks via RAII; test cancel/abort paths.

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

All tests must pass before any commit. **This is a release hook -- it must
always pass.**

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

### CI Gate (Release Hooks -- Must Always Pass)

These are the **hard requirements**. Every PR must pass these gates:
- [ ] **Build with zero warnings** (`/W4 /WX`) -- non-negotiable
- [ ] **Pass all tests** (100% pass rate) -- non-negotiable
- [ ] Include tests for new features

Function length, complexity, magic numbers and line length are each enforced by a
pre-commit hook that rejects the commit. The quality table near the top of this file
says which rules block and which rely on review.

---

## Architecture Quick Reference

### Directory Structure

```
include/sak/          -- Public headers (one .h per class, 160 headers)
include/sak/actions/  -- Action headers (7 files, 167 headers total)
src/core/             -- Core business logic, workers, parsers, managers (106 files)
src/gui/              -- Qt widget panels, dialogs, and themes (37 files)
src/actions/          -- Quick action workers (one file per action, 7 files)
src/threading/        -- Thread workers (backup, scan, hash, flash, 4 files)
src/third_party/      -- Bundled third-party source (qrcodegen)
tests/unit/           -- Qt Test unit tests (112 files)
tests/unit/actions/   -- Action validation tests (2 files)
tests/integration/    -- End-to-end workflow tests (3 files)
resources/            -- QRC files, icons, themes
scripts/              -- Build/lint/utility scripts
docs/                 -- Project documentation
cmake/                -- CMake modules and build config
```

### Key Patterns

- **Panel + Controller** -- GUI panels delegate logic to controllers.
  `FooPanel` (UI) -> `FooController` (logic). Keep panels thin.
- **Worker threads** -- Long operations use `WorkerBase` subclasses moved to
  `QThread`. Communicate via signals/slots only. Never touch GUI from a worker.
- **Modal dialog isolation** -- Modal dialogs that share a controller with the
  parent panel must disconnect overlapping signals before `dialog.exec()` and
  reconnect after. Use `disconnectDialogSignals()` / `reconnectDialogSignals()`
  helpers plus a `m_dialog_active` guard flag for lambdas.
- **Action system** -- Quick actions inherit `QuickAction` and implement `execute()`.
  Actions are registered with `QuickActionController` in the panels that host them.
- **Logging** -- `sak::logInfo`, `sak::logWarning`, `sak::logError` write to
  `_logs/` with rotation. Uses `std::vformat` -- all args must be `std::string`.

### Important Conventions

- **Portable mode** -- Detected by a WRITABLE `data/` directory beside the executable
  (`dataRoot()` in `src/core/app_paths.cpp`), and only when the build is not running as a
  packaged (MSIX) app. All paths relative to exe, no registry writes; if that directory is
  absent or not writable the app falls back to the OS per-user writable location.
  (Corrected 2026-08-30: this said "Detected by `portable.ini` in the exe directory".
  Nothing in `src/` or `include/` reads, writes, or mentions `portable.ini` -- the only
  reference in the repo is `scripts/stage_portable_release.ps1`, which CREATES the file
  into the release package as a marker nothing consumes.)
- **Signal naming** -- `Q_SIGNALS` use past tense: `scanFinished`, `errorOccurred`,
  `progressUpdated`.
- **Slot naming** -- Private slots use `on` prefix: `onScanClicked`, `onTimerExpired`.
- **Section separators** -- Use `// ======` comment blocks to visually divide
  major sections within source files.

---

## What NOT To Do

### Hard Rules (will break release hooks)

- Do not introduce build warnings -- the build **must** pass `/W4 /WX`.
- Do not break existing tests -- all tests **must** pass.
- Do not bypass build warnings with pragmas or casts.
- Do not silence errors or swallow exceptions.
- Do not use `emit` -- use `Q_EMIT` (project uses `QT_NO_KEYWORDS`).

### Also expected, though nothing checks them

- Do not add a feature without tests.
- Prefer `static_cast` / `dynamic_cast` over C-style casts.

---
