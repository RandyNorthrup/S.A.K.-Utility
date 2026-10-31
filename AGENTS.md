# Working on S.A.K. Utility as an agent

Operating rules for any AI agent (Claude Code, Codex, or a subagent) that changes this
repository. This file is the agent-facing entry point.

It deliberately does NOT restate coding standards. Those live in
[docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) (naming, Qt rules, error
handling, magic numbers, test layout, build commands, architecture) and
[CONTRIBUTING.md](CONTRIBUTING.md) (contributor process, C++ style). Read those for
HOW to write code here. This file is about how to WORK here.

**If `AGENTS.local.md` exists at the repo root, read it too.** It is gitignored and holds
machine-local facts -- this workstation's certification rigs and their addresses, and the owner's
working preferences. It is deliberately not committed because it describes a developer's machine
rather than the product, and it deliberately contains no credentials (those live in the untracked
`temp/creds.md` and are read at use time). Its absence on another machine is normal.

---

## Non-negotiables

These are owner rulings. They override convenience, and they override an agent's own judgement
about what is "obviously fine".

**Owner rulings, 2026-10-24 -- these win where older text below disagrees.**

- **The release bar is the ship bar** in [docs/RELEASE_READINESS.md](docs/RELEASE_READINESS.md):
  CI green on `main`, no startup freeze, clean-package smoke, signed, Windows 11 VM smoke, and no
  known crash, data-loss or unpatched-security item. Hardware-only claims stay labeled uncertified.
  Open-ended programs (zero findings anywhere, no surviving mutant anywhere, full review-ledger
  coverage) are not release requirements. The R5 campaign document is archived.
- **Gates are tiered** (see "The gate" below). A gate exists to catch product risk. A check whose
  only subject is another gate, a ledger, or a doc's wording is not added; propose it to the owner
  first.
- **Fail closed does not mean crash.** When cleanup of a contained child cannot be proven, report
  the error and hand the owner to `sak::ContainedProcessQuarantine`; kill-on-close Job ownership
  still guarantees the child cannot outlive the app. Keep qFatal for true invariants only
  (a programming error), never for a deadline that load can exceed.
- **"Do the optionals" means product optionals.** An optional that reduces product risk is done.
  Process and bookkeeping optionals are proposed, not assumed.
- **The app behaves like a normal, well-built Windows tool -- nothing weird, nothing extra.**
  Diagnostic probes, experiments, measurement-only calls, test hooks and escape hatches do not
  live in production paths; when an investigation adds one, the same change that concludes the
  investigation removes it. Smoke and audit modes run the startup work users get, never a
  shortcut. No background work the user did not ask for (polling a hidden tab, rescanning on a
  timer, network calls while typing). A portable app keeps its state in its own folder, not the
  registry. Reference case: two font calls added to diagnose a slow startup became the 6-50 s
  startup freeze (`docs/GUI_STARTUP_FIELD_NOTES.md`). The 2026-10-24 behavior-audit decisions are
  in `docs/RELEASE_READINESS.md`.

**Make a proper decision about it.** Keep or remove is a real decision, and it has to be made on
evidence rather than on whichever is easier at that moment. Deleting something to make a problem
go away is one failure; carrying dead weight because a rule said "never delete" is the same
failure pointing the other way. Before deciding, check the tracking doc and the callers, and work
out what the thing was FOR.

Both directions have bitten here. A `queryPowerPlan()` with no caller looked like dead code and
was actually the missing half of the feature its own report apologised for -- deleting it would
have destroyed real work. A style guide cited across 25 files went the other way: stale, so it was
removed once that was established.

A removal still needs the owner's authorization -- not because removal is wrong, but because
which way that decision goes is his call, not an agent's.

**The application is Windows-only.** S.A.K. Utility ships for Windows 10/11 x64. macOS machines
are external APFS/HFS validation rigs; they are not application build targets. Linux ISO support,
MBOX interoperability, macOS profile export and Apple filesystem output are cross-FORMAT features,
not cross-platform application support. Do not implement POSIX application branches or describe
them as product gaps. Remove unsupported-OS stubs and fallbacks as dead/legacy code. Owner ruling,
2026-09-01.

