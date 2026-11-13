"""Writes certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json. That file is never edited by hand.

Each row says what a person's Apply does (the operations, in order, through the real controller and
helper), what it may honestly run on, and what a separate tool must read back afterwards. The
verifier evaluates these checks over a row's readback, not over the application's report, so a run
that went wrong cannot pass by having filled in the right keys.

A check names the promise it tests, a path into the row's readback, and one test: equals, equals_path,
differs_from_path, at_least, is_true, is_false or contains. With minus_path, the test is of the difference.
A snapshot in a readback carries the raw bytes of a partition table; the verifier parses those
again, so "data_partitions" below is worked out by the verifier from the bytes.

Usage: generate_partition_manager_matrix.py          rewrite the matrix
       generate_partition_manager_matrix.py --check  fail if the matrix differs from this
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

MIB = 1024 * 1024
VIRTUAL_DISK = "virtual_disk"
VIRTUAL_MACHINE = "virtual_machine"
PHYSICAL = "physical"
GPT_BASIC_DATA = "gpt:{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"


def check(contract: str, path: str, **test: object) -> dict[str, object]:
    return {"contract": contract, "path": path, **test}


def step(step_id: str, operation: str, expect: str = "success") -> dict[str, str]:
    return {"id": step_id, "operation": operation, "expect": expect}


def healthy(contract_prefix: str, path: str) -> list[dict[str, object]]:
    """What Windows' own checks must say of a volume the operation left behind."""
    return [
        check(f"{contract_prefix}_repair_scan_clean", f"{path}.repair_scan", equals="NoErrorsFound"),
        check(f"{contract_prefix}_chkdsk_clean", f"{path}.chkdsk_exit_code", equals=0),
        check(f"{contract_prefix}_not_dirty", f"{path}.dirty", is_false=True),
    ]


def files_unchanged(contract: str, before: str, after: str) -> list[dict[str, object]]:
    return [
        check(contract, f"{after}.digest", equals_path=f"{before}.digest"),
        check(f"{contract}_counted", f"{after}.file_count", at_least=1),
    ]


def row(
    row_id: str,
    name: str,
    accepted: list[str],
    steps: list[dict[str, str]],
    checks: list[dict[str, object]],
    not_proved: str,
    physical_claim: bool = False,
    physical_requirement: dict[str, list[str]] | None = None,
    client: str = "unelevated",
) -> dict[str, object]:
    built: dict[str, object] = {
        "id": row_id,
        "name": name,
        "ran_on": {"accepted": accepted, "physical_claim": physical_claim},
        "client": client,
        "steps": steps,
        "checks": checks,
        "not_proved": not_proved,
    }
    if physical_requirement:
        built["ran_on"]["physical_requirement"] = physical_requirement  # type: ignore[index]
    return built


