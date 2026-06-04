# Partition Manager External VM/Hardware Certification Checklist

- Generated UTC: 2026-06-02T05:27:33.7685145Z
- Manifest: artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json
- Matrix: docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json
- Scope: external boot, removable media, SSD/NVMe, BitLocker, partition move/allocation/metadata conversion, and physical wipe gates.
- Rule: use disposable VMs/media only. Do not run destructive steps on production disks.

## Completion Rules

- [ ] Each gate below is run in the lab environment named by its safety contract.
- [ ] Every required evidence key is filled in the JSON manifest with a non-empty value.
- [ ] Every passed gate has an existing local `evidence_path` or absolute HTTP/HTTPS `evidence_url`.
- [ ] `scripts/verify_partition_manager_certification.ps1 -RequireVhdDataDiskEvidence -RequireExternalGateEvidence` passes.
- [ ] `scripts/get_partition_manager_certification_status.ps1` reports `HardwareCertified` before release wording uses that claim.

## external.system-mbr2gpt - System MBR-to-GPT conversion in disposable boot VM

Safety contract:
- [ ] `disposable_boot_vm_only`
- [ ] `pre_conversion_snapshot`
- [ ] `validate_before_convert`
- [ ] `post_conversion_boot_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `vm_id` |  |  |
| `before_partition_style` |  |  |
| `mbr2gpt_validate_exit_code` |  |  |
| `mbr2gpt_convert_exit_code` |  |  |
| `after_partition_style` |  |  |
| `boot_result` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.system-mbr2gpt/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.os-migration-reboot - OS migration target boot and firmware-order proof

Safety contract:
- [ ] `disposable_vm_or_lab_disk`
- [ ] `target_overwrite_confirmed`
- [ ] `source_target_identity_recorded`
- [ ] `target_boot_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `source_disk_id` |  |  |
| `target_disk_id` |  |  |
| `clone_verification_mode` |  |  |
| `boot_validation_output` |  |  |
| `firmware_boot_order` |  |  |
| `target_boot_result` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.os-migration-reboot/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.boot-repair-uefi - UEFI boot repair with intentionally broken BCD

Safety contract:
- [ ] `disposable_uefi_boot_vm_only`
- [ ] `pre_repair_snapshot`
- [ ] `commands_logged`
- [ ] `post_repair_boot_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `vm_id` |  |  |
| `boot_mode` | Allowed: UEFI |  |
| `broken_boot_symptom` |  |  |
| `repair_commands` |  |  |
| `post_repair_boot_result` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.boot-repair-uefi/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.boot-repair-bios - BIOS/MBR boot repair with intentionally broken BCD

Safety contract:
- [ ] `disposable_bios_boot_vm_only`
- [ ] `pre_repair_snapshot`
- [ ] `commands_logged`
- [ ] `post_repair_boot_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `vm_id` |  |  |
| `boot_mode` | Allowed: BIOS, Legacy BIOS |  |
| `broken_boot_symptom` |  |  |
| `repair_commands` |  |  |
| `post_repair_boot_result` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.boot-repair-bios/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.usb-removable - USB removable disk destructive operation proof

Safety contract:
- [ ] `disposable_usb_media_only`
- [ ] `device_identity_recorded`
- [ ] `operator_confirmation_recorded`
- [ ] `post_operation_layout_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `device_model` |  |  |
| `serial_number` |  |  |
| `bus_type` |  |  |
| `operation` |  |  |
| `before_layout` |  |  |
| `after_layout` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.usb-removable/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.file-level-data-recovery - File-level Data Recovery scan and restore proof

Safety contract:
- [ ] `disposable_recovery_volume`
- [ ] `read_only_source_scan`
- [ ] `restore_to_separate_destination`
- [ ] `recovered_hash_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `source_volume_id` |  |  |
| `file_system` |  |  |
| `deleted_fixture_name` |  |  |
| `scan_result` |  |  |
| `restore_destination` |  |  |
| `recovered_file_hash` |  |  |
| `source_not_mutated` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.file-level-data-recovery/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.ssd-retrim - SSD/NVMe ReTrim and vendor purge warning proof

Safety contract:
- [ ] `ssd_or_nvme_device`
- [ ] `no_hdd_defrag_path`
- [ ] `purge_warning_present`
- [ ] `retrim_output_recorded`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `device_model` |  |  |
| `media_type` |  |  |
| `trim_status_before` |  |  |
| `retrim_output` |  |  |
| `purge_warning_visible` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.ssd-retrim/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.hdd-defrag-execution - Direct in-app HDD defrag execution proof

Safety contract:
- [ ] `disposable_hdd_volume_only`
- [ ] `media_type_verified_not_ssd`
- [ ] `long_running_operation_cancellable`
- [ ] `post_defrag_health_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `device_model` |  |  |
| `media_type` |  |  |
| `drive_letter` |  |  |
| `analyze_output` |  |  |
| `defrag_output` |  |  |
| `duration_seconds` |  |  |
| `post_defrag_health_check` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.hdd-defrag-execution/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.ssd-secure-erase - SSD/NVMe secure erase command execution proof

Safety contract:
- [ ] `disposable_ssd_or_nvme_device_only`
- [ ] `non_system_disk_identity_recorded`
- [ ] `typed_operator_confirmation_recorded`
- [ ] `post_purge_empty_or_vendor_expected_layout_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `device_model` |  |  |
| `serial_number` |  |  |
| `media_type` |  |  |
| `purge_command` |  |  |
| `operator_confirmation` |  |  |
| `before_layout` |  |  |
| `after_layout` |  |  |
| `post_purge_identity_check` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.ssd-secure-erase/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.bitlocker - BitLocker locked/unlocked data-volume blocker proof

