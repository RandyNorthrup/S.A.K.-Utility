#!/usr/bin/env python3
"""Auto-fix assertion-density warnings by inserting Q_ASSERT preconditions.

Reads the TigerStyle lint output, analyses each flagged function's signature
and body, and inserts appropriate Q_ASSERT() or assert() calls.

Safety-first design:
  - Only assert member *pointer* dereferences (m_foo->), never value members
  - Never assert non-bool-convertible types (QMap, QVector, etc.)
  - Filter out C++ type keywords from parameter matching
  - Add #include <QtGlobal> for files missing Qt headers
  - Track brace depth to avoid grabbing variables from nested scopes
  - Use .empty() for std:: types, .isEmpty() for Qt types
  - Respect 70-line function limit

Usage:
    python scripts/fix_assertions.py [--dry-run]
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

DRY_RUN = "--dry-run" in sys.argv

LINT_FILE = Path("build/lint_full2.txt")
WARNING_RE = re.compile(
    r"^(.+?):(\d+): WARNING: \[assertion-density\] "
    r"Function '(.+?)' has (\d+) assertion"
)

MEMBER_DEREF_RE = re.compile(r'\b(m_\w+)\s*->')
FUNC_SIG_RE = re.compile(r'\(([^)]*)\)')
QOBJECT_CAST_RE = re.compile(r'qobject_cast<[^>]+>\((\w+)\)')
DYNAMIC_CAST_RE = re.compile(r'dynamic_cast<[^>]+>\((\w+)\)')
EXISTING_ASSERT_RE = re.compile(
    r'(?:Q_ASSERT|Q_ASSERT_X|assert|QVERIFY|QCOMPARE|QVERIFY2)'
    r'\s*\(([^()]*(?:\([^()]*\))*[^()]*)\)'
)

# Qt headers detection (any line starting with #include <Q... or #include "Q...)
QT_HEADER_RE = re.compile(r'#include\s*[<"]Q')

# C++ / Windows type keywords — must NEVER be treated as variable names
TYPE_KEYWORDS = frozenset({
    # C++ keywords
    'int', 'bool', 'float', 'double', 'char', 'void', 'auto', 'wchar_t',
    'char8_t', 'char16_t', 'char32_t',
    'unsigned', 'signed', 'long', 'short', 'const', 'volatile', 'mutable',
    'static', 'extern', 'inline', 'virtual', 'override', 'final',
    'return', 'true', 'false', 'nullptr', 'this', 'new', 'delete',
    'class', 'struct', 'enum', 'union', 'namespace', 'template',
    'typename', 'typedef', 'using', 'if', 'else', 'for', 'while',
    'do', 'switch', 'case', 'break', 'continue', 'default', 'try',
    'catch', 'throw', 'sizeof', 'alignof', 'decltype', 'constexpr',
    'noexcept', 'explicit', 'friend', 'operator', 'public', 'private',
    'protected', 'co_await', 'co_yield', 'co_return', 'concept',
    'requires', 'consteval', 'constinit',
    # Qt keywords
    'emit', 'Q_EMIT', 'Q_UNUSED', 'Q_NULLPTR',
    # Windows types
    'DWORD', 'HANDLE', 'HRESULT', 'NTSTATUS', 'BOOL', 'BYTE',
    'WORD', 'LONG', 'ULONG', 'LPVOID', 'PVOID', 'LPCSTR', 'LPCWSTR',
    'LPCTSTR', 'LPSTR', 'LPWSTR', 'LPTSTR', 'HMODULE', 'HINSTANCE',
    'HWND', 'HDC', 'HBITMAP', 'HFONT', 'HPEN', 'HBRUSH', 'HCURSOR',
    'HMENU', 'HICON', 'HKEY', 'SC_HANDLE', 'SERVICE_STATUS_HANDLE',
    'PSECURITY_DESCRIPTOR', 'PUCHAR', 'FILETIME', 'SYSTEMTIME',
    'BCRYPT_ALG_HANDLE', 'BCRYPT_KEY_HANDLE',
    'UCHAR', 'USHORT', 'UINT', 'INT', 'CHAR', 'WCHAR',
    'LARGE_INTEGER', 'ULARGE_INTEGER', 'SIZE_T', 'SSIZE_T',
    'LRESULT', 'WPARAM', 'LPARAM',
})

# Parameters to always skip
SKIP_PARAMS = frozenset({
    'this', 'parent', 'self', 'argc', 'argv', 'event', 'e',
    'ok', 'result', 'success', 'enabled', 'checked', 'visible',
    's', 'i', 'j', 'k', 'n', 'c', 'x', 'y', 'w', 'h',
})

# Qt types that have .isEmpty()
QT_ISEMPTY_TYPES = frozenset({
    'QString', 'QStringView', 'QByteArray', 'QStringList',
    'QList', 'QVector', 'QMap', 'QHash', 'QMultiMap', 'QMultiHash',
    'QSet', 'QUrl', 'QDir', 'QJsonObject', 'QJsonArray',
})

# Types that do NOT have .isEmpty() — skip asserting these
NO_ISEMPTY_TYPES = frozenset({
    'QFileInfo', 'QColor', 'QFont', 'QPixmap', 'QImage', 'QIcon',
    'QBrush', 'QPen', 'QRect', 'QRectF', 'QSize', 'QSizeF',
    'QPoint', 'QPointF', 'QDateTime', 'QDate', 'QTime',
    'QVariant', 'QJsonValue', 'QModelIndex',
    'QPersistentModelIndex', 'QRegularExpression',
    'QRegularExpressionMatch',
})


def parse_lint_warnings():
    """Parse lint output, return list of (file, line, func_name, count)."""
    if not LINT_FILE.exists():
        print(f"ERROR: {LINT_FILE} not found. Run lint first.")
        sys.exit(1)
    warnings = []
    text = LINT_FILE.read_text(encoding='utf-8-sig')
    for raw_line in text.splitlines():
        m = WARNING_RE.match(raw_line)
        if m:
            warnings.append((
                m.group(1),
                int(m.group(2)),
                m.group(3),
                int(m.group(4)),
            ))
    return warnings


def read_function_body(lines, start_line):
    """Find function body boundaries from start_line (1-indexed).
    Returns (brace_line_idx, body_start_idx, body_end_idx) or None.
    """
    idx = start_line - 1
    brace_depth = 0
    found_open = False
    brace_line_idx = -1

    for i in range(idx, min(idx + 10, len(lines))):
        if '{' in lines[i]:
            found_open = True
            brace_line_idx = i
            brace_depth += lines[i].count('{') - lines[i].count('}')
            break

    if not found_open:
        return None

    body_start_idx = brace_line_idx + 1
    for i in range(brace_line_idx + 1, len(lines)):
        brace_depth += lines[i].count('{') - lines[i].count('}')
        if brace_depth <= 0:
            return (brace_line_idx, body_start_idx, i)
    return None


def extract_signature(lines, start_line):
    """Extract function signature (may span multiple lines)."""
    idx = start_line - 1
    parts = []
    for i in range(idx, min(idx + 10, len(lines))):
        parts.append(lines[i].strip())
        if '{' in lines[i]:
            break
    return ' '.join(parts)


def get_param_string(signature):
    """Extract the parameter list string from a signature."""
    m = FUNC_SIG_RE.search(signature)
    if not m:
        return ''
    # Strip C-style comments from parameter list
    param_str = m.group(1)
    param_str = re.sub(r'/\*.*?\*/', '', param_str)
    return param_str


def find_pointer_params(param_str):
    """Extract pointer parameter names from parameter string."""
    params = []
    for part in param_str.split(','):
        part = part.strip()
        if not part or '*' not in part:
            continue
        clean = part.split('=')[0].strip()
        # Strip array brackets
        clean = re.sub(r'\[\]', '', clean)
        tokens = clean.replace('*', ' * ').split()
        name = None
        for t in reversed(tokens):
            if t not in ('*', '&', 'const', 'volatile'):
                if t not in TYPE_KEYWORDS:
                    name = t
                break
        if (name and name not in SKIP_PARAMS
                and not name[0].isupper()
                and re.match(r'^\w+$', name)):
            params.append(name)
    return params


def find_member_deref_any_depth(lines, start_idx, end_idx):
    """Find member pointers dereferenced via -> at any depth.
    Member variables (m_xxx) are always accessible at function scope,
    so it's safe to assert them even if used inside nested blocks.
    """
    members = []
    seen = set()
    for i in range(start_idx, end_idx):
        for m in MEMBER_DEREF_RE.finditer(lines[i]):
            name = m.group(1)
            if name not in seen:
                seen.add(name)
                members.append(name)
    return members


def find_existing_assertions(lines, start_idx, end_idx):
    """Find what's already asserted in the function."""
    asserted = set()
    for i in range(start_idx, end_idx):
        for m in EXISTING_ASSERT_RE.finditer(lines[i]):
            expr = m.group(1).strip()
            expr = expr.replace(' != nullptr', '').replace('!= nullptr', '')
            asserted.add(expr)
    return asserted


