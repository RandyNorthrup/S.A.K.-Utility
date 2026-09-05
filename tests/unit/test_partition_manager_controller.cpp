// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/partition_manager_controller.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace sak {

// Dedicated controller tests inject the apply executor function. These inert definitions keep
// this target independent of PartitionScriptBuilder's full raw-filesystem writer graph; reaching
// them is a fail-closed test setup error, not a fallback production path.
PartitionExecutor::PartitionExecutor(QObject* parent) : QObject(parent) {}

PartitionExecutionResult PartitionExecutor::execute(const QVector<PartitionOperation>&,
                                                    bool dry_run,
                                                    bool) {
    PartitionExecutionResult result;
    result.dry_run = dry_run;
    result.message = QStringLiteral("Controller test did not inject an apply executor");
    return result;
}

void PartitionExecutor::cancel() {
    m_cancelled.store(true);
}

}  // namespace sak

namespace {

constexpr uint32_t kTestDiskNumber = 7;
constexpr uint64_t kTestDiskBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kTestAlignmentBytes = 1024ULL * 1024ULL;
constexpr int kAsyncTimeoutMs = 5000;
constexpr qsizetype kOverflowPreviewSize = 2;
constexpr int kPreflightAndVerificationScanCount = 2;
constexpr qsizetype kTwoQueuedOperations = 2;
constexpr uint32_t kRawTargetDiskNumber = 8;
constexpr uint64_t kProjectedCreateBytes = 48ULL * 1024ULL * 1024ULL;

sak::PartitionInventory healthyInventory(const QString& layout_hash = QStringLiteral("layout-a")) {
    sak::PartitionDiskInfo disk;
    disk.disk_number = sak::DiskNumber{kTestDiskNumber};
    disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive7");
    disk.unique_id = QStringLiteral("test-unique-id");
    disk.model = QStringLiteral("Test disk");
    disk.serial_number = QStringLiteral("TEST-SERIAL");
    disk.partition_style = QStringLiteral("RAW");
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.reliability_query_succeeded = true;
    disk.size_bytes = kTestDiskBytes;

    sak::PartitionInventory inventory;
    inventory.disks.append(disk);
    inventory.captured_at = QDateTime::currentDateTime();
    inventory.layout_hash = layout_hash;
    return inventory;
}

sak::PartitionTarget testDiskTarget() {
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Disk;
    target.disk_number = sak::DiskNumber{kTestDiskNumber};
    return target;
}

sak::PartitionExecutionResult successfulResult(bool dry_run) {
    sak::PartitionExecutionResult result;
    result.batch_id = QStringLiteral("controller-test");
    result.success = true;
    result.dry_run = dry_run;
    result.message = QStringLiteral("execution complete");
    return result;
}

sak::StorageInventoryScanResult inventoryResult(sak::PartitionInventory inventory,
                                                bool cancelled = false) {
    return {.inventory = std::move(inventory), .cancelled = cancelled};
}

bool waitUntil(const std::function<bool()>& predicate, int timeout_ms = kAsyncTimeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return predicate();
}

bool statesAppearInOrder(const QVector<sak::PartitionManagerState>& states,
                         const QVector<sak::PartitionManagerState>& expected) {
    qsizetype next = 0;
    for (const auto state : states) {
        if (next < expected.size() && state == expected[next]) {
            ++next;
        }
    }
    return next == expected.size();
}

}  // namespace

class PartitionManagerControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void activePhasesBlockInteraction();
    void refreshDerivesReadyPartialAndFailedStates();
    void queueCapacityRefusalIsAtomic();
    void controllerBatchQueueRefusalIsAtomic();
    void quickPartitionBatchUsesSequentialProjectedInventory();
    void changedQueueReviewTokenRefusesApply();
    void undoLastOperationRestoresReadyState();
    void freshSameHashMetadataBlocksApply();
    void blankSerialDiskSwapWithSameLayoutTextBlocksApply();
    void rawPayloadTargetSwapBlocksApply();
    void missingStableDiskIdentityBlocksPreflight();
    void dryRunUsesFreshInventoryAndKeepsQueueDirty();
    void realApplyTransitionsAndCleansBeforeSignals();
    void failedVerificationRetainsQueueAndDowngradesReport();
    void inventoryRefreshCanBeCancelled();
    void finishedInventoryFutureCannotBeRetroactivelyCancelled();
    void filesystemCheckCanBeCancelled();
    void destructorCancelsAndJoinsInventoryScan();
    void storageInventoryPassesCancellationToProcessRunner();

