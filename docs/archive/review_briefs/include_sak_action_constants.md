- [x] [LOW] Six constants had no caller and described policies the live implementations did not
  use: three retry-backoff variants, Chocolatey exit code 2, a ten-package batch ceiling, and the
  20 percent progress marker.
  RESOLVED 2026-09-02: removed the dead declarations. AppInstallationWorker retains its real
  exponential five-second retry policy; ChocolateyManager accepts only its tested success/reboot
  codes; no batch ceiling is claimed without enforcement.
- [x] [MEDIUM] Elevated quick-action names and task IDs could drift between action metadata,
  QuickActionController and elevated dispatch.
  VERIFIED 2026-09-02: the shared constants remain referenced by actions, controller mapping,
  dispatcher/metadata tests and exact ID tests. Reset Network and BitLocker deliberately retain
  distinct display names and task IDs.

Re-review result: zero open findings. Every remaining constant has at least one production or test
consumer, and changed-file dead-code/magic-number gates cover the header.