**Scan first. Always.** Before writing a file, class, function, gate, test or document, search
for it. Not to be tidy -- to avoid building a second copy of something that already works,
because a parallel implementation is worse than either version alone: two things now drift, and
the one a caller reaches is decided by an include rather than by a decision.

Search for the DELIVERABLE, not for a word that sounds like it:

- The names the thing would actually have -- the class, the function, the tool id, the file.
- What it DOES, in the vocabulary this tree already uses (`grep -rl` a behaviour, not a title).
- Whether it exists under a different name, in a different layer, or unused.

Two failures in this repo, in both directions. `wifi_profile_scanner` was written as a headless
scanner and then sat UNUSED while the same logic lived on as static members of the wifi widget
-- a parallel implementation shipped and drifted before anything reached it. And a plan was
reported as having "no code in the tree" because a grep for a word from its FILENAME returned
nothing, when the feature was fully built: 64 registered action ids, a registry, a bridge, a
service and a unit test, all findable by grepping the names the plan itself specified. Acting
on that would have meant rebuilding a finished subsystem.

If a search turns up something close but not identical, that is a decision to make and state,
not a reason to add another one quietly.

**The GUI has no commands.** A panel does not build an argument vector and does not launch a
process whose output it reads or whose effect changes the machine. It calls the headless seam --
the same code the assistant drives -- and keeps only what is genuinely its own: the worker
thread, the confirmation dialog, the progress and the reporting. Owner ruling, 2026-08-27.

Launching a UI is not running a command: `explorer.exe ms-settings:`, `control.exe ncpa.cpl`, a
troubleshooter, or a terminal opened in the browsed folder all stay in the GUI, because nothing
is parsed and nothing is mutated by us. The test is whether the panel consumes the result or the
machine changes.

When the two sides already disagree, do NOT pick a winner while merging. Every difference that
changes what the machine does becomes a named, documented parameter, and each caller passes what
it has always passed -- so unifying the code path changes no behaviour and the remaining decision
stays visible and cheap to flip. `AdapterIpv4Dialect` is the worked example: `gwmetric` and
`register` differed between the panel and the restore path, both change live networking, and
neither can be certified on this machine.

**Unify duplicates, and expect the copies to already disagree.** A second implementation is not
a future drift risk, it is a present disagreement, and the copy nobody is looking at is the weak
one. Sweeping for functions defined in both `src/gui` and `src/core` found 47 name collisions and
six real duplicates, every one of which had diverged: a path-traversal guard that let `..`
through, a WiFi security resolver that turned the panel's own default into an unusable profile,
an SSD test that disagreed with the validator blocking on it, two CSV writers with no
formula-injection guard, five byte formatters, and an inventory parser reading a negative size as
18 exabytes. Note the direction of that last one: the weak copy was the CORE one. "Core is the
trustworthy side" is a heuristic that stops a sweep early.

Merging is still a decision. Two functions can share a name and answer different questions --
`detectImageMime` sniffs four magic bytes for what a report may embed in one place, and asks
QImageReader what Qt can render in the other. Forcing those together breaks one of them. State
that outcome; do not merge on the strength of a matching name.

**Fail closed. No fallbacks.** A fallback disguises an error and ships the failure silently.
No PATH fallback, no stale-cache fallback, no guessed default, no "log it and return success".
Surface the real error and refuse. This is the single most load-bearing rule in the codebase.

**No deferrals.** Nothing is "deferred". An item is either INCOMPLETE (real open work) or a
DESIGN DECISION (a deliberate scope or tool limit, with the reason). Ground every status claim
in a real commit or file:line against the LIVE tree, not against what you remember.

**Fix every issue found.** Any defect noticed while doing something else -- preexisting or not --
gets fixed on the spot if it is quick, or logged to the tracking doc if it is not. Never step
over one.

**Suppressions and silences are a last resort.** Fix the code, test, tool invocation, or gate
first. Add a suppression or silence only when no real fix is possible, after proving why, and
record the exact bounded reason beside it. Convenience, warning volume, and legacy behavior are
not reasons. Existing suppressions and silences are not grandfathered: each quality/review
campaign must re-run the relevant reveal check, verify whether the underlying condition still
fires, and identify any that can now be replaced by a direct fix or removed.

