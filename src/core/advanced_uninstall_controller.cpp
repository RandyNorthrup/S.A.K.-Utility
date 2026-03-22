// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_controller.cpp
/// @brief Orchestrates uninstall pipeline, manages queue, and persists settings

#include "sak/advanced_uninstall_controller.h"

#include "sak/cleanup_worker.h"
#include "sak/config_manager.h"
#include "sak/program_enumerator.h"
#include "sak/restore_point_manager.h"
#include "sak/uninstall_worker.h"

#include <QThread>
#include <QTimer>

namespace sak {

namespace {
constexpr int kStatusTimeoutShortMs = 3000;
constexpr int kStatusTimeoutLongMs = 5000;
constexpr int kThreadWaitMs = 5000;
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

    // Run enumeration in a background thread so the GUI stays responsive
    if (m_enumThread) {
        m_enumThread->quit();
        m_enumThread->wait(kThreadWaitMs);
        m_enumThread = nullptr;  // previous thread cleans up via deleteLater
    }
    m_enumThread = new QThread(this);
    m_enumerator->moveToThread(m_enumThread);
    m_enumerator->resetCancel();
    connect(m_enumThread, &QThread::started, m_enumerator.get(), &ProgramEnumerator::enumerateAll);
    connect(m_enumThread, &QThread::finished, m_enumThread, &QObject::deleteLater);
    m_enumThread->start();
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

    // Determine mode based on program source
    UninstallWorker::Mode mode;
    if (program.source == ProgramInfo::Source::UWP ||
        program.source == ProgramInfo::Source::Provisioned) {
        mode = UninstallWorker::Mode::UwpRemove;
    } else {
        mode = UninstallWorker::Mode::Standard;
    }

    m_uninstall_worker =
        std::make_unique<UninstallWorker>(program, mode, scanLevel, createRestorePoint, this);

    // Connect worker signals
    connect(m_uninstall_worker.get(),
            &UninstallWorker::nativeUninstallerStarted,
            this,
            &AdvancedUninstallController::nativeUninstallerStarted);
    connect(m_uninstall_worker.get(),
            &UninstallWorker::nativeUninstallerFinished,
            this,
            &AdvancedUninstallController::nativeUninstallerFinished);
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
                Q_EMIT uninstallProgress(total > 0 ? (current * 100 / total) : 0, msg);
                Q_EMIT progressUpdate(current, total);
            });

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

    m_uninstall_worker = std::make_unique<UninstallWorker>(
        program, UninstallWorker::Mode::ForcedUninstall, scanLevel, createRestorePoint, this);

    // Connect worker signals (same as standard uninstall)
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
                Q_EMIT uninstallProgress(total > 0 ? (current * 100 / total) : 0, msg);
                Q_EMIT progressUpdate(current, total);
            });

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

    setState(State::Cleaning);

    int total = 0;
    for (const auto& item : selectedItems) {
        if (item.selected) {
            ++total;
        }
    }

    Q_EMIT cleanupStarted(total);
    Q_EMIT statusMessage(QString("Cleaning %1 leftover items...").arg(total), 0);

    m_cleanup_worker = std::make_unique<CleanupWorker>(selectedItems, m_useRecycleBin, this);

    connect(m_cleanup_worker.get(),
            &CleanupWorker::itemCleaned,
            this,
            &AdvancedUninstallController::itemCleaned);
    connect(m_cleanup_worker.get(),
            &CleanupWorker::cleanupComplete,
            this,
            &AdvancedUninstallController::onCleanupComplete);
    connect(m_cleanup_worker.get(),
            &CleanupWorker::rebootPendingItems,
            this,
            &AdvancedUninstallController::rebootPendingItems);
    connect(m_cleanup_worker.get(),
            &CleanupWorker::failed,
            this,
            &AdvancedUninstallController::onCleanupWorkerFailed);
    connect(m_cleanup_worker.get(),
            &CleanupWorker::progress,
            this,
            [this](int current, int total, const QString& /*msg*/) {
                Q_EMIT progressUpdate(current, total);
            });

    m_cleanup_worker->start();
}

