// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/partition_manager_controller.h"
#include "sak/partition_report_generator.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <algorithm>
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

sak::PartitionInventory volumeInventory() {
    auto inventory = healthyInventory();
    auto& disk = inventory.disks[0];
    disk.partition_style = QStringLiteral("GPT");
    sak::PartitionVolumeInfo volume;
    volume.drive_letter = QStringLiteral("K");
    volume.volume_guid = QStringLiteral("volume-k");
    volume.file_system = QStringLiteral("NTFS");
    volume.total_bytes = 256 * 1024 * 1024;
    volume.free_bytes = 128 * 1024 * 1024;
    volume.bitlocker_state_known = true;
    volume.dirty_bit_known = true;
    sak::PartitionInfoEx partition;
    partition.disk_number = disk.disk_number;
    partition.partition_number = sak::PartitionNumber{1};
    partition.type_name = QStringLiteral("Basic");
    partition.offset_bytes = 1024 * 1024;
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;
    disk.partitions.append(partition);
    return inventory;
}

sak::PartitionTarget volumeTarget() {
    const auto inventory = volumeInventory();
    const auto& partition = inventory.disks.first().partitions.first();
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Partition;
    target.disk_number = partition.disk_number;
    target.partition_number = partition.partition_number;
    target.offset_bytes = partition.offset_bytes;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = QStringLiteral("K");
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
    void onlyApplyScansWithElevation();
    void applyRefusesOnWhatItsOwnScanFinds();
    void applyDecidesDefragOnTheDrivesOwnRotationRate();
    void applyStopsAndSaysWhyWithoutTheHelper();
    void queueRefusalGivesItsReasons();
    void logTextIsPlainAndWhole();
    void quickPartitionBatchUsesSequentialProjectedInventory();
    void changedQueueReviewTokenRefusesApply();
    void undoLastOperationRestoresReadyState();
    void freshSameHashMetadataBlocksApply();
    void blankSerialDiskSwapWithSameLayoutTextBlocksApply();
    void rawPayloadTargetSwapBlocksApply();
    void missingStableDiskIdentityBlocksPreflight();
    void dryRunUsesFreshInventoryAndKeepsQueueDirty();
    void readOnlyOperationRunsNowOutsideTheQueue();
    void readOnlyRunRefusesMutatingOperations();
    void realApplyTransitionsAndCleansBeforeSignals();
    void failedVerificationRetainsQueueAndDowngradesReport();
    void verificationScanThatDoesNotFinishIsRetried();
    void applyOnAnUnfinishedScanSaysItIsIncomplete();
    void inventoryRefreshCanBeCancelled();
    void finishedInventoryFutureCannotBeRetroactivelyCancelled();
    void filesystemCheckCanBeCancelled();
    void readOnlyCheckRunsInTheElevatedHelper();
    void windowsPhaseTakesCancelAwayUntilItEnds();
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
    // A scan that stopped did not return no disks: its status line says it did not finish.
    sak::PartitionInventory unfinished;
    unfinished.scan_did_not_finish = true;
    unfinished.warnings.append(QStringLiteral(
        "The disk scan did not finish: no progress for 30 s (last step: disk 2 of 5)"));
    QCOMPARE(sak::PartitionManagerController::inventoryStatusMessage(unfinished),
             QStringLiteral("Partition Manager: The disk scan did not finish: no progress for 30 s "
                            "(last step: disk 2 of 5). The scan is incomplete, not a PC without "
                            "disks: click Scan Disks to try again."));
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

// The status bar said only that a batch failed validation. The reason is what a person acts on.
// Apply is what may ask for elevation, so it scans the other way from a refresh.
void PartitionManagerControllerTests::onlyApplyScansWithElevation() {
    sak::PartitionManagerController controller;
    int browsing = 0;
    int applying = 0;
    controller.setInventoryScannerForTesting([&browsing](const sak::CancelCheck&) {
        ++browsing;
        return inventoryResult(healthyInventory());
    });
    controller.setApplyInventoryScannerForTesting([&applying](const sak::CancelCheck&) {
        ++applying;
        return inventoryResult(healthyInventory());
    });
    controller.setApplyExecutorForTesting(
        [](const QVector<sak::PartitionOperation>&, bool, bool, const sak::CancelCheck&) {
            sak::PartitionExecutionResult result;
            result.success = true;
            return result;
        });
    controller.refreshInventory();
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(browsing, 1);
    QCOMPARE(applying, 0);

    QVERIFY(controller.queueOperation(sak::PartitionOperationType::InitializeDisk,
                                      testDiskTarget(),
                                      QJsonObject{{"target_style", "GPT"}}));
    // A dry run writes nothing and may not ask.
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), true, false));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(browsing, 2);
    QCOMPARE(applying, 0);

    if (controller.queue().isEmpty()) {
        QVERIFY(controller.queueOperation(sak::PartitionOperationType::InitializeDisk,
                                          testDiskTarget(),
                                          QJsonObject{{"target_style", "GPT"}}));
    }
    const int browsing_before = browsing;
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, true));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QCOMPARE(applying, 2);  // the preflight, and the check after writing
    QCOMPARE(browsing, browsing_before);
}

