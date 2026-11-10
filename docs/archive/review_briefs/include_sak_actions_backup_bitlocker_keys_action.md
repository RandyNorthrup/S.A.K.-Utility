- [x] [MEDIUM] The public class comment claimed BitLocker key-package data, but production retrieves
  recovery passwords, protector metadata and external-key filenames, not binary key packages.
  RESOLVED 2026-09-02: narrowed the documented contract to the evidence actually collected.
- [x] [CRITICAL] Query/parse failure or a protector-only result could produce a plausible backup
  with no usable recovery password.
  VERIFIED 2026-09-02: volume/protector queries carry independent success state; malformed, partial
  or over-cap JSON fails closed; the key gate requires at least one recovery password; every
  applicable key file must atomically commit.
- [x] [CRITICAL] Plaintext keys could escape through an unsafe destination, permissive ACL or
  partial failure.
  VERIFIED 2026-09-02: the destination rejects relative/UNC/device/reparse/mapped-drive paths,
  creates a unique directory, requires restrictive ACL success, uses atomic files, and removes
  partial output on cancel/failure while surfacing cleanup failure.

Re-review result: zero open findings. All state members and private seams have live source/test
callers; focused tests cover parsers, path policy, key gating, formatters and cleanup decisions.
