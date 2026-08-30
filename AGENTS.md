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

**Do the optionals.** Anything an agent flags as "optional" or "nice to have" is required.

**Docs are 7-bit ASCII.** No em dash, en dash, arrows, checkmarks or smart quotes in tracked
text. Use `--` for a dash. Verify zero bytes above 0x7F before committing; a pre-commit hook
enforces it.

**Never `--no-verify`.** If a hook fails, fix the cause. A hook bypassed once becomes a hook
nobody trusts.

---

## The gate

Every commit that touches code must pass the full Release gate first: a complete build AND the
complete CTest suite, with **zero** failures. Not the affected target -- the whole suite. A fix
that changes a message another subsystem asserts on is not proved by its own file's tests.

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

---

## Ways a green run means nothing

Every one of these has happened here.

| Trap | What you see | Defence |
|---|---|---|
| Stale binary | Build FAILED, ctest runs the previous .exe and prints "Passed" | Capture and read BUILD EXIT separately |
| Wrong exit code | `ctest ... \| Select-String` leaves `$LASTEXITCODE` from the cmdlet, printing 0 for a failed run | Capture the code straight after the native command |
| Mutation that will not compile | `if (false && ...)` trips C4127 under warnings-as-errors; the build dies and the old binary runs green | Write mutations that still compile and still use their operands (`if (indent < 0 && ...)`, or compare a value with itself) |
| PowerShell `-File` array binding | `powershell -File x.ps1 -Files $arr` passes only the FIRST element; a scan "PASSED" having read 1 of 11 files | Call it in-process: `& ./x.ps1 -Files $files` |
| Persistent working directory | A stale `cd build` from an earlier tool call runs the wrong binary | `Set-Location` explicitly each call; prefer absolute paths |
| Search tool skips gitignored paths | `Grep` (and other repo-aware search) honours .gitignore, and `artifacts/` IS gitignored -- so a hunt for a certification value reports "appears in no artifact" having never opened the evidence directory. The same shape hid a live guard: grepping one function name and not finding it read as "no such guard exists" when the guard was three functions away under another name | For anything under `artifacts/`, `build*/`, `temp/` or any other ignored path, use a plain `grep`/`rg` that does not respect gitignore. And before concluding something is ABSENT, prove the search would have found it -- search for a string you know is there |
| .NET ignores the shell cwd | `[System.IO.File]::ReadAllBytes("docs/x.md")` resolves against the PROCESS cwd and can throw while a relative `Get-Content` succeeds -- a null result then reads as "0 problems found" | Absolute paths with .NET APIs, and assert the byte count actually read |

---

## Where truth lives

Status belongs in exactly one place, and that place is versioned.

- Item status and remediation history: the tracking doc under `docs/`.
- What landed: `git log`.
- Registered test count and coverage claims: `tests/README.md` (a pre-commit gate verifies it
  against the real CTest registration -- a README once hid nine dead test files by asserting
  coverage that did not exist).
- Certification claims for the partition tools: the matrix under `certification/`, which is
  machine-readable data rather than prose, plus the gated docs that cite it. This tool writes
  to raw disks; an uncertified operation appearing as certified is a data-destruction risk,
  not a documentation problem.
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
earlier commit already contains. There are TWO commit-time scans -- `scan_secrets.ps1`
(regex over current files) and the `gitleaks-staged` hook (`gitleaks protect --staged`,
the full rule set against the staged diff, so a secret is rejected before it can enter
history at all) -- and the FULL-HISTORY gitleaks scan runs at push time.
(Corrected 2026-08-30: this said the commit-time scan is "regex-only over current files by
design", which stopped being true when `gitleaks-staged` was added.)

Install it -- `pre-commit install` alone does not:

```bash
pre-commit install --hook-type pre-push
```

If it fails, resolve the finding. Do not add it to `.gitleaksignore`: that entry would be the
only thing between a live credential and the remote. Note also that the scanner's `--redact`
masks the matched token but still prints the surrounding line, so scan OUTPUT is itself
sensitive and must not be pasted into logs or CI artifacts.
