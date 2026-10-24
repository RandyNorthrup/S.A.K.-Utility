- [x] [HIGH] File-count cap occurred after QDir::entryList materialized and sorted every file.
  RESOLVED 2026-09-01: native enumeration uses bounded FindFirstFileExW/FindNextFileW, stops on
  entry 513, and refuses the whole directory instead of loading an arbitrary partial set.
- [x] [HIGH] Symlink/reparse checking was path-based and racy before open.
  RESOLVED 2026-09-01: the directory and file are opened with FILE_FLAG_OPEN_REPARSE_POINT; both
  are rejected when reparse-tagged, and the opened file parent identity must equal the pinned
  directory handle. A pre-open directory-to-junction swap is refused deterministically.
- [x] [HIGH] Pre-read size check did not bound a file that grew before readAll.
  RESOLVED 2026-09-01: the handle read is capped at 1 MiB + 1 byte and size is checked before and
  after the read.
- [x] [MEDIUM] Short reads could admit truncated skill content.
  RESOLVED 2026-09-01: before size, bytes read, and after size must match exactly.
- [x] [MEDIUM] Padded IDs could become unreachable or collide after lookup normalization.
  ALREADY RESOLVED by the preceding strict skill-parser wave: Skill::isValid rejects leading or
  trailing ID whitespace before addSkill indexes it; the exact rejection is covered by tests.
- [x] [LOW] Enumeration failures were indistinguishable from empty directories.
  RESOLVED 2026-09-01: directory open and enumeration failures carry distinct Win32 diagnostics;
  ERROR_FILE_NOT_FOUND alone means a valid empty directory.
- [x] [LOW] Disk skills were validated twice and the maintained index was rebuilt after load.
  RESOLVED 2026-09-01: loaders only parse; addSkill performs the single validation and maintains
  the index incrementally. rebuildIndex is deleted.

Re-review result: zero open findings. The registered suite covers truncation, growth, root
reparse, pre-open directory replacement, the 513-file cap, enumeration error, overrides, padded
IDs and built-in resources. The ai_skill_store mutation catalog kills 4/4 boundary mutants.
The native/resource loader now lives in ai_store_file_loader and is shared with WorkflowStore;
SkillStore retains only its Markdown policy, parser and index ownership.