void AdvancedUninstallController::cancelOperation() {
    // Cancel enumeration if running
    if (m_state == State::Enumerating && m_enumerator) {
        m_enumerator->requestCancel();
        if (m_enumThread) {
            m_enumThread->quit();
            if (!m_enumThread->wait(kThreadWaitMs)) {
                m_enumThread->terminate();
                m_enumThread->wait(kThreadWaitMs);
            }
            m_enumThread = nullptr;
        }
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

    int raw_scan_level = cfg.getValue("advuninstall/default_scan_level", 1).toInt();
    if (raw_scan_level < 0 || raw_scan_level > 2) {
        raw_scan_level = 1;  // Default to Moderate if out of range
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

void AdvancedUninstallController::onEnumerationFinished(QVector<ProgramInfo> programs) {
    // Guard against stale signal after cancelOperation() already cleaned up
    if (m_state != State::Enumerating) {
        return;
    }

    // Move enumerator back to main thread and stop the enum thread
    m_enumerator->moveToThread(thread());
    if (m_enumThread) {
        m_enumThread->quit();
        m_enumThread = nullptr;  // deleteLater handles cleanup
    }

    m_programs = programs;
    setState(State::Idle);
    Q_EMIT statusMessage(QString("Found %1 installed programs.").arg(programs.size()),
                         kStatusTimeoutLongMs);
    Q_EMIT enumerationFinished(programs);
}

void AdvancedUninstallController::onEnumerationFailed(const QString& error) {
    // Guard against stale signal after cancelOperation() already cleaned up
    if (m_state != State::Enumerating) {
        return;
    }

    // Move enumerator back to main thread and stop the enum thread
    m_enumerator->moveToThread(thread());
    if (m_enumThread) {
        m_enumThread->quit();
        m_enumThread = nullptr;  // deleteLater handles cleanup
    }

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
        double mb = bytesRecovered / (1024.0 * 1024.0);
        msg += QString(" (%1 MB recovered)").arg(mb, 0, 'f', 1);
    }
    Q_EMIT statusMessage(msg, kStatusTimeoutLongMs);
}

void AdvancedUninstallController::onCleanupWorkerFailed(int /*errorCode*/, const QString& message) {
    setState(State::Idle);
    Q_EMIT statusMessage(QString("Cleanup failed: %1").arg(message), kStatusTimeoutLongMs);
}

// -- Private -----------------------------------------------------------------

void AdvancedUninstallController::setState(State newState) {
    if (m_state != newState) {
        m_state = newState;
        Q_EMIT stateChanged(newState);
    }
}

void AdvancedUninstallController::cleanupWorkers() {
    // Clean up enumeration thread
    if (m_enumThread) {
        m_enumerator->requestCancel();
        m_enumThread->quit();
        if (!m_enumThread->wait(kStatusTimeoutShortMs)) {
            m_enumThread->terminate();
            m_enumThread->wait(kStatusTimeoutShortMs);
        }
        m_enumThread = nullptr;
    }

    if (m_uninstall_worker) {
        if (m_uninstall_worker->isExecuting()) {
            m_uninstall_worker->requestStop();
            if (!m_uninstall_worker->wait(kThreadWaitMs)) {
                m_uninstall_worker->terminate();
                m_uninstall_worker->wait();
            }
        }
        m_uninstall_worker.reset();
    }

    if (m_cleanup_worker) {
        if (m_cleanup_worker->isExecuting()) {
            m_cleanup_worker->requestStop();
            if (!m_cleanup_worker->wait(kThreadWaitMs)) {
                m_cleanup_worker->terminate();
                m_cleanup_worker->wait();
            }
        }
        m_cleanup_worker.reset();
    }
}

void AdvancedUninstallController::processNextQueueItem() {
    if (m_queue.isEmpty()) {
        return;
    }

    ++m_batchIndex;

    if (m_batchIndex >= m_queue.size()) {
        // All done
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
        Q_EMIT statusMessage(QString("Batch uninstall complete: %1 succeeded, %2 failed.")
                                 .arg(succeeded)
                                 .arg(failed),
                             kStatusTimeoutLongMs);
        m_batchIndex = -1;
        return;
    }

    auto& qi = m_queue[m_batchIndex];
    qi.status = UninstallQueueItem::Status::InProgress;
    Q_EMIT queueItemStatusChanged(m_batchIndex, UninstallQueueItem::Status::InProgress);

    // Determine uninstall mode
    UninstallWorker::Mode mode;
    if (qi.program.source == ProgramInfo::Source::UWP ||
        qi.program.source == ProgramInfo::Source::Provisioned) {
        mode = UninstallWorker::Mode::UwpRemove;
    } else {
        mode = UninstallWorker::Mode::Standard;
    }

    // Don't create individual restore points in batch (already done)
    // Ensure previous worker is fully stopped before creating a new one
    if (m_uninstall_worker) {
        m_uninstall_worker->wait(kThreadWaitMs);
    }
    m_uninstall_worker =
        std::make_unique<UninstallWorker>(qi.program, mode, qi.scanLevel, false, this);

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
                Q_EMIT uninstallProgress(total > 0 ? (current * 100 / total) : 0, msg);
                Q_EMIT progressUpdate(current, total);
            });

    Q_EMIT uninstallStarted(qi.program.displayName);
    Q_EMIT statusMessage(QString("Batch %1/%2: Uninstalling %3...")
                             .arg(m_batchIndex + 1)
                             .arg(m_queue.size())
                             .arg(qi.program.displayName),
                         0);

    m_uninstall_worker->start();
}

}  // namespace sak
