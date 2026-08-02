// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_controller.cpp
/// @brief Orchestrates uninstall pipeline, manages queue, and persists settings

#include "sak/advanced_uninstall_controller.h"

#include "sak/cleanup_worker.h"
#include "sak/config_manager.h"
#include "sak/layout_constants.h"
#include "sak/leftover_cleanup_item_guard.h"
#include "sak/logger.h"
#include "sak/program_enumerator.h"
#include "sak/restore_point_manager.h"
#include "sak/uninstall_worker.h"

#include <QThread>
#include <QTimer>

#include <memory>

namespace sak {

namespace {
constexpr int kStatusTimeoutShortMs = 3000;
constexpr int kStatusTimeoutLongMs = 5000;
constexpr int kThreadWaitMs = 5000;
constexpr int kDefaultScanLevelValue = 1;
constexpr int kMaxScanLevelValue = 2;
constexpr int kRecoveredSizeDisplayPrecision = 1;

// Safely dispose of a worker QThread before it is replaced or torn down (B10-09).
// A completion slot fires from a WORKER signal that is delivered before QThread::run
// has fully returned, so setState(Idle) can let the next op reassign the unique_ptr
// while the thread is still live -- destroying it triggers the fatal "QThread:
// Destroyed while thread is still running" abort. This cooperatively stops the
// thread and, if it refuses within the bound, DETACHES it (bounded leak, self-deletes
// when it finally exits) instead of force-terminating and destroying it.
template <typename WorkerT>
void retireWorker(std::unique_ptr<WorkerT>& worker) {
    if (!worker) {
        return;
    }
    if (worker->isExecuting() || worker->isRunning()) {
        worker->requestStop();
        if (!worker->wait(kThreadWaitMs)) {
            WorkerT* stale = worker.release();
            stale->setParent(nullptr);
            QObject::connect(stale, &QThread::finished, stale, &QObject::deleteLater);
            return;
        }
    }
    worker.reset();
}

UninstallWorker::Mode uninstallModeForProgram(const ProgramInfo& program) {
    if (program.source == ProgramInfo::Source::UWP ||
        program.source == ProgramInfo::Source::Provisioned) {
        return UninstallWorker::Mode::UwpRemove;
    }
    return UninstallWorker::Mode::Standard;
}
}  // namespace

AdvancedUninstallController::AdvancedUninstallController(QObject* parent)
    : QObject(parent)
    , m_enumerator(std::make_unique<ProgramEnumerator>())
    , m_restore_manager(std::make_unique<RestorePointManager>(this)) {
    // Register custom types for cross-thread signal delivery
    qRegisterMetaType<sak::ProgramInfo>("sak::ProgramInfo");
    qRegisterMetaType<sak::LeftoverItem>("sak::LeftoverItem");
    qRegisterMetaType<sak::UninstallReport>("sak::UninstallReport");
    qRegisterMetaType<sak::ScanLevel>("sak::ScanLevel");
    qRegisterMetaType<QVector<sak::ProgramInfo>>("QVector<sak::ProgramInfo>");
    qRegisterMetaType<QVector<sak::LeftoverItem>>("QVector<sak::LeftoverItem>");

    // Wire enumerator signals
    connect(m_enumerator.get(),
            &ProgramEnumerator::enumerationStarted,
            this,
            &AdvancedUninstallController::enumerationStarted);
    connect(m_enumerator.get(),
            &ProgramEnumerator::enumerationProgress,
            this,
            &AdvancedUninstallController::enumerationProgress);
    connect(m_enumerator.get(),
            &ProgramEnumerator::enumerationFinished,
            this,
            &AdvancedUninstallController::onEnumerationFinished);
    connect(m_enumerator.get(),
            &ProgramEnumerator::enumerationFailed,
            this,
            &AdvancedUninstallController::onEnumerationFailed);
    connect(m_enumerator.get(),
            &ProgramEnumerator::enumerationWarning,
            this,
            [this](const QString& message) {
                Q_EMIT statusMessage(message, kStatusTimeoutLongMs);
            });

    // Enumeration runs on a persistent worker thread. The enumerator keeps its affinity to this
    // thread for the controller lifetime, so a Refresh only re-invokes enumerateAll via a queued
    // call -- avoiding the per-Refresh moveToThread churn whose move-back ran on the main thread
    // (the wrong thread) rather than the worker thread that owned the object.
    m_enumThread = new QThread(this);
    m_enumThread->setObjectName(QStringLiteral("sak-enum"));
    m_enumerator->moveToThread(m_enumThread);
    m_enumThread->start();

    loadSettings();
}

AdvancedUninstallController::~AdvancedUninstallController() {
    cleanupWorkers();
    saveSettings();
}

AdvancedUninstallController::State AdvancedUninstallController::currentState() const {
    return m_state;
}

// -- Program Enumeration -----------------------------------------------------

void AdvancedUninstallController::refreshPrograms() {
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Cannot refresh while another operation is in progress.",
                             kStatusTimeoutShortMs);
        return;
    }

    setState(State::Enumerating);
    Q_EMIT statusMessage("Enumerating installed programs...", 0);

    // Run enumeration on the persistent worker thread so the GUI stays responsive. The
    // enumerator already lives on m_enumThread; queue the work onto that thread instead of
    // moving the object around.
    if (!m_enumThread || !m_enumThread->isRunning()) {
        setState(State::Idle);
        Q_EMIT statusMessage("Enumeration thread unavailable.", kStatusTimeoutShortMs);
        return;
    }
    m_enumerator->resetCancel();
    // Bump the generation so a still-running prior enumeration (cancelled, but whose cooperative
    // stop was cleared by resetCancel above) cannot pass its stale completion off as this run's.
    const int generation = ++m_enum_generation;
    QMetaObject::invokeMethod(
        m_enumerator.get(),
        [enumerator = m_enumerator.get(), generation]() { enumerator->enumerateAll(generation); },
        Qt::QueuedConnection);
}

