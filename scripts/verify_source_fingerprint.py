"""Fail-closed Windows path and byte checks for native certification source manifests."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
import sys


MAX_MANIFEST_BYTES = 8 * 1024 * 1024
HASH_READ_BYTES = 1024 * 1024
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")


class FingerprintError(ValueError):
    """Input evidence cannot establish the requested source binding."""


def ordinary_path(path: Path, *, directory: bool = False) -> Path:
    """Inspect lexical ancestors before opening a child, including Windows junctions."""
    if os.name != "nt":
        raise FingerprintError("Source fingerprint path verification requires Windows")
    absolute = Path(os.path.abspath(path))
    for component in reversed((absolute, *absolute.parents)):
        info = component.lstat()
        if info.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise FingerprintError(f"Reparse source or evidence path is refused: {component}")
    info = absolute.stat()
    expected_type = stat.S_ISDIR if directory else stat.S_ISREG
    if not expected_type(info.st_mode):
        raise FingerprintError(f"Source fingerprint path has the wrong file type: {absolute}")
    return absolute


def source_input(root: Path, value: str) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = root / candidate
    candidate = Path(os.path.abspath(candidate))
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise FingerprintError(f"Source fingerprint input escapes source root: {value}") from error
    return ordinary_path(candidate)


def read_evidence(path: Path) -> bytes:
    admitted = ordinary_path(path)
    with admitted.open("rb") as stream:
        content = stream.read(MAX_MANIFEST_BYTES + 1)
    ordinary_path(admitted)
    if not content or len(content) > MAX_MANIFEST_BYTES:
        raise FingerprintError("Source fingerprint evidence is empty or exceeds its size limit")
    return content


def source_hash(path: Path) -> str:
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(HASH_READ_BYTES):
            digest.update(block)
    ordinary_path(path)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (
            after.st_size, after.st_mtime_ns, after.st_ino):
        raise FingerprintError(f"Source input changed while hashing: {path}")
    return digest.hexdigest()


def check_paths(source_root: Path, paths_file: Path) -> None:
    root = ordinary_path(source_root, directory=True)
    values = read_evidence(paths_file).decode("utf-8").splitlines()
    if not values or any(not value for value in values):
        raise FingerprintError("Source fingerprint requires nonempty input paths")
    for value in values:
        source_input(root, value)


def manifest_records(content: bytes) -> list[tuple[str, str]]:
    if b"\r" in content or not content.endswith(b"\n") or content.startswith(b"\xef\xbb\xbf"):
        raise FingerprintError("Source fingerprint manifest must use exact UTF-8 LF records")
    records: list[tuple[str, str]] = []
    seen: set[str] = set()
    for line in content.decode("utf-8").split("\n")[:-1]:
        relative, separator, digest = line.rpartition(":")
        parts = relative.split("/")
        if (not separator or not SHA256_PATTERN.fullmatch(digest) or ":" in relative
                or "\\" in relative or any(part in ("", ".", "..") for part in parts)
                or any(ord(character) < 32 for character in relative)):
            raise FingerprintError("Source fingerprint manifest contains an invalid record")
        identity = os.path.normcase(relative)
        if identity in seen:
            raise FingerprintError(f"Source fingerprint manifest repeats an input: {relative}")
        seen.add(identity)
        records.append((relative, digest))
    if not records or [path for path, _ in records] != sorted(path for path, _ in records):
        raise FingerprintError("Source fingerprint manifest is empty or not canonically sorted")
    return records


def verify_manifest(source_root: Path, manifest: Path, expected_sha256: str) -> None:
    if not SHA256_PATTERN.fullmatch(expected_sha256):
        raise FingerprintError("Expected source fingerprint is not a SHA-256 digest")
    root = ordinary_path(source_root, directory=True)
    content = read_evidence(manifest)
    if hashlib.sha256(content).hexdigest() != expected_sha256:
        raise FingerprintError("Source manifest bytes differ from the configured fingerprint")
    for relative, expected in manifest_records(content):
        path = source_input(root, relative)
        if source_hash(path) != expected:
            raise FingerprintError(f"Source bytes changed since configuration: {relative}; reconfigure required")
    ordinary_path(root, directory=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    paths = commands.add_parser("check-paths")
    paths.add_argument("--source-root", type=Path, required=True)
    paths.add_argument("--paths-file", type=Path, required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--source-root", type=Path, required=True)
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--expected-sha256", required=True)
    args = parser.parse_args()
    try:
        if args.command == "check-paths":
            check_paths(args.source_root, args.paths_file)
        else:
            verify_manifest(args.source_root, args.manifest, args.expected_sha256)
    except (FingerprintError, OSError, UnicodeError) as error:
        parser.exit(1, f"Source fingerprint verification failed: {error}\n")
    print("Source fingerprint verification passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