// What Apply's scan finds is what decides, right before the write, and it says why.
void PartitionManagerControllerTests::applyRefusesOnWhatItsOwnScanFinds() {
    sak::PartitionManagerController controller;
    auto browsing_view = volumeInventory();
    browsing_view.disks[0].partitions[0].volume->bitlocker_state_known = false;
    browsing_view.disks[0].partitions[0].volume->bitlocker_state_error = QStringLiteral("denied");
    browsing_view.disks[0].partitions[0].volume->administrator_state_pending = true;
    controller.setTestInventory(browsing_view);
    auto apply_view = volumeInventory();
    apply_view.disks[0].partitions[0].volume->bitlocker_state_known = true;
    apply_view.disks[0].partitions[0].volume->bitlocker_enabled = true;
    apply_view.disks[0].partitions[0].volume->bitlocker_locked = true;
    controller.setApplyInventoryScannerForTesting(
        [apply_view](const sak::CancelCheck&) { return inventoryResult(apply_view); });
    bool executed = false;
    controller.setApplyExecutorForTesting(
        [&executed](const QVector<sak::PartitionOperation>&, bool, bool, const sak::CancelCheck&) {
            executed = true;
            return sak::PartitionExecutionResult{};
        });
    QSignalSpy refused(&controller, &sak::PartitionManagerController::applyRefused);

    QVERIFY2(controller.queueOperation(sak::PartitionOperationType::Format,
                                       volumeTarget(),
                                       QJsonObject{{"file_system", "NTFS"}}),
             "a volume whose state is pending queues");
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, true));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QVERIFY2(!executed, "a locked volume must stop Apply before it writes");
    QCOMPARE(refused.size(), 1);
    QVERIFY(refused.first()
                .first()
                .toStringList()
                .join(QLatin1Char(' '))
                .contains(QStringLiteral("BitLocker volume is locked")));
}

// A drive Windows cannot name queues on "unknown until Apply", and Apply's scan asks it.
void PartitionManagerControllerTests::applyDecidesDefragOnTheDrivesOwnRotationRate() {
    for (const int rotation_rate : {5425, 0}) {
        sak::PartitionManagerController controller;
        auto browsing_view = volumeInventory();
        browsing_view.disks[0].media_type = QStringLiteral("Unspecified");
        browsing_view.disks[0].rotation_rate_pending = true;
        controller.setTestInventory(browsing_view);
        auto apply_view = volumeInventory();
        apply_view.disks[0].media_type = QStringLiteral("Unspecified");
        apply_view.disks[0].rotation_rate_rpm = rotation_rate;
        controller.setApplyInventoryScannerForTesting(
            [apply_view](const sak::CancelCheck&) { return inventoryResult(apply_view); });
        bool executed = false;
        controller.setApplyExecutorForTesting([&executed](const QVector<sak::PartitionOperation>&,
                                                          bool,
                                                          bool,
                                                          const sak::CancelCheck&) {
            executed = true;
            return sak::PartitionExecutionResult{};
        });
        QSignalSpy refused(&controller, &sak::PartitionManagerController::applyRefused);

        QVERIFY2(controller.queueOperation(
                     sak::PartitionOperationType::DefragVolume, volumeTarget(), QJsonObject{}),
                 "a drive the scan could not ask queues");
        QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, true));
        QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
        QCOMPARE(executed, rotation_rate > 0);
        QCOMPARE(refused.size(), rotation_rate > 0 ? 0 : 1);
        if (rotation_rate == 0) {
            QVERIFY(refused.first()
                        .first()
                        .toStringList()
                        .join(QLatin1Char(' '))
                        .contains(QStringLiteral("reported as HDD")));
        }
    }
}

