// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_controller.cpp
/// @brief Partition Manager controller implementation.

#include "sak/partition_manager_controller.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/partition_raw_device_io.h"
#include "sak/partition_report_generator.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QtConcurrent>
#include <QThread>

#include <utility>

namespace sak {

namespace {
constexpr uint64_t kProjectedPartitionAlignmentBytes = 1024ULL * 1024ULL;

std::shared_ptr<PartitionExecutor> makeAsyncExecutor() {
    return std::shared_ptr<PartitionExecutor>(
        new PartitionExecutor(), [](PartitionExecutor* executor) {
            if (!executor) {
                return;
            }
            auto* target_thread = executor->thread();
            if (target_thread && target_thread != QThread::currentThread() &&
                target_thread->isRunning()) {
                QMetaObject::invokeMethod(executor, &QObject::deleteLater, Qt::QueuedConnection);
                return;
            }
            delete executor;
        });
}

QString runtimeFilesystemManifestPath() {
    // Root of trust for the elevated filesystem tools: resolve only from the trusted
    // application directory, never a CWD-relative manifest an attacker could plant.
    return PartitionFileSystemToolManifest::resolveRuntimeManifestPath();
}

// Cancel-then-JOIN a future watcher and delete it, instead of detaching it. The
// caller must have already issued any cooperative cancel so waitForFinished()
// returns promptly. Disconnecting before the wait keeps the finished slot from
// firing on a half-destroyed owner; waitForFinished() does not spin the event
// loop, so no re-entrancy.
template <typename ResultT>
void cancelJoinDeleteWatcher(QFutureWatcher<ResultT>*& watcher, QObject* owner) {
    if (!watcher) {
        return;
    }
    QObject::disconnect(watcher, nullptr, owner, nullptr);
    // SAK-ALLOW-BLOCKING: `delete watcher` follows on the next line, so abandoning the wait
    // would free the watcher underneath a running task. The caller has already issued the
    // cooperative cancel this relies on (see the contract above).
    watcher->waitForFinished();
    delete watcher;
    watcher = nullptr;
}

template <typename ResultT>
bool requestWatcherCancellation(QFutureWatcher<ResultT>* watcher,
                                const std::shared_ptr<std::atomic_bool>& token) {
    if (watcher == nullptr || watcher->isFinished() || !token) {
        return false;
    }
    token->store(true);
    return true;
}

bool projectCreateOperation(PartitionDiskInfo* disk, const PartitionOperation& operation) {
    const uint64_t create_size =
        operation.payload.value(QStringLiteral("size_bytes")).toString().toULongLong();
    const uint64_t create_start = operation.target.offset_bytes;
    auto region_it = std::ranges::find_if(disk->unallocated_regions, [&](const auto& region) {
        return create_start >= region.offset_bytes && create_size <= region.size_bytes &&
               create_start - region.offset_bytes <= region.size_bytes - create_size;
    });
    if (region_it == disk->unallocated_regions.end()) {
        return false;
    }
    const uint64_t region_end = saturatingAdd(region_it->offset_bytes, region_it->size_bytes);
    const uint64_t create_end = saturatingAdd(create_start, create_size);
    const auto disk_number = disk->disk_number;
    const uint64_t region_start = region_it->offset_bytes;
    disk->unallocated_regions.erase(region_it);
    if (create_start > region_start) {
        disk->unallocated_regions.append({.disk_number = disk_number,
                                          .offset_bytes = region_start,
                                          .size_bytes = create_start - region_start});
    }
    if (create_end < region_end) {
        disk->unallocated_regions.append({.disk_number = disk_number,
                                          .offset_bytes = create_end,
                                          .size_bytes = region_end - create_end});
    }
    return true;
}
}  // namespace

PartitionManagerController::PartitionManagerController(QObject* parent)
    : QObject(parent)
    , m_inventory_scanner([](const CancelCheck& should_cancel) {
        return StorageInventoryWorker::scanCurrentSystemResult(true, should_cancel);
    })
    , m_file_system_check_runner([](const PartitionFileSystemReadOnlyCheckRequest& request,
                                    const CancelCheck& should_cancel) {
        return PartitionFileSystemToolRunner::runReadOnlyCheck(request, should_cancel);
    }) {}

PartitionManagerController::~PartitionManagerController() {
    // JOIN the outstanding apply/check futures instead of detaching them, so
    // destructive disk work (elevated diskpart/format) cannot keep mutating the
    // disk after the controller and its panel are gone. cancel() first so the
    // in-flight op / elevated broker is interrupted and the join returns promptly.
    if (m_inventory_cancel) {
        m_inventory_cancel->store(true);
    }
    if (m_apply_cancel) {
        m_apply_cancel->store(true);
    }
    if (m_file_system_check_cancel) {
        m_file_system_check_cancel->store(true);
    }
    if (m_apply_executor) {
        m_apply_executor->cancel();
    }
    cancelJoinDeleteWatcher(m_inventory_watcher, this);
    cancelJoinDeleteWatcher(m_apply_watcher, this);
    // The apply run lambda has released its executor shared_ptr copy by now, so
    // the executor is destroyed here on the GUI thread.
    m_apply_executor.reset();
    cancelJoinDeleteWatcher(m_file_system_check_watcher, this);
}

const PartitionInventory& PartitionManagerController::inventory() const noexcept {
    return m_inventory;
}

const PartitionOperationQueue& PartitionManagerController::queue() const noexcept {
    return m_queue;
}

PartitionManagerState PartitionManagerController::state() const noexcept {
    return m_state;
}

bool PartitionManagerController::interactionBlocked() const noexcept {
    const bool watcher_active = m_inventory_watcher != nullptr || m_apply_watcher != nullptr ||
                                m_file_system_check_watcher != nullptr;
    const bool phase_active = m_state == PartitionManagerState::RefreshingInventory ||
                              m_state == PartitionManagerState::PlanningOperation ||
                              m_state == PartitionManagerState::PreflightRunning ||
                              m_state == PartitionManagerState::AwaitingElevation ||
                              m_state == PartitionManagerState::Applying ||
                              m_state == PartitionManagerState::Verifying;
    return watcher_active || phase_active;
}

bool PartitionManagerController::canCancel() const noexcept {
    return (m_inventory_watcher != nullptr && !m_inventory_watcher->isFinished()) ||
           (m_apply_watcher != nullptr && !m_apply_watcher->isFinished()) ||
           (m_file_system_check_watcher != nullptr && !m_file_system_check_watcher->isFinished());
}

PartitionQueueReviewToken PartitionManagerController::queueReviewToken() const {
    return {.revision = m_queue.revision(),
            .fingerprint = m_queue.fingerprint(),
            .layout_hash = m_queue.baseLayoutHash()};
}

PartitionManagerState PartitionManagerController::inventoryReadyState(
    const PartitionInventory& inventory) {
    if (inventory.isEmpty() || inventory.layout_hash.isEmpty()) {
        return PartitionManagerState::Failed;
    }
    return inventory.hasPartitionEnumerationFailure() || !inventory.warnings.isEmpty()
               ? PartitionManagerState::ReadyPartial
               : PartitionManagerState::Ready;
}

QString PartitionManagerController::inventoryStatusMessage(const PartitionInventory& inventory) {
    if (inventory.isEmpty()) {
        const QString detail =
            inventory.warnings.isEmpty()
                ? QString()
                : QStringLiteral(": %1").arg(inventory.warnings.join(QStringLiteral("; ")));
        return QStringLiteral("Partition Manager: inventory FAILED -- no disks were returned%1")
            .arg(detail);
    }
    if (inventory.layout_hash.isEmpty()) {
        return QStringLiteral("Partition Manager: inventory FAILED -- layout identity missing");
    }
    QStringList failed;
    for (const auto& disk : inventory.disks) {
        if (disk.partition_enumeration_failed) {
            failed.append(QString::number(disk.disk_number.value()));
        }
    }
    if (failed.isEmpty() && inventory.warnings.isEmpty()) {
        return QStringLiteral("Partition Manager: inventory ready");
    }
    if (!failed.isEmpty()) {
        return QStringLiteral(
                   "Partition Manager: inventory INCOMPLETE -- partition enumeration "
                   "failed for disk(s) %1; operations on them are refused until a "
                   "refresh succeeds")
            .arg(failed.join(QStringLiteral(", ")));
    }
    return QStringLiteral("Partition Manager: inventory PARTIAL -- %1")
        .arg(inventory.warnings.join(QStringLiteral("; ")));
}

void PartitionManagerController::refreshInventory() {
    if (interactionBlocked()) {
        Q_EMIT statusMessage(
            QStringLiteral("Partition Manager: wait for the active operation before refreshing"),
            sak::kTimerStatusDefaultMs);
        return;
    }
    startInventoryScan(InventoryScanPurpose::Refresh);
}

bool PartitionManagerController::queueOperation(PartitionOperationType type,
                                                const PartitionTarget& target,
                                                const QJsonObject& payload) {
    return queueOperations(
        {PartitionOperationRequest{.type = type, .target = target, .payload = payload}});
}

bool PartitionManagerController::queueOperations(
    const QVector<PartitionOperationRequest>& requests) {
    if (rejectQueueMutationIfBusy()) {
        return false;
    }
    if (requests.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("No partition operations were supplied"),
                             sak::kTimerStatusDefaultMs);
        return false;
    }
    setState(PartitionManagerState::PlanningOperation);
    OperationPreview batch;
    batch.before_layout_hash = m_inventory.layout_hash;
    PartitionInventory projected_inventory = m_inventory;
    for (const auto& request : requests) {
        auto operation =
            PartitionOperationPlanner::makeOperation(request.type, request.target, request.payload);
        bindDiskIdentity(m_inventory, &operation);
        bindRawPayloadTargetIdentity(m_inventory, &operation);
        bindBackupVolumeIdentity(&operation);
        const auto preview = m_planner.previewOperation(projected_inventory, operation);
        batch.operations.append(preview.operations);
        batch.blockers.append(preview.blockers);
        batch.warnings.append(preview.warnings);
        if (!preview.canApply() || preview.operations.isEmpty() ||
            !projectQueuedOperation(&projected_inventory, preview.operations.constLast())) {
            setState(stableState());
            Q_EMIT statusMessage(
                QStringLiteral("Partition operation batch failed sequential safety validation"),
                sak::kTimerStatusDefaultMs);
            return false;
        }
    }
    if (!m_queue.addPreview(batch)) {
        setState(stableState());
        Q_EMIT statusMessage(QStringLiteral("Partition queue capacity reached; operation refused"),
                             sak::kTimerStatusDefaultMs);
        return false;
    }
    setState(PartitionManagerState::QueueDirty);
    emitQueueChanged();
    const QString msg = batch.canApply()
                            ? QStringLiteral("Partition operation queued")
                            : QStringLiteral("Partition operation queued with blockers");
    Q_EMIT statusMessage(msg, sak::kTimerStatusDefaultMs);
    return true;
}