VHD_ROWS = [
    row(
        "vhd.create-format-resize-delete",
        "Create, format, resize, and delete data partition",
        [VIRTUAL_DISK],
        [
            step("create", "Create"),
            step("format", "Format"),
            step("resize", "Resize"),
            step("delete", "Delete"),
        ],
        [
            check("created_at_the_requested_size", "snapshots.create.data_partitions[0].size_bytes",
                  equals=256 * MIB),
            check("created_with_the_requested_file_system", "create_volume.file_system", equals="NTFS"),
            check("created_with_the_requested_label", "create_volume.label", equals="SAKCERT"),
            *healthy("created", "create_health"),
            check("format_changed_the_label", "format_volume.label", equals="SAKFMT"),
            check("format_changed_the_cluster_size", "format_volume.cluster_bytes", equals=8192),
            check("format_destroyed_the_old_files", "format_marker_present", is_false=True),
            check("format_left_the_partition_where_it_was",
                  "snapshots.format.data_partitions[0].offset_bytes",
                  equals_path="snapshots.create.data_partitions[0].offset_bytes"),
            *healthy("formatted", "format_health"),
            check("resized_to_the_requested_size", "snapshots.resize.data_partitions[0].size_bytes",
                  equals=384 * MIB),
            check("resize_kept_the_start", "snapshots.resize.data_partitions[0].offset_bytes",
                  equals_path="snapshots.create.data_partitions[0].offset_bytes"),
            check("resize_grew_the_file_system", "resize_volume.size_bytes", at_least=380 * MIB),
            *healthy("resized", "resize_health"),
            check("delete_removed_the_partition", "snapshots.delete.data_partition_count", equals=0),
        ],
        "The dialogs that collect these choices, and any file system but NTFS.",
    ),
    row(
        "vhd.fat32-to-ntfs",
        "FAT32 to NTFS in-place conversion",
        [VIRTUAL_DISK],
        [step("convert", "ConvertFileSystem")],
        [
            check("started_as_fat32", "volume_before.file_system", equals="FAT32"),
            check("converted_to_ntfs", "volume_after.file_system", equals="NTFS"),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("converted", "health"),
            check("the_partition_did_not_move", "snapshots.after.data_partitions[0].offset_bytes",
                  equals_path="snapshots.before.data_partitions[0].offset_bytes"),
        ],
        "A volume with a label convert.exe asks about, beyond the one here.",
    ),
    row(
        "vhd.quick-partition",
        "Quick Partition equal-size data-disk layout",
        [VIRTUAL_DISK],
        [step("layout", "QuickPartition")],
        [
            check("the_old_partition_is_gone", "old_partition_still_there", is_false=True),
            check("made_the_requested_count", "snapshots.after.data_partition_count", equals=4),
            check("the_table_says_gpt", "snapshots.after.table.style", equals="GPT"),
            check("every_slice_is_equal_but_the_last", "equal_slices", is_true=True),
            check("the_slices_are_contiguous", "contiguous", is_true=True),
            check("every_volume_formatted_ntfs", "ntfs_volumes", equals=4),
            check("every_volume_is_labelled", "labels", contains="SAKQP4"),
            check("every_volume_is_clean", "unhealthy_volumes", equals=0),
        ],
        "The dialog's presets.",
    ),
    row(
        "vhd.quick-partition-custom",
        "Quick Partition custom size and label layout",
        [VIRTUAL_DISK],
        [step("layout", "QuickPartition")],
        [
            check("made_the_requested_count", "snapshots.after.data_partition_count", equals=3),
            check("first_is_the_size_asked_for", "snapshots.after.data_partitions[0].size_bytes",
                  equals=128 * MIB),
            check("second_is_the_size_asked_for", "snapshots.after.data_partitions[1].size_bytes",
                  equals=192 * MIB),
            check("third_is_the_size_asked_for", "snapshots.after.data_partitions[2].size_bytes",
                  equals=256 * MIB),
            check("labels_are_in_the_order_asked_for", "labels", equals="ALPHA,BRAVO,CHARLIE"),
            check("every_volume_is_clean", "unhealthy_volumes", equals=0),
        ],
        "The dialog's table editors.",
    ),
    row(
        "vhd.quick-partition-mbr",
        "Quick Partition MBR four-data-partition layout",
        [VIRTUAL_DISK],
        [step("layout", "QuickPartition")],
        [
            check("the_table_says_mbr", "snapshots.after.table.style", equals="MBR"),
            check("made_four_data_partitions", "snapshots.after.data_partition_count", equals=4),
            check("the_slices_do_not_overlap", "no_overlap", is_true=True),
            check("every_volume_is_clean", "unhealthy_volumes", equals=0),
            check("every_volume_formatted_ntfs", "ntfs_volumes", equals=4),
        ],
        "More than four partitions, which the app refuses on MBR.",
    ),
    row(
        "vhd.adjacent-extend",
        "Adjacent free-space Extend Partition Wizard path",
        [VIRTUAL_DISK],
        [step("extend", "Resize")],
        [
            check("the_start_did_not_move", "snapshots.after.data_partitions[0].offset_bytes",
                  equals_path="snapshots.before.data_partitions[0].offset_bytes"),
            check("grew_to_the_size_asked_for", "snapshots.after.data_partitions[0].size_bytes",
                  equals=512 * MIB),
            check("the_file_system_grew", "volume_after.size_bytes", at_least=500 * MIB),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("extended", "health"),
        ],
        "The wizard's checks before it calls Resize.",
    ),
    row(
        "vhd.recovered-partition-restore",
        "Recovered partition write-back candidate restore",
        [VIRTUAL_DISK],
        [step("scan", "PartitionRecoveryScan"), step("restore", "RestoreRecoveredPartition")],
        [
            check("the_partition_really_was_gone", "snapshots.deleted.data_partition_count", equals=0),
            check("the_scan_said_what_it_found", "scan_found_the_partition", is_true=True),
            check("restored_at_its_old_offset", "snapshots.after.data_partitions[0].offset_bytes",
                  equals_path="snapshots.before.data_partitions[0].offset_bytes"),
            check("restored_at_its_old_size", "snapshots.after.data_partitions[0].size_bytes",
                  equals_path="snapshots.before.data_partitions[0].size_bytes"),
            check("restored_with_its_old_type", "snapshots.after.data_partitions[0].type",
                  equals_path="snapshots.before.data_partitions[0].type"),
            *files_unchanged("files_came_back", "files_before", "files_after"),
            *healthy("restored", "health"),
        ],
        "A partition whose table entry and data have both been damaged.",
    ),
    row(
        "vhd.empty-style-conversion",
        "Empty data disk GPT/MBR conversion",
        [VIRTUAL_DISK],
        [step("to_mbr", "ConvertPartitionStyle"), step("to_gpt", "ConvertPartitionStyle")],
        [
            check("started_as_gpt", "snapshots.before.table.style", equals="GPT"),
            check("became_mbr", "snapshots.mbr.table.style", equals="MBR"),
            check("became_gpt_again", "snapshots.gpt.table.style", equals="GPT"),
            check("stayed_empty_as_mbr", "snapshots.mbr.data_partition_count", equals=0),
            check("stayed_empty_as_gpt", "snapshots.gpt.data_partition_count", equals=0),
        ],
        "A disk that holds partitions, which the app refuses.",
    ),
    row(
        "vhd.clear-disk-wipe",
        "Clear-level non-system disk wipe",
        [VIRTUAL_MACHINE],
        # The thin-disk refusal is the queue's since 9623ad18 (it was an Apply that failed).
        [step("refused_on_thin", "WipeDisk", expect="refused"), step("wipe", "WipeDisk")],
        [
            check("a_thin_disk_keeps_its_table", "thin_table_after", equals_path="thin_table_before"),
            *files_unchanged("a_thin_disk_keeps_its_files", "thin_files_before", "thin_files_after"),
            check("the_marker_was_on_the_disk", "scan_before.marker_hits", at_least=1),
            check("the_wipe_destroyed_every_marker", "scan_after.marker_hits", equals=0),
            check("the_whole_disk_was_read_back", "scan_after.bytes_read", equals_path="disk_size_bytes"),
            check("no_partition_is_left", "snapshots.after.data_partition_count", equals=0),
        ],
        "Whether a device hides sectors from a write, which only a physical device could show.",
    ),
    row(
        "vhd.image-clone",
        "Offline VHD image clone and sentinel verification",
        [VIRTUAL_DISK],
        [step("clone", "CloneDisk")],
        [
            check("the_target_differed_first", "target_hash_before",
                  differs_from_path="source_hash"),
            check("the_source_was_not_changed", "source_hash", equals_path="source_hash_before"),
            check("the_raw_pattern_is_a_real_size", "pattern_bytes", at_least=64 * MIB),
            check("the_raw_pattern_was_copied", "pattern_on_target", equals_path="pattern_on_source"),
            check("every_sector_of_the_volume_matches", "difference_summary.bytes_inside_the_mounted_volume", equals=0),
            check("every_other_sector_matches", "difference_summary.runs_anywhere_else", equals=0),
            check("the_marker_after_the_source_is_kept", "outside_marker_kept", is_true=True),
            *files_unchanged("the_target_holds_the_files", "files_source", "files_target"),
        ],
        "Disks of different sector sizes. The partition table, which Windows corrects for a larger disk. A read-write source: Windows writes to a volume as it mounts it, so the fixture is read-only to compare every byte.",
    ),
    row(
        "vhd.image-restore",
        "Offline VHD image restore and overwrite verification",
        [VIRTUAL_DISK],
        [step("create_image", "CreateImage"), step("restore_image", "RestoreImage")],
        [
            check("the_image_equals_the_source", "image_hash", equals_path="source_hash"),
            check("the_target_differed_first", "target_hash_before", differs_from_path="source_hash"),
            check("the_raw_pattern_is_a_real_size", "pattern_bytes", at_least=64 * MIB),
            check("the_raw_pattern_was_restored", "pattern_on_target", equals_path="pattern_on_source"),
            check("every_sector_of_the_volume_matches", "difference_summary.bytes_inside_the_mounted_volume", equals=0),
            check("every_other_sector_matches", "difference_summary.runs_anywhere_else", equals=0),
            *files_unchanged("the_target_holds_the_files", "files_source", "files_target"),
            *healthy("restored", "health"),
        ],
        "An image on a network path. The partition table, which Windows corrects for a larger disk. A read-write source: Windows writes to a volume as it mounts it, so the fixture is read-only to compare every byte.",
    ),
    row(
        "vhd.partition-clone-region",
        "Partition clone to raw target region",
        [VIRTUAL_DISK],
        [step("clone", "ClonePartition")],
        [
            check("the_region_differed_first", "region_hash_before", differs_from_path="source_hash"),
            check("the_region_equals_the_partition", "region_hash_after", equals_path="source_hash"),
            check("the_marker_before_the_region_is_kept", "marker_before_kept", is_true=True),
            check("the_marker_after_the_region_is_kept", "marker_after_kept", is_true=True),
        ],
        "A target that is a volume path rather than free space.",
    ),
]