QVector<ProgramInfo> AdvancedUninstallController::programs() const {
    return m_programs;
}

// -- Uninstall Operations ----------------------------------------------------

void AdvancedUninstallController::uninstallProgram(const ProgramInfo& program,
                                                   ScanLevel scanLevel,
                                                   bool createRestorePoint,
                                                   bool autoCleanSafe) {
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Another operation is already in progress.", kStatusTimeoutShortMs);
        return;
    }

    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage("Cannot uninstall: program name is empty.", kStatusTimeoutShortMs);
        return;
    }

    m_autoCleanSafe = autoCleanSafe;
    setState(State::Uninstalling);

    // Safely dispose of any prior worker whose thread may still be exiting before the
    // unique_ptr is overwritten (B10-09).
    retireWorker(m_uninstall_worker);
    m_uninstall_worker = std::make_unique<UninstallWorker>(
        program, uninstallModeForProgram(program), scanLevel, createRestorePoint, this);
    connectUninstallWorkerSignals(true);

    Q_EMIT uninstallStarted(program.displayName);
    Q_EMIT statusMessage(QString("Uninstalling %1...").arg(program.displayName), 0);

    m_uninstall_worker->start();
}

void AdvancedUninstallController::forceUninstall(const ProgramInfo& program,
                                                 ScanLevel scanLevel,
                                                 bool createRestorePoint) {
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Another operation is already in progress.", kStatusTimeoutShortMs);
        return;
    }

    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage("Cannot uninstall: program name is empty.", kStatusTimeoutShortMs);
        return;
    }

    m_autoCleanSafe = false;  // Force uninstall always shows results for review
    setState(State::Uninstalling);

    retireWorker(m_uninstall_worker);
    m_uninstall_worker = std::make_unique<UninstallWorker>(
        program, UninstallWorker::Mode::ForcedUninstall, scanLevel, createRestorePoint, this);

    connectUninstallWorkerSignals(false);

    Q_EMIT uninstallStarted(program.displayName);
    Q_EMIT statusMessage(QString("Forced uninstall: %1...").arg(program.displayName), 0);

    m_uninstall_worker->start();
}