**Do the optionals.** Anything an agent flags as "optional" or "nice to have" is required.

**Docs are 7-bit ASCII.** No em dash, en dash, arrows, checkmarks or smart quotes in tracked
text. Use `--` for a dash. Verify zero bytes above 0x7F before committing; a pre-commit hook
enforces it.

**Never `--no-verify`.** If a hook fails, fix the cause. A hook bypassed once becomes a hook
nobody trusts.

---

## The gate

Gates are tiered (owner ruling 2026-10-24). Each tier states its scope; never cite a narrower
tier as proof of a wider one.

| Tier | Runs | Budget | Holds |
|---|---|---|---|
| T1 | pre-commit, changed files only | under 60 s | hygiene, format, lizard, cppcheck, staged secret scan, ASCII, blocking patterns, test registration, claim/manifest checks when their inputs change |
| T1p | pre-push | ~1 min | full-history secret scan |
| T2 | CI on every push to `dev` and every PR | ~25 min | build, CTest in parallel over the `product`, `release-tooling`, `tooling` and `cert` labels, whole-tree style gates, licenses, startup budget, accessibility, skip audit, runtime DLLs |
| T3 | CI on a version tag only | -- | package, smoke, end-to-end, readiness, sign, verify, publish, certification evidence checks (including the generated certification-state block) |
| T4 | nightly and manual | -- | ASan, coverage, whole-tree cppcheck and dead code, clang-tidy, mutation, and the `tooling-extended`, `mutation`, `coverage`, `tidy`, `cert-tooling` and `fuzz-extended` CTest labels |

CTest labels are set in one block at the end of `tests/CMakeLists.txt`; `tests/README.md` lists
what each holds. Configure fails for a script test with no label.

Before a local commit that touches code: T1 hooks, a build, and the tests your change can reach
(`ctest -L <label>` or `-R`). A fix that changes a message another subsystem asserts on is not
proved by its own file's tests -- search for the message and run those too. The full suite runs
in T2 on every push; a red T2 is fixed before anything else lands on `dev`. Nothing merges to
`main` without a green T2.

Before every push to `dev`: read the latest T2 conclusion
(`gh run list --branch dev --workflow "CI (T2)" --limit 1`) -- if it is red, your push fixes it
or waits -- and run the whole-tree style scripts from ci.yml's static-gates job locally (a few
minutes, no build). They are pre-commit `manual` stage, so no local commit runs them: a raw
`setContentsMargins(0, 0, 0, 0)` sat red on `dev` across three pushes on 2026-10-25.

Capture the build exit code in its own statement and read it before believing any test result:

```powershell
cmake --build build --config Release; "BUILD EXIT: $LASTEXITCODE"
ctest --test-dir build -C Release; "CTEST EXIT: $LASTEXITCODE"
```

Run the cheap hooks BEFORE the gate, not after -- clang-format, lizard, the magic-number check
and the secret scan take seconds, and each has rejected a commit that had already spent 15
minutes passing the full suite.

When lizard rejects a function, fix it STRUCTURALLY -- extract a seam. Never by deleting the
comments that explain why the code is shaped the way it is. A function over the length limit at
CCN 2 is a comment-density signal, not a complexity problem.

---

## Prove the test fails without the fix

A regression test that has never been RUN against the broken code is not evidence. For every
fix: break the production code on purpose, confirm the test goes RED, restore it. Assertions
that look strong pass for unrelated reasons more often than anyone expects.

Real examples from this repo: a URL-redaction test passed with the fix reverted because an
unrelated six-word title cap dropped the tokens anyway; two tests asserted that a leaked
credential appeared in output, so the suite was pinning the bug; a clamp test pinned a hard-coded
ceiling that was four times what the enforcer accepted.

Drill each guard SEPARATELY. A combined drill cannot tell you which guard the red belongs to.

A drill harness restores the original bytes in a `finally`. **Killing the process defeats that
`finally` and leaves production source mutated.** It happened here: a drill was stopped
mid-run and left a planted `// cppcheck-suppress` in `eml_writer.cpp`, in a file that also
carried a real change, so `git checkout` was not the way back. Let a drill finish, or expect to
un-plant by hand -- and check `git diff` afterwards either way, because the drill's own report
cannot tell you about a restore it never reached.

