// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_apfs_writer.h
/// @brief Fail-closed APFS write preflight for future Partition Manager mutation support.

#pragma once

#include "sak/partition_file_system_detector.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace sak {

enum class PartitionApfsWriteOperation {
    CreateDirectory,
    DeleteDirectory,
    CreateFile,
    ReplaceFile,
    DeleteFile,
    ChangeVolumeLabel,
    FormatContainer,
    RepairContainer,
    ResizeContainer,
};

struct PartitionApfsWriteOptions {
    bool enable_experimental_writer{false};
    bool image_only{true};
    bool destructive_certification_evidence{false};
    bool raw_media_hardware_certification_evidence{false};
    bool allow_encrypted_or_protected_volume{false};
    bool allow_compressed_file_mutation{false};
    bool allow_snapshots{false};
    bool allow_multi_volume_container{false};
    uint64_t max_payload_bytes{0};
    QString evidence_id;
};

struct PartitionApfsWritePreflight {
    bool allowed{false};
    QStringList blockers;
    QStringList warnings;
    QStringList required_evidence;
};

struct PartitionApfsImageMutationStep {
    QString name;
    QString description;
    bool writes_metadata{false};
    bool requires_checkpoint{false};
    QStringList required_evidence;
};

struct PartitionApfsImageMutationPlan {
    bool buildable{false};
    bool executable{false};
    PartitionApfsWritePreflight preflight;
    QString operation;
    QString target_path;
    QString volume_name;
    QString evidence_id;
    uint64_t max_payload_bytes{0};
    uint64_t target_container_bytes{0};
    uint32_t block_size_bytes{0};
    QVector<PartitionApfsImageMutationStep> steps;
    QStringList post_apply_verification;
    QStringList execution_blockers;
};

struct PartitionApfsWriterExecutionEvidence {
    QString evidence_id;
    QString operation;
    QString target_path;
    bool structure_mapping_verified{false};
    bool object_checksum_vectors_verified{false};
    bool source_image_hash_verified{false};
    bool scratch_image_hash_verified{false};
    bool copy_on_write_checkpoint_verified{false};
    bool object_map_update_verified{false};
    bool space_manager_accounting_verified{false};
    bool fsck_validation_verified{false};
    bool target_readback_verified{false};
    bool crash_replay_verified{false};
    bool rollback_boundary_verified{false};
    bool hardware_raw_media_verified{false};
    QStringList artifacts;
};

struct PartitionApfsImageBuildResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString image_path;
    QString image_sha256;
    QStringList blockers;
    QStringList warnings;
};

struct PartitionApfsImageRepairResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString source_image_path;
    QString repaired_image_path;
    QString repaired_image_sha256;
    QStringList blockers;
    QStringList warnings;
    uint64_t scanned_blocks{0};
    uint64_t repaired_checksum_blocks{0};
};

struct PartitionApfsImageFileWriteResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString source_image_path;
    QString written_image_path;
    QString written_image_sha256;
    QString directory_name;
    QString file_name;
    uint64_t file_bytes{0};
    QString payload_sha256;
    QString readback_sha256;
    QStringList blockers;
    QStringList warnings;
    uint64_t written_data_blocks{0};
};

struct PartitionApfsImageFileDeleteResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString source_image_path;
    QString written_image_path;
    QString written_image_sha256;
    QString directory_name;
    QString file_name;
    uint64_t deleted_file_bytes{0};
    QString deleted_file_sha256;
    QStringList blockers;
    QStringList warnings;
    uint64_t freed_data_blocks{0};
};

struct PartitionApfsImageDirectoryMutationResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString source_image_path;
    QString written_image_path;
    QString written_image_sha256;
    QString directory_name;
    QStringList blockers;
    QStringList warnings;
};

struct PartitionApfsImageVolumeLabelResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString source_image_path;
    QString written_image_path;
    QString written_image_sha256;
    QString old_volume_name;
    QString new_volume_name;
    QStringList blockers;
    QStringList warnings;
};

struct PartitionApfsImageFilePatchResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString source_image_path;
    QString written_image_path;
    QString written_image_sha256;
    QString directory_name;
    QString file_name;
    uint64_t file_bytes{0};
    uint64_t patch_offset_bytes{0};
    uint64_t patch_bytes{0};
    QString patch_sha256;
    QString readback_sha256;
    QStringList blockers;
    QStringList warnings;
    uint64_t written_data_blocks{0};
};

struct PartitionApfsRawFileWriteResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString target_path;
    QString directory_name;
    QString file_name;
    uint64_t file_bytes{0};
    QString payload_sha256;
    QString readback_sha256;
    QStringList blockers;
    QStringList warnings;
    uint64_t written_data_blocks{0};
};

struct PartitionApfsRawFileDeleteResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString target_path;
    QString directory_name;
    QString file_name;
    uint64_t deleted_file_bytes{0};
    QString deleted_file_sha256;
    QStringList blockers;
    QStringList warnings;
    uint64_t freed_data_blocks{0};
};

struct PartitionApfsRawFilePatchResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString target_path;
    QString directory_name;
    QString file_name;
    uint64_t file_bytes{0};
    uint64_t patch_offset_bytes{0};
    uint64_t patch_bytes{0};
    QString patch_sha256;
    QString readback_sha256;
    QStringList blockers;
    QStringList warnings;
    uint64_t written_data_blocks{0};
};

struct PartitionApfsRawDirectoryMutationResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString target_path;
    QString directory_name;
    QStringList blockers;
    QStringList warnings;
};

struct PartitionApfsRawVolumeLabelResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString target_path;
    QString old_volume_name;
    QString new_volume_name;
    QStringList blockers;
    QStringList warnings;
};

struct PartitionApfsRawRepairResult {
    bool ok{false};
    PartitionApfsImageMutationPlan plan;
    QString target_path;
    QStringList blockers;
    QStringList warnings;
    uint64_t scanned_blocks{0};
    uint64_t repaired_checksum_blocks{0};
};