void AdvancedUninstallController::removeUwpPackage(const ProgramInfo& program) {
    uninstallProgram(program, ScanLevel::Safe, false, false);
}

void AdvancedUninstallController::removeRegistryEntry(const ProgramInfo& program) {
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Another operation is already in progress.", kStatusTimeoutShortMs);
        return;
    }

    if (program.registryKeyPath.isEmpty()) {
        Q_EMIT statusMessage("Cannot remove: no registry key path.", kStatusTimeoutShortMs);
        return;
    }

    setState(State::Uninstalling);

    retireWorker(m_uninstall_worker);
    m_uninstall_worker = std::make_unique<UninstallWorker>(
        program, UninstallWorker::Mode::RegistryOnly, ScanLevel::Safe, false, this);

    connect(m_uninstall_worker.get(),
            &UninstallWorker::uninstallComplete,
            this,
            &AdvancedUninstallController::onUninstallComplete);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::failed,
            this,
            &AdvancedUninstallController::onUninstallWorkerFailed);

    Q_EMIT uninstallStarted(program.displayName);
    m_uninstall_worker->start();
}

QVector<LeftoverItem> AdvancedUninstallController::screenCleanupItems(
    const QVector<LeftoverItem>& selectedItems, int* refusedCount) {
    QVector<LeftoverItem> screenedItems;
    screenedItems.reserve(selectedItems.size());
    int refused = 0;
    for (const auto& item : selectedItems) {
        if (!item.selected) {
            continue;
        }
        const QString refusal = cleanupItemRefusal(item);
        if (refusal.isEmpty()) {
            screenedItems.append(item);
        } else {
            ++refused;
            Q_EMIT statusMessage(QString("Skipped protected item %1: %2").arg(item.path, refusal),
                                 kStatusTimeoutShortMs);
        }
    }
    if (refusedCount != nullptr) {
        *refusedCount = refused;
    }
    return screenedItems;
}

void AdvancedUninstallController::cleanLeftovers(const QVector<LeftoverItem>& selectedItems) {
    if (selectedItems.isEmpty()) {
        Q_EMIT statusMessage("No leftover items selected.", kStatusTimeoutShortMs);
        return;
    }

    if (m_state != State::Idle && m_state != State::Uninstalling) {
        Q_EMIT statusMessage("Cannot clean while another operation is in progress.",
                             kStatusTimeoutShortMs);
        return;
    }

    // Second, independent safety layer (the same fail-closed screens the AI
    // clean-leftovers op uses): refuse OS-critical / unrecoverable / injection
    // targets before any item reaches CleanupWorker, even if the user selected
    // one. Without this the GUI path could hand CleanupWorker a protected root
    // (e.g. a C:\Windows leftover misclassified Safe) for permanent deletion.
    int refused = 0;
    const QVector<LeftoverItem> screenedItems = screenCleanupItems(selectedItems, &refused);

    if (screenedItems.isEmpty()) {
        setState(State::Idle);
        Q_EMIT statusMessage(QString(
                                 "No cleanable leftover items after safety screening (%1 refused).")
                                 .arg(refused),
                             kStatusTimeoutShortMs);
        return;
    }

    setState(State::Cleaning);

    const int total = static_cast<int>(screenedItems.size());
    Q_EMIT cleanupStarted(total);
    Q_EMIT statusMessage(QString("Cleaning %1 leftover items...").arg(total), 0);

    retireWorker(m_cleanup_worker);
    m_cleanup_worker = std::make_unique<CleanupWorker>(screenedItems, m_useRecycleBin, this);
    connectCleanupWorkerSignals(m_cleanup_worker.get());
    m_cleanup_worker->start();
}

QVector<LeftoverItem> AdvancedUninstallController::safeLeftovers(
    const QVector<LeftoverItem>& items) {
    QVector<LeftoverItem> safe;
    for (const LeftoverItem& item : items) {
        if (item.risk == LeftoverItem::RiskLevel::Safe) {
            LeftoverItem copy = item;
            copy.selected = true;
            safe.append(copy);
        }
    }
    return safe;
}