void PartitionManagerController::undo() {
    if (rejectQueueMutationIfBusy()) {
        return;
    }
    if (m_queue.undo()) {
        emitQueueChanged();
        setState(stableState());
    }
}

void PartitionManagerController::redo() {
    if (rejectQueueMutationIfBusy()) {
        return;
    }
    if (m_queue.redo()) {
        emitQueueChanged();
        setState(PartitionManagerState::QueueDirty);
    }
}

void PartitionManagerController::discardQueue() {
    if (rejectQueueMutationIfBusy()) {
        return;
    }
    m_queue.discard();
    emitQueueChanged();
    setState(stableState());
}

bool PartitionManagerController::applyQueue(const PartitionQueueReviewToken& reviewed,
                                            bool dry_run,
                                            bool use_elevation) {
    if (interactionBlocked()) {
        Q_EMIT statusMessage(QStringLiteral("Partition operation already running"),
                             sak::kTimerStatusDefaultMs);
        return false;
    }
    if (m_queue.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Partition queue is empty"),
                             sak::kTimerStatusDefaultMs);
        return false;
    }
    if (!reviewTokenMatches(reviewed)) {
        Q_EMIT statusMessage(QStringLiteral("Partition queue changed since review; review again"),
                             sak::kTimerStatusDefaultMs);
        return false;
    }
    m_pending_apply_request = PendingApplyRequest{.dry_run = dry_run,
                                                  .use_elevation = use_elevation,
                                                  .reviewed = reviewed};
    startInventoryScan(InventoryScanPurpose::ApplyPreflight);
    return true;
}

