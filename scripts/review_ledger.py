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
One reviewable tracked file, enumerated by an explicit rule (see UNIT_RULES). The rule lives
here rather than in someone's memory so the denominator is reproducible: run `inventory` and you
get today's number, not a number from a tree that has since gained and lost files.

WHAT MAKES RESUMPTION REAL
Every driver unit binds filtered working-tree Git blobs for BOTH source and stored brief. A unit
counts as reviewed only while both match; edit either file and it returns to pending. That
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
  python scripts/review_ledger.py inventory            # reconcile tracked working-tree units
  python scripts/review_ledger.py seed-from-citations  # mark cited units, with evidence=cited
  python scripts/review_ledger.py status               # counts by state and evidence
  python scripts/review_ledger.py next --count 20      # the next pending units to review
  python scripts/review_ledger.py mark <path> --brief <file> --expected-blob <sha>
                                                        # record exact reviewed bytes
  python scripts/review_ledger.py migrate-brief-bindings --manifest <file>
                                                        # explicit legacy evidence confirmation
  python scripts/review_ledger.py relocate-briefs --manifest <file>
                                                        # reconcile owner-moved exact brief bytes
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
STATE_PATH = REPO_ROOT / "scripts" / "review_ledger_state.json"
REMEDIATION_DOC = REPO_ROOT / "docs" / "CODEX_REVIEW_5_REMEDIATION.md"
STATE_SCHEMA_VERSION = 2
GIT_TIMEOUT_SECONDS = 120
MAX_BRIEF_BYTES = 1024 * 1024
MAX_MIGRATION_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_RELOCATION_ENTRIES = 1000
MAX_RELOCATION_HISTORY = 1000
GIT_BLOB_PATTERN = r"(?:[0-9a-f]{40}|[0-9a-f]{64})"

# The unit rule, stated once and executed here. Anything not matched is not a review unit --
# generated files, binaries, resources and docs are excluded on purpose.
UNIT_RULES = {
    "include": (".h", ".hpp"),
    "src": (".c", ".cpp", ".h", ".hpp"),
    "tests": (".c", ".cpp", ".h", ".hpp", ".js", ".mjs", ".py", ".ps1", ".sh"),
    "scripts": (".py", ".ps1", ".sh", ".cmd", ".bat"),
    "browser": (".js",),
    "cmake": (".cmake",),
}
UNIT_FILE_RULES = {"CMakeLists.txt": "cmake", "tests/CMakeLists.txt": "cmake"}


def fail(message: str) -> None:
    print(f"REVIEW LEDGER FAILED: {message}")
    sys.exit(1)


def git(*args: str, input_text: str | None = None) -> str:
    try:
        # Text-mode stdin on Windows turns an existing CRLF into CRCRLF. Feed exact UTF-8
        # bytes so brief hashes and path lists are not changed by the transport itself.
        payload = input_text.encode("utf-8") if input_text is not None else None
        proc = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True,
                              input=payload, check=False,
                              timeout=GIT_TIMEOUT_SECONDS)
        stdout = proc.stdout.decode("utf-8")
        stderr = proc.stderr.decode("utf-8")
    except (OSError, subprocess.TimeoutExpired, UnicodeError) as error:
        fail(f"git {' '.join(args)} could not complete: {error}")
    if proc.returncode != 0:
        fail(f"git {' '.join(args)}: {stderr.strip()}")
    return stdout


def classify(path: str) -> str | None:
    if path in UNIT_FILE_RULES:
        return UNIT_FILE_RULES[path]
    for group, extensions in UNIT_RULES.items():
        if path.startswith(f"{group}/") and path.endswith(extensions):
            return group
    return None


def unit_rule_description() -> str:
    patterns = [f"{group}/**{'|'.join(extensions)}" for group, extensions in UNIT_RULES.items()]
    patterns.extend(f"{path}={group}" for path, group in UNIT_FILE_RULES.items())
    return ", ".join(patterns)