bool AdvancedUninstallController::maybeAutoCleanSafeLeftovers(const UninstallReport& report,
                                                              bool autoClean) {
    if (!autoClean) {
        return false;
    }
    const QVector<LeftoverItem> safe = safeLeftovers(report.foundLeftovers);
    if (safe.isEmpty()) {
        return false;
    }
    Q_EMIT autoCleanSafeStarted(static_cast<int>(safe.size()));
    Q_EMIT statusMessage(QString("Auto-cleaning %1 safe leftover item(s) from %2 "
                                 "(Review/Risky items kept for your review)...")
                             .arg(safe.size())
                             .arg(report.programName),
                         0);
    // Reuse the vetted GUI cleanup path: it re-screens against the OS-critical denylist and runs
    // CleanupWorker with the recycle-bin default, so an auto-clean is recoverable and can never
    // permanently delete a misclassified protected target.
    cleanLeftovers(safe);
    return m_state == State::Cleaning;
}

void AdvancedUninstallController::connectCleanupWorkerSignals(CleanupWorker* worker) {
    connect(worker, &CleanupWorker::itemCleaned, this, &AdvancedUninstallController::itemCleaned);
    connect(worker,
            &CleanupWorker::cleanupComplete,
            this,
            &AdvancedUninstallController::onCleanupComplete);
    connect(worker,
            &CleanupWorker::rebootPendingItems,
            this,
            &AdvancedUninstallController::rebootPendingItems);
    // recycleFallbackItems: items destroyed permanently after the Recycle Bin failed -- re-emitted
    // so the panel can warn the user the recoverable-default was not honored.
    connect(worker,
            &CleanupWorker::recycleFallbackItems,
            this,
            &AdvancedUninstallController::recycleFallbackItems);
    connect(
        worker, &CleanupWorker::failed, this, &AdvancedUninstallController::onCleanupWorkerFailed);
    connect(worker,
            &CleanupWorker::cancelled,
            this,
            &AdvancedUninstallController::onCleanupWorkerCancelled);
    connect(worker, &CleanupWorker::progress, this, [this](int current, int total, const QString&) {
        Q_EMIT progressUpdate(current, total);
    });
}

void AdvancedUninstallController::cancelOperation() {
    // Cancel enumeration if running; the persistent worker thread stays alive for reuse and is
    // torn down only in cleanupWorkers().
    if (m_state == State::Enumerating && m_enumerator) {
        m_enumerator->requestCancel();
        setState(State::Idle);
    }

    if (m_uninstall_worker && m_uninstall_worker->isExecuting()) {
        m_uninstall_worker->requestStop();
    }
    if (m_cleanup_worker && m_cleanup_worker->isExecuting()) {
        m_cleanup_worker->requestStop();
    }
}

// -- Batch Uninstall ---------------------------------------------------------

void AdvancedUninstallController::addToQueue(const ProgramInfo& program,
                                             ScanLevel scanLevel,
                                             bool autoCleanSafe) {
    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage("Cannot add to queue: program name is empty.", kStatusTimeoutShortMs);
        return;
    }

    UninstallQueueItem item;
    item.program = program;
    item.scanLevel = scanLevel;
    item.autoCleanSafeLeftovers = autoCleanSafe;
    item.status = UninstallQueueItem::Status::Queued;
    m_queue.append(item);
}

void AdvancedUninstallController::removeFromQueue(int index) {
    if (index >= 0 && index < m_queue.size()) {
        m_queue.removeAt(index);
    }
}

QVector<UninstallQueueItem> AdvancedUninstallController::queue() const {
    return m_queue;
}

void AdvancedUninstallController::clearQueue() {
    m_queue.clear();
}