private:
    static void queueInitialize(sak::PartitionManagerController* controller);
    void verifyFailedPostApplyScan(const sak::PartitionInventory& failed_inventory,
                                   const QString& expected_reason);
};

void PartitionManagerControllerTests::queueInitialize(sak::PartitionManagerController* controller) {
    QVERIFY(
        controller->queueOperation(sak::PartitionOperationType::InitializeDisk, testDiskTarget()));
    QCOMPARE(controller->state(), sak::PartitionManagerState::QueueDirty);
    QCOMPARE(controller->queue().operations().size(), 1);
}

void PartitionManagerControllerTests::activePhasesBlockInteraction() {
    sak::PartitionManagerController controller;
    for (const auto state : {sak::PartitionManagerState::RefreshingInventory,
                             sak::PartitionManagerState::PlanningOperation,
                             sak::PartitionManagerState::PreflightRunning,
                             sak::PartitionManagerState::AwaitingElevation,
                             sak::PartitionManagerState::Applying,
                             sak::PartitionManagerState::Verifying}) {
        controller.setTestState(state);
        QVERIFY(controller.interactionBlocked());
    }
    for (const auto state : {sak::PartitionManagerState::Idle,
                             sak::PartitionManagerState::Ready,
                             sak::PartitionManagerState::QueueDirty,
                             sak::PartitionManagerState::Failed,
                             sak::PartitionManagerState::Cancelled,
                             sak::PartitionManagerState::ReadyPartial}) {
        controller.setTestState(state);
        QVERIFY(!controller.interactionBlocked());
    }
}

void PartitionManagerControllerTests::refreshDerivesReadyPartialAndFailedStates() {
    const auto refresh_state = [](const sak::PartitionInventory& inventory) {
        sak::PartitionManagerController controller;
        controller.setInventoryScannerForTesting(
            [inventory](const sak::CancelCheck&) { return inventoryResult(inventory); });
        controller.refreshInventory();
        if (!waitUntil([&controller]() { return !controller.interactionBlocked(); })) {
            return std::optional<sak::PartitionManagerState>{};
        }
        return std::optional<sak::PartitionManagerState>{controller.state()};
    };

    const auto ready = refresh_state(healthyInventory());
    QVERIFY(ready.has_value());
    QCOMPARE(*ready, sak::PartitionManagerState::Ready);
    auto warning = healthyInventory();
    warning.warnings.append(QStringLiteral("metadata incomplete"));
    const auto partial = refresh_state(warning);
    QVERIFY(partial.has_value());
    QCOMPARE(*partial, sak::PartitionManagerState::ReadyPartial);
    const auto failed = refresh_state({});
    QVERIFY(failed.has_value());
    QCOMPARE(*failed, sak::PartitionManagerState::Failed);
    auto missing_layout = healthyInventory();
    missing_layout.layout_hash.clear();
    const auto missing_layout_state = refresh_state(missing_layout);
    QVERIFY(missing_layout_state.has_value());
    QCOMPARE(*missing_layout_state, sak::PartitionManagerState::Failed);
    sak::PartitionInventory empty_warning;
    empty_warning.warnings.append(QStringLiteral("PowerShell failed"));
    QVERIFY(sak::PartitionManagerController::inventoryStatusMessage(empty_warning)
                .contains(QStringLiteral("PowerShell failed")));
}

