# Review ledger full-file and source-language coverage review

Historical initial scope: all of `scripts/review_ledger.py` and the then-current 51 tests in
`tests/unit/test_review_ledger.py`. The real review driver, its false-green command test, and the
existing batch-hashing probe were inspected for ledger compatibility; this is not a whole-file
review claim for those callers.

- [x] [HIGH] First-party C source, C fixtures, Python and PowerShell tests, and Windows command
  launchers were missing from the R5 review denominator despite being executable source.
  RESOLVED: explicit group-to-extension rules include those languages while preserving existing
  review units. Directory-prefix boundaries still exclude similarly named unrelated roots.
- [x] [MEDIUM] The displayed unit rule was independently hard-coded and could disagree with the
  actual classifier.
  RESOLVED: the inventory's rule description is generated from the same extension mapping used by
  `classify`.
- [x] [HIGH] Executable first-party CMake programs were still outside the denominator.
  RESOLVED in this follow-up: the `cmake` group includes `cmake/**/*.cmake`, `CMakeLists.txt`, and
  `tests/CMakeLists.txt`. Exact-file and prefix declarations also generate the displayed rule.
  Generated build directories and vendor roots remain excluded. The five positive CMake examples
  failed against the old classifier before the fix; real inventory tests prove new CMake units
  start pending without inheriting review credit.

Independent bounded review on 2026-09-04 found no remaining issue in this delta. All seven focused
tests passed, including source/test language additions, existing-language preservation, and
resource/generated-path exclusion. The owner agent separately ran five negative drills against
the original classifier and reported each expected test failure.

This review did not refresh or modify the live ledger inventory or its state. The following gate
is the historical Wave 31 checkpoint; it predates the integrity and CMake-coverage changes below.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in docs/archive/CODEX_REVIEW_5_REMEDIATION.md.
These follow-up changes still require a new integrated gate.

## Full-file integrity follow-up

The original full-file review covered path admission, current-byte hashing, state reconciliation,
evidence retention, normalization, persistence, and CLI selection. It found four gaps at these
historical blobs:

- Script: `5441699a399913e912a2a8a38ceb4a64413e6ca3`.
- Tests: `e691ba8f2d67122222a8a33f5216b03322bfe871`.

- [x] [HIGH] Inventory trusted the index whenever `git diff-files` hid a working-tree edit.
  RESOLVED: every regular tracked review unit is hashed from its live file through one
  `git hash-object --stdin-paths` batch. Per-path Git filters are preserved; missing files, unsafe
  modes/paths, malformed hashes, and wrong result counts fail closed. Real temporary Git
  repositories prove both assume-unchanged and skip-worktree edits invalidate source evidence.
- [x] [HIGH] Empty briefs could be credited, and missing or changed briefs stayed reviewed.
  RESOLVED: admission requires bounded, nonempty UTF-8 Markdown inside `docs/review_briefs`.
  A driver mark binds `reviewed_blob` and `reviewed_brief_blob`; inventory revalidates both.
  Missing/changed evidence returns to pending while preserving prior source, brief, and evidence
  fields. Historical driver entries without brief hashes require explicit migration; ordinary
  commands cannot silently pin their current contents.
- [x] [MEDIUM] An invalid mark could normalize an unrestricted file before rejecting its target.
  RESOLVED: target membership and expected source bytes are checked before brief normalization;
  normalization is prepared in memory. Traversal, absolute/ADS paths, wrong roots/extensions, and
  Windows junction escapes refuse before writes. Source and original brief bytes are checked
  again before atomic replacement. Invalid UTF-8, unsupported characters/control bytes, and
  normalization that would empty the evidence also refuse without writes.
- [x] [LOW] Negative counts and unknown groups misleadingly succeeded.
  RESOLVED: `next` requires a positive integer and declared group before inventory can write.
  The driver's existing `--expected-blob` flag, two-space pending-path output, exit propagation,
  and `previous_review_blob`/`previous_evidence` history contract are preserved.

The tests also caught a Windows transport defect: text-mode stdin doubled CR in CRLF brief
payloads, producing a different hash from the stored file. Git now receives exact UTF-8 byte
pipes. The CRLF/filter comparison test proved red before that correction.

## Explicit binding migration

`migrate-brief-bindings --manifest <file>` requires a version-1 manifest with `state_sha256` and
an exact `entries` list. Each entry has `path`, the existing `reviewed_blob`, the existing `brief`
path, and independently confirmed `expected_brief_blob`. No extra, duplicate, or omitted legacy
driver entry is accepted. The original state, every source and brief, and a final recheck must
match before one atomic state update. Source-review metadata is preserved; the state records the
manifest and original-state digests as migration provenance. This binds existing evidence and
does not create new source-review credit.