bool PartitionManagerController::rejectQueueMutationIfBusy() {
    if (!interactionBlocked()) {
        return false;
    }
    Q_EMIT statusMessage(QStringLiteral("Wait for the active inventory scan or operation before "
                                        "changing the partition queue"),
                         sak::kTimerStatusDefaultMs);
    return true;
}

PartitionManagerState PartitionManagerController::stableState() const {
    if (m_inventory.isEmpty()) {
        return PartitionManagerState::Failed;
    }
    if (!m_queue.isEmpty()) {
        return PartitionManagerState::QueueDirty;
    }
    return inventoryReadyState(m_inventory);
}

bool PartitionManagerController::reviewTokenMatches(
    const PartitionQueueReviewToken& reviewed) const {
    return reviewed.revision == m_queue.revision() &&
           reviewed.fingerprint == m_queue.fingerprint() &&
           reviewed.layout_hash == m_queue.baseLayoutHash();
}

void PartitionManagerController::bindDiskIdentity(const PartitionInventory& inventory,
                                                  PartitionOperation* operation) {
    const auto* disk = PartitionSafetyValidator::findDisk(inventory, operation->target.disk_number);
    if (disk == nullptr) {
        return;
    }
    operation->target.disk_unique_id = disk->unique_id.trimmed();
    operation->target.disk_serial_number = disk->serial_number.trimmed();
    operation->target.disk_size_bytes = disk->size_bytes;
}

