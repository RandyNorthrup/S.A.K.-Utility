#!/usr/bin/env python3
"""Apply execute() decomposition patches to source files."""
import os, sys

SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "actions")
FRAG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fragments")

def patch(src_name, frag_name, marker=None):
    filepath = os.path.join(SRC, src_name)
    fragpath = os.path.join(FRAG, frag_name)
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    with open(fragpath, 'r', encoding='utf-8') as f:
        new_code = f.read()
    # Find execute() signature
    sig_idx = None
    for i, l in enumerate(lines):
        if 'execute()' in l and 'void' in l and '::' in l:
            sig_idx = i
            break
    if sig_idx is None:
        print(f"  ERROR: Could not find execute() in {src_name}")
        return False
    # Find end of execute()
    if marker:
        marker_idx = None
        for i in range(sig_idx, len(lines)):
            if marker in lines[i]:
                marker_idx = i
                break
        if marker_idx is None:
            print(f"  ERROR: Could not find marker '{marker}' in {src_name}")
            return False
        end_idx = None
        for i in range(marker_idx - 1, sig_idx, -1):
            if lines[i].strip() == '}':
                end_idx = i
                break
    else:
        ns_idx = None
        for i in range(len(lines) - 1, sig_idx, -1):
            if '} // namespace sak' in lines[i]:
                ns_idx = i
                break
        if ns_idx is None:
            print(f"  ERROR: Could not find '}} // namespace sak' in {src_name}")
            return False
        end_idx = None
        for i in range(ns_idx - 1, sig_idx, -1):
            if lines[i].strip() == '}':
                end_idx = i
                break
    if end_idx is None:
        print(f"  ERROR: Could not find end of execute() in {src_name}")
        return False
    # Apply patch
    result = lines[:sig_idx]
    if not new_code.endswith('\n'):
        new_code += '\n'
    result.append(new_code)
    result.extend(lines[end_idx + 1:])
    with open(filepath, 'w', encoding='utf-8') as f:
        f.writelines(result)
    print(f"  OK: {src_name} - replaced lines {sig_idx+1}-{end_idx+1}")
    return True

ok = True
print("Patching execute() methods...")
ok &= patch("backup_bitlocker_keys_action.cpp", "backup_bitlocker.cpp", marker="File Output")
ok &= patch("disable_startup_programs_action.cpp", "disable_startup.cpp")
ok &= patch("clear_event_logs_action.cpp", "clear_event_logs.cpp")
ok &= patch("check_disk_errors_action.cpp", "check_disk_errors.cpp")
ok &= patch("check_bloatware_action.cpp", "check_bloatware.cpp")
ok &= patch("windows_update_action.cpp", "windows_update.cpp")
print("Done!" if ok else "ERRORS occurred!")
sys.exit(0 if ok else 1)
