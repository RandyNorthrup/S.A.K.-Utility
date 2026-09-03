1. **HIGH** -- [`advanced_uninstall_controller.h:309`](/include/sak/advanced_uninstall_controller.h:309): Failed enumeration-thread join violates stated teardown contract; destruction can delete live `m_enumThread` and in-use `m_enumerator`, causing abort or use-after-free.

2. **HIGH** -- [`advanced_uninstall_controller.h:105`](/include/sak/advanced_uninstall_controller.h:105): Enumeration cancellation has no terminal signal. Started consumers can remain permanently busy after cancellation.

3. **HIGH** -- [`advanced_uninstall_controller.h:78`](/include/sak/advanced_uninstall_controller.h:78): Registry-only cancellation is unwired; early cancellation can leave controller in `Uninstalling` with no `uninstallCancelled`, failure, or completion signal.

4. **MEDIUM** -- [`advanced_uninstall_controller.h:225`](/include/sak/advanced_uninstall_controller.h:225): Cleanup supports completion and cancellation but no public failure terminal despite private failure handling at line 262. Manual cleanup failure leaves UI consumers busy.

5. **MEDIUM** -- [`advanced_uninstall_controller.h:137`](/include/sak/advanced_uninstall_controller.h:137): Persisted auto-clean preference and per-operation auto-clean flag share `m_autoCleanSafe` at line 340. Forced and UWP operations overwrite and later persist unrelated preference state.

6. **MEDIUM** -- [`advanced_uninstall_controller.h:127`](/include/sak/advanced_uninstall_controller.h:127): `loadSettings()` and `saveSettings()` expose no failure result or signal; implementation ignores store health and disk-sync failure, contradicting persistence contract.

7. **MEDIUM** -- [`advanced_uninstall_controller.h:67`](/include/sak/advanced_uninstall_controller.h:67): Operational and queued `ScanLevel` inputs lack validation. Invalid enum values silently reach scanner and behave like reduced Safe scans while remaining invalid in reports.

8. **MEDIUM** -- [`advanced_uninstall_controller.h:29`](/include/sak/advanced_uninstall_controller.h:29): `State::LeftoverScanning` is never entered; `currentState()` and `stateChanged` report `Uninstalling` throughout scanning, violating stated state machine.

9. **LOW** -- [`advanced_uninstall_controller.h:84`](/include/sak/advanced_uninstall_controller.h:84): `nonSafeLeftovers()` cannot be documented complement of `safeLeftovers()` because non-deletable Safe items belong to neither result. Both helpers are production-dead and duplicate active filtering logic.

10. **LOW** -- [`advanced_uninstall_controller.h:113`](/include/sak/advanced_uninstall_controller.h:113): `removeFromQueue()` silently ignores invalid indices; API provides neither failure return nor error signal.

11. **LOW** -- [`advanced_uninstall_controller.h:338`](/include/sak/advanced_uninstall_controller.h:338): `m_batchRestorePointCreated` holds only a temporary result used within one method; unnecessary persistent state.

12. **LOW** -- [`advanced_uninstall_controller.h:325`](/include/sak/advanced_uninstall_controller.h:325): `m_enum_generation`, `m_uninstall_worker`, `m_cleanup_worker`, and `m_restore_manager` violate file/project `m_camelCaseMember` naming used by remaining members.