void PartitionManagerControllerTests::queueCapacityRefusalIsAtomic() {
    sak::PartitionOperationQueue queue;
    sak::OperationPreview initial;
    initial.before_layout_hash = QStringLiteral("capacity-base");
    initial.operations.resize(sak::PartitionOperationQueue::kMaxOperationCount - 1);
    initial.operations.last().id = QStringLiteral("capacity-sentinel");
    QVERIFY(queue.addPreview(initial));
    const auto revision = queue.revision();
    const auto fingerprint = queue.fingerprint();

    sak::OperationPreview overflow;
    overflow.before_layout_hash = QStringLiteral("must-not-replace-base");
    overflow.operations.resize(kOverflowPreviewSize);
    const auto before_size = queue.operations().size();
    QVERIFY(!queue.addPreview(overflow));
    QCOMPARE(queue.operations().size(), before_size);
    QCOMPARE(queue.operations().constLast().id, QStringLiteral("capacity-sentinel"));
    QCOMPARE(queue.baseLayoutHash(), QStringLiteral("capacity-base"));
    QCOMPARE(queue.revision(), revision);
    QCOMPARE(queue.fingerprint(), fingerprint);
}

void PartitionManagerControllerTests::controllerBatchQueueRefusalIsAtomic() {
    sak::PartitionManagerController controller;
    controller.setTestInventory(healthyInventory());
    QVector<sak::PartitionOperationRequest> requests;
    requests.resize(sak::PartitionOperationQueue::kMaxOperationCount + 1);
    for (auto& request : requests) {
        request.type = sak::PartitionOperationType::InitializeDisk;
        request.target = testDiskTarget();
    }
    QVERIFY(!controller.queueOperations(requests));
    QVERIFY(controller.queue().isEmpty());
    QCOMPARE(controller.state(), sak::PartitionManagerState::Ready);
}

void PartitionManagerControllerTests::quickPartitionBatchUsesSequentialProjectedInventory() {
    sak::PartitionManagerController controller;
    auto inventory = healthyInventory();
    inventory.disks[0].partition_style = QStringLiteral("RAW");
    inventory.disks[0].unallocated_regions = {
        {.disk_number = sak::DiskNumber{kTestDiskNumber},
         .offset_bytes = kTestAlignmentBytes,
         .size_bytes = inventory.disks[0].size_bytes - kTestAlignmentBytes}};
    controller.setTestInventory(inventory);

    sak::PartitionTarget create_target = testDiskTarget();
    create_target.kind = sak::PartitionTargetKind::Unallocated;
    create_target.offset_bytes = kTestAlignmentBytes;
    create_target.size_bytes = kProjectedCreateBytes;
    QJsonObject create_payload;
    create_payload[QStringLiteral("size_bytes")] = QString::number(kProjectedCreateBytes);
    create_payload[QStringLiteral("file_system")] = QStringLiteral("APFS");
    create_payload[QStringLiteral("filesystem_tool")] = true;
    create_payload[QStringLiteral("target_wipe_confirmed")] = true;
    const QVector<sak::PartitionOperationRequest> requests = {
        {.type = sak::PartitionOperationType::InitializeDisk,
         .target = testDiskTarget(),
         .payload = QJsonObject{{QStringLiteral("target_style"), QStringLiteral("GPT")}}},
        {.type = sak::PartitionOperationType::Create,
         .target = create_target,
         .payload = create_payload}};
    QVERIFY(controller.queueOperations(requests));
    QCOMPARE(controller.queue().operations().size(), kTwoQueuedOperations);

    controller.setInventoryScannerForTesting(
        [inventory](const sak::CancelCheck&) { return inventoryResult(inventory); });
    controller.setApplyExecutorForTesting([](const QVector<sak::PartitionOperation>& operations,
                                             bool dry_run,
                                             bool,
                                             const sak::CancelCheck&) {
        auto result = successfulResult(dry_run);
        result.success =
            operations.size() == kTwoQueuedOperations &&
            operations.constFirst().type == sak::PartitionOperationType::InitializeDisk &&
            operations.constLast().type == sak::PartitionOperationType::Create &&
            operations.constFirst().blockers.isEmpty() && operations.constLast().blockers.isEmpty();
        return result;
    });
    std::optional<sak::PartitionExecutionResult> delivered;
    connect(&controller,
            &sak::PartitionManagerController::executionFinished,
            this,
            [&delivered](const auto& result) { delivered = result; });

    QVERIFY(controller.applyQueue(controller.queueReviewToken(), true, false));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QVERIFY2(delivered.has_value(), qPrintable(controller.queue().blockers().join(';')));
    QVERIFY(delivered->success);
    QVERIFY(delivered->dry_run);
    QCOMPARE(controller.state(), sak::PartitionManagerState::QueueDirty);
    QCOMPARE(controller.queue().operations().size(), kTwoQueuedOperations);
}