void AdvancedUninstallController::startBatchUninstall(bool createRestorePoint) {
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Another operation is already in progress.", kStatusTimeoutShortMs);
        return;
    }

    if (m_queue.isEmpty()) {
        Q_EMIT statusMessage("Batch queue is empty.", kStatusTimeoutShortMs);
        return;
    }

    m_batchIndex = -1;
    m_batchRestorePointCreated = false;
    m_batchAutoCleanItems.clear();

    // Create a single restore point for the entire batch
    if (createRestorePoint) {
        QString desc = QString("SAK: Before batch uninstall (%1 programs)").arg(m_queue.size());
        m_batchRestorePointCreated = m_restore_manager->createRestorePoint(desc);
    }

    processNextQueueItem();
}

// -- Settings ----------------------------------------------------------------

void AdvancedUninstallController::loadSettings() {
    auto& cfg = ConfigManager::instance();

    int raw_scan_level =
        cfg.getValue("advuninstall/default_scan_level", kDefaultScanLevelValue).toInt();
    if (raw_scan_level < 0 || raw_scan_level > kMaxScanLevelValue) {
        raw_scan_level = kDefaultScanLevelValue;
    }
    m_defaultScanLevel = static_cast<ScanLevel>(raw_scan_level);
    m_autoRestorePoint = cfg.getValue("advuninstall/auto_restore_point", true).toBool();
    m_autoCleanSafe = cfg.getValue("advuninstall/auto_clean_safe", true).toBool();
    m_showSystemComponents = cfg.getValue("advuninstall/show_system_components", false).toBool();
    m_useRecycleBin = cfg.getValue("advuninstall/use_recycle_bin", false).toBool();
    m_selectAllByDefault = cfg.getValue("advuninstall/select_all_by_default", false).toBool();
}

void AdvancedUninstallController::saveSettings() {
    auto& cfg = ConfigManager::instance();

    cfg.setValue("advuninstall/default_scan_level", static_cast<int>(m_defaultScanLevel));
    cfg.setValue("advuninstall/auto_restore_point", m_autoRestorePoint);
    cfg.setValue("advuninstall/auto_clean_safe", m_autoCleanSafe);
    cfg.setValue("advuninstall/show_system_components", m_showSystemComponents);
    cfg.setValue("advuninstall/use_recycle_bin", m_useRecycleBin);
    cfg.setValue("advuninstall/select_all_by_default", m_selectAllByDefault);
}

bool AdvancedUninstallController::autoRestorePoint() const {
    return m_autoRestorePoint;
}

void AdvancedUninstallController::setAutoRestorePoint(bool enabled) {
    m_autoRestorePoint = enabled;
}

bool AdvancedUninstallController::autoCleanSafe() const {
    return m_autoCleanSafe;
}

void AdvancedUninstallController::setAutoCleanSafe(bool enabled) {
    m_autoCleanSafe = enabled;
}

ScanLevel AdvancedUninstallController::defaultScanLevel() const {
    return m_defaultScanLevel;
}

void AdvancedUninstallController::setDefaultScanLevel(ScanLevel level) {
    m_defaultScanLevel = level;
}

bool AdvancedUninstallController::showSystemComponents() const {
    return m_showSystemComponents;
}

void AdvancedUninstallController::setShowSystemComponents(bool show) {
    m_showSystemComponents = show;
}

bool AdvancedUninstallController::useRecycleBin() const {
    return m_useRecycleBin;
}

void AdvancedUninstallController::setUseRecycleBin(bool enabled) {
    m_useRecycleBin = enabled;
}

bool AdvancedUninstallController::selectAllByDefault() const {
    return m_selectAllByDefault;
}

void AdvancedUninstallController::setSelectAllByDefault(bool enabled) {
    m_selectAllByDefault = enabled;
}

RestorePointManager* AdvancedUninstallController::restorePointManager() const {
    return m_restore_manager.get();
}

// -- Private Slots -----------------------------------------------------------