// With the prompt declined the inventory carries the reason, and Apply stops on it.
void PartitionManagerControllerTests::applyStopsAndSaysWhyWithoutTheHelper() {
    sak::PartitionManagerController controller;
    controller.setTestInventory(healthyInventory());
    auto declined = healthyInventory();
    declined.warnings.append(QStringLiteral("Apply could not get administrator rights"));
    controller.setApplyInventoryScannerForTesting(
        [declined](const sak::CancelCheck&) { return inventoryResult(declined); });
    bool executed = false;
    controller.setApplyExecutorForTesting(
        [&executed](const QVector<sak::PartitionOperation>&, bool, bool, const sak::CancelCheck&) {
            executed = true;
            return sak::PartitionExecutionResult{};
        });
    QSignalSpy refused(&controller, &sak::PartitionManagerController::applyRefused);
    QVERIFY(controller.queueOperation(sak::PartitionOperationType::InitializeDisk,
                                      testDiskTarget(),
                                      QJsonObject{{"target_style", "GPT"}}));
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, true));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QVERIFY(!executed);
    QCOMPARE(refused.size(), 1);
    QVERIFY(refused.first()
                .first()
                .toStringList()
                .join(QLatin1Char(' '))
                .contains(QStringLiteral("administrator rights")));
}

void PartitionManagerControllerTests::queueRefusalGivesItsReasons() {
    sak::PartitionManagerController controller;
    controller.setTestInventory(healthyInventory());
    QSignalSpy refused(&controller, &sak::PartitionManagerController::queueRefused);
    QSignalSpy status(&controller, &sak::PartitionManagerController::statusMessage);

    sak::PartitionTarget absent = testDiskTarget();
    absent.disk_number = sak::DiskNumber{42};
    QVERIFY(!controller.queueOperation(sak::PartitionOperationType::InitializeDisk, absent));
    QVERIFY(controller.queue().isEmpty());
    QCOMPARE(refused.count(), 1);
    const QStringList reasons = refused.first().at(0).toStringList();
    QVERIFY(!reasons.isEmpty());
    QVERIFY(std::ranges::all_of(reasons, [](const QString& reason) { return !reason.isEmpty(); }));
    const QString shown = status.last().at(0).toString();
    QVERIFY2(shown.startsWith(QStringLiteral("Not added to the queue: ")), qPrintable(shown));
    QVERIFY2(shown.contains(reasons.first()), qPrintable(shown));
}

