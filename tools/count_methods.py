"""Count method body line lengths with string-literal-aware brace matching."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

def count_method_lines(filepath, method_name):
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    # Find method definition line
    start = None
    for i, line in enumerate(lines):
        if '::' + method_name + '(' in line:
            stripped = line.strip()
            if any(stripped.startswith(t) for t in ['void', 'bool', 'QString', 'int', 'static']):
                start = i
                break
    if start is None:
        return method_name, -1, -1

    # Track braces, ignoring those inside string literals and char literals
    depth = 0
    found_brace = False
    end = start
    for i in range(start, len(lines)):
        line = lines[i]
        in_string = False
        j = 0
        while j < len(line):
            ch = line[j]
            # Skip escaped characters
            if ch == '\\' and j + 1 < len(line):
                j += 2
                continue
            # Toggle string state on unescaped quote
            if ch == '"':
                in_string = not in_string
                j += 1
                continue
            # Only count braces outside strings
            if not in_string:
                if ch == '{':
                    depth += 1
                    found_brace = True
                elif ch == '}':
                    depth -= 1
                    if found_brace and depth == 0:
                        end = i
                        break
            j += 1
        if found_brace and depth == 0:
            break

    count = end - start + 1
    return method_name, start + 1, count


def _default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--repo-root',
        type=Path,
        default=_default_repo_root(),
        help='Path to the S.A.K.-Utility repo root (defaults to script-relative).',
    )
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    actions_dir = repo_root / 'src' / 'actions'

    files: list[tuple[Path, list[str]]] = [
        (
            actions_dir / 'backup_bitlocker_keys_action.cpp',
            [
                'execute',
                'executeDiscoverVolumes',
                'executeExtractKeys',
                'executeSaveKeyFiles',
                'writeJsonBackup',
                'executeBuildReport',
            ],
        ),
        (
            actions_dir / 'disable_startup_programs_action.cpp',
            [
                'execute',
                'executeScanRegistry',
                'executeScanTaskScheduler',
                'formatStartupProgramsSection',
                'formatStartupTasksSection',
                'executeDisableEntries',
                'executeBuildReport',
            ],
        ),
        (
            actions_dir / 'clear_event_logs_action.cpp',
            ['execute', 'executeEnumerateLogs', 'executeClearLogs', 'executeBuildReport'],
        ),
        (
            actions_dir / 'check_disk_errors_action.cpp',
            [
                'execute',
                'executeEnumerateVolumes',
                'buildRepairVolumeScript',
                'parseDriveScanResult',
                'executeRunChkdsk',
                'executeBuildReport',
            ],
        ),
        (
            actions_dir / 'check_bloatware_action.cpp',
            ['execute', 'executeScanApps', 'executeMatchBloatware', 'executeBuildReport'],
        ),
        (
            actions_dir / 'windows_update_action.cpp',
            ['execute', 'executeInitSession', 'executeSearchUpdates', 'executeBuildReport'],
        ),
    ]

    for fpath, methods in files:
        print(f'=== {fpath.name} ===')
        for method in methods:
            name, line, count = count_method_lines(fpath, method)
            if count < 0:
                print(f'  {name}: NOT FOUND')
            else:
                tag = 'OVER' if count > 70 else 'OK'
                print(f'  {name}: {count} lines (L{line}) [{tag}]')

    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