void AdvancedUninstallController::onEnumerationFinished(int generation,
                                                        QVector<ProgramInfo> programs) {
    // Drop a stale completion: a run cancelled then restarted would otherwise apply the OLD (or an
    // un-cancelled superseded) result to the NEW run. Also guard the post-cancel Idle state.
    if (generation != m_enum_generation || m_state != State::Enumerating) {
        return;
    }

    // The enumerator stays on its persistent worker thread; nothing to move back.
    m_programs = programs;
    setState(State::Idle);
    Q_EMIT statusMessage(QString("Found %1 installed programs.").arg(programs.size()),
                         kStatusTimeoutLongMs);
    Q_EMIT enumerationFinished(programs);
}

void AdvancedUninstallController::onEnumerationFailed(int generation, const QString& error) {
    // Drop a stale failure (a superseded run's cancel) so it cannot force the NEW run's state to
    // Idle.
    if (generation != m_enum_generation || m_state != State::Enumerating) {
        return;
    }

    // The enumerator stays on its persistent worker thread; nothing to move back.
    setState(State::Idle);
    Q_EMIT statusMessage(QString("Enumeration failed: %1").arg(error), kStatusTimeoutLongMs);
    Q_EMIT enumerationFailed(error);
}

void AdvancedUninstallController::onUninstallComplete(UninstallReport report) {
    m_lastReport = report;

    // If in batch mode, process next
    if (m_batchIndex >= 0 && m_batchIndex < m_queue.size()) {
        auto& qi = m_queue[m_batchIndex];
        qi.report = report;
        qi.status = (report.uninstallResult == UninstallReport::UninstallResult::Success ||
                     report.uninstallResult == UninstallReport::UninstallResult::Skipped)
                        ? UninstallQueueItem::Status::Completed
                        : UninstallQueueItem::Status::Failed;
        Q_EMIT queueItemStatusChanged(m_batchIndex, qi.status);

        // Accumulate this item's SAFE leftovers for a single auto-clean pass at batch end (only if
        // the item opted in). Running a cleanup now would race the next queued uninstall, so it is
        // deferred until the whole batch is done and no uninstall worker is active.
        if (qi.autoCleanSafeLeftovers) {
            m_batchAutoCleanItems.append(safeLeftovers(report.foundLeftovers));
        }

        // Process next queued item
        processNextQueueItem();
        return;
    }

    setState(State::Idle);
    Q_EMIT uninstallFinished(report);
    Q_EMIT statusMessage(QString("Uninstall of %1 complete. Found %2 leftovers.")
                             .arg(report.programName)
                             .arg(report.foundLeftovers.size()),
                         kStatusTimeoutLongMs);

    // Auto-clean the SAFE leftovers if enabled (Review/Risky always stay for human review). Starts
    // a recoverable, denylist-screened CleanupWorker; a no-op when disabled or nothing is safe.
    maybeAutoCleanSafeLeftovers(report, m_autoCleanSafe);
}

void AdvancedUninstallController::onUninstallWorkerFailed(int /*errorCode*/,
                                                          const QString& message) {
    if (m_batchIndex >= 0 && m_batchIndex < m_queue.size()) {
        m_queue[m_batchIndex].status = UninstallQueueItem::Status::Failed;
        Q_EMIT queueItemStatusChanged(m_batchIndex, UninstallQueueItem::Status::Failed);
        processNextQueueItem();
        return;
    }

    setState(State::Idle);
    Q_EMIT uninstallFailed(message);
    Q_EMIT statusMessage(QString("Uninstall failed: %1").arg(message), kStatusTimeoutLongMs);
}

void AdvancedUninstallController::onUninstallWorkerCancelled() {
    if (m_batchIndex >= 0 && m_batchIndex < m_queue.size()) {
        m_queue[m_batchIndex].status = UninstallQueueItem::Status::Cancelled;
        // Count batch results so far
        int batch_succeeded = 0;
        int batch_failed = 0;
        for (const auto& qi : m_queue) {
            if (qi.status == UninstallQueueItem::Status::Completed) {
                ++batch_succeeded;
            } else if (qi.status == UninstallQueueItem::Status::Failed) {
                ++batch_failed;
            }
        }
        m_batchIndex = -1;
        setState(State::Idle);
        Q_EMIT batchFinished(batch_succeeded, batch_failed);
        Q_EMIT uninstallCancelled();
        Q_EMIT statusMessage("Batch cancelled.", kStatusTimeoutShortMs);
        return;
    }

    setState(State::Idle);
    Q_EMIT uninstallCancelled();
    Q_EMIT statusMessage("Uninstall cancelled.", kStatusTimeoutShortMs);
}