void PartitionManagerControllerTests::changedQueueReviewTokenRefusesApply() {
    sak::PartitionManagerController controller;
    std::atomic_int scans{0};
    controller.setTestInventory(healthyInventory());
    controller.setInventoryScannerForTesting([&scans](const sak::CancelCheck&) {
        ++scans;
        return inventoryResult(healthyInventory());
    });
    queueInitialize(&controller);
    const auto reviewed = controller.queueReviewToken();
    auto wrong_revision = reviewed;
    ++wrong_revision.revision;
    QVERIFY(!controller.applyQueue(wrong_revision, true, false));
    QCOMPARE(scans.load(), 0);
    QVERIFY(
        controller.queueOperation(sak::PartitionOperationType::InitializeDisk, testDiskTarget()));
    QVERIFY(!controller.applyQueue(reviewed, true, false));
    QCOMPARE(scans.load(), 0);
    QCOMPARE(controller.queue().operations().size(), kTwoQueuedOperations);
}

void PartitionManagerControllerTests::undoLastOperationRestoresReadyState() {
    sak::PartitionManagerController controller;
    controller.setTestInventory(healthyInventory());
    queueInitialize(&controller);
    controller.undo();
    QVERIFY(controller.queue().isEmpty());
    QCOMPARE(controller.state(), sak::PartitionManagerState::Ready);
    controller.redo();
    QCOMPARE(controller.state(), sak::PartitionManagerState::QueueDirty);
}

void PartitionManagerControllerTests::freshSameHashMetadataBlocksApply() {
    sak::PartitionManagerController controller;
    const auto original = healthyInventory();
    auto now_read_only = original;
    now_read_only.disks[0].is_read_only = true;
    std::atomic_int executions{0};
    controller.setTestInventory(original);
    controller.setInventoryScannerForTesting(
        [now_read_only](const sak::CancelCheck&) { return inventoryResult(now_read_only); });
    controller.setApplyExecutorForTesting([&executions](const QVector<sak::PartitionOperation>&,
                                                        bool dry_run,
                                                        bool,
                                                        const sak::CancelCheck&) {
        ++executions;
        return successfulResult(dry_run);
    });
    queueInitialize(&controller);

    QVERIFY(controller.applyQueue(controller.queueReviewToken(), true, false));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(executions.load(), 0);
    QCOMPARE(controller.state(), sak::PartitionManagerState::QueueDirty);
    QCOMPARE(controller.queue().operations().size(), 1);
    QVERIFY(controller.queue().operations().constFirst().blockers.contains(
        QStringLiteral("Target disk is read-only")));
}

void PartitionManagerControllerTests::blankSerialDiskSwapWithSameLayoutTextBlocksApply() {
    sak::PartitionManagerController controller;
    auto original = healthyInventory();
    original.disks[0].serial_number.clear();
    auto swapped = original;
    swapped.disks[0].unique_id = QStringLiteral("replacement-unique-id");
    controller.setTestInventory(original);
    controller.setInventoryScannerForTesting(
        [swapped](const sak::CancelCheck&) { return inventoryResult(swapped); });
    controller.setApplyExecutorForTesting(
        [](const QVector<sak::PartitionOperation>&, bool dry_run, bool, const sak::CancelCheck&) {
            return successfulResult(dry_run);
        });
    queueInitialize(&controller);
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), true, false));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(controller.state(), sak::PartitionManagerState::QueueDirty);
    QVERIFY(controller.queue().operations().constFirst().blockers.contains(
        QStringLiteral("Target disk identity changed since the operation was queued")));
}