void PartitionManagerControllerTests::logTextIsPlainAndWhole() {
    sak::PartitionExecutionResult result;
    result.message = QStringLiteral("Dry run complete");
    sak::PartitionExecutionStep step;
    step.summary = QStringLiteral("Wipe <Disk 4>");
    step.success = true;
    step.stdout_text = QStringLiteral("line\n") + QString(5000, QLatin1Char('x')) +
                       QStringLiteral("THE-END");
    result.steps.append(step);
    sak::PartitionExecutionStep failed;
    failed.summary = QStringLiteral("Second step");
    failed.error_message = QStringLiteral("it broke");
    failed.stderr_text = QStringLiteral("stderr here");
    result.steps.append(failed);

    const QString text = sak::PartitionReportGenerator::generateLogText(result);
    QVERIFY(text.startsWith(QStringLiteral("Dry run complete\n")));
    QVERIFY(text.contains(QStringLiteral("Success: Wipe <Disk 4>\n")));
    QVERIFY2(text.contains(QStringLiteral("THE-END")), "a step's output is not cut in the log");
    QVERIFY(text.contains(QStringLiteral("Failed: Second step\nit broke\nstderr here")));
    QVERIFY(!text.contains(QStringLiteral("<table")));
    QVERIFY(!text.contains(QStringLiteral("&lt;")));
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
    create_payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
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

// A scan changes nothing, so it runs the moment it is asked for: no queue entry, no Apply, no
// preflight or verification inventory scan -- exactly one elevated execution of one operation.
void PartitionManagerControllerTests::readOnlyOperationRunsNowOutsideTheQueue() {
    sak::PartitionManagerController controller;
    std::atomic_int scans{0};
    std::atomic_int executions{0};
    std::optional<sak::PartitionExecutionResult> delivered;
    QVector<sak::PartitionOperation> executed;
    bool executed_dry_run = true;
    bool executed_elevated = false;
    controller.setTestInventory(healthyInventory());
    controller.setInventoryScannerForTesting([&scans](const sak::CancelCheck&) {
        ++scans;
        return inventoryResult(healthyInventory());
    });
    controller.setApplyExecutorForTesting(
        [&executions, &executed, &executed_dry_run, &executed_elevated](
            const QVector<sak::PartitionOperation>& operations,
            bool dry_run,
            bool use_elevation,
            const sak::CancelCheck&) {
            executed = operations;
            executed_dry_run = dry_run;
            executed_elevated = use_elevation;
            ++executions;
            return successfulResult(dry_run);
        });
    connect(&controller,
            &sak::PartitionManagerController::executionFinished,
            this,
            [&delivered](const auto& result) { delivered = result; });

    QVERIFY(controller.runReadOnlyOperation(sak::PartitionOperationType::SurfaceTest,
                                            testDiskTarget()));
    QVERIFY(waitUntil([&delivered]() { return delivered.has_value(); }));
    QVERIFY(delivered->success);
    QCOMPARE(executions.load(), 1);
    QCOMPARE(executed.size(), 1);
    QCOMPARE(executed.constFirst().type, sak::PartitionOperationType::SurfaceTest);
    QCOMPARE(executed.constFirst().risk, sak::OperationRisk::ReadOnly);
    QVERIFY(!executed_dry_run);
    QVERIFY(executed_elevated);
    QCOMPARE(scans.load(), 0);
    QVERIFY(controller.queue().isEmpty());
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
}

// The run-now path is for scans only. Anything that changes the disk is refused outright -- it
// never reaches the executor -- and must go through the queue, its review and Apply.
void PartitionManagerControllerTests::readOnlyRunRefusesMutatingOperations() {
    sak::PartitionManagerController controller;
    std::atomic_int executions{0};
    controller.setTestInventory(healthyInventory());
    controller.setApplyExecutorForTesting([&executions](const QVector<sak::PartitionOperation>&,
                                                        bool dry_run,
                                                        bool,
                                                        const sak::CancelCheck&) {
        ++executions;
        return successfulResult(dry_run);
    });
    QVERIFY(!controller.runReadOnlyOperation(sak::PartitionOperationType::InitializeDisk,
                                             testDiskTarget(),
                                             {{QStringLiteral("target_style"),
                                               QStringLiteral("GPT")}}));
    QVERIFY(!controller.runReadOnlyOperation(sak::PartitionOperationType::WipeDisk,
                                             testDiskTarget(),
                                             {{QStringLiteral("target_wipe_confirmed"), true}}));
    // A non-Windows "check" is the bundled tool's repair; the type alone must not admit it.
    QVERIFY(!controller.runReadOnlyOperation(sak::PartitionOperationType::CheckFileSystem,
                                             testDiskTarget(),
                                             {{QStringLiteral("non_native_file_system_tool"), true},
                                              {QStringLiteral("file_system"),
                                               QStringLiteral("ext4")}}));
    QCoreApplication::processEvents();
    QCOMPARE(executions.load(), 0);
    QVERIFY(!controller.interactionBlocked());
    QVERIFY(controller.queue().isEmpty());
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

namespace {

/// What a scan that did not finish returns: no disks, and the reason (F26).
sak::PartitionInventory unfinishedScanInventory() {
    sak::PartitionInventory inventory;
    inventory.scan_did_not_finish = true;
    inventory.warnings.append(QStringLiteral(
        "The disk scan did not finish: no progress for 90 s (last step: disk 2 of 5)"));
    return inventory;
}

}  // namespace

// F26 (operation proof, 2026-11-12): on a PC under heavy disk load the scan after an Apply timed
// out, and a Move that had finished was reported failed. The verification scan is tried twice more,
// after a wait; one that finishes verifies the Apply, and when none does the Apply that ran is not
// a failure: it says it could not be verified, and nothing can be queued until a scan finishes.
void PartitionManagerControllerTests::verificationScanThatDoesNotFinishIsRetried() {
    for (const int unfinished_scans : {2, 3}) {
        sak::PartitionManagerController controller;
        controller.setVerificationBackoffForTesting(10);
        std::atomic_int scans{0};
        std::optional<sak::PartitionExecutionResult> delivered;
        controller.setTestInventory(healthyInventory());
        controller.setInventoryScannerForTesting(
            [&scans, unfinished_scans](const sak::CancelCheck&) {
                const int scan = scans.fetch_add(1);
                // The first is Apply's preflight, on the queue's layout; then the verification
                // scans.
                if (scan == 0) {
                    return inventoryResult(healthyInventory());
                }
                if (scan <= unfinished_scans) {
                    return inventoryResult(unfinishedScanInventory());
                }
                return inventoryResult(healthyInventory(QStringLiteral("layout-b")));
            });
        controller.setApplyExecutorForTesting(
            [](const QVector<sak::PartitionOperation>&,
               bool dry_run,
               bool,
               const sak::CancelCheck&) { return successfulResult(dry_run); });
        connect(&controller,
                &sak::PartitionManagerController::executionFinished,
                this,
                [&delivered](const auto& result) { delivered = result; });
        queueInitialize(&controller);
        QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, false));
        QVERIFY(waitUntil([&delivered]() { return delivered.has_value(); }));
        QVERIFY2(delivered->success, qPrintable(delivered->message));
        QVERIFY(controller.queue().isEmpty());
        if (unfinished_scans == 2) {
            // The third verification scan finished: verified, and the disks are the new ones.
            QCOMPARE(scans.load(), 4);
            QVERIFY(!delivered->message.contains(QStringLiteral("could not re-read")));
            QCOMPARE(controller.inventory().layout_hash, QStringLiteral("layout-b"));
            continue;
        }
        QCOMPARE(scans.load(), 4);
        QCOMPARE(delivered->message, QString::fromLatin1(sak::kPartitionUnverifiedMessage));
        QVERIFY(!delivered->message.contains(QStringLiteral("no disks")));
        // Fail closed: no disks to queue against until a scan finishes.
        QVERIFY(controller.inventory().isEmpty());
        QVERIFY(controller.inventory().scan_did_not_finish);
        QVERIFY(!controller.queueOperation(sak::PartitionOperationType::InitializeDisk,
                                           testDiskTarget()));
    }
}