void AdvancedUninstallController::onCleanupComplete(int succeeded,
                                                    int failed,
                                                    qint64 bytesRecovered) {
    setState(State::Idle);
    Q_EMIT cleanupFinished(succeeded, failed, bytesRecovered);

    QString msg = QString("Cleanup complete: %1 items removed").arg(succeeded);
    if (failed > 0) {
        msg += QString(", %1 failed").arg(failed);
    }
    if (bytesRecovered > 0) {
        double mb = bytesRecovered / kBytesPerMBf;
        msg += QString(" (%1 MB recovered)").arg(mb, 0, 'f', kRecoveredSizeDisplayPrecision);
    }
    Q_EMIT statusMessage(msg, kStatusTimeoutLongMs);
}

void AdvancedUninstallController::onCleanupWorkerFailed(int /*errorCode*/, const QString& message) {
    setState(State::Idle);
    Q_EMIT statusMessage(QString("Cleanup failed: %1").arg(message), kStatusTimeoutLongMs);
}

void AdvancedUninstallController::onCleanupWorkerCancelled() {
    // A cancelled cleanup is NOT a completed one: surface it as cancelled rather
    // than emitting cleanupFinished with partial (success-looking) counts.
    setState(State::Idle);
    Q_EMIT cleanupCancelled();
    Q_EMIT statusMessage("Cleanup cancelled.", kStatusTimeoutShortMs);
}

// -- Private -----------------------------------------------------------------

void AdvancedUninstallController::setState(State newState) {
    if (m_state != newState) {
        m_state = newState;
        Q_EMIT stateChanged(newState);
    }
}

void AdvancedUninstallController::stopEnumThread() {
    if (!m_enumThread) {
        return;
    }
    m_enumerator->requestCancel();
    m_enumThread->quit();
    if (!m_enumThread->wait(kThreadWaitMs)) {
        m_enumThread->terminate();
        if (!m_enumThread->wait(kThreadWaitMs)) {
            // Cannot safely delete a still-running QThread: leave the (parented)
            // handle rather than abort. This is the inherent unkillable-thread case.
            sak::logError("Enumeration thread refused to stop");
            return;
        }
    }
    // Join confirmed: delete here, before ~AdvancedUninstallController destroys the
    // m_enumerator unique_ptr that a running enumerateAll() would otherwise use.
    delete m_enumThread;
    m_enumThread = nullptr;
}

void AdvancedUninstallController::cleanupWorkers() {
    stopEnumThread();
    // Cooperative stop + detach-if-refuses, never terminate()+destroy a live QThread.
    retireWorker(m_uninstall_worker);
    retireWorker(m_cleanup_worker);
}

void AdvancedUninstallController::connectUninstallWorkerSignals(bool includeNativeSignals) {
    if (includeNativeSignals) {
        connect(m_uninstall_worker.get(),
                &UninstallWorker::nativeUninstallerStarted,
                this,
                &AdvancedUninstallController::nativeUninstallerStarted);
        connect(m_uninstall_worker.get(),
                &UninstallWorker::nativeUninstallerFinished,
                this,
                &AdvancedUninstallController::nativeUninstallerFinished);
    }

    connect(m_uninstall_worker.get(),
            &UninstallWorker::restorePointCreated,
            this,
            &AdvancedUninstallController::restorePointCreated);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::leftoverScanStarted,
            this,
            &AdvancedUninstallController::leftoverScanStarted);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::leftoverScanProgress,
            this,
            &AdvancedUninstallController::leftoverScanProgress);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::leftoverScanFinished,
            this,
            &AdvancedUninstallController::leftoverScanFinished);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::uninstallComplete,
            this,
            &AdvancedUninstallController::onUninstallComplete);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::failed,
            this,
            &AdvancedUninstallController::onUninstallWorkerFailed);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::cancelled,
            this,
            &AdvancedUninstallController::onUninstallWorkerCancelled);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::progress,
            this,
            [this](int current, int total, const QString& msg) {
                Q_EMIT uninstallProgress(total > 0 ? (current * kPercentMax / total) : 0, msg);
                Q_EMIT progressUpdate(current, total);
            });
}