def find_qt_string_params(param_str):
    """Find Qt string/container reference params that have .isEmpty()."""
    results = []
    # Match: const QString& name, QByteArray& name, etc.
    type_alt = '|'.join(sorted(QT_ISEMPTY_TYPES, key=len, reverse=True))
    pat = re.compile(
        rf'(?:const\s+)?({type_alt})'
        r'(?:\s*<[^>]*>)?'      # optional template args
        r'\s*(?:const\s*)?&\s*'  # reference
        r'(\w+)'
    )
    for pm in pat.finditer(param_str):
        name = pm.group(2)
        if name not in SKIP_PARAMS and name not in TYPE_KEYWORDS:
            results.append(name)
    return results


def find_int_params(param_str):
    """Find integer params suitable for >= 0 assertions.
    Only matches simple int types (not compound like 'unsigned long').
    """
    results = []
    # Match: int foo, qint64 bar — but NOT unsigned long foo
    pat = re.compile(
        r'(?:^|[,(])\s*'
        r'(?:const\s+)?'
        r'(int|qint64|qint32|qsizetype)\s+'
        r'(\w+)'
    )
    for pm in pat.finditer(param_str):
        name = pm.group(2)
        if name not in SKIP_PARAMS and name not in TYPE_KEYWORDS:
            results.append(name)
    return results