struct PartitionApfsImageFormatRequest {
    QString image_path;
    uint64_t target_container_bytes{0};
    uint32_t block_size_bytes{0};
    QString volume_name;
    QString seed_file_name;
    QByteArray seed_file_data;
    bool target_wipe_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRepairRequest {
    QString source_image_path;
    QString repaired_image_path;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRootFileWriteRequest {
    QString source_image_path;
    QString written_image_path;
    QString file_name;
    QByteArray file_data;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRootFileDeleteRequest {
    QString source_image_path;
    QString written_image_path;
    QString file_name;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRootDirectoryFileWriteRequest {
    QString source_image_path;
    QString written_image_path;
    QString directory_name;
    QString file_name;
    QByteArray file_data;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRootDirectoryFileDeleteRequest {
    QString source_image_path;
    QString written_image_path;
    QString directory_name;
    QString file_name;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRootDirectoryFilePatchRequest {
    QString source_image_path;
    QString written_image_path;
    QString directory_name;
    QString file_name;
    uint64_t patch_offset_bytes{0};
    QByteArray patch_data;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRootFilePatchRequest {
    QString source_image_path;
    QString written_image_path;
    QString file_name;
    uint64_t patch_offset_bytes{0};
    QByteArray patch_data;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageRootDirectoryMutationRequest {
    QString source_image_path;
    QString written_image_path;
    QString directory_name;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsImageVolumeLabelRequest {
    QString source_image_path;
    QString written_image_path;
    QString volume_name;
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRootFileWriteRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString file_name;
    QByteArray file_data;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRootFileDeleteRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString file_name;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRootDirectoryFileWriteRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    QString file_name;
    QByteArray file_data;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRootDirectoryFileDeleteRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    QString file_name;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRootDirectoryFilePatchRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    QString file_name;
    uint64_t patch_offset_bytes{0};
    QByteArray patch_data;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRootFilePatchRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString file_name;
    uint64_t patch_offset_bytes{0};
    QByteArray patch_data;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRootDirectoryMutationRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawVolumeLabelRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString volume_name;
    bool target_write_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

struct PartitionApfsRawRepairRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    bool target_repair_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to advance an existing generated APFS container's checkpoint
///        by one transaction in place (A2: in-place COW checkpoint commit).
///        If @c written_image_path differs from the source the source is cloned
///        first and the commit applied to the clone.
struct PartitionApfsImageCheckpointCommitRequest {
    QString source_image_path;
    QString written_image_path;
    PartitionApfsWriteOptions options;
};

/// @brief Result of an in-place checkpoint commit: the advanced transaction id
///        and the descriptor-ring blocks the new checkpoint-map and
///        nx_superblock landed on.
struct PartitionApfsImageCheckpointCommitResult {
    QString source_image_path;
    QString written_image_path;
    bool ok{false};
    uint64_t previous_xid{0};
    uint64_t new_xid{0};
    uint64_t checkpoint_map_block{0};
    uint64_t superblock_block{0};
    QStringList blockers;
    QStringList warnings;
};

/// @brief Request to insert one empty file into a generated APFS container with
///        a true in-place copy-on-write checkpoint commit (A2 increment 2).
struct PartitionApfsImageFileInsertCommitRequest {
    QString source_image_path;
    QString written_image_path;
    QString file_name;
    QByteArray file_data;
    PartitionApfsWriteOptions options;
};

/// @brief Request to delete one root file from a generated APFS container with a
///        true in-place copy-on-write checkpoint commit (A2).
struct PartitionApfsImageFileDeleteCommitRequest {
    QString source_image_path;
    QString written_image_path;
    QString file_name;
    PartitionApfsWriteOptions options;
};

/// @brief Request to rename one root file in a generated APFS container with a
///        true in-place copy-on-write checkpoint commit (A2).
struct PartitionApfsImageFileRenameCommitRequest {
    QString source_image_path;
    QString written_image_path;
    QString file_name;
    QString new_file_name;
    PartitionApfsWriteOptions options;
};

/// @brief Request to rename one file inside a root directory (same parent) in a
///        generated APFS container with an in-place copy-on-write commit (A2-3.2).
struct PartitionApfsImageRootDirectoryFileRenameRequest {
    QString source_image_path;
    QString written_image_path;
    QString directory_name;
    QString file_name;
    QString new_file_name;
    PartitionApfsWriteOptions options;
};

/// @brief Request to move one file between parents (root or a root directory) in a
///        generated APFS container with an in-place copy-on-write commit (A2-3.2). An
///        empty directory name means the container root.
struct PartitionApfsImageFileMoveCommitRequest {
    QString source_image_path;
    QString written_image_path;
    QString source_directory_name;       // empty = root
    QString file_name;
    QString destination_directory_name;  // empty = root
    QString new_file_name;
    PartitionApfsWriteOptions options;
};

/// @brief Request to patch one file's byte range in a generated APFS container with a
///        true in-place copy-on-write commit (preserves the file's object id; crash-safe).
///        An empty directory name means the container root.
struct PartitionApfsImageFilePatchCommitRequest {
    QString source_image_path;
    QString written_image_path;
    QString directory_name;  // empty = root
    QString file_name;
    uint64_t patch_offset_bytes{0};
    QByteArray patch_data;
    PartitionApfsWriteOptions options;
};

/// @brief Request to insert one file into a generated APFS container on a raw
///        device with a true in-place copy-on-write checkpoint commit (A2). The
///        commit is applied to the device in place (no scratch clone), so it is
///        gated by explicit destructive-target confirmation and raw opt-in.
struct PartitionApfsRawFileInsertCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString file_name;
    QByteArray file_data;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to delete one root file from a generated APFS container on a
///        raw device with a true in-place copy-on-write checkpoint commit (A2).
struct PartitionApfsRawFileDeleteCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString file_name;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to rename one root file in a generated APFS container on a raw
///        device with a true in-place copy-on-write checkpoint commit (A2).
struct PartitionApfsRawFileRenameCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString file_name;
    QString new_file_name;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to create or delete one empty root directory in a generated APFS
///        container on a raw device with an in-place copy-on-write commit (A2-3.2).
struct PartitionApfsRawDirectoryMutationCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to create-or-replace one file inside a root directory in a generated
///        APFS container on a raw device with an in-place copy-on-write commit (A2-3.2).
struct PartitionApfsRawDirectoryChildWriteCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    QString file_name;
    QByteArray file_data;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to delete one file inside a root directory in a generated APFS
///        container on a raw device with an in-place copy-on-write commit (A2-3.2).
struct PartitionApfsRawDirectoryChildDeleteCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    QString file_name;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to rename one file inside a root directory (same parent) in a
///        generated APFS container on a raw device with an in-place commit (A2-3.2).
struct PartitionApfsRawDirectoryChildRenameCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;
    QString file_name;
    QString new_file_name;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to move one file between parents (root or a root directory) in a
///        generated APFS container on a raw device with an in-place commit (A2-3.2). An
///        empty directory name means the container root.
struct PartitionApfsRawFileMoveCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString source_directory_name;       // empty = root
    QString file_name;
    QString destination_directory_name;  // empty = root
    QString new_file_name;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to patch one file's byte range in a generated APFS container on a raw
///        device with a true in-place copy-on-write commit (A2). An empty directory name
///        means the container root.
struct PartitionApfsRawFilePatchCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString directory_name;  // empty = root
    QString file_name;
    uint64_t patch_offset_bytes{0};
    QByteArray patch_data;
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Request to create one snapshot of a generated APFS container with a true
///        in-place copy-on-write checkpoint commit (A3). Freezes the volume's current
///        tree state into a snapshot the kernel and fsck_apfs can enumerate; the live
///        volume keeps writing. Fails closed if the volume already carries a snapshot.
struct PartitionApfsImageSnapshotCreateCommitRequest {
    QString source_image_path;
    QString written_image_path;
    QString snapshot_name;
    uint64_t create_time_ns{0};  // 0 = stamp with the current wall-clock time
    PartitionApfsWriteOptions options;
};

/// @brief Request to create one snapshot of a generated APFS container on a raw device
///        with a true in-place copy-on-write checkpoint commit (A3). Applied to the
///        device in place, so gated by destructive-target confirmation and raw opt-in.
struct PartitionApfsRawSnapshotCreateCommitRequest {
    QString target_path;
    uint64_t target_container_bytes{0};
    QString snapshot_name;
    uint64_t create_time_ns{0};  // 0 = stamp with the current wall-clock time
    bool target_mutation_confirmed{false};
    bool allow_raw_device_target{false};
    PartitionApfsWriteOptions options;
};

/// @brief Derived geometry of an APFS container's space-manager device:
///        how many spaceman chunks, chunk-info blocks (CIBs), chunk-info
///        address blocks (CABs), per-chunk allocation bitmaps, and internal-pool
///        blocks a container of @c block_count blocks requires.
///
/// This is the geometry the multi-chunk format builder (A1 of the APFS/HFS
/// full driver plan) must emit to exceed the current single-chunk
/// (64-128 MiB) certified envelope. The internal-pool sizing
/// @c ip_block_count = 3 * (cib_count + chunk_count) was derived from two real
/// Apple `newfs_apfs` containers (apple-fresh.img: 1 CIB + 1 chunk -> 6;
/// ref 3.apfs: 1 CIB + 10 chunks -> 33) and is verified for the single-CIB
/// tier; the CIB term remains a hypothesis for the multi-CIB tier until a
/// >126-chunk Apple container is harvested in the macOS VM.
struct PartitionApfsContainerGeometry {
    uint64_t block_count{0};
    uint32_t block_size{0};
    uint64_t blocks_per_chunk{0};
    uint64_t chunk_count{0};
    uint64_t chunks_per_cib{0};
    uint64_t cib_count{0};
    uint64_t cibs_per_cab{0};
    uint64_t cab_count{0};
    uint64_t chunk_bitmap_block_count{0};
    uint64_t ip_bitmap_block_count{0};
    uint64_t ip_block_count{0};
    bool single_chunk{false};  ///< true for the 64-128 MiB certified envelope
    bool multi_cib{false};     ///< chunk_count > chunks_per_cib -> needs the CAB tier
};

class PartitionApfsWriter {
public:
    [[nodiscard]] static QString operationName(PartitionApfsWriteOperation operation);
    /// @brief Compute the multi-chunk/multi-CIB space-manager geometry for a
    ///        container of @p block_count blocks (default APFS 4096-byte blocks).
    ///        Pure function; no I/O. See @ref PartitionApfsContainerGeometry.
    [[nodiscard]] static PartitionApfsContainerGeometry computeContainerGeometry(
        uint64_t block_count, uint32_t block_size = 4096);
    [[nodiscard]] static std::optional<uint64_t> computeObjectChecksum(
        const QByteArray& object_bytes);
    [[nodiscard]] static bool stampObjectChecksum(QByteArray* object_bytes);
    /// @brief Build the file-system B-tree node blocks for @p file_names (empty
    ///        root files) - a single ROOT|LEAF node, or an internal root over
    ///        leaf nodes when they overflow (A2 multi-leaf fs-tree). Leaf oids
    ///        run from @p first_leaf_oid; nodes[0] is the root. Diagnostic entry
    ///        point for the multi-leaf builder ahead of its commit-path wiring.
    [[nodiscard]] static QVector<QByteArray> buildFsTreeNodeBlocks(uint32_t block_size,
                                                                   const QStringList& file_names,
                                                                   uint64_t first_leaf_oid,
                                                                   QStringList* blockers);
    [[nodiscard]] static bool verifyObjectChecksum(const QByteArray& object_bytes);
    /// \brief The internal-pool cib/bitmap slot {cib_block, bitmap_block} a
    ///        crash-safe in-place commit writes next, given the live cib block of
    ///        a generated single-chunk container. Round-robins the three IP slots
    ///        (live 187 -> {189,190}; 189 -> {185,186}; 185 -> {187,188}) so the
    ///        previous checkpoint's cib/bitmap stay intact. Foundation for the
    ///        crash-safe IP rotation (docs/APFS_A2_CRASH_SAFETY_DESIGN.md).
    [[nodiscard]] static QPair<quint64, quint64> nextCrashSafeIpSlot(quint64 live_cib,
                                                                     quint64 block_count);
    /// \brief The live spaceman's first cib_addr in the generated container at
    ///        @p image_path - the chunk-info block the live checkpoint uses (187
    ///        for a freshly formatted container; the crash-safe rotation moves it
    ///        through the IP slots). Walks the live checkpoint-map to the
    ///        ephemeral spaceman. Returns 0 on read failure.
    [[nodiscard]] static quint64 readGeneratedLiveCibAddr(const QString& image_path);
    [[nodiscard]] static QStringList enterpriseCertificationRequirements();
    [[nodiscard]] static PartitionApfsWritePreflight preflightExistingContainer(
        const PartitionFileSystemDetection& detection,
        PartitionApfsWriteOperation operation,
        const PartitionApfsWriteOptions& options);
    [[nodiscard]] static PartitionApfsImageMutationPlan planImageOnlyMutation(
        const PartitionFileSystemDetection& detection,
        PartitionApfsWriteOperation operation,
        const PartitionApfsWriteOptions& options,
        const QString& target_path);
    [[nodiscard]] static PartitionApfsImageMutationPlan planImageOnlyFormat(
        uint64_t target_container_bytes,
        uint32_t block_size_bytes,
        const QString& volume_name,
        const PartitionApfsWriteOptions& options);
    [[nodiscard]] static PartitionApfsImageBuildResult buildImageOnlyFormatImage(
        const PartitionApfsImageFormatRequest& request);
    [[nodiscard]] static PartitionApfsImageBuildResult buildImageOnlyFormatImageWithSeedFile(
        const PartitionApfsImageFormatRequest& request);
    [[nodiscard]] static PartitionApfsImageBuildResult formatExistingImageOnlyContainer(
        const PartitionApfsImageFormatRequest& request);
    [[nodiscard]] static PartitionApfsImageBuildResult formatExistingContainerTarget(
        const PartitionApfsImageFormatRequest& request);
    [[nodiscard]] static PartitionApfsImageRepairResult repairImageOnlyObjectChecksums(
        const PartitionApfsImageRepairRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyCheckpoint(
        const PartitionApfsImageCheckpointCommitRequest& request);
    /// @brief Create-or-replace one root file with an in-place COW commit (the
    ///        production file-write primitive): a new name is one insert commit, an
    ///        existing name is a delete-then-insert replace. Reuses the insert request.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyFileWrite(
        const PartitionApfsImageFileInsertCommitRequest& request);
    /// @brief Create one empty root directory with an in-place COW commit (the
    ///        full-tree analogue of the legacy rewrite-based createImageOnlyRootDirectory).
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyDirectoryCreate(
        const PartitionApfsImageRootDirectoryMutationRequest& request);
    /// @brief Delete one empty root directory with an in-place COW commit.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyDirectoryDelete(
        const PartitionApfsImageRootDirectoryMutationRequest& request);
    /// @brief Create-or-replace one file inside a root directory with an in-place COW commit.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult
    commitImageOnlyDirectoryChildWrite(
        const PartitionApfsImageRootDirectoryFileWriteRequest& request);
    /// @brief Delete one file inside a root directory with an in-place COW commit.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult
    commitImageOnlyDirectoryChildDelete(
        const PartitionApfsImageRootDirectoryFileDeleteRequest& request);
    /// @brief Rename one file inside a root directory (same parent) with an in-place COW commit.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult
    commitImageOnlyDirectoryChildRename(
        const PartitionApfsImageRootDirectoryFileRenameRequest& request);
    /// @brief Move one file between parents (root or a root directory) with an in-place COW commit.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyFileMove(
        const PartitionApfsImageFileMoveCommitRequest& request);
    /// @brief Patch one file's byte range with a true in-place COW commit (keeps the object id).
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyFilePatch(
        const PartitionApfsImageFilePatchCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyFileInsert(
        const PartitionApfsImageFileInsertCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyFileDelete(
        const PartitionApfsImageFileDeleteCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlyFileRename(
        const PartitionApfsImageFileRenameCommitRequest& request);
    /// @brief In-place COW file write/insert/delete/rename commit applied directly to a
    ///        generated APFS container on a confirmed raw device (the on-hardware
    ///        analogue of the commitImageOnly* family; no scratch clone). commitRawFileWrite
    ///        is create-or-replace; commitRawFileInsert fails closed on an existing name.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawFileWrite(
        const PartitionApfsRawFileInsertCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawFileInsert(
        const PartitionApfsRawFileInsertCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawFileDelete(
        const PartitionApfsRawFileDeleteCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawFileRename(
        const PartitionApfsRawFileRenameCommitRequest& request);
    /// @brief In-place COW directory mutations applied directly to a generated APFS
    ///        container on a confirmed raw device (the on-hardware analogue of the
    ///        commitImageOnlyDirectory* family; no scratch clone).
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawDirectoryCreate(
        const PartitionApfsRawDirectoryMutationCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawDirectoryDelete(
        const PartitionApfsRawDirectoryMutationCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawDirectoryChildWrite(
        const PartitionApfsRawDirectoryChildWriteCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawDirectoryChildDelete(
        const PartitionApfsRawDirectoryChildDeleteCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawDirectoryChildRename(
        const PartitionApfsRawDirectoryChildRenameCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawFileMove(
        const PartitionApfsRawFileMoveCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawFilePatch(
        const PartitionApfsRawFilePatchCommitRequest& request);
    /// @brief Create one snapshot of a generated APFS container with a true in-place COW
    ///        checkpoint commit (A3). commitImageOnlySnapshotCreate clones to a scratch
    ///        image; commitRawSnapshotCreate applies to a confirmed raw device in place.
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitImageOnlySnapshotCreate(
        const PartitionApfsImageSnapshotCreateCommitRequest& request);
    [[nodiscard]] static PartitionApfsImageCheckpointCommitResult commitRawSnapshotCreate(
        const PartitionApfsRawSnapshotCreateCommitRequest& request);
    /// @brief Test-only seam: override the predicate that classifies a path as an
    ///        acceptable raw-device commit target, so unit tests can drive the production
    ///        @c commitRaw* orchestration against a temporary file while every other
    ///        production guard (explicit confirmation, raw opt-in, non-image-only options,
    ///        size alignment, APFS detection) still runs. Pass a null predicate to restore
    ///        the production rule (@c isWindowsRawDevicePath). Never call from production code.
    static void setRawDeviceTargetPredicateForTesting(
        std::function<bool(const QString&)> predicate);
    [[nodiscard]] static PartitionApfsImageFileWriteResult writeImageOnlyRootFile(
        const PartitionApfsImageRootFileWriteRequest& request);
    [[nodiscard]] static PartitionApfsImageFileDeleteResult deleteImageOnlyRootFile(
        const PartitionApfsImageRootFileDeleteRequest& request);
    [[nodiscard]] static PartitionApfsImageFileWriteResult writeImageOnlyRootDirectoryFile(
        const PartitionApfsImageRootDirectoryFileWriteRequest& request);
    [[nodiscard]] static PartitionApfsImageFileDeleteResult deleteImageOnlyRootDirectoryFile(
        const PartitionApfsImageRootDirectoryFileDeleteRequest& request);
    [[nodiscard]] static PartitionApfsImageFilePatchResult patchImageOnlyRootDirectoryFile(
        const PartitionApfsImageRootDirectoryFilePatchRequest& request);
    [[nodiscard]] static PartitionApfsImageFilePatchResult patchImageOnlyRootFile(
        const PartitionApfsImageRootFilePatchRequest& request);
    [[nodiscard]] static PartitionApfsImageDirectoryMutationResult createImageOnlyRootDirectory(
        const PartitionApfsImageRootDirectoryMutationRequest& request);
    [[nodiscard]] static PartitionApfsImageDirectoryMutationResult deleteImageOnlyRootDirectory(
        const PartitionApfsImageRootDirectoryMutationRequest& request);
    [[nodiscard]] static PartitionApfsImageVolumeLabelResult changeImageOnlyVolumeLabel(
        const PartitionApfsImageVolumeLabelRequest& request);
    [[nodiscard]] static PartitionApfsRawFileWriteResult writeRawRootFile(
        const PartitionApfsRawRootFileWriteRequest& request);
    [[nodiscard]] static PartitionApfsRawFileDeleteResult deleteRawRootFile(
        const PartitionApfsRawRootFileDeleteRequest& request);
    [[nodiscard]] static PartitionApfsRawFileWriteResult writeRawRootDirectoryFile(
        const PartitionApfsRawRootDirectoryFileWriteRequest& request);
    [[nodiscard]] static PartitionApfsRawFileDeleteResult deleteRawRootDirectoryFile(
        const PartitionApfsRawRootDirectoryFileDeleteRequest& request);
    [[nodiscard]] static PartitionApfsRawFilePatchResult patchRawRootDirectoryFile(
        const PartitionApfsRawRootDirectoryFilePatchRequest& request);
    [[nodiscard]] static PartitionApfsRawFilePatchResult patchRawRootFile(
        const PartitionApfsRawRootFilePatchRequest& request);
    [[nodiscard]] static PartitionApfsRawDirectoryMutationResult createRawRootDirectory(
        const PartitionApfsRawRootDirectoryMutationRequest& request);
    [[nodiscard]] static PartitionApfsRawDirectoryMutationResult deleteRawRootDirectory(
        const PartitionApfsRawRootDirectoryMutationRequest& request);
    [[nodiscard]] static PartitionApfsRawVolumeLabelResult changeRawVolumeLabel(
        const PartitionApfsRawVolumeLabelRequest& request);
    [[nodiscard]] static PartitionApfsRawRepairResult repairRawObjectChecksums(
        const PartitionApfsRawRepairRequest& request);
    [[nodiscard]] static PartitionApfsWritePreflight validateImageOnlyExecutionEvidence(
        const PartitionApfsImageMutationPlan& plan,
        const PartitionApfsWriterExecutionEvidence& evidence);
};

}  // namespace sak