def working_tree_blob(path: str) -> str:
    blob = git("hash-object", f"--path={path}", "--", path).strip()
    if not re.fullmatch(GIT_BLOB_PATTERN, blob):
        fail(f"could not hash tracked review unit: {path}")
    return blob


def relative_file(path: str, *, brief: bool = False, archived: bool = False,
                  missing_ok: bool = False) -> Path:
    """Admit a regular repository-owned file without traversal, drive/ADS, or link escape."""
    if not isinstance(path, str) or not path or "\\" in path or ":" in path:
        raise ValueError("file path must be a repository-relative forward-slash path")
    parts = path.split("/")
    if any(part in ("", ".", "..") for part in parts) or any(ord(char) < 32 for char in path):
        raise ValueError("file path contains traversal or control characters")
    brief_root = "docs/review_briefs"
    if archived and parts[:3] == ["docs", "archive", "review_briefs"]:
        brief_root = "docs/archive/review_briefs"
    if brief and (not path.startswith(brief_root + "/") or not path.endswith(".md")):
        raise ValueError("review brief must be a Markdown file under its explicit allowed root")
    target = REPO_ROOT / path
    try:
        resolved = target.resolve(strict=not missing_ok)
        resolved.relative_to(REPO_ROOT.resolve(strict=True))
        if brief:
            resolved.relative_to((REPO_ROOT / brief_root).resolve(strict=not missing_ok))
        if missing_ok and not os.path.lexists(target):
            return target
        if not stat.S_ISREG(target.lstat().st_mode):
            raise ValueError("file must be regular, not a symbolic link or directory")
    except (OSError, ValueError) as error:
        raise ValueError(f"file is missing, unreadable, or outside its allowed root: {path}") from error
    return target


def working_tree_blobs(paths: list[str]) -> dict[str, str]:
    """Batch actual file reads, preserving per-path Git filters without trusting index stat bits."""
    if not paths:
        return {}
    # Paths are admitted before batching. Quoting preserves Git's path grammar; stdin avoids
    # Windows' aggregate command-line length limit without changing per-path filters.
    names = "".join(json.dumps(path, ensure_ascii=False) + "\n" for path in paths)
    blobs = git("hash-object", "--stdin-paths", input_text=names).splitlines()
    if len(blobs) != len(paths) or any(not re.fullmatch(GIT_BLOB_PATTERN, blob) for blob in blobs):
        fail("batch hashing did not return exactly one valid live blob per review unit")
    return dict(zip(paths, blobs, strict=True))


def current_units() -> dict[str, dict]:
    """Tracked unit paths with hashes of filtered working-tree bytes."""
    units: dict[str, dict] = {}
    for record in git("ls-files", "-s", "-z").split("\0"):
        if not record:
            continue
        metadata, separator, path = record.partition("\t")
        fields = metadata.split()
        if not separator or len(fields) != 3 or fields[2] != "0":
            fail(f"could not parse stage-zero tracked review unit: {record!r}")
        group = classify(path)
        if group:
            if fields[0] not in ("100644", "100755") or path in units:
                fail(f"tracked review unit is not one regular stage-zero file: {path}")
            try:
                relative_file(path)
            except ValueError as error:
                fail(str(error))
            units[path] = {"group": group}
    if not units:
        fail("no units enumerated from git ls-files; refusing to write an empty ledger.")
    blobs = working_tree_blobs(list(units))
    return {path: {**meta, "blob": blobs[path]} for path, meta in units.items()}


def legacy_driver_units(state: dict) -> dict[str, dict]:
    return {path: meta for path, meta in state["units"].items()
            if meta.get("status") == "reviewed" and meta.get("evidence") == "driver"
            and not meta.get("reviewed_brief_blob")}


