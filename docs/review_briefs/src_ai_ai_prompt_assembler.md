1. **HIGH** -- `src/ai/ai_prompt_assembler.cpp:323-327,353`: Chat mode still emits `baseGuardrails()`, advertising and directing calls to unavailable local tools before forbidding them. Violates absent-tool contract at `:298-301`.

2. **HIGH** -- `src/ai/ai_prompt_assembler.cpp:122-127`: Desktop capability description is stale. Local catalog already provides UIA inspection/clicking, capture, OCR, and input injection; prompt directs model to refuse supported tasks.

3. **MEDIUM** -- `src/ai/ai_prompt_assembler.cpp:90-94`: App-action risk behavior is misstated. Non-catastrophic Unattended mutations do not require confirmation, and destructive Assisted actions do not necessarily take restore points.

4. **MEDIUM** -- `src/ai/ai_prompt_assembler.cpp:294-319`: Untrusted catalogs, memory, context, and multiline steering are concatenated directly into privileged instructions without structural boundaries or escaping, so embedded text can impersonate peer guardrails despite contract at `:30-34`.

5. **MEDIUM** -- `src/ai/ai_prompt_assembler.cpp:323-347,354`: Access-mode label and authorization flags are independent and unvalidated. Mismatched input can declare "Unattended" while emitting disabled or confirm-first guidance, creating contradictory security instructions.

6. **LOW** -- `src/ai/ai_prompt_assembler.cpp:75-76,84-88`: App-action guidance conflicts: one rule requires `operation=list` first, while another requires enumeration only when unsure.