The integration owner audited the existing 56 driver marks and owns the manifest and live
migration. This implementation agent did not modify live ledger state. Its SHA-256 remained
`6c81e68e6ba276274b3da579b6c61b746a7e0997603cf03f89e9a2e677eac231` throughout this work. Ledger
commands remain serial in the supported review driver; concurrent writers were not claimed or
certified by this bounded change.

## Verification and remaining boundary

Before changing production, eight new regression cases produced ten expected assertion failures
against the original implementation; the existing positive selection control passed. After the
fix, 25 separately compiled in-memory semantic mutants were killed by their intended assertions,
with no syntax/infrastructure-error kills and no writes to production source. Final tests pass
51/51 in 33.662 seconds, with no skips. The real false-green command suite passes under Windows
PowerShell 5.1.22621.6060 and PowerShell 7.6.5, including a 256-file filtered batch with real hidden
Git edits and spaced/non-ASCII filenames. Python/PowerShell syntax, ASCII, and diff checks pass.

Full-file follow-up found no further issue in the ledger and its test suite. Exact reviewed blobs:

- `scripts/review_ledger.py`: `7b13ba9797b84eeff9c5191eda73de4d67143505`.
- `tests/unit/test_review_ledger.py`: `1d25f22e7f70c4ada2aa43e75c18bcd19aed1f07`.

At this earlier checkpoint, a new integrated gate and explicit live migration were still required.
Both are now satisfied as recorded below. The remaining archival support is a distinct lifecycle
change; broader campaign and external certification boundaries remain in
docs/archive/CODEX_REVIEW_5_REMEDIATION.md.

## Explicit archival of bound implementation briefs

The September 19 full Release/281-test gate and recorded live binding migration now satisfy
the earlier integration requirements above. A separate lifecycle defect remains: the current
reader accepts only docs/review_briefs, so moving a completed bound brief invalidates its review.
The current inventory contains 40 completed implementation briefs with 149 resolved tasks,
15 reference-only briefs, and one unresolved brief. Their exact source and brief blobs match.
Only the completed implementation set is selected for archival; reference and unresolved
documents remain active, and future plans are not selected.

The repair is explicit metadata relocation, not automatic discovery or a new review. Before
moving any bound brief, retain a manifest of the state/source/brief identities and original raw
bytes. After the owner moves those exact bytes, relocate-briefs must validate every old/new
binding before one atomic state update. Do not run inventory between the physical moves and
that explicit reconciliation. Refusal leaves the state and all supplied files unchanged for
inspection; it does not infer ownership or undo the owner's filesystem actions.

The structured contract below owns this bounded implementation. Applying it to the selected
live documents is a separate root-controlled archival operation after support is verified.

The implementation now provides `relocate-briefs --manifest <file>`. Its strict version-1
manifest contains state_sha256 and entries with path, reviewed_blob, brief,
reviewed_brief_blob, brief_sha256 and archived_brief. The command requires the owner's old
paths to be absent and the new archive files to retain exact raw bytes and the old filtered
Git identity. It computes the new path-sensitive Git identity without normalizing content.
It parses the pinned state snapshot, rechecks the complete selected set and state, then uses
the existing atomic writer. Duplicate JSON keys, unsafe paths, changed inputs, malformed or
exhausted history, and persistence errors refuse. Append-only root provenance survives later
review/archive cycles. New marks still accept active briefs only; inventory never searches
alternate paths for a missing binding.

The final maintained suite passed 77/77 cases, and 31 independent guard triples passed with
exact copied-source restoration. Original refusal, state-junction, pinned-state read/re-read
and removed-active-directory defects remain retained under
build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85. These targeted checks do not replace
the mandatory final project integration gate. The live 40-file operation subsequently passed
exact manifest/source/brief/state preflight and independent post-relocation checks. Every moved
brief retained its raw bytes, all 56 driver bindings remained valid, and the 16 retained briefs
were unchanged. The only state changes were the 40 explicit brief bindings and append-only
relocation provenance. Evidence is under build/wave38-live-brief-archive-9444f2b2abdf; no new source
review credit, application certification or future-plan work was inferred from the operation.