Safety contract:
- [ ] `disposable_bitlocker_volume`
- [ ] `locked_mutation_blocked`
- [ ] `unlocked_state_revalidated`
- [ ] `dirty_bit_handling_recorded`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `volume_id` |  |  |
| `locked_state_blocker` |  |  |
| `unlocked_state_behavior` |  |  |
| `dirty_bit_state` |  |  |
| `operation_attempted` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.bitlocker/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.bitlocker-mutation - In-app BitLocker unlock/suspend/resume mutation proof

Safety contract:
- [ ] `disposable_bitlocker_volume_only`
- [ ] `recovery_key_or_password_fixture_recorded`
- [ ] `mutation_commands_logged`
- [ ] `final_protection_state_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `volume_id` |  |  |
| `initial_lock_state` |  |  |
| `unlock_result` |  |  |
| `suspend_result` |  |  |
| `resume_result` |  |  |
| `final_protection_state` |  |  |
| `operation_audit_log` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.bitlocker-mutation/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.allocate-free-space - Allocate Free Space adjacent donor-volume backup/delete/extend/recreate/restore proof

Safety contract:
- [ ] `disposable_source_and_donor_volumes_only`
- [ ] `source_donor_identity_recorded`
- [ ] `before_after_layout_verified`
- [ ] `mount_and_file_hash_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `source_volume_id` |  |  |
| `donor_volume_id` |  |  |
| `before_layout` |  |  |
| `after_layout` |  |  |
| `volume_size_delta` |  |  |
| `file_hash_validation` |  |  |
| `mount_validation` |  |  |
| `rollback_or_backup_evidence` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.allocate-free-space/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.partition-move - Offline partition start-move proof

Safety contract:
- [ ] `disposable_partition_only`
- [ ] `offline_move_engine_used`
- [ ] `offset_change_verified`
- [ ] `mount_and_file_hash_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `partition_id` |  |  |
| `before_offset_bytes` |  |  |
| `after_offset_bytes` |  |  |
| `before_layout` |  |  |
| `after_layout` |  |  |
| `file_hash_validation` |  |  |
| `mount_validation` |  |  |
| `rollback_or_backup_evidence` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.partition-move/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.cluster-size-change - Existing-volume cluster-size change proof

Safety contract:
- [ ] `disposable_volume_only`
- [ ] `full_file_tree_copied`
- [ ] `metadata_preserved`
- [ ] `hash_acl_stream_validation_passed`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `volume_id` |  |  |
| `before_allocation_unit_size` |  |  |
| `after_allocation_unit_size` |  |  |
| `file_count` |  |  |
| `acl_validation` |  |  |
| `alternate_stream_validation` |  |  |
| `file_hash_validation` |  |  |
| `rollback_or_backup_evidence` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.cluster-size-change/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.primary-logical-conversion - Primary/logical conversion on disposable MBR disk

Safety contract:
- [ ] `disposable_mbr_disk_only`
- [ ] `extended_container_identity_recorded`
- [ ] `logical_volume_identity_recorded`
- [ ] `before_after_order_offsets_verified`
- [ ] `mount_and_file_hash_verified`
- [ ] `bootability_verified_when_applicable`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `disk_id` |  |  |
| `before_layout` |  |  |
| `extended_container_identity` |  |  |
| `logical_volume_identity` |  |  |
| `after_layout` |  |  |
| `partition_order_offsets` |  |  |
| `mount_validation` |  |  |
| `file_hash_validation` |  |  |
| `bootability_result` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.primary-logical-conversion/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.volume-serial-number - Volume serial-number metadata mutation proof

Safety contract:
- [ ] `disposable_volume_only`
- [ ] `file_system_specific_writer_used`
- [ ] `metadata_change_verified`
- [ ] `mount_chkdsk_hash_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `volume_id` |  |  |
| `file_system` |  |  |
| `before_serial_number` |  |  |
| `after_serial_number` |  |  |
| `mount_validation` |  |  |
| `chkdsk_output` |  |  |
| `file_hash_validation` |  |  |
| `rollback_or_backup_evidence` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.volume-serial-number/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.dynamic-to-basic - Dynamic disk to basic conversion proof

Safety contract:
- [ ] `disposable_dynamic_disk_vm_only`
- [ ] `pre_conversion_snapshot`
- [ ] `data_backup_before_destructive_conversion`
- [ ] `basic_disk_restore_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `vm_id` |  |  |
| `disk_id` |  |  |
| `before_disk_type` |  |  |
| `before_layout` |  |  |
| `backup_evidence` |  |  |
| `after_disk_type` |  |  |
| `after_layout` |  |  |
| `restore_hash_validation` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.dynamic-to-basic/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-

## external.hardware-wipe - Non-system hardware wipe on disposable physical disk

Safety contract:
- [ ] `disposable_physical_disk_only`
- [ ] `non_system_disk_identity_recorded`
- [ ] `typed_confirmation_recorded`
- [ ] `post_wipe_empty_layout_verified`

Required evidence payload:

| Key | Requirement | Recorded value |
| --- | --- | --- |
| `device_model` |  |  |
| `serial_number` |  |  |
| `operator_confirmation` |  |  |
| `wipe_method` |  |  |
| `before_layout` |  |  |
| `after_layout` |  |  |

Artifacts:
- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.
- [ ] Put local artifacts under `artifacts/partition-manager-certification/external/external.hardware-wipe/` or provide a stable HTTPS evidence URL.
- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.
- [ ] Fill `evidence_path` or `evidence_url` in the manifest.

Operator notes:

-