def find_fs_path_params(param_str):
    """Find std::filesystem::path params (use .empty() not .isEmpty())."""
    results = []
    pat = re.compile(
        r'(?:const\s+)?'
        r'(?:std::)?(?:filesystem::)?path\s*&?\s*'
        r'(\w+)'
    )
    for pm in pat.finditer(param_str):
        name = pm.group(1)
        if name not in SKIP_PARAMS and name not in TYPE_KEYWORDS:
            results.append(name)
    return results


def find_local_ptrs_depth0(lines, start_idx, end_idx):
    """Find local pointer variables declared at depth 0."""
    results = []
    seen = set()
    depth = 0
    pat = re.compile(r'(?:auto|[\w:]+)\s*\*\s+(\w+)\s*=')
    for i in range(start_idx, end_idx):
        line = lines[i]
        depth += line.count('{') - line.count('}')
        if depth > 0:
            continue
        for m in pat.finditer(line):
            name = m.group(1)
            if (name not in TYPE_KEYWORDS and name not in SKIP_PARAMS
                    and name not in seen):
                seen.add(name)
                results.append(name)
    return results


MEMBER_CONTAINER_RE = re.compile(
    r'\b(m_\w+)\s*\.'
    r'(?:isEmpty|size|count|length|append|push_back|contains|at|first'
    r'|last|begin|end|clear|remove|insert|prepend|reserve)\s*\('
)

STD_STRING_MEMBER_RE = re.compile(
    r'\b(m_\w+)\s*\.'
    r'(?:empty|size|length|substr|find|c_str|data|append|push_back)\s*\('
)

SMART_PTR_MEMBER_RE = re.compile(
    r'\b(m_\w+)\s*\.(?:get|reset)\s*\('
)


def find_member_container_access(lines, start_idx, end_idx):
    """Find member variables accessed via Qt container methods.

    If m_foo.size(), m_foo.append(), etc. appear, the member is a
    Qt container or string. We can assert !m_foo.isEmpty() as a
    precondition.
    """
    members = []
    seen = set()
    for i in range(start_idx, end_idx):
        line = lines[i]
        for match in MEMBER_CONTAINER_RE.finditer(line):
            name = match.group(1)
            if name not in seen:
                seen.add(name)
                members.append(('qt', name))
        for match in STD_STRING_MEMBER_RE.finditer(line):
            name = match.group(1)
            if name not in seen:
                seen.add(name)
                members.append(('std', name))
        for match in SMART_PTR_MEMBER_RE.finditer(line):
            name = match.group(1)
            if name not in seen:
                seen.add(name)
                members.append(('ptr', name))
    return members