---

## Ways a green run means nothing

Every one of these has happened here.

| Trap | What you see | Defence |
|---|---|---|
| Stale binary | Build FAILED, ctest runs the previous .exe and prints "Passed" | Capture and read BUILD EXIT separately |
| Drill restore keeps the old timestamp | A drill restores the original bytes by copying them back with their original mtime; the object built from the mutated source is now NEWER than the source, so MSBuild skips the rebuild and the next "green" run still executes the mutated binary (seen 2026-10-25: the first full suite after a restore failed exactly on the drilled targets) | After restoring, touch every restored file (or rebuild that target clean) before believing any later result |
| Wrong exit code | `ctest ... \| Select-String` leaves `$LASTEXITCODE` from the cmdlet, printing 0 for a failed run | Capture the code straight after the native command |
| Mutation that will not compile | `if (false && ...)` trips C4127 under warnings-as-errors; the build dies and the old binary runs green | Write mutations that still compile and still use their operands (`if (indent < 0 && ...)`, or compare a value with itself) |
| PowerShell `-File` array binding | `powershell -File x.ps1 -Files a.cpp b.cpp c.cpp` binds ONLY `a.cpp` to a plain `[string[]]` parameter and drops the rest **silently**. Dropping the `-Files` name does not help; bare positional values behave the same. This is how pre-commit invokes every PowerShell hook, so **four gates -- cppcheck, clang-format, powershell-syntax, ascii-only -- were each checking one file per invocation while printing PASSED**. Proven end to end: a file containing `function Broken( {` in SECOND position exited 0 with "PowerShell syntax check passed" | In-process (`& ./x.ps1 -Files $files`) binds correctly and is what CI uses -- but a pre-commit hook CANNOT call in-process, which is why documenting only that defence did not prevent this. The fix a hook needs is in the SCRIPT: `[CmdletBinding(PositionalBinding = $false)]` plus `[Parameter(ValueFromRemainingArguments = $true)] [string[]]$Files`, and the hook `entry` must NOT name `-Files`. Verified against both call styles. `PositionalBinding` alone is not enough -- it turns the silent drop into a hard error on the hook's normal case |
| Array splatting does not create named arguments | `@("-DestinationPath", $path)` splatted into a child script binds positionally; stage_portable_release passed the package path as `-Triplet` and CI packaging died | Use a hashtable (`@{ DestinationPath = $path }`) for named splatting. Regression-stage a real package and assert required artifacts |
| A trap you already wrote down | The `-File` binding trap above was in this table while four gates shipped with it, because the recorded defence ("call it in-process") did not apply to the caller that actually had the bug | When recording a trap, record the defence for EVERY caller shape, not just the one you hit. A defence that the failing caller cannot use reads as covered and is not |
| Persistent working directory | A stale `cd build` from an earlier tool call runs the wrong binary | `Set-Location` explicitly each call; prefer absolute paths |
| Empty dependency root becomes a plausible absolute path | Fresh-clone rehearsal used `$env:VCPKG_ROOT/scripts/...`; empty env produced `/scripts/buildsystems/vcpkg.cmake`, then build ran after failed configure and added noise | Resolve explicit/env/existing roots first, verify toolchain file, and never run build after configure failure. Registered path test proves auto-resolve and invalid-explicit refusal |
| Search tool skips gitignored paths | `Grep` (and other repo-aware search) honours .gitignore, and `artifacts/` IS gitignored -- so a hunt for a certification value reports "appears in no artifact" having never opened the evidence directory. The same shape hid a live guard: grepping one function name and not finding it read as "no such guard exists" when the guard was three functions away under another name | For anything under `artifacts/`, `build*/`, `temp/` or any other ignored path, use a plain `grep`/`rg` that does not respect gitignore. And before concluding something is ABSENT, prove the search would have found it -- search for a string you know is there |
| .NET ignores the shell cwd | `[System.IO.File]::ReadAllBytes("docs/x.md")` resolves against the PROCESS cwd and can throw while a relative `Get-Content` succeeds -- a null result then reads as "0 problems found" | Absolute paths with .NET APIs, and assert the byte count actually read |
| Gate wired where it never runs | A step sits in `.github/workflows/`, reads as enforcement in every review, and has never executed -- this workflow has not run for 776+ commits (an owner cost decision), so its whole-tree cppcheck step was enforcing nothing while 84 findings accumulated | Before citing a gate as enforcement, establish that it has RUN. A CI-only gate on a repo whose CI is off is a comment. CI triggers on every push to `dev` since 2026-10-24; check the run, not the workflow file |
| Changed-files scope read as whole-tree | The cppcheck pre-commit hook checks the files YOUR commit touched -- correct for a commit hook, and no evidence at all for "clean project-wide". The findings that accumulate are invisible until someone edits that file, then block THEIR commit | State a gate's SCOPE next to its claim. `scripts/cppcheck_whole_project_gate.ps1` is the whole-tree twin |
| Analyzer degrades silently | cppcheck drops to "Active checkers: 4/186" on an unparseable macro, finds nothing, and exits 0 -- 65 of 238 test files were once unanalyzed while the gate printed PASSED | Assert the tool's own self-report, not just its finding count. The gate fails under 170/186 |
| Dead-code checker is disabled | `--enable=all` sounds complete, but global `unusedFunction` suppression plus cppcheck `-j` without a build directory disables the check while the run still passes | Use `scripts/check_dead_code.ps1`: single-process unusedFunction, planted dead probe, and exact-reference intersection. Raw Qt/header findings are not deletion evidence |
| Baseline shrink checked only whole-tree | The JS lizard ratchet promised a fixed function's row must be deleted, but pre-commit passes filenames and stale-row checks ran only with no filenames -- so the normal hook could leave stale debt forever. A `./path` spelling also missed the baseline key | Canonicalize every path relative to the repo and check stale rows for every baseline file present in the explicit scan. Registered Python tests pin scoped pruning and missing-path refusal |
| Hook filters out the language it claims to gate | `run_lizard.py` and CI analyzed JavaScript, but the pre-commit hook declared only C/C++ types and extensions, so a changed `background.js` printed `Skipped` and ran nothing | Include every claimed language in both `types_or` and `files`; structurally test the hook section, then invoke the hook on a real file and require `Passed`, not `Skipped` |
| Gate parity exists only in prose | R5 said every pre-commit gate had a CI twin, while test registration and the real tool preflight were absent and eight generic hooks had no CI route | The tier table under "The gate" is the contract. A changed-files T1 check names its whole-tree T2 or T4 twin in the same table; a separate parity checker was retired 2026-10-24 as a gate that only checked gates |
| Build tree mistaken for package tree | Portable smoke correctly rejected `build/Release` because it contains mutable `data/temp`; manual hook pointed at build output instead of clean extracted archive | Stage/archive first, then run package gates against `build/Release/clean-smoke-extract`; parity proves route exists, rehearsal proves route executes |
| Redirected Start-Process loses the exit code | Windows PowerShell 5.1 can leave `.ExitCode` null after `Start-Process -PassThru` with redirected output, even after both WaitForExit forms and Refresh; a successful accessibility audit then false-reds as "code unknown" | Use `scripts/process_capture.ps1`: ProcessStartInfo plus asynchronous stdout/stderr drain, bounded wait, and captured integer exit code. Its registered test runs the exit/pipe/timeout contract |
| Concurrent analyzer redirects share one file | `pre-commit run --all-files` may split one hook into parallel processes. `run_cppcheck.ps1` once redirected every process to `build/cppcheck_errors.txt`, so they truncated each other's diagnostics, emitted NUL-corrupted output, and hid the real findings behind unexplained exit 1 failures. The dead-code wrapper had the same defect in its generated suppression/findings files and failed with `Stream was not readable` | Both cppcheck hooks are `require_serial: true`, and every default/generated capture is PID-local in the system temp directory. Explicit `-ErrorsFile` paths remain available for callers that deliberately retain separate passes |
| Suppression that suppresses nothing | A `// cppcheck-suppress` whose check no longer fires looks like documented risk and is dead code -- it will silence the check again the day an edit makes it fire | The gate's reveal pass re-enables `unmatchedSuppression`. It found two dead ones on its first run |
| Static-analysis fix that compiles | Making a member const or static is a LOCAL edit with a NON-LOCAL consequence: it can remove the last instance-bound call from its own CALLER, which the analyzer then reports as newly const-able. The build stays green, and the file you just "cleaned" is a fresh landmine. Four of these were introduced in one batch | Re-run the analyzer, not the compiler, and diff against the pre-fix baseline. "I followed the cascade" is a reading; a cascade check is a re-run |

