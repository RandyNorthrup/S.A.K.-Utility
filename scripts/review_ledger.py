#!/usr/bin/env python3
"""The per-file review ledger: which units have been reviewed, and which have CHANGED since.

R5-LEDGER-1/2/3.

WHY THIS EXISTS
The remediation doc claimed for months that "the drivers are idempotent and claim-based, so the
run resumes from exactly where it stopped". Checked against the live tree on 2026-08-25: there
are no drivers and no state anywhere in the repository. Nothing recorded which units had been
reviewed, so the headline "764 of 1098 units run" could not be verified from the tree, could not
be resumed from, and could not be audited. A number nobody can reproduce is not progress.

WHAT A UNIT IS
One reviewable file, enumerated from HEAD by an explicit rule (see UNIT_RULES). The rule lives
here rather than in someone's memory so the denominator is reproducible: run `inventory` and you
get today's number, not a number from a tree that has since gained and lost files.

WHAT MAKES RESUMPTION REAL
Every unit is keyed by path AND by its git BLOB SHA. A unit counts as reviewed only if its blob
is the exact one that was reviewed; edit the file and it returns to pending automatically. That
is the property the old claim asserted without implementing -- and it is what stops a review from
being credited to a file that has since been rewritten.

EVIDENCE, NOT ASSERTION
Each reviewed unit records HOW that is known:
  driver      -- this ledger ran the review and stored the brief. The only self-verifying status.
  cited       -- the file is cited in the remediation document's findings, so it was demonstrably
                 looked at by the earlier campaign, but no brief survives in the tree. Seeded by
                 `seed-from-citations` and deliberately distinguished from `driver`, because
                 inferring coverage from citations is exactly the assert-instead-of-measure
                 mistake this ledger exists to stop repeating.

Usage:
  python scripts/review_ledger.py inventory            # rebuild unit list from HEAD
  python scripts/review_ledger.py seed-from-citations  # mark cited units, with evidence=cited
  python scripts/review_ledger.py status               # counts by state and evidence
  python scripts/review_ledger.py next --count 20      # the next pending units to review
  python scripts/review_ledger.py mark <path> --brief <file>   # record a completed review
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
STATE_PATH = REPO_ROOT / "scripts" / "review_ledger_state.json"
REMEDIATION_DOC = REPO_ROOT / "docs" / "CODEX_REVIEW_5_REMEDIATION.md"

# The unit rule, stated once and executed here. Anything not matched is not a review unit --
# generated files, binaries, resources and docs are excluded on purpose.
UNIT_RULES = [
    ("include", lambda p: p.startswith("include/") and p.endswith((".h", ".hpp"))),
    ("src", lambda p: p.startswith("src/") and p.endswith((".cpp", ".h", ".hpp"))),
    ("tests", lambda p: p.startswith("tests/") and p.endswith((".cpp", ".h", ".mjs"))),
    ("scripts", lambda p: p.startswith("scripts/") and p.endswith((".py", ".ps1", ".sh"))),
    ("browser", lambda p: p.startswith("browser/") and p.endswith(".js")),
]


def fail(message: str) -> None:
    print(f"REVIEW LEDGER FAILED: {message}")
    sys.exit(1)


def git(*args: str) -> str:
    proc = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True, text=True,
                          check=False)
    if proc.returncode != 0:
        fail(f"git {' '.join(args)}: {proc.stderr.strip()}")
    return proc.stdout


def classify(path: str) -> str | None:
    for group, matches in UNIT_RULES:
        if matches(path):
            return group
    return None


def current_units() -> dict[str, dict]:
    """path -> {group, blob} for every unit at HEAD, straight from the index."""
    units: dict[str, dict] = {}
    # ls-files -s gives "<mode> <blob sha> <stage>\t<path>", so the blob comes from git rather
    # than from hashing the working tree -- a dirty file cannot be credited as reviewed.
    for line in git("ls-files", "-s").splitlines():
        if not line.strip():
            continue
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) < 2:
            continue
        group = classify(path)
        if group:
            units[path] = {"group": group, "blob": parts[1]}
    if not units:
        fail("no units enumerated from git ls-files; refusing to write an empty ledger.")
    return units


def load_state() -> dict:
    if not STATE_PATH.exists():
        return {"units": {}}
    return json.loads(STATE_PATH.read_text(encoding="utf-8"))


def save_state(state: dict) -> None:
    STATE_PATH.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def cmd_inventory() -> int:
    units = current_units()
    state = load_state()
    existing = state.get("units", {})
    merged: dict[str, dict] = {}
    revived = 0
    for path, meta in units.items():
        prior = existing.get(path)
        if prior and prior.get("reviewed_blob") == meta["blob"]:
            merged[path] = prior          # unchanged since its review: stays reviewed
        elif prior and prior.get("reviewed_blob"):
            # Reviewed, then EDITED. Back to pending -- the review no longer describes the file.
            merged[path] = {"group": meta["group"], "status": "pending",
                            "previous_review_blob": prior.get("reviewed_blob"),
                            "previous_evidence": prior.get("evidence")}
            revived += 1
        else:
            merged[path] = {"group": meta["group"], "status": "pending"}
    dropped = sorted(set(existing) - set(units))
    state["units"] = merged
    state["head_commit"] = git("rev-parse", "HEAD").strip()
    state["unit_rule"] = ("include/**.h|.hpp, src/**.cpp|.h|.hpp, tests/**.cpp|.h|.mjs, "
                          "scripts/**.py|.ps1|.sh, browser/**.js")
    save_state(state)
    print(f"inventory: {len(merged)} units at {state['head_commit'][:12]}")
    if revived:
        print(f"  {revived} previously-reviewed unit(s) CHANGED and returned to pending")
    if dropped:
        print(f"  {len(dropped)} unit(s) no longer exist and were removed from the ledger")
    return 0


def cmd_seed_from_citations() -> int:
    """Mark units the remediation doc demonstrably cites, with evidence=cited (not driver)."""
    if not REMEDIATION_DOC.exists():
        fail(f"{REMEDIATION_DOC} not found")
    doc = REMEDIATION_DOC.read_text(encoding="utf-8", errors="replace")
    state = load_state()
    units = state.get("units", {})
    if not units:
        fail("run `inventory` first")
    seeded = 0
    for path, meta in units.items():
        if meta.get("status") == "reviewed":
            continue
        basename = Path(path).name
        # A citation is the file's own name appearing in the document. Deliberately generous:
        # the point is to avoid re-billing work that was demonstrably done, and every such unit
        # is tagged `cited` so it is never mistaken for a self-verifying driver review.
        if re.search(r"\b" + re.escape(basename) + r"\b", doc):
            meta["status"] = "reviewed"
            meta["evidence"] = "cited"
            meta["reviewed_blob"] = None  # unknown: no brief survives, so it cannot be pinned
            seeded += 1
    state["units"] = units
    save_state(state)
    print(f"seeded {seeded} unit(s) as reviewed with evidence=cited")
    print("NOTE: evidence=cited means 'the doc cites this file', NOT 'a brief exists'. Only "
          "evidence=driver is self-verifying.")
    return 0


def cmd_status() -> int:
    state = load_state()
    units = state.get("units", {})
    if not units:
        fail("no ledger yet; run `inventory` first")
    by_group: dict[str, dict[str, int]] = {}
    for meta in units.values():
        group = meta.get("group", "?")
        key = meta.get("status", "pending")
        if key == "reviewed":
            key = f"reviewed({meta.get('evidence', 'unknown')})"
        by_group.setdefault(group, {}).setdefault(key, 0)
        by_group[group][key] += 1
    total_pending = sum(1 for m in units.values() if m.get("status") != "reviewed")
    print(f"ledger at {state.get('head_commit', '?')[:12]}: {len(units)} units, "
          f"{total_pending} pending")
    for group in sorted(by_group):
        detail = ", ".join(f"{k}={v}" for k, v in sorted(by_group[group].items()))
        print(f"  {group:9s} {detail}")
    driver_reviewed = sum(1 for m in units.values() if m.get("evidence") == "driver")
    print(f"\nself-verifying (evidence=driver, a stored brief): {driver_reviewed}")
    return 0


def cmd_next(count: int, group: str) -> int:
    state = load_state()
    units = state.get("units", {})
    pending = [p for p, m in sorted(units.items())
               if m.get("status") != "reviewed" and (not group or m.get("group") == group)]
    if not pending:
        print("no pending units")
        return 0
    print(f"{len(pending)} pending; next {min(count, len(pending))}:")
    for path in pending[:count]:
        print(f"  {path}")
    return 0


# Codex writes em dashes and curly quotes. The repo requires plain 7-bit ASCII in tracked text
# (there is a pre-commit gate for it), so a brief is normalized on the way in rather than left to
# fail the commit later -- and rather than being quietly dropped from the tree, which is how the
# previous campaign ended up with no briefs at all.
ASCII_REPLACEMENTS = {
    chr(0x2014): "--",   # em dash
    chr(0x2013): "-",    # en dash
    chr(0x2018): chr(0x27),   # left single quote
    chr(0x2019): chr(0x27),   # right single quote
    chr(0x201C): chr(0x22),   # left double quote
    chr(0x201D): chr(0x22),   # right double quote
    chr(0x2026): "...",  # ellipsis
    chr(0x00A0): " ",    # non-breaking space
    chr(0x2192): "->",   # rightwards arrow
    chr(0x2713): "[x]",  # check mark
    chr(0x00B7): "-",    # middle dot
}


def normalize_to_ascii(text: str) -> tuple[str, int]:
    for bad, good in ASCII_REPLACEMENTS.items():
        text = text.replace(bad, good)
    # Anything still outside ASCII is replaced rather than silently kept: an unreadable brief is
    # better than a commit that cannot land.
    replaced = sum(1 for ch in text if ord(ch) > 0x7F)
    if replaced:
        text = text.encode("ascii", "replace").decode("ascii")
    return text, replaced


def cmd_mark(path: str, brief: str) -> int:
    brief_path = REPO_ROOT / brief
    if brief_path.exists():
        original = brief_path.read_text(encoding="utf-8", errors="replace")
        cleaned, forced = normalize_to_ascii(original)
        if cleaned != original:
            brief_path.write_text(cleaned, encoding="utf-8", newline="")
            note = f" ({forced} char(s) had no ASCII equivalent)" if forced else ""
            print(f"normalized {brief} to 7-bit ASCII{note}")
    state = load_state()
    units = state.get("units", {})
    if path not in units:
        fail(f"{path} is not a unit in the ledger (run `inventory` if it is new)")
    blobs = current_units()
    units[path].update({"status": "reviewed", "evidence": "driver",
                        "reviewed_blob": blobs[path]["blob"], "brief": brief})
    state["units"] = units
    save_state(state)
    print(f"marked {path} reviewed (blob {blobs[path]['blob'][:12]}, brief {brief})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("inventory")
    sub.add_parser("seed-from-citations")
    sub.add_parser("status")
    nxt = sub.add_parser("next")
    nxt.add_argument("--count", type=int, default=20)
    nxt.add_argument("--group", default="")
    mark = sub.add_parser("mark")
    mark.add_argument("path")
    mark.add_argument("--brief", required=True)

    args = parser.parse_args()
    if args.cmd == "inventory":
        return cmd_inventory()
    if args.cmd == "seed-from-citations":
        return cmd_seed_from_citations()
    if args.cmd == "status":
        return cmd_status()
    if args.cmd == "next":
        return cmd_next(args.count, args.group)
    return cmd_mark(args.path, args.brief)


if __name__ == "__main__":
    sys.exit(main())