def find_std_string_params(param_str):
    """Find std::string or std::string_view parameters."""
    results = []
    pat = re.compile(
        r'(?:const\s+)?'
        r'(?:std::)?(?:string_view|string)\s*&?\s*'
        r'(\w+)'
    )
    for pm in pat.finditer(param_str):
        name = pm.group(1)
        if name not in SKIP_PARAMS and name not in TYPE_KEYWORDS:
            results.append(name)
    return results


def determine_assertions(lines, start_line, func_name, needed_count):
    """Determine which assertions to add. Returns list of expressions."""
    result = read_function_body(lines, start_line)
    if not result:
        return []

    brace_line_idx, body_start_idx, body_end_idx = result
    signature = extract_signature(lines, start_line)
    param_str = get_param_string(signature)
    already = find_existing_assertions(lines, body_start_idx, body_end_idx)
    candidates = []

    def add(expr):
        if expr not in already and expr not in candidates:
            candidates.append(expr)

    # 1. Member pointer dereferences (m_foo->) — highest confidence
    for mem in find_member_deref_any_depth(lines, body_start_idx, body_end_idx):
        add(mem)

    # 2. Pointer parameters
    for param in find_pointer_params(param_str):
        add(param)

    # 3. qobject_cast / dynamic_cast source args (depth 0 only)
    depth = 0
    for i in range(body_start_idx, body_end_idx):
        line = lines[i]
        depth += line.count('{') - line.count('}')
        if depth > 0:
            continue
        for mc in QOBJECT_CAST_RE.finditer(line):
            add(mc.group(1))
        for mc in DYNAMIC_CAST_RE.finditer(line):
            add(mc.group(1))

    # 4. Qt string/container ref params → !param.isEmpty()
    if len(candidates) < needed_count:
        for name in find_qt_string_params(param_str):
            add(f"!{name}.isEmpty()")

    # 4b. Qt string/container by-value params → !param.isEmpty()
    if len(candidates) < needed_count:
        type_alt = '|'.join(sorted(QT_ISEMPTY_TYPES, key=len, reverse=True))
        val_pat = re.compile(
            rf'(?:const\s+)?({type_alt})'
            r'(?:\s*<[^>]*>)?'
            r'\s+(\w+)'
        )
        for pm in val_pat.finditer(param_str):
            name = pm.group(2)
            if name not in SKIP_PARAMS and name not in TYPE_KEYWORDS:
                add(f"!{name}.isEmpty()")

    # 5. Integer params → param >= 0
    if len(candidates) < needed_count:
        for name in find_int_params(param_str):
            add(f"{name} >= 0")

    # 6. std::filesystem::path params → !path.empty()
    if len(candidates) < needed_count:
        for name in find_fs_path_params(param_str):
            add(f"!{name}.empty()")

    # 7. std::string / std::string_view params → !param.empty()
    if len(candidates) < needed_count:
        for name in find_std_string_params(param_str):
            add(f"!{name}.empty()")

    # 8. Smart pointer member access (m_foo.get() → Q_ASSERT(m_foo))
    if len(candidates) < needed_count:
        for kind, name in find_member_container_access(
            lines, body_start_idx, body_end_idx
        ):
            if kind == 'ptr':  # smart pointer — bool-convertible
                add(name)

    return candidates[:needed_count]


def file_has_qt_headers(lines):
    """Check if the file includes any Qt headers (first 100 lines)."""
    for line in lines[:100]:
        if QT_HEADER_RE.search(line):
            return True
    return False


def find_last_include_line(lines):
    """Find the index of the last #include line."""
    last = -1
    for i, line in enumerate(lines[:120]):
        if line.strip().startswith('#include'):
            last = i
    return last


def get_indentation(lines, body_start_idx, body_end_idx):
    """Determine indentation of the function body."""
    for i in range(body_start_idx, body_end_idx):
        line = lines[i]
        stripped = line.lstrip()
        if stripped and not stripped.startswith('//'):
            return line[:len(line) - len(stripped)]
    return '    '


