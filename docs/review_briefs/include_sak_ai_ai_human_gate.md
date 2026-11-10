- [x] [CRITICAL] An absent/unknown gate status could erase or resume a real pending approval.
  VERIFIED 2026-09-02: pending means the exact waiting status; lifecycle strings are a closed set;
  unknown/statusless records are malformed and ignored by the gate store. Serialization and every
  terminal status have focused tests.

Re-review result: zero open findings. All fields/status helpers have live run-state/panel/store
consumers and the header matches current persistence semantics.