void PartitionManagerController::bindRawPayloadTargetIdentity(const PartitionInventory& inventory,
                                                              PartitionOperation* operation) {
    const QString target_path =
        operation->payload.value(QStringLiteral("target_path")).toString().trimmed();
    const auto disk_number = rawDevicePhysicalDriveNumber(target_path);
    if (!disk_number) {
        return;
    }
    const auto* disk = PartitionSafetyValidator::findDisk(inventory, DiskNumber{*disk_number});
    if (disk == nullptr) {
        return;
    }
    operation->payload[QStringLiteral("target_disk_unique_id")] = disk->unique_id.trimmed();
    operation->payload[QStringLiteral("target_disk_serial_number")] = disk->serial_number.trimmed();
    operation->payload[QStringLiteral("target_disk_size_bytes")] =
        QString::number(disk->size_bytes);
}

void PartitionManagerController::bindBackupVolumeIdentity(PartitionOperation* operation) {
    const QString backup_directory =
        operation->payload.value(QStringLiteral("backup_directory")).toString().trimmed();
    if (backup_directory.isEmpty()) {
        return;
    }
    const auto identity = resolvePartitionPathVolumeIdentity(backup_directory);
    operation->payload[QStringLiteral("backup_volume_guid")] = identity.volume_guid;
    operation->payload[QStringLiteral("backup_volume_identity_error")] = identity.error;
}

QString PartitionManagerController::freshDiskIdentityBlocker(const PartitionInventory& inventory,
                                                             const PartitionOperation& operation) {
    if (operation.target.disk_unique_id.isEmpty()) {
        return QStringLiteral("Target disk has no stable Get-Disk UniqueId");
    }
    const auto* disk = PartitionSafetyValidator::findDisk(inventory, operation.target.disk_number);
    if (disk == nullptr || disk->unique_id.trimmed() != operation.target.disk_unique_id ||
        disk->serial_number.trimmed() != operation.target.disk_serial_number ||
        disk->size_bytes != operation.target.disk_size_bytes) {
        return QStringLiteral("Target disk identity changed since the operation was queued");
    }
    return {};
}

QString PartitionManagerController::freshBackupIdentityBlocker(
    const PartitionOperation& operation) {
    const QString backup_directory =
        operation.payload.value(QStringLiteral("backup_directory")).toString().trimmed();
    if (backup_directory.isEmpty()) {
        return {};
    }
    const auto current_backup = resolvePartitionPathVolumeIdentity(backup_directory);
    const QString reviewed_backup =
        operation.payload.value(QStringLiteral("backup_volume_guid")).toString();
    if (!current_backup.valid() || reviewed_backup.isEmpty() ||
        current_backup.volume_guid != reviewed_backup) {
        return QStringLiteral("Backup directory volume identity changed or could not be resolved");
    }
    return {};
}

QString PartitionManagerController::freshRawPayloadTargetIdentityBlocker(
    const PartitionInventory& inventory, const PartitionOperation& operation) {
    const QString target_path =
        operation.payload.value(QStringLiteral("target_path")).toString().trimmed();
    const auto disk_number = rawDevicePhysicalDriveNumber(target_path);
    if (!disk_number) {
        return {};
    }
    const QString unique_id =
        operation.payload.value(QStringLiteral("target_disk_unique_id")).toString().trimmed();
    const QString serial =
        operation.payload.value(QStringLiteral("target_disk_serial_number")).toString().trimmed();
    const uint64_t size =
        operation.payload.value(QStringLiteral("target_disk_size_bytes")).toString().toULongLong();
    const auto* disk = PartitionSafetyValidator::findDisk(inventory, DiskNumber{*disk_number});
    if (unique_id.isEmpty() || size == 0 || disk == nullptr ||
        disk->unique_id.trimmed() != unique_id || disk->serial_number.trimmed() != serial ||
        disk->size_bytes != size) {
        return QStringLiteral("Raw payload target disk identity changed or is unavailable");
    }
    return {};
}

bool PartitionManagerController::projectQueuedOperation(PartitionInventory* inventory,
                                                        const PartitionOperation& operation) {
    auto disk_it = std::ranges::find_if(inventory->disks, [&operation](const auto& disk) {
        return disk.disk_number == operation.target.disk_number;
    });
    if (disk_it == inventory->disks.end()) {
        return false;
    }
    if (operation.type == PartitionOperationType::InitializeDisk) {
        disk_it->partition_style =
            operation.payload.value(QStringLiteral("target_style")).toString();
        return true;
    }
    if (operation.type == PartitionOperationType::DeleteAllPartitions) {
        if (disk_it->size_bytes <= kProjectedPartitionAlignmentBytes) {
            return false;
        }
        disk_it->partitions.clear();
        disk_it->unallocated_regions = {
            {.disk_number = disk_it->disk_number,
             .offset_bytes = kProjectedPartitionAlignmentBytes,
             .size_bytes = disk_it->size_bytes - kProjectedPartitionAlignmentBytes}};
        return true;
    }
    if (operation.type != PartitionOperationType::Create) {
        return true;
    }
    return projectCreateOperation(&(*disk_it), operation);
}

