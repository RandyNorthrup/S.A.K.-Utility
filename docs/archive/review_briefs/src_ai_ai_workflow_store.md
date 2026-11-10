- [x] [HIGH] Workflow files were opened by name and consumed with unbounded readAll after a racy
  path lookup.
  RESOLVED 2026-09-01: native workflow directories and files use the shared handle-pinned,
  no-follow, stable exact-read loader; bundled Qt resources retain a bounded resource loader.
- [x] [HIGH] QDir::entryList materialized and sorted the whole directory before any count limit.
  RESOLVED 2026-09-01: enumeration stops on JSON file 513 and refuses the whole directory.
- [x] [HIGH] Workflow files had no size ceiling.
  RESOLVED 2026-09-01: each template is capped at 1 MiB before parsing, including grow/truncate
  revalidation around the exact handle read.
- [x] [LOW] loadDirectory rebuilt an index that addWorkflow already maintained.
  RESOLVED 2026-09-01: redundant rebuildIndex and its declaration are deleted.

Re-review result: zero open findings. Six focused guards failed separately against the old loader.
The workflow policy catalog kills 2/2 mutants; the shared loader catalog kills 4/4.