def count_code_lines(lines, start_idx, end_idx):
    """Count non-blank, non-comment-only lines in a range.

    Matches the linter's code-line counting: blank lines and lines
    that are purely comments (// ...) are excluded.
    """
    count = 0
    for i in range(start_idx, end_idx + 1):
        stripped = lines[i].strip() if i < len(lines) else ''
        if stripped and not stripped.startswith('//'):
            count += 1
    return count


def remove_blank_lines_if_needed(lines, body_start_idx, body_end_idx,
                                  current_code_lines, lines_to_add):
    """Find blank lines to remove to stay within 70 code-line limit."""
    new_length = current_code_lines + lines_to_add
    if new_length <= 70:
        return []
    excess = new_length - 70
    removable = []
    for i in range(body_start_idx, body_end_idx):
        if lines[i].strip() == '':
            removable.append(i)
            if len(removable) >= excess:
                break
    return removable[:excess]


def process_file(filepath, warnings_for_file):
    """Process all assertion-density warnings for a single file."""
    path = Path(filepath)
    if not path.exists():
        return 0

    lines = path.read_text(encoding='utf-8').splitlines(keepends=True)
    fixed_count = 0

    has_qt = file_has_qt_headers(lines)

    # Choose assertion macro
    if has_qt:
        assert_macro = "Q_ASSERT"
    else:
        assert_macro = "Q_ASSERT"  # Will add #include <QtGlobal>

    # Process in REVERSE line order to preserve line numbers
    sorted_warnings = sorted(
        warnings_for_file, key=lambda w: w[1], reverse=True
    )

    for _, line_num, func_name, current_count in sorted_warnings:
        needed = 2 - current_count
        if needed <= 0:
            continue

        assertions = determine_assertions(
            lines, line_num, func_name, needed
        )
        if not assertions:
            continue

        result = read_function_body(lines, line_num)
        if not result:
            continue
        brace_line_idx, body_start_idx, body_end_idx = result

        indent = get_indentation(lines, body_start_idx, body_end_idx)

        assertion_lines = []
        for expr in assertions:
            assertion_lines.append(f"{indent}{assert_macro}({expr});\n")

        current_code = count_code_lines(
            lines, line_num - 1, body_end_idx
        )
        lines_to_remove = remove_blank_lines_if_needed(
            lines, body_start_idx, body_end_idx,
            current_code, len(assertion_lines)
        )

        # Assertion lines are code, so they add to code count;
        # removed blank lines don't reduce code count (they're blank)
        final_code = current_code + len(assertion_lines)
        if final_code > 70:
            continue  # Can't safely add — would exceed limit

        for rm_idx in sorted(lines_to_remove, reverse=True):
            lines.pop(rm_idx)
            body_end_idx -= 1
            if rm_idx < body_start_idx:
                body_start_idx -= 1

        for i, aline in enumerate(assertion_lines):
            lines.insert(body_start_idx + i, aline)

        fixed_count += 1

    # Add #include <QtGlobal> for non-Qt files that got assertions
    if fixed_count > 0 and not has_qt:
        last_inc = find_last_include_line(lines)
        if last_inc >= 0:
            lines.insert(last_inc + 1, "#include <QtGlobal>\n")

    if fixed_count > 0 and not DRY_RUN:
        path.write_text(''.join(lines), encoding='utf-8')

    return fixed_count


def main():
    warnings = parse_lint_warnings()
    print(f"Parsed {len(warnings)} assertion-density warnings")

    by_file = defaultdict(list)
    for w in warnings:
        by_file[w[0]].append(w)

    print(f"Across {len(by_file)} files")
    if DRY_RUN:
        print("DRY RUN — no files will be modified")

    total_fixed = 0
    total_skipped = 0

    for filepath in sorted(by_file.keys()):
        file_warnings = by_file[filepath]
        try:
            fixed = process_file(filepath, file_warnings)
        except PermissionError:
            print(f"  {filepath}: SKIPPED (permission denied)")
            total_skipped += len(file_warnings)
            continue
        skipped = len(file_warnings) - fixed
        total_fixed += fixed
        total_skipped += skipped
        if fixed > 0 or skipped > 0:
            status = "OK" if skipped == 0 else f"{skipped} skipped"
            print(f"  {filepath}: {fixed} fixed, {status}")

    print(f"\nTotal: {total_fixed} fixed, {total_skipped} skipped")


if __name__ == "__main__":
    main()