---

## Where truth lives

Status belongs in exactly one place, and that place is versioned.

- Item status and remediation history: the tracking doc under `docs/`.
- What landed: `git log`.
- Registered tests: the CTest registration itself (`ctest -N`). The test-registration hook
  refuses a test source with no registered test -- a README once hid nine dead test files by
  asserting coverage that did not exist. Docs describe tests; they do not carry counts.
- Certification claims for the partition tools: the JSON under `certification/` and the claim
  level computed from it are the only truth. Doc status blocks are generated from that JSON,
  never hand-written. This tool writes to raw disks; an uncertified operation appearing as
  certified is a data-destruction risk, not a documentation problem.
- Any version number: the file that owns it (e.g. `browser/extension/manifest.json`).

**`docs/` has three buckets, and which one a file sits in IS its status.** A plan in the wrong
bucket is a false claim, because the location is what a reader trusts before reading a word.

- `docs/` itself -- work with real open items that someone is expected to finish.
- `docs/archive/` -- realized or closed. The feature shipped, or the campaign hit zero. Kept
  for the reasoning, not as a to-do list.
- `docs/future/` -- "might implement one day". Unchecked boxes here are a parked design, NOT
  open work, and nothing should chase them.

**Moving the file is part of finishing the work, not tidying to do later** (owner, 2026-08-27).
The moment a document is fully realized AND its work is certified -- gate green, not merely
"all boxes ticked" -- move it to `docs/archive/` in the SAME commit that closes it. A finished
campaign left sitting in `docs/` is a false claim of open work, and it is the expensive
direction of the error: the next agent re-reads it, re-triages items that are already done,
and reports them as findings. Do not wait for a separate cleanup pass; there is never one.