void PartitionManagerControllerTests::rawPayloadTargetSwapBlocksApply() {
    sak::PartitionManagerController controller;
    auto original = healthyInventory();
    auto target_disk = original.disks.constFirst();
    target_disk.disk_number = sak::DiskNumber{kRawTargetDiskNumber};
    target_disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive8");
    target_disk.unique_id = QStringLiteral("raw-target-original");
    target_disk.serial_number.clear();
    original.disks.append(target_disk);
    auto swapped = original;
    swapped.disks[1].unique_id = QStringLiteral("raw-target-replacement");
    std::atomic_int executions{0};
    controller.setTestInventory(original);
    controller.setInventoryScannerForTesting(
        [swapped](const sak::CancelCheck&) { return inventoryResult(swapped); });
    controller.setApplyExecutorForTesting([&executions](const QVector<sak::PartitionOperation>&,
                                                        bool dry_run,
                                                        bool,
                                                        const sak::CancelCheck&) {
        ++executions;
        return successfulResult(dry_run);
    });
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive7");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive8");
    payload[QStringLiteral("source_size_bytes")] = QString::number(kTestDiskBytes);
    payload[QStringLiteral("target_size_bytes")] = QString::number(kTestDiskBytes);
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    QVERIFY(controller.queueOperation(
        sak::PartitionOperationType::CloneDisk, testDiskTarget(), payload));
    QCOMPARE(controller.queue()
                 .operations()
                 .constFirst()
                 .payload.value(QStringLiteral("target_disk_unique_id"))
                 .toString(),
             QStringLiteral("raw-target-original"));
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), true, false));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(executions.load(), 0);
    QVERIFY(controller.queue().operations().constFirst().blockers.contains(
        QStringLiteral("Raw payload target disk identity changed or is unavailable")));
}

void PartitionManagerControllerTests::missingStableDiskIdentityBlocksPreflight() {
    sak::PartitionManagerController controller;
    auto inventory = healthyInventory();
    inventory.disks[0].unique_id.clear();
    controller.setTestInventory(inventory);
    controller.setInventoryScannerForTesting(
        [inventory](const sak::CancelCheck&) { return inventoryResult(inventory); });
    controller.setApplyExecutorForTesting(
        [](const QVector<sak::PartitionOperation>&, bool dry_run, bool, const sak::CancelCheck&) {
            return successfulResult(dry_run);
        });
    queueInitialize(&controller);
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), true, false));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(controller.state(), sak::PartitionManagerState::QueueDirty);
    QVERIFY(controller.queue().operations().constFirst().blockers.contains(
        QStringLiteral("Target disk has no stable Get-Disk UniqueId")));
}

void PartitionManagerControllerTests::dryRunUsesFreshInventoryAndKeepsQueueDirty() {
    sak::PartitionManagerController controller;
    std::atomic_int scans{0};
    std::optional<sak::PartitionExecutionResult> delivered;
    controller.setTestInventory(healthyInventory());
    controller.setInventoryScannerForTesting([&scans](const sak::CancelCheck&) {
        ++scans;
        return inventoryResult(healthyInventory());
    });
    controller.setApplyExecutorForTesting(
        [](const QVector<sak::PartitionOperation>&, bool dry_run, bool, const sak::CancelCheck&) {
            return successfulResult(dry_run);
        });
    connect(&controller,
            &sak::PartitionManagerController::executionFinished,
            this,
            [&delivered](const auto& result) { delivered = result; });
    queueInitialize(&controller);

    QVERIFY(controller.applyQueue(controller.queueReviewToken(), true, true));
    QVERIFY(waitUntil([&delivered]() { return delivered.has_value(); }));
    QCOMPARE(scans.load(), 1);
    QVERIFY(delivered->success);
    QVERIFY(delivered->dry_run);
    QCOMPARE(controller.state(), sak::PartitionManagerState::QueueDirty);
    QVERIFY(!controller.queue().isEmpty());
}