void AdvancedUninstallController::connectBatchUninstallWorkerSignals() {
    connect(m_uninstall_worker.get(),
            &UninstallWorker::uninstallComplete,
            this,
            &AdvancedUninstallController::onUninstallComplete);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::failed,
            this,
            &AdvancedUninstallController::onUninstallWorkerFailed);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::cancelled,
            this,
            &AdvancedUninstallController::onUninstallWorkerCancelled);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::leftoverScanFinished,
            this,
            &AdvancedUninstallController::leftoverScanFinished);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::progress,
            this,
            [this](int current, int total, const QString& msg) {
                Q_EMIT uninstallProgress(total > 0 ? (current * kPercentMax / total) : 0, msg);
                Q_EMIT progressUpdate(current, total);
            });
}

void AdvancedUninstallController::finishBatchUninstall() {
    int succeeded = 0;
    int failed = 0;
    for (const auto& qi : m_queue) {
        if (qi.status == UninstallQueueItem::Status::Completed) {
            ++succeeded;
        } else if (qi.status == UninstallQueueItem::Status::Failed) {
            ++failed;
        }
    }

    setState(State::Idle);
    Q_EMIT batchFinished(succeeded, failed);
    Q_EMIT statusMessage(
        QString("Batch uninstall complete: %1 succeeded, %2 failed.").arg(succeeded).arg(failed),
        kStatusTimeoutLongMs);
    m_batchIndex = -1;

    // Now that no uninstall worker is running, auto-clean the SAFE leftovers accumulated from every
    // opted-in batch item in one recoverable, denylist-screened CleanupWorker pass.
    const QVector<LeftoverItem> auto_clean = m_batchAutoCleanItems;
    m_batchAutoCleanItems.clear();
    if (!auto_clean.isEmpty()) {
        Q_EMIT autoCleanSafeStarted(static_cast<int>(auto_clean.size()));
        Q_EMIT statusMessage(QString("Auto-cleaning %1 safe leftover item(s) from the batch...")
                                 .arg(auto_clean.size()),
                             0);
        cleanLeftovers(auto_clean);
    }
}

void AdvancedUninstallController::processNextQueueItem() {
    if (m_queue.isEmpty()) {
        return;
    }

    ++m_batchIndex;

    if (m_batchIndex >= m_queue.size()) {
        finishBatchUninstall();
        return;
    }

    auto& qi = m_queue[m_batchIndex];
    qi.status = UninstallQueueItem::Status::InProgress;
    Q_EMIT queueItemStatusChanged(m_batchIndex, UninstallQueueItem::Status::InProgress);

    // Don't create individual restore points in batch (already done).
    // Ensure the previous worker is safely retired (detach if it refuses to stop)
    // before its unique_ptr is overwritten, so we never destroy a live QThread.
    retireWorker(m_uninstall_worker);
    m_uninstall_worker = std::make_unique<UninstallWorker>(
        qi.program, uninstallModeForProgram(qi.program), qi.scanLevel, false, this);
    connectBatchUninstallWorkerSignals();

    Q_EMIT uninstallStarted(qi.program.displayName);
    Q_EMIT statusMessage(QString("Batch %1/%2: Uninstalling %3...")
                             .arg(m_batchIndex + 1)
                             .arg(m_queue.size())
                             .arg(qi.program.displayName),
                         0);

    m_uninstall_worker->start();
}

}  // namespace sak