def load_state(*, allow_legacy: bool = False, raw_state: bytes | None = None) -> dict:
    if raw_state is None and not STATE_PATH.exists():
        return {"schema_version": STATE_SCHEMA_VERSION, "units": {}}
    try:
        payload = STATE_PATH.read_bytes() if raw_state is None else raw_state
        state = json.loads(payload.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"could not read ledger state: {error}")
    if (not isinstance(state, dict) or not isinstance(state.get("units"), dict)
            or type(state.get("schema_version", 1)) is not int
            or state.get("schema_version", 1) not in (1, STATE_SCHEMA_VERSION)):
        fail("ledger state has an unsupported schema")
    for path, meta in state["units"].items():
        if (not isinstance(path, str) or not isinstance(meta, dict)
                or meta.get("status") not in ("pending", "reviewed")):
            fail("ledger unit has an invalid path, record, or status")
        if meta.get("status") == "reviewed" and meta.get("evidence") not in ("driver", "cited"):
            fail(f"reviewed unit has unknown evidence: {path}")
        for field in ("reviewed_blob", "reviewed_brief_blob"):
            value = meta.get(field)
            if value is not None and (not isinstance(value, str)
                                      or not re.fullmatch(GIT_BLOB_PATTERN, value)):
                fail(f"invalid {field} in ledger unit: {path}")
    if not allow_legacy and legacy_driver_units(state):
        fail("legacy driver evidence requires explicit migrate-brief-bindings confirmation")
    return state


def atomic_write(path: Path, payload: bytes) -> None:
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", dir=path.parent,
                                         prefix=".review-ledger-", delete=False) as temporary:
            temporary_path = Path(temporary.name)
            temporary.write(payload)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, path)
    except OSError as error:
        fail(f"could not atomically write {path.name}: {error}")
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def save_state(state: dict) -> None:
    state["schema_version"] = STATE_SCHEMA_VERSION
    atomic_write(STATE_PATH, (json.dumps(state, indent=2, sort_keys=True) + "\n").encode("utf-8"))


def read_brief(brief: str, *, archived: bool = False) -> tuple[Path, bytes, str]:
    target = relative_file(brief, brief=True, archived=archived)
    try:
        with target.open("rb") as stream:
            original = stream.read(MAX_BRIEF_BYTES + 1)
        if len(original) > MAX_BRIEF_BYTES:
            raise ValueError("review brief exceeds the size limit")
        text = original.decode("utf-8")
    except (OSError, UnicodeError) as error:
        raise ValueError(f"review brief is unreadable UTF-8: {brief}") from error
    if not text.strip():
        raise ValueError(f"review brief is empty: {brief}")
    return target, original, text


def brief_blob(brief: str, text: str) -> str:
    blob = git("hash-object", f"--path={brief}", "--stdin", input_text=text).strip()
    if not re.fullmatch(GIT_BLOB_PATTERN, blob):
        fail(f"could not hash review brief: {brief}")
    return blob


def driver_evidence_matches(prior: dict, source_blob: str) -> bool:
    if prior.get("reviewed_blob") != source_blob:
        return False
    try:
        _, _, text = read_brief(prior.get("brief", ""), archived=True)
    except ValueError:
        return False
    return brief_blob(prior["brief"], text) == prior.get("reviewed_brief_blob")


def invalidate_review(prior: dict, group: str) -> dict:
    return {"group": group, "status": "pending",
            "previous_review_blob": prior.get("reviewed_blob"),
            "previous_evidence": prior.get("evidence"),
            "previous_brief": prior.get("brief"),
            "previous_brief_blob": prior.get("reviewed_brief_blob")}


