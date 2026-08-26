- **HIGH -- Correctness/security:** Live bearer lease IDs leak through denial messages and `activeLeaseIds()`; `release()` authenticates only by ID, allowing another caller to release active lease and break single-writer guarantee. [src/ai/ai_lease_manager.cpp:51](src/ai/ai_lease_manager.cpp:51), [src/ai/ai_lease_manager.cpp:76](src/ai/ai_lease_manager.cpp:76), [src/ai/ai_lease_manager.cpp:125](src/ai/ai_lease_manager.cpp:125)

- **HIGH -- Correctness:** Adjustable/caller-supplied wall time is ORed with monotonic expiry; forward clock jumps or future `now_utc` values reclaim still-running leases before TTL elapsed, permitting concurrent mutations. [src/ai/ai_lease_manager.cpp:45](src/ai/ai_lease_manager.cpp:45), [src/ai/ai_lease_manager.cpp:86](src/ai/ai_lease_manager.cpp:86), [src/ai/ai_lease_manager.cpp:98](src/ai/ai_lease_manager.cpp:98)

- **MEDIUM -- Contract/security:** Token claimed unguessable is generated with `QRandomGenerator::global()` instead of cryptographic system generator. [src/ai/ai_lease_manager.cpp:18](src/ai/ai_lease_manager.cpp:18), [src/ai/ai_lease_manager.cpp:57](src/ai/ai_lease_manager.cpp:57)

- **Next:** Remove live-token disclosure, use monotonic expiry as authority, use system CSPRNG. **Blockers:** none.