EXTERNAL_ROWS = [
    row(
        "external.system-mbr2gpt",
        "System MBR-to-GPT conversion in disposable boot VM",
        [VIRTUAL_MACHINE],
        [step("convert", "ConvertPartitionStyle")],
        [
            check("started_as_mbr", "snapshots.before.table.style", equals="MBR"),
            check("became_gpt", "snapshots.after.table.style", equals="GPT"),
            check("it_started_under_uefi", "boot.firmware", equals="UEFI"),
            check("it_reached_windows", "boot.reached_sign_in", is_true=True),
            check("windows_runs_from_the_same_letter", "boot.system_drive", equals="C:"),
            check("the_volume_is_clean", "boot.repair_scan", equals="NoErrorsFound"),
        ],
        "Real firmware, whose boot entries a VM's do not stand for.",
    ),
    row(
        "external.os-migration-reboot",
        "OS migration target boot and firmware-order proof",
        [VIRTUAL_MACHINE],
        [step("migrate", "MigrateOs")],
        [
            check("the_target_was_blank", "snapshots.before.data_partition_count", equals=0),
            check("the_target_started_alone", "boot.only_disk_attached", is_true=True),
            check("it_reached_windows", "boot.reached_sign_in", is_true=True),
            check("windows_runs_from_c", "boot.system_drive", equals="C:"),
            check("the_copied_marker_is_there", "boot.source_marker_present", is_true=True),
            check("the_volume_is_clean", "boot.repair_scan", equals="NoErrorsFound"),
        ],
        "A physical drive, NVMe, 4Kn, or a real firmware's boot order.",
    ),
    row(
        "external.boot-repair-uefi",
        "UEFI boot repair with intentionally broken BCD",
        [VIRTUAL_MACHINE],
        [step("repair", "RepairBoot")],
        [
            check("it_started_before", "boot_before.reached_sign_in", is_true=True),
            check("breaking_it_stopped_it_starting", "boot_broken.reached_sign_in", is_false=True),
            check("the_boot_files_were_gone", "broken_files_after", equals=0),
            check("it_started_after_the_repair", "boot_after.reached_sign_in", is_true=True),
            check("it_started_under_uefi", "boot_after.firmware", equals="UEFI"),
            check("the_volume_is_clean_after", "boot_after.repair_scan", equals="NoErrorsFound"),
        ],
        "Real firmware's boot entries, which an attached-disk repair leaves alone on purpose.",
    ),
    row(
        "external.boot-repair-bios",
        "BIOS/MBR boot repair with intentionally broken BCD",
        [VIRTUAL_MACHINE],
        [step("repair", "RepairBoot")],
        [
            check("it_started_before", "boot_before.reached_sign_in", is_true=True),
            check("breaking_it_stopped_it_starting", "boot_broken.reached_sign_in", is_false=True),
            check("the_boot_files_were_gone", "broken_files_after", equals=0),
            check("it_started_after_the_repair", "boot_after.reached_sign_in", is_true=True),
            check("it_started_under_bios", "boot_after.firmware", equals="Legacy"),
            check("the_volume_is_clean_after", "boot_after.repair_scan", equals="NoErrorsFound"),
        ],
        "A physical BIOS.",
    ),
    row(
        "external.usb-removable",
        "USB removable disk destructive operation proof",
        [VIRTUAL_MACHINE, PHYSICAL],
        [step("create", "Create"), step("format", "Format"), step("delete", "Delete")],
        [
            check("the_disk_is_on_usb", "bus_type", equals="USB"),
            check("created_at_the_requested_size", "snapshots.create.data_partitions[0].size_bytes",
                  equals=1024 * MIB),
            check("formatted_as_asked", "format_volume.label", equals="SAKUSB"),
            *files_unchanged("files_read_back_whole", "files_written", "files_read_back"),
            *healthy("formatted", "health"),
            check("delete_removed_it", "snapshots.delete.data_partition_count", equals=0),
        ],
        "A device Windows flags as removable: a USB hard drive reports as a fixed disk.",
        physical_claim=True,
        physical_requirement={"bus_type": ["USB"]},
    ),
    row(
        "external.file-level-data-recovery",
        "File-level Data Recovery scan and restore proof",
        [VIRTUAL_DISK, VIRTUAL_MACHINE],
        [step("recover", "FileRecovery")],
        [
            check("the_files_were_deleted_first", "deleted_files_present", equals=0),
            check("the_scan_read_the_whole_volume", "recovered.scan_bytes_read", equals_path="volume_bytes"),
            check("the_scan_met_no_error", "recovered.scan_warnings", equals=""),
            check("every_fixture_was_recovered", "recovered.missing_hashes", equals=0),
            check("the_source_was_not_changed", "source_hash_after", equals_path="source_hash_before"),
        ],
        "The panel's dialogs around the same engine.",
        client="elevated",
    ),
    row(
        "external.ssd-retrim",
        "SSD/NVMe ReTrim proof",
        [VIRTUAL_MACHINE, PHYSICAL],
        [step("retrim", "OptimizeSsd")],
        [
            check("windows_reports_a_solid_state_disk", "media_type", equals="SSD"),
            check("windows_logged_a_retrim", "retrim_logged", is_true=True),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("trimmed", "health"),
        ],
        "Whether the drive itself freed the blocks. TRIM's effect on flash cannot be read back: "
        "only that Windows sent it, and that nothing was lost.",
        physical_claim=True,
        physical_requirement={"media_type": ["SSD"]},
    ),
    row(
        "external.hdd-defrag-execution",
        "Direct in-app HDD defrag execution proof",
        [PHYSICAL],
        [step("defrag", "DefragVolume")],
        [
            # Windows says HDD, or says Unspecified and the drive itself reports a nonzero rate.
            check("drive_reports_it_spins", "media_type", spins_by_rotation_path="rotation_rate_rpm"),
            check("windows_logged_a_defrag", "defrag_logged", is_true=True),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("defragged", "health"),
        ],
        "Fragmentation dropping on any given volume: Windows decides how much it moves. That the "
        "drive spins rests on Windows reporting HDD, or on Windows reporting Unspecified and the "
        "drive itself reporting a nonzero rotation rate through smartctl. A VirtualBox disk is not "
        "accepted at all, so this row runs only on a physical device.",
        physical_claim=True,
        physical_requirement={"media_type": ["HDD", "Unspecified"]},
    ),
    row(
        "external.ssd-secure-erase",
        "SSD/NVMe secure erase command execution proof",
        [VIRTUAL_MACHINE, PHYSICAL],
        [step("erase", "WipeDisk")],
        [
            check("windows_reports_a_solid_state_disk", "media_type", equals="SSD"),
            check("the_app_warned_this_is_not_a_purge", "app_warnings", contains="purge"),
            check("every_marker_was_on_the_disk", "markers_before.marker_hits", equals_path="markers_before.places_read"),
            check("enough_of_the_disk_was_marked", "markers_before.places_read", at_least=32),
            check("the_erase_destroyed_every_marker", "markers_after.marker_hits", equals=0),
            check("no_partition_is_left", "snapshots.after.data_partition_count", equals=0),
            check("the_same_device_is_still_there", "serial_sha256_after", equals_path="serial_sha256_before"),
        ],
        "A sanitize of the flash. This is TRIM plus an overwrite, not an ATA or NVMe sanitize, and "
        "it cannot reach what a controller has remapped.",
        physical_claim=True,
        physical_requirement={"media_type": ["SSD"]},
    ),
    row(
        "external.bitlocker",
        "BitLocker locked/unlocked data-volume blocker proof",
        [VIRTUAL_DISK, VIRTUAL_MACHINE],
        [step("refused_while_locked", "Format", expect="refused")],
        [
            check("the_volume_was_locked", "bitlocker_before.locked", is_true=True),
            check("the_app_refused_and_said_why", "refusal", contains="BitLocker"),
            check("the_volume_was_not_changed", "table_after", equals_path="table_before"),
            check("it_is_still_locked", "bitlocker_after.locked", is_true=True),
            check("the_unlocked_files_are_intact", "files_after.digest", equals_path="files_before.digest"),
        ],
        "A volume whose lock state changes between the check and the write.",
    ),
    row(
        "external.bitlocker-mutation",
        "In-app BitLocker unlock/suspend/resume mutation proof",
        [VIRTUAL_DISK, VIRTUAL_MACHINE],
        [
            step("unlock", "BitLockerUnlock"),
            step("suspend", "BitLockerSuspend"),
            step("resume", "BitLockerResume"),
        ],
        [
            check("started_locked", "bitlocker_before.locked", is_true=True),
            check("unlock_opened_it", "bitlocker_unlocked.locked", is_false=True),
            check("suspend_turned_protection_off", "bitlocker_suspended.protection", equals="Off"),
            check("resume_turned_protection_on", "bitlocker_resumed.protection", equals="On"),
            check("the_password_stayed_out_of_the_report", "password_in_reports", equals=0),
            *files_unchanged("the_files_are_intact", "files_before", "files_after"),
        ],
        "A TPM protector: a data volume uses a password here.",
    ),
    row(
        "external.allocate-free-space",
        "Allocate Free Space adjacent donor-volume backup/delete/extend/recreate/restore proof",
        [VIRTUAL_DISK, VIRTUAL_MACHINE, PHYSICAL],
        [step("allocate", "AllocateFreeSpace")],
        [
            check("the_target_grew_by_the_amount", "snapshots.after.data_partitions[0].size_bytes",
                  minus_path="snapshots.before.data_partitions[0].size_bytes", equals=64 * MIB),
            check("the_donor_shrank_by_the_amount", "snapshots.before.data_partitions[1].size_bytes",
                  minus_path="snapshots.after.data_partitions[1].size_bytes", equals=64 * MIB),
            *files_unchanged("the_target_kept_its_files", "target_files_before", "target_files_after"),
            *files_unchanged("the_donor_kept_its_files", "donor_files_before", "donor_files_after"),
            *healthy("target", "target_health"),
            *healthy("donor", "donor_health"),
        ],
        "A failure halfway, which leaves the backup for a person to restore.",
    ),
    row(
        "external.partition-move",
        "Offline partition start-move proof",
        [VIRTUAL_DISK, VIRTUAL_MACHINE, PHYSICAL],
        [step("move", "MovePartition")],
        [
            check("the_start_moved", "snapshots.after.data_partitions[0].offset_bytes",
                  differs_from_path="snapshots.before.data_partitions[0].offset_bytes"),
            check("it_moved_where_asked", "snapshots.after.data_partitions[0].offset_bytes",
                  equals_path="requested_offset_bytes"),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("moved", "health"),
        ],
        "That the bytes stayed put during the rebuild: the app backs up, recreates and restores, "
        "so this proves the files, not a block move.",
    ),
    row(
        "external.cluster-size-change",
        "Existing-volume cluster-size change proof",
        [VIRTUAL_DISK, VIRTUAL_MACHINE, PHYSICAL],
        [step("change", "ChangeClusterSize")],
        [
            check("started_at_another_size", "volume_before.cluster_bytes", equals=4096),
            check("changed_to_the_size_asked_for", "volume_after.cluster_bytes", equals=16384),
            *files_unchanged("files_survived", "files_before", "files_after"),
            check("the_alternate_stream_survived", "stream_after", equals_path="stream_before"),
            check("the_acl_survived", "acl_after", equals_path="acl_before"),
            *healthy("changed", "health"),
        ],
        "A failure halfway, which leaves the backup for a person to restore.",
    ),
    row(
        "external.primary-logical-conversion",
        "Primary/logical conversion on disposable MBR disk",
        [VIRTUAL_DISK, VIRTUAL_MACHINE, PHYSICAL],
        [step("to_logical", "ConvertPrimaryLogical")],
        [
            check("started_primary", "snapshots.before.table.partitions[0].layout", equals="primary"),
            check("the_table_holds_a_logical_partition", "snapshots.after.logical_partition_count",
                  equals=1),
            check("the_table_holds_an_extended_container", "snapshots.after.extended_partition_count",
                  equals=1),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("converted", "health"),
        ],
        "A boot disk, which the app refuses here.",
    ),
    row(
        "external.volume-serial-number",
        "Volume serial-number metadata mutation proof",
        [VIRTUAL_DISK, VIRTUAL_MACHINE, PHYSICAL],
        [step("change", "ChangeVolumeSerialNumber")],
        [
            check("the_boot_sector_holds_a_new_serial", "boot_sector_serial_after",
                  differs_from_path="boot_sector_serial_before"),
            check("windows_reports_the_same_serial", "volume_after.serial_number",
                  equals_path="boot_sector_serial_after"),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("changed", "health"),
        ],
        "A serial chosen by a person: the app reformats, and the file system picks a new one.",
    ),
    row(
        "external.dynamic-to-basic",
        "Dynamic disk to basic conversion proof",
        [VIRTUAL_MACHINE],
        [step("convert", "ConvertDynamicDiskToBasic")],
        [
            check("the_disk_was_dynamic", "snapshots.before.dynamic_entry_count", at_least=1),
            check("get_disk_did_not_list_it", "windows_listed_it_with_get_disk", is_false=True),
            check("get_disk_lists_it_now", "listed_by_get_disk_after", is_true=True),
            check("the_disk_is_basic", "snapshots.after.dynamic_entry_count", equals=0),
            *files_unchanged("files_survived", "files_before", "files_after"),
            *healthy("converted", "health"),
        ],
        "A spanned or mirrored dynamic volume, which the app refuses.",
    ),
    row(
        "external.hardware-wipe",
        "Non-system hardware wipe on disposable physical disk",
        [VIRTUAL_MACHINE, PHYSICAL],
        [step("wipe", "WipeDisk")],
        [
            check("every_marker_was_on_the_disk", "markers_before.marker_hits", equals_path="markers_before.places_read"),
            check("enough_of_the_disk_was_marked", "markers_before.places_read", at_least=32),
            check("the_wipe_destroyed_every_marker", "markers_after.marker_hits", equals=0),
            check("no_partition_is_left", "snapshots.after.data_partition_count", equals=0),
            check("the_same_device_is_still_there", "serial_sha256_after", equals_path="serial_sha256_before"),
        ],
        "Sectors a drive has remapped, and a disk too large to read back whole.",
        physical_claim=True,
        physical_requirement={},
    ),
]


def matrix() -> dict[str, object]:
    return {
        "schema_version": 2,
        "generated_by": "scripts/generate_partition_manager_matrix.py",
        "vhd_scenarios": VHD_ROWS,
        "external_gates": EXTERNAL_ROWS,
    }


def rendered() -> str:
    return json.dumps(matrix(), indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path, help="write it here instead")
    arguments = parser.parse_args()
    target = arguments.root / "certification" / "PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    if arguments.output is not None:
        target = arguments.output
    if arguments.check:
        current = target.read_bytes().replace(b"\r\n", b"\n").decode("utf-8")
        if current != rendered():
            print(f"{target} is not what scripts/generate_partition_manager_matrix.py writes.")
            return 1
        print("The Partition Manager matrix is what its generator writes.")
        return 0
    target.write_text(rendered(), encoding="utf-8", newline="\n")
    print(f"Wrote {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