bool PartitionManagerController::inventoryIsAuthoritative(const PartitionInventory& inventory) {
    return !inventory.isEmpty() && !inventory.hasPartitionEnumerationFailure() &&
           inventory.warnings.isEmpty() && !inventory.layout_hash.isEmpty();
}

QString PartitionManagerController::inventoryFailureMessage(const PartitionInventory& inventory) {
    if (inventory.isEmpty()) {
        return inventory.warnings.isEmpty() ? QStringLiteral("inventory returned no disks")
                                            : inventory.warnings.join(QStringLiteral("; "));
    }
    if (inventory.hasPartitionEnumerationFailure()) {
        return QStringLiteral("partition enumeration was incomplete");
    }
    if (!inventory.warnings.isEmpty()) {
        return inventory.warnings.join(QStringLiteral("; "));
    }
    if (inventory.layout_hash.isEmpty()) {
        return QStringLiteral("inventory returned no layout identity");
    }
    return {};
}

bool PartitionManagerController::revalidateQueuedOperations(
    const PartitionInventory& inventory, QVector<PartitionOperation>* operations) {
    operations->clear();
    PartitionInventory projected_inventory = inventory;
    bool allowed = true;
    for (auto operation : m_queue.operations()) {
        operation.blockers.clear();
        operation.warnings.clear();
        if (const QString blocker = freshDiskIdentityBlocker(inventory, operation);
            !blocker.isEmpty()) {
            operation.blockers.append(blocker);
        }
        if (const QString blocker = freshBackupIdentityBlocker(operation); !blocker.isEmpty()) {
            operation.blockers.append(blocker);
        }
        if (const QString blocker = freshRawPayloadTargetIdentityBlocker(inventory, operation);
            !blocker.isEmpty()) {
            operation.blockers.append(blocker);
        }
        // Device identity comes from the real scan above. Layout-dependent validation must
        // instead include the earlier queued changes, exactly as it did during queue admission.
        const auto preview = m_planner.previewOperation(projected_inventory, operation);
        const bool projection_valid = preview.canApply() && !preview.operations.isEmpty() &&
                                      projectQueuedOperation(&projected_inventory,
                                                             preview.operations.constLast());
        allowed = allowed && projection_valid;
        operations->append(preview.operations);
    }
    if (!m_queue.replaceOperations(*operations)) {
        return false;
    }
    return allowed && m_queue.canApply(inventory.layout_hash);
}

void PartitionManagerController::startInventoryScan(InventoryScanPurpose purpose) {
    auto cancel_requested = std::make_shared<std::atomic_bool>(false);
    auto* watcher = new QFutureWatcher<StorageInventoryScanResult>(this);
    const auto scanner = m_inventory_scanner;
    m_inventory_cancel = cancel_requested;
    m_inventory_scan_purpose = purpose;
    m_inventory_watcher = watcher;
    connect(watcher,
            &QFutureWatcher<StorageInventoryScanResult>::finished,
            this,
            [this, watcher]() { finishInventoryScan(watcher); });
    watcher->setFuture(QtConcurrent::run([scanner, cancel_requested]() {
        const CancelCheck should_cancel = [cancel_requested]() {
            return cancel_requested->load();
        };
        return scanner(should_cancel);
    }));

    const auto next_state = purpose == InventoryScanPurpose::Refresh
                                ? PartitionManagerState::RefreshingInventory
                                : (purpose == InventoryScanPurpose::ApplyPreflight
                                       ? PartitionManagerState::PreflightRunning
                                       : PartitionManagerState::Verifying);
    setState(next_state);
    Q_EMIT progressUpdate(0, kPercentMax);
    Q_EMIT statusMessage(purpose == InventoryScanPurpose::Refresh
                             ? QStringLiteral("Partition Manager: refreshing storage inventory")
                             : (purpose == InventoryScanPurpose::ApplyPreflight
                                    ? QStringLiteral("Validating queued operations against fresh "
                                                     "storage inventory")
                                    : QStringLiteral("Verifying storage after partition apply")),
                         sak::kTimerStatusDefaultMs);
}

void PartitionManagerController::finishInventoryScan(
    QFutureWatcher<StorageInventoryScanResult>* watcher) {
    if (watcher == nullptr || watcher != m_inventory_watcher) {
        return;
    }
    auto scan_result = watcher->result();
    const auto purpose = m_inventory_scan_purpose;
    std::optional<PartitionManagerState> terminal_state;
    if (purpose == InventoryScanPurpose::Refresh) {
        terminal_state = finishRefresh(std::move(scan_result.inventory), scan_result.cancelled);
    } else if (purpose == InventoryScanPurpose::ApplyPreflight) {
        terminal_state = finishApplyPreflight(std::move(scan_result.inventory),
                                              scan_result.cancelled);
    } else if (purpose == InventoryScanPurpose::ApplyVerification) {
        terminal_state = finishApplyVerification(std::move(scan_result.inventory),
                                                 scan_result.cancelled);
    }

    QObject::disconnect(watcher, nullptr, this, nullptr);
    m_inventory_watcher = nullptr;
    m_inventory_scan_purpose = InventoryScanPurpose::None;
    m_inventory_cancel.reset();
    watcher->deleteLater();
    if (terminal_state) {
        setState(*terminal_state);
    }
}