void PartitionManagerControllerTests::realApplyTransitionsAndCleansBeforeSignals() {
    sak::PartitionManagerController controller;
    std::atomic_int scans{0};
    QVector<sak::PartitionManagerState> states;
    bool awaiting_reentry_was_blocked = false;
    bool completion_signal_was_blocked = false;
    bool terminal_state_was_unblocked = false;
    std::optional<sak::PartitionExecutionResult> delivered;
    controller.setTestInventory(healthyInventory());
    controller.setInventoryScannerForTesting([&scans](const sak::CancelCheck&) {
        const int scan_number = scans.fetch_add(1);
        return inventoryResult(healthyInventory(scan_number == 0 ? QStringLiteral("layout-a")
                                                                 : QStringLiteral("layout-b")));
    });
    controller.setApplyExecutorForTesting(
        [](const QVector<sak::PartitionOperation>&, bool dry_run, bool, const sak::CancelCheck&) {
            return successfulResult(dry_run);
        });
    connect(&controller,
            &sak::PartitionManagerController::stateChanged,
            this,
            [&controller, &states, &awaiting_reentry_was_blocked, &terminal_state_was_unblocked](
                auto state) {
                states.append(state);
                if (state == sak::PartitionManagerState::AwaitingElevation) {
                    const auto before = controller.queue().operations().size();
                    controller.queueOperation(sak::PartitionOperationType::InitializeDisk,
                                              testDiskTarget());
                    awaiting_reentry_was_blocked = controller.queue().operations().size() == before;
                }
                if (state == sak::PartitionManagerState::Ready) {
                    terminal_state_was_unblocked = !controller.interactionBlocked();
                }
            });
    connect(&controller,
            &sak::PartitionManagerController::executionFinished,
            this,
            [&controller, &completion_signal_was_blocked, &delivered](const auto& result) {
                completion_signal_was_blocked = controller.interactionBlocked();
                delivered = result;
            });
    queueInitialize(&controller);

    QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, true));
    QVERIFY(waitUntil([&delivered]() { return delivered.has_value(); }));
    QVERIFY(delivered->success);
    QCOMPARE(scans.load(), kPreflightAndVerificationScanCount);
    QVERIFY(awaiting_reentry_was_blocked);
    QVERIFY(completion_signal_was_blocked);
    QVERIFY(terminal_state_was_unblocked);
    QVERIFY(controller.queue().isEmpty());
    QVERIFY(statesAppearInOrder(states,
                                {sak::PartitionManagerState::PreflightRunning,
                                 sak::PartitionManagerState::AwaitingElevation,
                                 sak::PartitionManagerState::Applying,
                                 sak::PartitionManagerState::Verifying,
                                 sak::PartitionManagerState::Ready}));
}

void PartitionManagerControllerTests::verifyFailedPostApplyScan(
    const sak::PartitionInventory& failed_inventory, const QString& expected_reason) {
    sak::PartitionManagerController controller;
    std::atomic_int scans{0};
    std::optional<sak::PartitionExecutionResult> delivered;
    controller.setTestInventory(healthyInventory());
    controller.setInventoryScannerForTesting([&scans, failed_inventory](const sak::CancelCheck&) {
        return inventoryResult(scans.fetch_add(1) == 0 ? healthyInventory() : failed_inventory);
    });
    controller.setApplyExecutorForTesting(
        [](const QVector<sak::PartitionOperation>&, bool dry_run, bool, const sak::CancelCheck&) {
            return successfulResult(dry_run);
        });
    connect(&controller,
            &sak::PartitionManagerController::executionFinished,
            this,
            [&delivered](const auto& result) { delivered = result; });
    queueInitialize(&controller);
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, false));

    QVERIFY(waitUntil([&delivered]() { return delivered.has_value(); }));
    QVERIFY(!delivered->success);
    QVERIFY(delivered->message.contains(QStringLiteral("Post-apply verification failed")));
    QVERIFY(delivered->message.contains(expected_reason));
    QVERIFY(delivered->report_html.contains(QStringLiteral("Failed")));
    QVERIFY(delivered->report_json.contains(QStringLiteral("\"success\": false")));
    QCOMPARE(controller.state(), sak::PartitionManagerState::Failed);
    QCOMPARE(controller.queue().operations().size(), 1);
}

void PartitionManagerControllerTests::failedVerificationRetainsQueueAndDowngradesReport() {
    sak::PartitionInventory empty;
    verifyFailedPostApplyScan(empty, QStringLiteral("inventory returned no disks"));

    auto partial = healthyInventory(QStringLiteral("partial-layout"));
    partial.disks[0].partition_enumeration_failed = true;
    verifyFailedPostApplyScan(partial, QStringLiteral("partition enumeration was incomplete"));

    auto warning = healthyInventory(QStringLiteral("warning-layout"));
    warning.warnings.append(QStringLiteral("SMART metadata unavailable"));
    verifyFailedPostApplyScan(warning, QStringLiteral("SMART metadata unavailable"));
}