// F26: Apply's own scan that does not finish refuses the Apply as incomplete, with the way on, and
// never as a PC without disks; nothing runs.
void PartitionManagerControllerTests::applyOnAnUnfinishedScanSaysItIsIncomplete() {
    sak::PartitionManagerController controller;
    controller.setTestInventory(healthyInventory());
    controller.setInventoryScannerForTesting(
        [](const sak::CancelCheck&) { return inventoryResult(unfinishedScanInventory()); });
    bool executed = false;
    controller.setApplyExecutorForTesting(
        [&executed](const QVector<sak::PartitionOperation>&, bool, bool, const sak::CancelCheck&) {
            executed = true;
            return sak::PartitionExecutionResult{};
        });
    QSignalSpy refused(&controller, &sak::PartitionManagerController::applyRefused);
    queueInitialize(&controller);
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, false));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QVERIFY(!executed);
    QCOMPARE(refused.size(), 1);
    const QString why = refused.first().first().toStringList().join(QLatin1Char(' '));
    QVERIFY2(why.contains(QStringLiteral("The disk scan did not finish")) &&
                 why.contains(
                     QStringLiteral("The scan is incomplete, not a PC without disks: click "
                                    "Scan Disks to try again.")) &&
                 !why.contains(QStringLiteral("no disks")),
             qPrintable(why));
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