PartitionManagerState PartitionManagerController::finishRefresh(PartitionInventory inventory,
                                                                bool cancelled) {
    m_inventory = std::move(inventory);
    publishInventoryDiagnostics(m_inventory);
    if (!cancelled && inventoryIsAuthoritative(m_inventory)) {
        m_queue.setBaseLayoutHash(m_inventory.layout_hash);
    }
    Q_EMIT inventoryChanged(m_inventory);
    emitQueueChanged();
    Q_EMIT progressUpdate(kPercentMax, kPercentMax);
    Q_EMIT statusMessage(cancelled ? QStringLiteral("Storage inventory refresh cancelled")
                                   : inventoryStatusMessage(m_inventory),
                         sak::kTimerStatusDefaultMs);
    return cancelled ? PartitionManagerState::Cancelled : stableState();
}

std::optional<PartitionManagerState> PartitionManagerController::finishApplyPreflight(
    PartitionInventory inventory, bool cancelled) {
    const auto request = m_pending_apply_request;
    m_pending_apply_request.reset();
    m_inventory = std::move(inventory);
    publishInventoryDiagnostics(m_inventory);
    if (cancelled || !request.has_value()) {
        Q_EMIT inventoryChanged(m_inventory);
        emitQueueChanged();
        Q_EMIT progressUpdate(kPercentMax, kPercentMax);
        Q_EMIT statusMessage(QStringLiteral("Partition apply preflight cancelled"),
                             sak::kTimerStatusDefaultMs);
        return PartitionManagerState::Cancelled;
    }
    if (!inventoryIsAuthoritative(m_inventory)) {
        Q_EMIT inventoryChanged(m_inventory);
        emitQueueChanged();
        Q_EMIT progressUpdate(kPercentMax, kPercentMax);
        Q_EMIT statusMessage(QStringLiteral("Partition apply preflight failed: %1")
                                 .arg(inventoryFailureMessage(m_inventory)),
                             sak::kTimerStatusDefaultMs);
        return PartitionManagerState::Failed;
    }
    if (!reviewTokenMatches(request->reviewed) ||
        m_queue.baseLayoutHash() != m_inventory.layout_hash) {
        Q_EMIT inventoryChanged(m_inventory);
        emitQueueChanged();
        Q_EMIT progressUpdate(kPercentMax, kPercentMax);
        Q_EMIT statusMessage(
            QStringLiteral("Storage changed since planning; refresh and re-review before applying"),
            sak::kTimerStatusDefaultMs);
        return PartitionManagerState::QueueDirty;
    }

    QVector<PartitionOperation> operations;
    if (!revalidateQueuedOperations(m_inventory, &operations)) {
        Q_EMIT inventoryChanged(m_inventory);
        emitQueueChanged();
        Q_EMIT progressUpdate(kPercentMax, kPercentMax);
        Q_EMIT statusMessage(QStringLiteral(
                                 "Partition queue failed fresh safety validation; review blockers"),
                             sak::kTimerStatusDefaultMs);
        return PartitionManagerState::QueueDirty;
    }
    m_apply_before_inventory = m_inventory;
    Q_EMIT inventoryChanged(m_inventory);
    emitQueueChanged();
    Q_EMIT progressUpdate(kPercentMax, kPercentMax);
    Q_EMIT statusMessage(QStringLiteral("Partition apply preflight complete"),
                         sak::kTimerStatusDefaultMs);
    startApplyExecution(operations, *request);
    return std::nullopt;
}

void PartitionManagerController::startApplyExecution(const QVector<PartitionOperation>& operations,
                                                     const PendingApplyRequest& request) {
    const bool use_elevation = request.use_elevation && !request.dry_run;
    auto cancel_requested = std::make_shared<std::atomic_bool>(false);
    auto executor = m_apply_executor_for_testing ? std::shared_ptr<PartitionExecutor>{}
                                                 : makeAsyncExecutor();
    if (executor) {
        connect(executor.get(),
                &PartitionExecutor::progressUpdated,
                this,
                [this](int percent, const QString& status) {
                    Q_EMIT progressUpdate(percent, kPercentMax);
                    Q_EMIT statusMessage(status, sak::kTimerStatusDefaultMs);
                });
        connect(executor.get(),
                &PartitionExecutor::logMessage,
                this,
                &PartitionManagerController::logOutput);
    }

    auto* watcher = new QFutureWatcher<PartitionExecutionResult>(this);
    const auto test_executor = m_apply_executor_for_testing;
    m_apply_cancel = cancel_requested;
    m_apply_executor = executor;
    m_active_apply_request = request;
    m_apply_watcher = watcher;
    connect(watcher, &QFutureWatcher<PartitionExecutionResult>::finished, this, [this, watcher]() {
        finishApplyExecution(watcher);
    });
    watcher->setFuture(QtConcurrent::run(
        [executor, test_executor, operations, request, use_elevation, cancel_requested]() {
            const CancelCheck should_cancel = [cancel_requested]() {
                return cancel_requested->load();
            };
            return test_executor
                       ? test_executor(operations, request.dry_run, use_elevation, should_cancel)
                       : executor->execute(operations, request.dry_run, use_elevation);
        }));

    if (use_elevation) {
        setState(PartitionManagerState::AwaitingElevation);
    }
    if (!cancel_requested->load()) {
        setState(PartitionManagerState::Applying);
    }
}