def cmd_inventory(quiet: bool = False) -> int:
    state = load_state()
    units = current_units()
    existing = state.get("units", {})
    merged: dict[str, dict] = {}
    revived = 0
    for path, meta in units.items():
        prior = existing.get(path)
        if (prior and prior.get("status") == "reviewed" and prior.get("evidence") == "driver"
                and driver_evidence_matches(prior, meta["blob"])):
            merged[path] = prior          # unchanged since its review: stays reviewed
        elif (prior and prior.get("status") == "reviewed" and
              prior.get("evidence") == "cited" and not prior.get("reviewed_blob")):
            merged[path] = prior          # citation evidence has no surviving blob to invalidate
        elif prior and prior.get("reviewed_blob"):
            # Reviewed, then EDITED. Back to pending -- the review no longer describes the file.
            merged[path] = invalidate_review(prior, meta["group"])
            revived += 1
        elif prior:
            # Repeated status/next refreshes must not discard why a pending unit was revived.
            merged[path] = {**prior, "group": meta["group"], "status": "pending"}
        else:
            merged[path] = {"group": meta["group"], "status": "pending"}
    dropped = sorted(set(existing) - set(units))
    state["units"] = merged
    state["head_commit"] = git("rev-parse", "HEAD").strip()
    state["unit_rule"] = unit_rule_description()
    save_state(state)
    if not quiet:
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
    cmd_inventory(quiet=True)
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
    driver_reviewed = sum(1 for m in units.values()
                          if m.get("status") == "reviewed" and m.get("evidence") == "driver")
    print(f"\nself-verifying (evidence=driver, a stored brief): {driver_reviewed}")
    return 0


def cmd_next(count: int, group: str) -> int:
    if type(count) is not int or count <= 0:
        fail("--count must be a positive integer")
    if group and group not in UNIT_RULES:
        fail(f"unknown review group: {group}")
    cmd_inventory(quiet=True)
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


def strip_absolute_repo_paths(text: str) -> str:
    """Rewrite absolute paths into the repo as repo-relative ones.

    Codex cites files by absolute path. Those carry a developer's user directory, which the
    repo's secret/path scan rejects outright (and rightly: a committed absolute path leaks the
    machine it was produced on). They are also useless to anyone else, since the repo lives
    somewhere different on every machine.
    """
    root = str(REPO_ROOT)
    for variant in (root.replace("\\", "/"), root, root.replace("/", "\\")):
        text = text.replace(variant + "/", "").replace(variant + "\\", "").replace(variant, "")
    return text


def redact_user_paths(text: str) -> str:
    """Replace a concrete Windows user-profile path with the sanctioned placeholder.

    A brief may legitimately quote a user-profile path as an EXAMPLE, but the repo's secret/path
    scan rejects any committed profile path whose name segment is not one of its placeholders --
    correctly, since a real profile name identifies a person and the machine the review ran on.
    The example keeps its meaning with the name segment replaced by the placeholder.
    """
    return re.sub(r"([A-Za-z]:[\\/]+Users[\\/]+)"
                  r"(?!Username\b|Public\b|Default\b|All Users\b)[^\\/\s\"']+",
                  r"\1Username",
                  text)


def normalize_to_ascii(text: str) -> str:
    for bad, good in ASCII_REPLACEMENTS.items():
        text = text.replace(bad, good)
    # Unknown characters cannot silently become question marks in review evidence. Known,
    # meaning-preserving substitutions above remain part of the documented ASCII intake.
    if any(ord(char) > 0x7F for char in text):
        fail("review brief contains characters without a defined ASCII substitution")
    if any(ord(char) < 32 and char not in "\r\n\t" for char in text):
        fail("review brief contains unsupported control characters")
    return text


