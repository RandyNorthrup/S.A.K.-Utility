#!/usr/bin/env python3
"""Prove review-ledger inventory hashes only dirty tracked working-tree files."""

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

    (root / "src").mkdir(parents=True)
    unit_count = 256
    for index in range(unit_count):
        (root / "src" / f"unit_{index}.cpp").write_text(
            "int value = 0;\n", encoding="ascii")

    clean_blob = "a" * 40
    dirty_blob = "b" * 40
    dirty = {"src/unit_7.cpp", "src/unit_199.cpp"}
    index_records = "".join(
        f"100644 {clean_blob} 0\tsrc/unit_{index}.cpp\0"
        for index in range(unit_count)
    )
    calls: list[tuple[str, ...]] = []

    def fake_git(*args: str) -> str:
        calls.append(args)
        if args == ("diff-files", "--name-only", "-z", "--"):
            return "\0".join(sorted(dirty)) + "\0"
        if args == ("ls-files", "-s", "-z"):
            return index_records
        if args[0] == "hash-object":
            return dirty_blob + "\n"
        raise AssertionError(f"unexpected git call: {args}")

    ledger.REPO_ROOT = root
    ledger.git = fake_git
    units = ledger.current_units()
    hash_calls = [call for call in calls if call[0] == "hash-object"]
    assert len(units) == unit_count
    assert len(hash_calls) == len(dirty), hash_calls
    assert {call[-1] for call in hash_calls} == dirty
    assert units["src/unit_0.cpp"]["blob"] == clean_blob
    assert units["src/unit_7.cpp"]["blob"] == dirty_blob
    print("Review ledger batch hashing probe passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
