1. **HIGH** -- `src/ai/ai_conversation_store.cpp:979`: `artifactSubdir()` accepts absolute paths and `..` traversal, permitting directory creation and artifact writes outside session artifact root.

2. **HIGH** -- `src/ai/ai_conversation_store.cpp:836`, `src/ai/ai_conversation_store.cpp:1171`, `src/ai/ai_conversation_store.cpp:1219`: `openSession()` accepts manifest IDs differing from requested directory; later writes use manifest ID, potentially corrupting another session. Rejected IDs produce empty paths that become working-directory-relative file paths.

3. **MEDIUM** -- `src/ai/ai_conversation_store.cpp:933`: `appendCommand()` redacts only command string. Secret-bearing `result` fields persist unredacted in both command log and search index, violating stated persistence-boundary redaction.

4. **MEDIUM** -- `src/ai/ai_conversation_store.cpp:920`, `src/ai/ai_conversation_store.cpp:944`, `src/ai/ai_conversation_store.cpp:781`: Search-index append failures are ignored, while any readable index remains authoritative. Successfully persisted transcript/command records can become permanently unsearchable.

5. **MEDIUM** -- `src/ai/ai_conversation_store.cpp:913`: Transcript record persists before manifest update. Manifest failure returns `false` after data commit, making retries duplicate records. Same ordering affects command, context, memory, and usage writes.

6. **MEDIUM** -- `src/ai/ai_conversation_store.cpp:818`, `src/ai/ai_conversation_store.cpp:861`: Failed session start or rename leaves partial state: active-session data changes before manifest success, and rename moves artifacts before durable title update.

7. **MEDIUM** -- `src/ai/ai_conversation_store.cpp:165`, `src/ai/ai_conversation_store.cpp:366`: Memory limits named and measured as bytes are applied as UTF-16 character counts. Non-ASCII memory can remain far above 256 KiB after trimming.

8. **MEDIUM** -- `src/ai/ai_conversation_store.cpp:1116`: Memory append write results are unchecked. Short writes or close-time failures can silently truncate entries while method reports success.

9. **LOW** -- `src/ai/ai_conversation_store.cpp:1293`: Artifact-directory sanitizer permits Windows reserved names and trailing periods, causing valid session titles such as `CON` or `Report.` to fail or alias another directory.

10. **LOW** -- `src/ai/ai_conversation_store.cpp:314`: Required-memory-section check verifies only two of six sections, allowing partially structured memory files to bypass normalization.

11. **LOW** -- `src/ai/ai_conversation_store.cpp:1031`: Empty or dot-only filename resolves to artifact directory itself and is returned as successful "file" path.