void PartitionManagerController::finishApplyExecution(
    QFutureWatcher<PartitionExecutionResult>* watcher) {
    if (watcher == nullptr || watcher != m_apply_watcher || !m_active_apply_request) {
        return;
    }
    auto result = watcher->result();
    const auto request = *m_active_apply_request;
    result.dry_run = request.dry_run;

    if (request.dry_run) {
        const auto terminal_state = publishExecutionResult(std::move(result),
                                                           m_apply_before_inventory);
        QObject::disconnect(watcher, nullptr, this, nullptr);
        m_apply_watcher = nullptr;
        m_apply_cancel.reset();
        m_active_apply_request.reset();
        m_apply_executor.reset();
        watcher->deleteLater();
        setState(terminal_state);
        return;
    }
    m_pending_execution_result = std::move(result);
    startInventoryScan(InventoryScanPurpose::ApplyVerification);
    QObject::disconnect(watcher, nullptr, this, nullptr);
    m_apply_watcher = nullptr;
    m_apply_cancel.reset();
    m_active_apply_request.reset();
    m_apply_executor.reset();
    watcher->deleteLater();
}

PartitionManagerState PartitionManagerController::finishApplyVerification(
    PartitionInventory inventory, bool cancelled) {
    if (!m_pending_execution_result) {
        Q_EMIT progressUpdate(kPercentMax, kPercentMax);
        Q_EMIT statusMessage(QStringLiteral("Partition verification result was unavailable"),
                             sak::kTimerStatusDefaultMs);
        return PartitionManagerState::Failed;
    }
    auto result = std::move(*m_pending_execution_result);
    m_pending_execution_result.reset();
    publishInventoryDiagnostics(inventory);
    const bool verified = !cancelled && inventoryIsAuthoritative(inventory);
    if (!verified) {
        result.success = false;
        result.cancelled = result.cancelled || cancelled;
        const QString reason = cancelled ? QStringLiteral("verification scan cancelled")
                                         : inventoryFailureMessage(inventory);
        const QString failure = QStringLiteral("Post-apply verification failed: %1").arg(reason);
        result.message = result.message.isEmpty()
                             ? failure
                             : QStringLiteral("%1; %2").arg(result.message, failure);
    }
    if (result.success) {
        m_queue.discard();
    }
    return publishExecutionResult(std::move(result), inventory);
}

PartitionManagerState PartitionManagerController::publishExecutionResult(
    PartitionExecutionResult result, const PartitionInventory& after_inventory) {
    m_inventory = after_inventory;
    const PartitionReportGenerator report_generator;
    result.report_html =
        report_generator.generateHtml(m_apply_before_inventory, m_inventory, result);
    result.report_json =
        report_generator.generateJson(m_apply_before_inventory, m_inventory, result);
    Q_EMIT inventoryChanged(m_inventory);
    emitQueueChanged();
    Q_EMIT progressUpdate(kPercentMax, kPercentMax);
    Q_EMIT statusMessage(result.message.isEmpty()
                             ? (result.success ? QStringLiteral("Partition operation complete")
                                               : QStringLiteral("Partition operation failed"))
                             : result.message,
                         sak::kTimerStatusDefaultMs);
    Q_EMIT executionFinished(result);
    return result.cancelled ? PartitionManagerState::Cancelled
                            : (result.success ? stableState() : PartitionManagerState::Failed);
}

void PartitionManagerController::publishInventoryDiagnostics(const PartitionInventory& inventory) {
    logInfo("Partition Manager inventory scan returned {} disk(s), {} warning(s)",
            inventory.disks.size(),
            inventory.warnings.size());
    for (const auto& warning : inventory.warnings) {
        logWarning("Partition Manager inventory warning: {}", warning.toStdString());
        Q_EMIT logOutput(warning);
    }
}