def cmd_mark(path: str, brief: str, expected_blob: str) -> int:
    if not re.fullmatch(GIT_BLOB_PATTERN, expected_blob):
        fail("--expected-blob must be a Git object hash")
    state = load_state()
    units = state.get("units", {})
    if path not in units:
        fail(f"{path} is not a unit in the ledger (run `inventory` if it is new)")
    blobs = current_units()
    if path not in blobs:
        fail(f"tracked review target is missing or unreadable: {path}")
    actual_blob = working_tree_blob(path)
    if actual_blob != expected_blob:
        fail(f"{path} changed after review: expected {expected_blob}, "
             f"found {actual_blob}")
    try:
        brief_path, original_bytes, original = read_brief(brief)
    except ValueError as error:
        fail(str(error))
    cleaned = normalize_to_ascii(redact_user_paths(strip_absolute_repo_paths(original)))
    if not cleaned.strip():
        fail("review brief is empty after normalization")
    reviewed_brief_blob = brief_blob(brief, cleaned)
    # Admission is complete before either file is touched. Recheck the input that will be
    # replaced, so a concurrently edited brief cannot be silently overwritten by normalization.
    if brief_path.read_bytes() != original_bytes or working_tree_blob(path) != expected_blob:
        fail("review source or brief changed during mark admission")
    if cleaned != original:
        atomic_write(brief_path, cleaned.encode("utf-8"))
        print(f"normalized {brief} to 7-bit ASCII")
    units[path].update({"status": "reviewed", "evidence": "driver",
                        "reviewed_blob": expected_blob, "brief": brief,
                        "reviewed_brief_blob": reviewed_brief_blob})
    state["units"] = units
    save_state(state)
    print(f"marked {path} reviewed (blob {actual_blob[:12]}, brief {brief})")
    return 0


