#!/usr/bin/env python3
"""Prove one filtered live-hash batch covers every unit, including Git-hidden file edits."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


def main() -> int:
    ledger_path = Path(sys.argv[1])
    root = Path(sys.argv[2])
    spec = importlib.util.spec_from_file_location("review_ledger_probe", ledger_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load review ledger: {ledger_path}")
    ledger = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ledger)

    if root.exists():
        raise RuntimeError("batch probe requires a new disposable fixture directory")
    (root / "src").mkdir(parents=True)
    unit_count = 256
    paths = [f"src/unit_{index}.cpp" for index in range(unit_count)]
    paths[8] = "src/unit with space.cpp"
    paths[9] = "src/unit_caf\u00e9.cpp"
    for path in paths:
        (root / path).write_bytes(b"int value = 0;\r\n")
    (root / ".gitattributes").write_text("*.cpp text eol=lf\n", encoding="ascii")
    ledger.REPO_ROOT = root
    real_git = ledger.git
    real_git("init", "--quiet")
    real_git("config", "core.autocrlf", "false")
    real_git("add", ".gitattributes", "src")
    real_git("update-index", "--assume-unchanged", "src/unit_7.cpp")
    real_git("update-index", "--skip-worktree", "src/unit_199.cpp")
    dirty = {"src/unit_7.cpp", "src/unit_199.cpp"}
    for path in dirty:
        (root / path).write_bytes(b"int value = 1;\r\n")
    assert not real_git("diff-files", "--name-only").strip()
    clean_blob = real_git("hash-object", "--path=src/unit_0.cpp", "--", "src/unit_0.cpp").strip()
    dirty_blob = real_git("hash-object", "--path=src/unit_7.cpp", "--", "src/unit_7.cpp").strip()
    assert clean_blob != dirty_blob
    assert clean_blob != real_git("hash-object", "--no-filters", "--", "src/unit_0.cpp").strip()
    calls: list[tuple[str, ...]] = []

    def recording_git(*args: str, **keywords) -> str:
        calls.append(args)
        return real_git(*args, **keywords)

    ledger.git = recording_git
    units = ledger.current_units()
    hash_calls = [call for call in calls if call[0] == "hash-object"]
    assert len(units) == unit_count
    assert hash_calls == [("hash-object", "--stdin-paths")], hash_calls
    assert not any(call[0] == "diff-files" for call in calls)
    for path in paths:
        assert units[path]["blob"] == (dirty_blob if path in dirty else clean_blob), path
    print("Review ledger batch hashing probe passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