void PartitionManagerController::cancel() {
    if (!canCancel()) {
        Q_EMIT statusMessage(QStringLiteral("No cancellable partition operation is running"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    requestWatcherCancellation(m_inventory_watcher, m_inventory_cancel);
    const bool apply_cancelled = requestWatcherCancellation(m_apply_watcher, m_apply_cancel);
    requestWatcherCancellation(m_file_system_check_watcher, m_file_system_check_cancel);
    if (apply_cancelled && m_apply_executor) {
        m_apply_executor->cancel();
    }
    Q_EMIT statusMessage(QStringLiteral("Partition operation or scan cancellation requested"),
                         sak::kTimerStatusDefaultMs);
}

void PartitionManagerController::runReadOnlyFileSystemCheck(const QString& file_system,
                                                            const QString& target_path) {
    if (interactionBlocked()) {
        Q_EMIT statusMessage(QStringLiteral("Wait for the active operation before starting a "
                                            "filesystem check"),
                             sak::kTimerStatusDefaultMs);
        return;
    }

    const QString manifest_path = runtimeFilesystemManifestPath();
    const QString tools_root = QFileInfo(manifest_path).absolutePath();
    const PartitionFileSystemReadOnlyCheckRequest request{.manifest_path = manifest_path,
                                                          .tools_root = tools_root,
                                                          .file_system = file_system,
                                                          .target_path = target_path,
                                                          .timeout_ms = 0};
    auto cancel_requested = std::make_shared<std::atomic_bool>(false);
    auto* watcher = new QFutureWatcher<PartitionFileSystemToolRunResult>(this);
    const auto runner = m_file_system_check_runner;
    m_file_system_check_cancel = cancel_requested;
    m_file_system_check_watcher = watcher;
    connect(watcher,
            &QFutureWatcher<PartitionFileSystemToolRunResult>::finished,
            this,
            [this, watcher]() {
                auto result = watcher->result();
                if (m_file_system_check_watcher != watcher) {
                    return;
                }
                if (!result.std_out.trimmed().isEmpty()) {
                    Q_EMIT logOutput(result.std_out.trimmed());
                }
                if (!result.std_err.trimmed().isEmpty()) {
                    Q_EMIT logOutput(result.std_err.trimmed());
                }
                if (result.cancelled) {
                    Q_EMIT statusMessage(QStringLiteral("Filesystem check cancelled"),
                                         sak::kTimerStatusDefaultMs);
                } else if (result.blocked) {
                    Q_EMIT statusMessage(QStringLiteral("Filesystem check blocked: %1")
                                             .arg(result.blockers.join(QStringLiteral("; "))),
                                         sak::kTimerStatusDefaultMs);
                } else if (result.success) {
                    Q_EMIT statusMessage(QStringLiteral("Filesystem check complete"),
                                         sak::kTimerStatusDefaultMs);
                } else {
                    Q_EMIT statusMessage(QStringLiteral("Filesystem check failed with exit code %1")
                                             .arg(result.exit_code),
                                         sak::kTimerStatusDefaultMs);
                }
                Q_EMIT fileSystemCheckFinished(result);
                const auto terminal_state = stableState();
                QObject::disconnect(watcher, nullptr, this, nullptr);
                m_file_system_check_watcher = nullptr;
                m_file_system_check_cancel.reset();
                watcher->deleteLater();
                setState(terminal_state);
            });
    watcher->setFuture(QtConcurrent::run([request, runner, cancel_requested]() {
        const CancelCheck should_cancel = [cancel_requested]() {
            return cancel_requested->load();
        };
        return runner(request, should_cancel);
    }));
    setState(PartitionManagerState::PreflightRunning);
    Q_EMIT statusMessage(QStringLiteral("Running read-only %1 filesystem check").arg(file_system),
                         0);
}

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
void PartitionManagerController::setTestInventory(const PartitionInventory& inventory) {
    m_inventory = inventory;
    m_queue.setBaseLayoutHash(m_inventory.layout_hash);
    Q_EMIT inventoryChanged(m_inventory);
    emitQueueChanged();
}

void PartitionManagerController::setTestState(PartitionManagerState state) {
    setState(state);
}

void PartitionManagerController::setInventoryScannerForTesting(InventoryScanner scanner) {
    if (!interactionBlocked() && scanner) {
        m_inventory_scanner = std::move(scanner);
    }
}

void PartitionManagerController::setApplyExecutorForTesting(ApplyExecutor executor) {
    if (!interactionBlocked()) {
        m_apply_executor_for_testing = std::move(executor);
    }
}

void PartitionManagerController::setFileSystemCheckRunnerForTesting(FileSystemCheckRunner runner) {
    if (!interactionBlocked() && runner) {
        m_file_system_check_runner = std::move(runner);
    }
}
#endif

void PartitionManagerController::setState(PartitionManagerState state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(state);
}

void PartitionManagerController::emitQueueChanged() {
    Q_EMIT queueChanged(m_queue.operations());
}

}  // namespace sak