**Production code must not depend on anything under `docs/`.** Code and tests stand on their
own; if the code genuinely needs data, that data is a JSON somewhere sensible, not prose. A
comment may CITE a document for provenance -- that is a reference, not a dependency, and the
distinction is worth keeping straight: an earlier version of this file called seven such
comments a dependency and used it to justify leaving files where they were.

**Check a plan against the deliverables it NAMES, never against a keyword from its title.**
A plan called ASSISTANT_HEADLESS_DOMINION was reported as having "no code in the tree" because
a grep for "headless" returned nothing. It was fully built: the plan names AppActionRegistry
and the sak_app_actions tool, and 64 registered action ids were sitting right there.

**Never keep a second copy of a status in an agent's private notes.** Counts, tallies,
percentages and COMPLETE labels rot on the next commit, and nothing updates the copy. An audit of
one assistant's notes against this tree found the large majority of its factual claims stale or
false -- every one of them a number that had been true when written. Cite the source and read it.

---

## Secrets

A push uploads HISTORY, not the working tree, so redacting a file does not unpublish what an
earlier commit already contains. The commit-time scan (T1) is the `gitleaks-staged` hook
(`gitleaks protect --staged`, the full rule set against the staged diff, so a secret is
rejected before it can enter history at all); the FULL-HISTORY scan runs at push time (T1p);
and `scan_secrets.ps1 -SkipExternalTools` (regex and developer-path rules over the current
tree) runs in T2 CI, with the manual `secret-path-scan` hook as its local twin.

Install it -- `pre-commit install` alone does not:

```bash
pre-commit install --hook-type pre-push
```

If it fails, resolve the finding. Do not add it to `.gitleaksignore`: that entry would be the
only thing between a live credential and the remote. Note also that the scanner's `--redact`
masks the matched token but still prints the surrounding line, so scan OUTPUT is itself
sensitive and must not be pasted into logs or CI artifacts.