void PartitionManagerControllerTests::inventoryRefreshCanBeCancelled() {
    sak::PartitionManagerController controller;
    std::atomic_bool entered{false};
    controller.setInventoryScannerForTesting([&entered](const sak::CancelCheck& should_cancel) {
        entered.store(true);
        while (!should_cancel()) {
            QThread::msleep(1);
        }
        sak::PartitionInventory cancelled;
        cancelled.warnings.append(QStringLiteral("Inventory scan cancelled"));
        return inventoryResult(std::move(cancelled), true);
    });
    controller.refreshInventory();
    QVERIFY(waitUntil([&entered]() { return entered.load(); }));
    QVERIFY(controller.interactionBlocked());
    controller.cancel();
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(controller.state(), sak::PartitionManagerState::Cancelled);
}

void PartitionManagerControllerTests::finishedInventoryFutureCannotBeRetroactivelyCancelled() {
    sak::PartitionManagerController controller;
    controller.setInventoryScannerForTesting(
        [](const sak::CancelCheck&) { return inventoryResult(healthyInventory()); });
    controller.refreshInventory();
    QElapsedTimer timer;
    timer.start();
    while (controller.canCancel() && timer.elapsed() < kAsyncTimeoutMs) {
        QThread::msleep(1);
    }
    QVERIFY(controller.interactionBlocked());
    QVERIFY(!controller.canCancel());
    controller.cancel();
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(controller.state(), sak::PartitionManagerState::Ready);
}

void PartitionManagerControllerTests::filesystemCheckCanBeCancelled() {
    sak::PartitionManagerController controller;
    std::atomic_bool entered{false};
    std::optional<sak::PartitionFileSystemToolRunResult> delivered;
    controller.setTestInventory(healthyInventory());
    controller.setFileSystemCheckRunnerForTesting(
        [&entered](const sak::PartitionFileSystemReadOnlyCheckRequest&,
                   const sak::CancelCheck& should_cancel) {
            entered.store(true);
            while (!should_cancel()) {
                QThread::msleep(1);
            }
            sak::PartitionFileSystemToolRunResult result;
            result.cancelled = true;
            return result;
        });
    connect(&controller,
            &sak::PartitionManagerController::fileSystemCheckFinished,
            this,
            [&delivered](const auto& result) { delivered = result; });
    controller.runReadOnlyFileSystemCheck(QStringLiteral("NTFS"), QStringLiteral("X:"));
    QVERIFY(waitUntil([&entered]() { return entered.load(); }));
    controller.cancel();
    QVERIFY(waitUntil([&delivered]() { return delivered.has_value(); }));
    QVERIFY(delivered->cancelled);
    QVERIFY(!delivered->success);
    QCOMPARE(controller.state(), sak::PartitionManagerState::Ready);
    QVERIFY(!controller.interactionBlocked());
}

void PartitionManagerControllerTests::destructorCancelsAndJoinsInventoryScan() {
    std::atomic_bool entered{false};
    std::atomic_bool exited{false};
    auto controller = std::make_unique<sak::PartitionManagerController>();
    controller->setInventoryScannerForTesting(
        [&entered, &exited](const sak::CancelCheck& should_cancel) {
            entered.store(true);
            while (!should_cancel()) {
                QThread::msleep(1);
            }
            exited.store(true);
            return inventoryResult(healthyInventory(), true);
        });
    controller->refreshInventory();
    QVERIFY(waitUntil([&entered]() { return entered.load(); }));
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    QVERIFY(exited.load());
    QVERIFY(timer.elapsed() < kAsyncTimeoutMs);
}

void PartitionManagerControllerTests::storageInventoryPassesCancellationToProcessRunner() {
    std::atomic_int launches{0};
    const sak::ScopedProcessFaultInjector injector(
        [&launches](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            ++launches;
            return std::nullopt;
        });
    const auto inventory = sak::StorageInventoryWorker::scanCurrentSystem(false,
                                                                          []() { return true; });
    QCOMPARE(launches.load(), 1);
    QVERIFY(inventory.isEmpty());
    QCOMPARE(inventory.warnings, QStringList{QStringLiteral("Inventory scan cancelled")});
}

QTEST_GUILESS_MAIN(PartitionManagerControllerTests)

#include "test_partition_manager_controller.moc"