// F25 (operation proof, 2026-11-12): Check Non-Windows File System ran e2fsck in this
// standard-user process, which may not open the raw partition, and the refusal read as a corrupt
// superblock. The check is now the read-only operation the elevated helper runs, built from the
// scan's partition, and what the helper's run says reaches the result in its own words.
void PartitionManagerControllerTests::readOnlyCheckRunsInTheElevatedHelper() {
    auto inventory = volumeInventory();
    auto& partition = inventory.disks[0].partitions[0];
    partition.volume->file_system = QStringLiteral("ext4");
    partition.volume->drive_letter.clear();
    sak::PartitionManagerController controller;
    controller.setTestInventory(inventory);
    std::optional<sak::PartitionFileSystemReadOnlyCheckRequest> seen;
    controller.setFileSystemCheckRunnerForTesting(
        [&seen](const sak::PartitionFileSystemReadOnlyCheckRequest& request,
                const sak::CancelCheck&) {
            seen = request;
            return sak::PartitionFileSystemToolRunResult{};
        });
    const QString raw_path = QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk7\\Partition1");
    controller.runReadOnlyFileSystemCheck(QStringLiteral("ext4"), raw_path);
    QVERIFY(waitUntil([&seen]() { return seen.has_value(); }));
    QVERIFY(seen->helper_operation.has_value());
    const auto& operation = *seen->helper_operation;
    QCOMPARE(operation.type, sak::PartitionOperationType::CheckFileSystem);
    QCOMPARE(operation.risk, sak::OperationRisk::ReadOnly);
    QVERIFY(operation.payload.value(QStringLiteral("read_only_check")).toBool());
    QCOMPARE(operation.payload.value(QStringLiteral("target_path")).toString(), raw_path);
    QCOMPARE(operation.target.partition_number.value(), 1U);
    QCOMPARE(operation.target.disk_unique_id, QStringLiteral("test-unique-id"));
    QVERIFY2(seen->helper_blockers.isEmpty(), qPrintable(seen->helper_blockers.join(u';')));

    sak::PartitionExecutionStep refusal;
    refusal.error_message = QStringLiteral(
        "S.A.K. could not open the partition: Access is denied. The check read nothing, so this "
        "says nothing about the file system.");
    sak::PartitionExecutionResult refused;
    refused.message = QStringLiteral("batch failed");
    refused.steps.append(refusal);
    std::optional<sak::PartitionOperation> ran;
    const auto failed = sak::PartitionManagerController::runReadOnlyCheckThroughHelper(
        *seen, [&ran, &refused](const sak::PartitionOperation& run) {
            ran = run;
            return refused;
        });
    QVERIFY(ran.has_value() && ran->payload.value(QStringLiteral("read_only_check")).toBool());
    QVERIFY(!failed.success && !failed.blocked);
    QCOMPARE(failed.problem, refusal.error_message);
    sak::PartitionExecutionResult passed;
    passed.success = true;
    const auto clean = sak::PartitionManagerController::runReadOnlyCheckThroughHelper(
        *seen, [&passed](const sak::PartitionOperation&) { return passed; });
    QVERIFY(clean.success && clean.problem.isEmpty());

    // A partition the scan does not have is not checked at all.
    auto unknown = *seen;
    unknown.helper_operation.reset();
    bool called = false;
    const auto blocked = sak::PartitionManagerController::runReadOnlyCheckThroughHelper(
        unknown, [&called, &passed](const sak::PartitionOperation&) {
            called = true;
            return passed;
        });
    QVERIFY(blocked.blocked && !called);
    QVERIFY(blocked.blockers.join(u' ').contains(QStringLiteral("not in the current scan")));
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

// A full format Windows has taken over goes on when its script is killed. While it runs, Cancel
// is not offered and a cancel request stops nothing and says why; before and after it, Cancel
// works. Closing still stops the script and everything queued after it.
void PartitionManagerControllerTests::windowsPhaseTakesCancelAwayUntilItEnds() {
    sak::PartitionManagerController controller;
    controller.setTestInventory(healthyInventory());
    controller.setInventoryScannerForTesting(
        [](const sak::CancelCheck&) { return inventoryResult(healthyInventory()); });
    std::atomic_bool entered{false};
    std::atomic_bool cancel_seen{false};
    controller.setApplyExecutorForTesting(
        [&entered, &cancel_seen](const QVector<sak::PartitionOperation>&,
                                 bool dry_run,
                                 bool,
                                 const sak::CancelCheck& should_cancel) {
            entered.store(true);
            while (!should_cancel()) {
                QThread::msleep(1);
            }
            cancel_seen.store(true);
            auto result = successfulResult(dry_run);
            result.success = false;
            result.cancelled = true;
            return result;
        });
    QSignalSpy status(&controller, &sak::PartitionManagerController::statusMessage);
    QSignalSpy phase_changes(&controller, &sak::PartitionManagerController::windowsPhaseChanged);
    queueInitialize(&controller);
    QVERIFY(controller.applyQueue(controller.queueReviewToken(), false, false));
    QVERIFY(waitUntil([&entered]() { return entered.load(); }));
    QVERIFY2(controller.canCancel(), "before Windows has the work, Cancel stops the step");
    QVERIFY(!controller.windowsPhase().has_value());

    const sak::WindowsContinuedPhase phase{.step_summary = QStringLiteral("Wipe Disk - Disk 7"),
                                           .marker = {.begin = true,
                                                      .work = sak::WindowsContinuedWork::FullFormat,
                                                      .size_bytes = 1024ULL * 1024ULL * 1024ULL},
                                           .started_at = QDateTime::currentDateTime()};
    controller.reportWindowsPhaseForTesting(phase);
    QCOMPARE(phase_changes.count(), 1);
    QVERIFY2(!controller.canCancel(), "no cancel can stop what Windows is carrying out");
    QVERIFY(controller.windowsPhase().has_value());
    QCOMPARE(controller.windowsPhase()->step_summary, phase.step_summary);
    controller.cancel();
    QTest::qWait(kAsyncTimeoutMs / 50);
    QVERIFY2(!cancel_seen.load(), "a refused cancel must not stop the script");
    QCOMPARE(status.last().first().toString(),
             QStringLiteral("Not cancelled: %1").arg(sak::windowsContinuedPhaseNotice(phase)));

    controller.reportWindowsPhaseForTesting(std::nullopt);
    QCOMPARE(phase_changes.count(), 2);
    QVERIFY2(controller.canCancel(), "once Windows hands back, Cancel works again");

    // Closing stops what can be stopped even while Windows has the work.
    controller.reportWindowsPhaseForTesting(phase);
    controller.cancelForExit();
    QVERIFY(waitUntil([&cancel_seen]() { return cancel_seen.load(); }));
    QVERIFY(waitUntil([&controller]() { return !controller.interactionBlocked(); }));
    QVERIFY2(!controller.windowsPhase().has_value(), "a finished batch has no phase left open");
}

QTEST_GUILESS_MAIN(PartitionManagerControllerTests)

#include "test_partition_manager_controller.moc"