```quality-ledger
{
  "schema_version": 1,
  "work": {
    "id": "R5-LEDGER-ARCHIVE",
    "title": "Preserve existing review evidence across explicit brief archival",
    "scope_revision": 1,
    "brief": null,
    "brief_reason": "Owner requires completed implementation documents to archive. Forty completed driver-bound briefs cannot currently move without losing their exact evidence bindings; fifteen reference briefs and one unresolved brief must stay active.",
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
      ".gitattributes",
      "tests/CMakeLists.txt",
      ".pre-commit-config.yaml"
    ],
    "environment": {
      "platform": "windows",
      "tools": {
        "python": "3.14.0",
        "git": "2.52.0.windows.1"
      }
    }
  },
  "requirements": [
    {
      "id": "R5-ARCH-REQ-BOUND",
      "statement": "Support explicit relocation of existing completed driver-bound review evidence into the archive without changing reviewed source identity, inventing new review credit, weakening path admission, or searching alternate paths.",
      "priority": "critical",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "superseded_by": null
    }
  ],
  "acceptance": [
    {
      "id": "R5-ARCH-AC-READ",
      "requirement": "R5-ARCH-REQ-BOUND",
      "given": "An existing driver record explicitly names an active or archived owned Markdown brief and exact source/brief Git blobs.",
      "when": "Inventory checks existing evidence, or mark admits a new review.",
      "then": "Existing exact archive paths under docs/archive/review_briefs can be validated; source/brief changes or absence invalidate credit. Missing active paths never search the archive. New marks remain restricted to active docs/review_briefs. Traversal, alternate roots, invalid files and link escapes refuse.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    },
    {
      "id": "R5-ARCH-AC-RELOCATE",
      "requirement": "R5-ARCH-REQ-BOUND",
      "given": "An explicit bounded versioned manifest pins the complete state bytes and a nonempty selected set of existing driver records, source blobs, old active paths/blobs, pre-move raw brief SHA256 and exact new archive paths; owner has moved those exact bytes.",
      "when": "The new metadata-only relocate-briefs command validates and applies the manifest.",
      "then": "Reject malformed/duplicate/unknown/stale/unbound entries, old paths still present, missing or unsafe new paths, changed source/brief/state and intervening admission edits before any write. Recheck the complete set, then atomically update only selected brief paths/path-sensitive Git blobs plus bounded provenance. Preserve source review blobs, statuses, evidence and unselected/history fields. Never move, normalize or overwrite brief/source files, auto-search paths, or grant new review credit. Failure leaves state and files unchanged.",
      "checks": [
        "behavior",
        "red"
      ],
      "manual_reason": null
    }
  ],
  "tasks": [
    {
      "id": "R5-ARCH-TASK-SUPPORT",
      "purpose": "Extend the canonical ledger reader, explicit manifest admission, atomic state writer and existing real-Git regression suite. Metadata-only relocation avoids a second filesystem transaction/recovery system; document moves remain owner-controlled and must be reconciled before inventory.",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "depends_on": [],
      "changes": [
        {
          "path": "scripts/review_ledger.py",
          "action": "modify"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "action": "modify"
        },
        {
          "path": "tests/README.md",
          "action": "modify"
        }
      ],
      "status": "verified",
      "evidence": [
        "R5-ARCH-READY-20260919",
        "R5-ARCH-FULL-77",
        "R5-ARCH-GUARD-BATCH",
        "R5-ARCH-RED-archive-reader",
        "R5-ARCH-RED-mark-active-only",
        "R5-ARCH-RED-nonempty-selection",
        "R5-ARCH-RED-duplicate-unit",
        "R5-ARCH-RED-manifest-size",
        "R5-ARCH-RED-duplicate-json-key",
        "R5-ARCH-RED-state-pin",
        "R5-ARCH-RED-pinned-state-snapshot",
        "R5-ARCH-RED-schema-preservation",
        "R5-ARCH-RED-reviewed-only",
        "R5-ARCH-RED-driver-only",
        "R5-ARCH-RED-existing-binding",
        "R5-ARCH-RED-live-source",
        "R5-ARCH-RED-raw-bytes",
        "R5-ARCH-RED-old-filtered-binding",
        "R5-ARCH-RED-old-absence",
        "R5-ARCH-RED-archive-namespace",
        "R5-ARCH-RED-old-active-only",
        "R5-ARCH-RED-brief-root-containment",
        "R5-ARCH-RED-missing-old-directory",
        "R5-ARCH-RED-missing-old-link-containment",
        "R5-ARCH-RED-complete-set-recheck",
        "R5-ARCH-RED-source-recheck",
        "R5-ARCH-RED-state-recheck",
        "R5-ARCH-RED-state-root-containment",
        "R5-ARCH-RED-new-filter-binding",
        "R5-ARCH-RED-persist-selection",
        "R5-ARCH-RED-atomic-failure-propagation",
        "R5-ARCH-RED-append-history",
        "R5-ARCH-RED-history-capacity",
        "R5-ARCH-RED-history-digests",
        "R5-ARCH-LIVE-40",
        "R5-ARCH-FULL-GATE-W38"
      ],
      "blocker": null,
      "superseded_by": null
    }
  ],
  "evidence": [
    {
      "id": "R5-ARCH-READY-20260919",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Root read current ledger path/evidence/atomic-write contracts and real-Git fixture; independently confirmed concrete archive need and explicit metadata-only design. Reviewed observable refusal, unchanged-credit, full-set recheck and path/filter/byte requirements; no product choice remains unresolved.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave38-archive-readiness.md",
        "sha256": "f09edc2d768a9b8d6b5747cce0fbd3feb24a33d25120d2f8771da8ee41103495"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        }
      ],
      "scope_sha256": "cc887c213b8ae71acc727a2237850a9ca56f794315cb9f409e22a7b78e1e029f",
      "red": null,
      "reason": "Documentation closure wording and tests/README declaration changed the scope fingerprint. Original readiness review retained; final readiness must bind the reconciled contract."
    },
    {
      "id": "R5-ARCH-READY-FINAL",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "readiness",
      "status": "stale",
      "command": [],
      "method": "Root reviewed final metadata-only implementation, strict manifest, pinned-state parsing, repeated-cycle history and missing-old-directory contracts against77 maintained cases and31 independent guard triples. Final documentation and README declarations agree with actual code/proof. Live selected40 operation has byte-exact pre-move snapshots and explicit manifest; no broader application certification is claimed.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave38-archive-readiness.md",
        "sha256": "f09edc2d768a9b8d6b5747cce0fbd3feb24a33d25120d2f8771da8ee41103495"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        }
      ],
      "scope_sha256": "4ef94613e768f101267cd7f21cd276a4a295a328ff15005a200aa867fcc77c44",
      "red": null,
      "reason": "Final portable document references and observed live archival outcome changed the narrative binding. Original review retained; reconcile final scope before integration closure."
    },
    {
      "id": "R5-ARCH-READY-CLOSURE",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "readiness",
      "status": "pass",
      "command": [],
      "method": "Root reconciled final documentation/README and reviewed final code,77 maintained tests,31 independent guard triples, exact live40 archival and independent56-binding postcheck. Requirements and failure boundaries remain unchanged; final project integration is still required before code commit.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": null,
      "artifact": {
        "path": "build/wave38-archive-readiness.md",
        "sha256": "f09edc2d768a9b8d6b5747cce0fbd3feb24a33d25120d2f8771da8ee41103495"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": null,
      "reason": null
    },
    {
      "id": "R5-ARCH-FULL-77",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "behavior",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "tests/unit/test_review_ledger.py",
        "-v"
      ],
      "method": "Complete maintained77-case suite passed in44.935s, including real temporary Git repositories, path-sensitive filters, actual CLI, junctions, state snapshot, missing old directory, atomic refusal and repeated archive/review cycles. Final source/test hashes remain unchanged; README count reconciled to actual execution.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/targeted-empty-root-final.log",
        "sha256": "17b3eb101e6394497d4b7f1e161563b48bef890235e3c0723ca1dff94ddf5ca1"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": null,
      "reason": null
    },
    {
      "id": "R5-ARCH-GUARD-BATCH",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "behavior",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "31 independent guard triples executed on isolated copies. Root checked all93 phase logs, intended assertions, nonzero reds, exact restoration and unchanged live inputs. Each phase ran exactly one named test; no errors/timeouts/syntax kills counted.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/results.json",
        "sha256": "6a364d546f51ca76987fae1adbbb6f4834c3a70f5812e70a9c4ac96cb7497eca"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": null,
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-archive-reader",
      "acceptance": [
        "R5-ARCH-AC-READ"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_explicit_archived_binding_keeps_exact_review_credit failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/archive-reader-red.log",
        "sha256": "d33a98765d074501318f457946bdbfef284d5cf5dac7db43d41f3ebe3dceea11"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In driver_evidence_matches, replace \"archived=True\" with \"archived=False\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "Explicit exact archived brief lost existing review credit",
        "observed_diagnostic": "Explicit exact archived brief lost existing review credit",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "747d247e6b4790466eaf6b2029a07d56374c55dde5134211604e7b491176d525"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-mark-active-only",
      "acceptance": [
        "R5-ARCH-AC-READ"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_mark_remains_active_only failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/mark-active-only-red.log",
        "sha256": "24bcb13e8240bc712bfe657aac949d3dd7485fe21ebe933f2eb6583752330f6d"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_mark, replace \"read_brief(brief)\" with \"read_brief(brief, archived=True)\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "32bf7c396c2f5c6240bbfec33c9448c58469fcfa89ccbde9e1b54b77462e8669"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-nonempty-selection",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_malformed_schema_and_duplicate_entries failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/nonempty-selection-red.log",
        "sha256": "c6af381fa83c459e9d0661d14b76056e8c5cf8cc13be57f46436d8d90dde183d"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relocation_entries, replace \"not 0 < len(manifest[\\\"entries\\\"]) <= MAX_RELOCATION_ENTRIES\" with \"len(manifest[\\\"entries\\\"]) > MAX_RELOCATION_ENTRIES\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "f9754f76b6344a2e852a8f915f815d3c54280014f8c35c21dd9d8691044567f0"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-duplicate-unit",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_malformed_schema_and_duplicate_entries failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/duplicate-unit-red.log",
        "sha256": "dec3bce8891d7cbf25029809df65db169f58c1443e35f4a76ad0a83cdeac589d"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relocation_entries, replace \"path in entries or \" with \"\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "3362527c25c150380b42b0989bf2604668da704fb57323530e9e4a5af88a56da"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-manifest-size",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_duplicate_json_keys_and_oversized_manifest failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/manifest-size-red.log",
        "sha256": "09820b4ddb919224d8d066e6b85e3dc03b8218a25341781a4568869f922299e4"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In migration_manifest, replace \"if len(raw) > MAX_MIGRATION_MANIFEST_BYTES:\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "18419beb84a996acac523a719ac058495a8f0c53f907f875ee8c2e3d9c427316"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-duplicate-json-key",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_duplicate_json_keys_and_oversized_manifest failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/duplicate-json-key-red.log",
        "sha256": "1c6bb155673e0137ef2e741bb005473045b1a191fbd85b99b429bb96163bf74b"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In migration_manifest, replace \", object_pairs_hook=unique_manifest_object\" with \"\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "a741cd9d5c56385c9a5cd8f675fb9b9a661df0c7f0b712733f0b8d999aad38ca"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-state-pin",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_stale_state_and_unbound_credit failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/state-pin-red.log",
        "sha256": "368a0c3d7a17c361a1514036d4b35219e80434028c83bc546b3c0748652937c0"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"if hashlib.sha256(original_state).hexdigest() != manifest[\\\"state_sha256\\\"]:\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "269cb78797f12e2778e49e5961d6797ac983ad3f187a273fd46a66caa9f6ad40"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-pinned-state-snapshot",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_validates_pinned_snapshot_not_intervening_state failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/pinned-state-snapshot-red.log",
        "sha256": "4c30416a019e3ee0d7e9bc8a6ea1d3802a58d99dd174aa8a4a4154ff650c5c69"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"load_state(raw_state=original_state)\" with \"load_state()\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "Relocation published history from unpinned state bytes",
        "observed_diagnostic": "Relocation published history from unpinned state bytes",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "97fda719d408004b62b2496e4ed93e95668027008e69e765b9e89059ee024b28"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-schema-preservation",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_stale_state_and_unbound_credit failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/schema-preservation-red.log",
        "sha256": "90c454b01fb2c53067339e13f885d0d0fea14b8fb6da2addc0f0f5c00c779107"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"if state.get(\\\"schema_version\\\") != STATE_SCHEMA_VERSION:\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "b5acd941f31a956c7ecd1489db26b406b510cf63da119626c579f98bb239d4b5"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-reviewed-only",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_stale_state_and_unbound_credit failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/reviewed-only-red.log",
        "sha256": "79a5b1ce4e14f46abb9672cc4cbe9d77590d81a6a2116aba234bf1259dbe326f"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"prior.get(\\\"status\\\") != \\\"reviewed\\\"\" with \"False\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "f1f109a0255163d412d20d8431a61b47632e23a441aa99eea27972e4e0dc8997"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-driver-only",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_stale_state_and_unbound_credit failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/driver-only-red.log",
        "sha256": "f382d9851c759c65635a517e77ee801f3d3c7ff7ba3aadb9952a630fcc811a97"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"prior.get(\\\"evidence\\\") != \\\"driver\\\"\" with \"False\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "1ba0761e55eec6912f0a108cbb9bd32635b0ebc40ceaff18bdcbfd31b138c34d"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-existing-binding",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_requires_exact_old_active_path failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/existing-binding-red.log",
        "sha256": "6c897117737226a825ba77168be0b56f6670d12ff752875d4e77c5f6cddb3954"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"any(entry[field] != prior.get(field)\\n                   for field in (\\\"reviewed_blob\\\", \\\"brief\\\", \\\"reviewed_brief_blob\\\"))\" with \"False\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "4d2a50c0155861ac53d2305a3810baf3a7ff3a6e9a815599e6f86494a88d3886"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-live-source",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_changed_source_and_relabelled_review failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/live-source-red.log",
        "sha256": "9ee9a0d4bda3eb5ebc1c8d39ec7e83403207cbe195122de558787e50a385c2d5"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"live[path][\\\"blob\\\"] != entry[\\\"reviewed_blob\\\"]\" with \"False\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "aa342f0d119027f01e1875828c37e08116e11a5657e116939724cc452d3737d7"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-raw-bytes",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_raw_changes_hidden_by_git_filter failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/raw-bytes-red.log",
        "sha256": "e821d26d1366c90c2db92db679ced23752d88b067e77f4bd3eebbcee4453a884"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"if hashlib.sha256(raw).hexdigest() != entry[\\\"brief_sha256\\\"]:\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "24f0e83bdee001c53e9263c5d1154d8db6fa33edd36419ab544ce45648dd1755"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-old-filtered-binding",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_changed_old_filtered_binding failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/old-filtered-binding-red.log",
        "sha256": "aa9f86ead6ec6067625c2d6e13d7e676e1ec0749e9ed2c6b06ea51c5444dc427"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"if brief_blob(entry[\\\"brief\\\"], text) != entry[\\\"reviewed_brief_blob\\\"]:\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "00f243b3b7ddcbce651704418b5c672aab9dbba37797214f0850d41f269ab4eb"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-old-absence",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_old_presence_and_invalid_new_paths failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/old-absence-red.log",
        "sha256": "cb428386447ab75db0b264b7d91c6458ccc08afe8fe54ef53d03716c7899b60a"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"if os.path.lexists(old_path):\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "2d9ffb587b84a32902a3bfbaa3fc1a569254e3eac7d5f3983906fbe15f30333f"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-archive-namespace",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_old_presence_and_invalid_new_paths failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/archive-namespace-red.log",
        "sha256": "eb7e5fcba911b258d1368792fd5f627b55f866fdbda3e1d63cdd1f82290dece3"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relocation_entries, replace \"or not entry[\\\"archived_brief\\\"].startswith(\\\"docs/archive/review_briefs/\\\")\" with \"or False\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "7fcc85e26a67bc806b82153d13b6ac5b1058ae29d79410477dc133ccfaa16db1"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-old-active-only",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_requires_exact_old_active_path failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/old-active-only-red.log",
        "sha256": "d550ea07162945ff8fa077d37334a81a80acf04f1fd1150836d6df14498c5b37"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"brief=True, missing_ok=True\" with \"brief=True, archived=True, missing_ok=True\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "c6f438c9b65ecd94e321a1c02dffbba9e8bb0d7a341807a1c68baa92396c28a9"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-brief-root-containment",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_junction_escape failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/brief-root-containment-red.log",
        "sha256": "c9c59e5cfac0495b85b0c20424a6268fa52ad16b85fcfcb83961b36a86c136e2"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relative_file, replace \"resolved.relative_to((REPO_ROOT / brief_root).resolve(strict=not missing_ok))\" with \"pass\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "dfe49907800d13e5408c7b92c0f80c845050e87e124c3e5e4a471fb37694ed65"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-missing-old-directory",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_accepts_removed_empty_active_directory failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/missing-old-directory-red.log",
        "sha256": "67d226bb98ec68718b83d9811f1abc68b36367c6b33a06c3e4ccea95e9bee6c4"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relative_file, replace \"(REPO_ROOT / brief_root).resolve(strict=not missing_ok)\" with \"(REPO_ROOT / brief_root).resolve(strict=True)\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "Absent old brief directory was refused",
        "observed_diagnostic": "Absent old brief directory was refused",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "57526fe257b00dccdd3148daaca348f83981aa485106e686f644384259d328dc"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-missing-old-link-containment",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_missing_old_path_still_refuses_junction_escape failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/missing-old-link-containment-red.log",
        "sha256": "a6bb6b8d2df18a1b395c9dc4404281ce7b3a90a95580d3b4bb827eebcdb738e6"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relative_file, replace \"resolved.relative_to((REPO_ROOT / brief_root).resolve(strict=not missing_ok))\" with \"pass\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "dfe49907800d13e5408c7b92c0f80c845050e87e124c3e5e4a471fb37694ed65"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-complete-set-recheck",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_rechecks_complete_set_after_later_admission failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/complete-set-recheck-red.log",
        "sha256": "1235ed69981cec21f946f8fd781e43686c461c155ffdbbec8ddb8fd5bf781403"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"for path, entry in entries.items():\\n            if confirm_relocation_entry\" with \"for path, entry in ():\\n            if confirm_relocation_entry\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "757b65f542f00902ddc49316b48d16277ffcad39c2f70614bb79a0bed9ed0db0"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-source-recheck",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_rechecks_source_state_and_filter_before_write failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/source-recheck-red.log",
        "sha256": "1fa13f25d4856fd684fecd5728bb6ef38b1f0f1eb6785e5a34f83dc0e17aed11"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"final_live = current_units()\" with \"final_live = live\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "244726d37b2cf171b35f83798c7fcd6852681ac73c2b28fcc3a4da1abe647c08"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-state-recheck",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_rechecks_source_state_and_filter_before_write failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/state-recheck-red.log",
        "sha256": "73d93c307978acb6b7e60da966d40c8a0a527a3fc7a8fe1fde9a7fb20014a6b5"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"if relative_file(state_relative).read_bytes() != original_state:\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "b19b5fb99f354b086dcde09179334e73190680cb356891278f88d5bd909654c4"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-state-root-containment",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_state_junction_escape failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/state-root-containment-red.log",
        "sha256": "eced2404f84c8fb88a2485813ba3429465da71919c1860512af955fc4478ad5a"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relative_file, replace \"resolved.relative_to(REPO_ROOT.resolve(strict=True))\" with \"pass\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "52726a9af5b1c59f4c192b14048c003615d5ba2551daba9e78dfd17f86e81b11"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-new-filter-binding",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_uses_new_path_git_filters_without_normalizing failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/new-filter-binding-red.log",
        "sha256": "48e57404b83ffeb1ce0908325ed1fd50176e75d2db84175cb9c6dba4cd78c0d5"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In confirm_relocation_entry, replace \"return brief_blob(entry[\\\"archived_brief\\\"], text)\" with \"return entry[\\\"reviewed_brief_blob\\\"]\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "Relocation retained the old-path filtered blob",
        "observed_diagnostic": "Relocation retained the old-path filtered blob",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "105606ecd7b758248b89589493b883aba20ab919733a291440f9a4d0785c8de0"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-persist-selection",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_real_cli_propagates_success_and_stale_refusal failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/persist-selection-red.log",
        "sha256": "a0c01a7e29a1c7ee510d6ffacf39f4e219f84e151cb5cfa78fa1d76521ffa59f"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"save_state(state)\" with \"pass\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "AssertionError",
        "observed_diagnostic": "AssertionError",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "b023ff0136bb2c69c4a8735d85ad4a903e9109480192d2955473a9d39001a72b"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-atomic-failure-propagation",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_atomic_failure_leaves_state_and_files_unchanged failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/atomic-failure-propagation-red.log",
        "sha256": "bfa05b72134705f7856284ada1f99b845d619c348effbd58f6f96cf77f097ca5"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In atomic_write, replace \"fail(f\\\"could not atomically write {path.name}: {error}\\\")\" with \"return\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "e10d7c3092b9548aa63bd94bb23c34ba23b136ed1b1a4de3919157bf190cef6f"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-append-history",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_preserves_history_across_rereview_cycles failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/append-history-red.log",
        "sha256": "a12eb130272505d869b21a0a0f16346110f5bb16f3255eb7e247e2c702b6474b"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In cmd_relocate_briefs, replace \"history + [{\" with \"[{\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "Relocation overwrote previous archival history",
        "observed_diagnostic": "Relocation overwrote previous archival history",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "b43ec0405785e73f191f16beb1719c9a80c66feca5b748aa44f3a7c5f8ce6f0e"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-history-capacity",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_preserves_history_across_rereview_cycles failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/history-capacity-red.log",
        "sha256": "1a588d9475ca3397eaee3e84f0352133f4568563bd61ade8f0a10dcf1e85990a"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relocation_history, replace \"len(history) >= MAX_RELOCATION_HISTORY\" with \"False\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "b1b0a75d3d393402bcd13d1fa226078691afb93fe4e27714466380c71fe7e4c1"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-RED-history-digests",
      "acceptance": [
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "red",
      "status": "pass",
      "command": [
        "python",
        "-B",
        "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/run-drills.py"
      ],
      "method": "Actual independently executed test test_archive_relocation_refuses_malformed_history failed its intended assertion only with this guard reversed, then passed after exact restoration. Full child argv, phase outputs and hashes remain in the bound batch artifacts.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-ledger-archive-eb27b959eeee4447908e5f3c6408de85/drills-6f40197e3ca34e88af0bef24ac3a379e/history-digests-red.log",
        "sha256": "3a0f95ecef627dbdb99d8e8a8b5711a32d95093734398e0b0925c20ee7787239"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": {
        "mutation": "In relocation_history, replace \"if any(not isinstance(record[field], str)\\n               or not re.fullmatch(r\\\"[0-9a-f]{64}\\\", record[field])\\n               for field in (\\\"original_state_sha256\\\", \\\"manifest_sha256\\\")):\" with \"if False:\" in the isolated canonical script copy.",
        "baseline_exit": 0,
        "mutated_exit": 1,
        "restored_exit": 0,
        "expected_diagnostic": "SystemExit not raised",
        "observed_diagnostic": "SystemExit not raised",
        "before": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ],
        "mutated": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "f9619d7f0ea879068c0e77d6c49b6f8c73fdf0d34a3ad9dcffe5b1c10ae1da9d"
          }
        ],
        "after": [
          {
            "path": "scripts/review_ledger.py",
            "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
          }
        ]
      },
      "reason": null
    },
    {
      "id": "R5-ARCH-LIVE-40",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "behavior",
      "status": "pass",
      "command": [
        "python",
        "scripts/review_ledger.py",
        "relocate-briefs",
        "--manifest",
        "build/wave38-live-brief-archive-9444f2b2abdf/relocate-briefs-manifest.json"
      ],
      "method": "Executed the explicit metadata-only CLI after40 separately authorized byte-exact owner moves. Public argv uses a repository-relative rendering of the actual absolute manifest path; immutable manifest SHA00d0108a identifies the exact input. Exit0. Independent post-audit checked all40 raw bytes and source blobs,16 retained briefs, all56 exact driver bindings and full state equality except the40 named refs plus append-only provenance. No source review credit changed. Original state9444f2b2/snapshots retained; resulting stateb4f38791. Pre-move live refusal separately proved no state write while old paths remained.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-live-brief-archive-9444f2b2abdf/live-apply-result.json",
        "sha256": "3b159d6d1f50fbde0757f4b68e1a7c4917f514c6c2c995585809e6322d8b937a"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": null,
      "reason": null
    },
    {
      "id": "R5-ARCH-FULL-GATE-W38",
      "acceptance": [
        "R5-ARCH-AC-READ",
        "R5-ARCH-AC-RELOCATE"
      ],
      "kind": "behavior",
      "status": "pass",
      "command": [
        "pwsh",
        "-NoProfile",
        "-NonInteractive",
        "-File",
        "build/wave38-final-gate.ps1"
      ],
      "method": "Exclusive gate49230 completed exit0: cheap changed-file hooks177.94s, complete Release build236.81s, all281 CTests1119.95s, skip audit1.76s. Independently rehashed all four phase logs and59 current pinned code inputs and counted281 individual Passed records. Skip audit retained27 reviewed runtime skips, none stale; this gate does not certify skipped external/hardware operations. Exact code binding manifest is build/wave38-final-code-inputs.json, SHA4631484369ee819d4e70f27d537719492ace11bfe887abe1093a449d5deb9d8c. New MCP code changes start only after this terminal checkpoint and require their own gate.",
      "environment": {
        "platform": "windows",
        "tools": {
          "git": "2.52.0.windows.1",
          "python": "3.14.0"
        }
      },
      "exit_code": 0,
      "artifact": {
        "path": "build/wave38-final-gate-results.json",
        "sha256": "2b27ef16da41008798b3f01a4ae2bb6030c11a6f43ffcf51eb856c9ec0d8de20"
      },
      "inputs": [
        {
          "path": ".gitattributes",
          "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
        },
        {
          "path": ".pre-commit-config.yaml",
          "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
        },
        {
          "path": "AGENTS.md",
          "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
          "path": "scripts/review_ledger.py",
          "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
        },
        {
          "path": "tests/CMakeLists.txt",
          "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
        },
        {
          "path": "tests/README.md",
          "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
        },
        {
          "path": "tests/unit/test_review_ledger.py",
          "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
        }
      ],
      "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
      "red": null,
      "reason": null
    }
  ],
  "checkpoint": {
    "scope_sha256": "470292e0d458f1b210105bb8298e012363727350adea147d0ebe931bdd55802b",
    "inputs": [
      {
        "path": ".gitattributes",
        "sha256": "d229943b94d38461d35d858eb300d387bb4add602953e42204bbac4e18dd6fc5"
      },
      {
        "path": ".pre-commit-config.yaml",
        "sha256": "ed2c2b722eb617a03b9162882aab5e4b0ec5fd67995972ccbc396d9aa2fbd750"
      },
      {
        "path": "AGENTS.md",
        "sha256": "da9ac2cbfb6a78e50b27245ed746ee0a4e6783798f666bcb8c10568a04436654"
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
        "path": "scripts/review_ledger.py",
        "sha256": "90747045df00486f4eba4377e02d114c03b1f42031859c2441e995cdd5f53157"
      },
      {
        "path": "tests/CMakeLists.txt",
        "sha256": "808fe51649129b218a1d66cbda60913979deb92dc13a7c51aaab118a21077a47"
      },
      {
        "path": "tests/README.md",
        "sha256": "fb5a2a26dea09dc5fc36bba7d76af7c504a507ae6c4ae19dd362517ce89fd4fc"
      },
      {
        "path": "tests/unit/test_review_ledger.py",
        "sha256": "73a469b668b4de0f014f57446431c0a0b38221968f3db7d1968105f3fe4153d7"
      }
    ],
    "environment": {
      "platform": "windows",
      "tools": {
        "git": "2.52.0.windows.1",
        "python": "3.14.0"
      }
    },
    "verified_tasks": [
      "R5-ARCH-TASK-SUPPORT"
    ],
    "pending_operations": [],
    "next_action": "Scoped archive support is verified:77 maintained tests,31 independent guard triples, explicit live40 migration and the complete wave38 hooks/Release/281CTest/skip gate. Archive this unbound completed plan byte-exactly, validate closure after move, and rerun doc hooks before commit. Broader R5, fresh CAP final-path receipts, MCP API/lifetime/containment, cold startup, full-app E2E and hardware certification remain incomplete; future plans stay parked.",
    "source_revision": "65fa4c58f251f4b5949797a74e71a80f5e7e4e60"
  }
}
```