def unique_manifest_object(pairs: list[tuple[str, object]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def migration_manifest(path: str) -> tuple[dict, bytes]:
    try:
        with Path(path).open("rb") as stream:
            raw = stream.read(MAX_MIGRATION_MANIFEST_BYTES + 1)
        if len(raw) > MAX_MIGRATION_MANIFEST_BYTES:
            fail("migration manifest exceeds the size limit")
        manifest = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_manifest_object)
    except (OSError, UnicodeError, ValueError) as error:
        fail(f"could not read migration manifest: {error}")
    if (not isinstance(manifest, dict) or type(manifest.get("schema_version")) is not int
            or manifest.get("schema_version") != 1
            or set(manifest) != {"schema_version", "state_sha256", "entries"}
            or not isinstance(manifest.get("entries"), list)
            or not isinstance(manifest.get("state_sha256"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", manifest["state_sha256"])):
        fail("migration manifest has an invalid schema or state hash")
    return manifest, raw


def migration_entries(manifest: dict, legacy: dict[str, dict]) -> dict[str, dict]:
    entries: dict[str, dict] = {}
    required = {"path", "reviewed_blob", "brief", "expected_brief_blob"}
    for entry in manifest["entries"]:
        if not isinstance(entry, dict) or set(entry) != required:
            fail("migration entry has invalid fields")
        if any(not isinstance(entry[field], str) for field in required):
            fail("migration entry fields must be strings")
        path = entry["path"]
        if path in entries:
            fail(f"duplicate migration unit: {path}")
        entries[path] = entry
    if not legacy or set(entries) != set(legacy):
        fail("migration manifest must name exactly every unbound driver unit")
    return entries


def confirm_migration_entry(entry: dict, prior: dict, live: dict[str, dict]) -> str:
    path = entry["path"]
    if (entry["reviewed_blob"] != prior.get("reviewed_blob")
            or path not in live or live[path]["blob"] != entry["reviewed_blob"]
            or entry["brief"] != prior.get("brief")):
        fail(f"legacy source identity or brief path differs from migration confirmation: {path}")
    try:
        _, _, text = read_brief(entry["brief"])
    except ValueError as error:
        fail(str(error))
    cleaned = normalize_to_ascii(redact_user_paths(strip_absolute_repo_paths(text)))
    if cleaned != text:
        fail(f"migration cannot rewrite reviewed brief: {entry['brief']}")
    digest = brief_blob(entry["brief"], text)
    if digest != entry["expected_brief_blob"]:
        fail(f"brief differs from explicit migration hash: {entry['brief']}")
    return digest


def cmd_migrate_brief_bindings(manifest_path: str) -> int:
    """Confirm existing evidence explicitly; never turn missing historic hashes into new reviews."""
    manifest, manifest_bytes = migration_manifest(manifest_path)
    try:
        original_state = STATE_PATH.read_bytes()
    except OSError as error:
        fail(f"migration requires an existing readable ledger: {error}")
    if hashlib.sha256(original_state).hexdigest() != manifest["state_sha256"]:
        fail("ledger state differs from the explicitly reviewed migration state")
    state = load_state(allow_legacy=True)
    legacy = legacy_driver_units(state)
    entries = migration_entries(manifest, legacy)
    live = current_units()
    bindings = {path: confirm_migration_entry(entry, legacy[path], live)
                for path, entry in entries.items()}
    # The manifest may cover many reviews. Recheck the full source/brief set after admission,
    # catching an earlier entry changed while a later one was being inspected.
    final_live = current_units()
    for path, entry in entries.items():
        confirm_migration_entry(entry, legacy[path], final_live)
    if STATE_PATH.read_bytes() != original_state:
        fail("ledger state changed during migration admission")
    for path, digest in bindings.items():
        state["units"][path]["reviewed_brief_blob"] = digest
    state["brief_binding_migration"] = {
        "original_state_sha256": manifest["state_sha256"],
        "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "confirmed_units": sorted(bindings),
    }
    save_state(state)
    print(f"confirmed brief bindings for {len(bindings)} existing driver reviews; no new review credit")
    return 0


def relocation_entries(manifest: dict, units: dict[str, dict]) -> dict[str, dict]:
    required = {"path", "reviewed_blob", "brief", "reviewed_brief_blob",
                "brief_sha256", "archived_brief"}
    if not 0 < len(manifest["entries"]) <= MAX_RELOCATION_ENTRIES:
        fail("relocation requires a nonempty bounded selection")
    entries: dict[str, dict] = {}
    destinations: set[str] = set()
    for entry in manifest["entries"]:
        if (not isinstance(entry, dict) or set(entry) != required
                or any(not isinstance(entry[field], str) for field in required)):
            fail("relocation entry has invalid fields")
        path = entry["path"]
        if path in entries or entry["archived_brief"].casefold() in destinations:
            fail("duplicate relocation unit or archive destination")
        if path not in units:
            fail(f"unknown relocation unit: {path}")
        if (any(not re.fullmatch(GIT_BLOB_PATTERN, entry[field])
                for field in ("reviewed_blob", "reviewed_brief_blob"))
                or not re.fullmatch(r"[0-9a-f]{64}", entry["brief_sha256"])
                or not entry["archived_brief"].startswith("docs/archive/review_briefs/")):
            fail("relocation entry has invalid hashes or archive path")
        entries[path] = entry
        destinations.add(entry["archived_brief"].casefold())
    return entries


def confirm_relocation_entry(entry: dict, prior: dict, live: dict[str, dict]) -> str:
    path = entry["path"]
    if (prior.get("status") != "reviewed" or prior.get("evidence") != "driver"
            or any(entry[field] != prior.get(field)
                   for field in ("reviewed_blob", "brief", "reviewed_brief_blob"))
            or path not in live or live[path]["blob"] != entry["reviewed_blob"]):
        fail(f"relocation requires unchanged bound driver evidence: {path}")
    try:
        old_path = relative_file(entry["brief"], brief=True, missing_ok=True)
        if os.path.lexists(old_path):
            fail(f"old relocation brief is still present: {entry['brief']}")
        _, raw, text = read_brief(entry["archived_brief"], archived=True)
    except ValueError as error:
        fail(str(error))
    if hashlib.sha256(raw).hexdigest() != entry["brief_sha256"]:
        fail(f"archived brief raw bytes differ from pre-move identity: {path}")
    if brief_blob(entry["brief"], text) != entry["reviewed_brief_blob"]:
        fail(f"archived bytes do not match the old filtered brief binding: {path}")
    return brief_blob(entry["archived_brief"], text)


def relocation_history(state: dict) -> list[dict]:
    history = state.get("brief_relocations", [])
    if not isinstance(history, list) or len(history) >= MAX_RELOCATION_HISTORY:
        fail("relocation history is malformed or its bounded capacity is exhausted")
    required = {"original_state_sha256", "manifest_sha256", "relocated_units"}
    for record in history:
        if not isinstance(record, dict) or set(record) != required:
            fail("relocation history record has invalid fields")
        if any(not isinstance(record[field], str)
               or not re.fullmatch(r"[0-9a-f]{64}", record[field])
               for field in ("original_state_sha256", "manifest_sha256")):
            fail("relocation history record has invalid digests")
        paths = record["relocated_units"]
        if (not isinstance(paths, list) or not 0 < len(paths) <= MAX_RELOCATION_ENTRIES
                or any(not isinstance(path, str) or not path for path in paths)):
            fail("relocation history has invalid unit paths")
        if paths != sorted(set(paths)):
            fail("relocation history unit paths must be sorted and unique")
    return history


def cmd_relocate_briefs(manifest_path: str) -> int:
    """Rebind exact owner-moved briefs; never move files or create source-review credit."""
    manifest, manifest_bytes = migration_manifest(manifest_path)
    try:
        state_relative = STATE_PATH.relative_to(REPO_ROOT).as_posix()
        original_state = relative_file(state_relative).read_bytes()
        if hashlib.sha256(original_state).hexdigest() != manifest["state_sha256"]:
            fail("ledger state differs from the explicitly reviewed relocation state")
        state = load_state(raw_state=original_state)
        if state.get("schema_version") != STATE_SCHEMA_VERSION:
            fail("relocation requires current-schema bound review state")
        history = relocation_history(state)
        entries = relocation_entries(manifest, state["units"])
        live = current_units()
        bindings = {path: confirm_relocation_entry(entry, state["units"][path], live)
                    for path, entry in entries.items()}
        # Revisit every selected file after all admissions; Git filters are path-sensitive.
        final_live = current_units()
        for path, entry in entries.items():
            if confirm_relocation_entry(entry, state["units"][path], final_live) != bindings[path]:
                fail(f"archive binding changed during relocation admission: {path}")
        if relative_file(state_relative).read_bytes() != original_state:
            fail("ledger state changed during relocation admission")
    except (OSError, ValueError) as error:
        fail(f"relocation requires readable unchanged inputs: {error}")
    for path, entry in entries.items():
        state["units"][path].update({
            "brief": entry["archived_brief"], "reviewed_brief_blob": bindings[path],
        })
    state["brief_relocations"] = history + [{
        "original_state_sha256": manifest["state_sha256"],
        "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "relocated_units": sorted(entries),
    }]
    save_state(state)
    print(f"relocated {len(entries)} exact brief bindings; no new review credit or file moves")
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
    mark.add_argument("--expected-blob", required=True)
    migration = sub.add_parser("migrate-brief-bindings")
    migration.add_argument("--manifest", required=True)
    relocation = sub.add_parser("relocate-briefs")
    relocation.add_argument("--manifest", required=True)

    args = parser.parse_args()
    if args.cmd == "inventory":
        return cmd_inventory()
    if args.cmd == "seed-from-citations":
        return cmd_seed_from_citations()
    if args.cmd == "status":
        return cmd_status()
    if args.cmd == "next":
        return cmd_next(args.count, args.group)
    if args.cmd == "migrate-brief-bindings":
        return cmd_migrate_brief_bindings(args.manifest)
    if args.cmd == "relocate-briefs":
        return cmd_relocate_briefs(args.manifest)
    return cmd_mark(args.path, args.brief, args.expected_blob)


if __name__ == "__main__":
    sys.exit(main())
