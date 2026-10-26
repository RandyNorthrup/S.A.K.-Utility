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

#include <QFutureWatcher>
#include <QMetaType>
#include <QtConcurrent/QtConcurrentRun>
#include <QThread>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <array>
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
// thread and, if it refuses within the bound, hands it to the shared worker custody (reported,
// destroyed once it finally exits) instead of force-terminating and destroying it.
template <typename WorkerT>
void retireWorker(WorkerPtr<WorkerT>& worker, const QObject* owner) {
    if (!worker) {
        return;
    }
    // Sever every signal back to the controller BEFORE letting go. A retained worker outlives
    // the operation that created it, and its late uninstallComplete / failed / cancelled /
    // cleanupComplete carries no run identity -- the controller would apply it to whatever runs
    // NEXT, mis-marking a queued program, re-emitting a stale report or forcing the live run to
    // Idle.
    QObject::disconnect(worker.get(), nullptr, owner, nullptr);
    releaseWorker(worker, kThreadWaitMs);
}

// Read a persisted boolean preference fail-closed. QVariant::toBool() coerces ANY non-empty,
// non-"0"/"false" string to true, so a truncated write or an attacker-edited config value would
// silently become a setting the user never chose. A value that is not recognizably a boolean is
// refused and @p malformed_safe (the conservative choice for that specific preference, not
// necessarily the shipped default) is kept instead.
bool loadBoolSetting(const char* key, bool absent_default, bool malformed_safe, bool& valid) {
    valid = true;
    const QVariant raw = ConfigManager::instance().getValue(key, absent_default);
    if (raw.typeId() == QMetaType::Bool) {
        return raw.toBool();
    }
    const QString text = raw.toString().trimmed().toLower();
    if (text == QLatin1String("true") || text == QLatin1String("1")) {
        return true;
    }
    if (text == QLatin1String("false") || text == QLatin1String("0")) {
        return false;
    }
    sak::logError("Malformed boolean setting {}; using the fail-closed value", key);
    valid = false;
    return malformed_safe;
}

bool isValidScanLevel(ScanLevel level) {
    const int value = static_cast<int>(level);
    return value >= 0 && value <= kMaxScanLevelValue;
}

// A registry key path is well-formed only when it is rooted at a recognized hive. Anything else is
// malformed and must never reach the destructive RegistryOnly worker (defense in depth at the
// controller layer; the worker does its own denylist/symlink screening).
bool isHiveRootedKeyPath(const QString& path) {
    static const QStringList kHiveRoots = {
        QStringLiteral("HKLM\\"),
        QStringLiteral("HKCU\\"),
        QStringLiteral("HKCR\\"),
        QStringLiteral("HKU\\"),
        QStringLiteral("HKCC\\"),
    };
    return std::ranges::any_of(kHiveRoots, [&path](const QString& root) {
        return path.startsWith(root, Qt::CaseInsensitive);
    });
}

UninstallWorker::Mode uninstallModeForProgram(const ProgramInfo& program) {
    if (program.source == ProgramInfo::Source::UWP ||
        program.source == ProgramInfo::Source::Provisioned) {
        return UninstallWorker::Mode::UwpRemove;
    }
    return UninstallWorker::Mode::Standard;
}

bool recordCleanupSuccess(UninstallReport& report, LeftoverItem::Type type) {
    switch (type) {
    case LeftoverItem::Type::File:
        ++report.filesDeleted;
        return true;
    case LeftoverItem::Type::Folder:
        ++report.foldersDeleted;
        return true;
    case LeftoverItem::Type::StartupEntry:
        ++report.startupEntriesRemoved;
        return true;
    default:
        return false;
    }
}
}  // namespace

AdvancedUninstallController::AdvancedUninstallController(QObject* parent)
    : QObject(parent)
    , m_enumerator(std::make_unique<ProgramEnumerator>())
    , m_batchRestorePointMaker([](const QString& description) {
        RestorePointManager manager;  // created on the pool thread that blocks on it
        return manager.ensureRestorePoint(description);
    }) {
    // Register custom types for cross-thread signal delivery
    qRegisterMetaType<sak::ProgramInfo>("sak::ProgramInfo");
    qRegisterMetaType<sak::LeftoverItem>("sak::LeftoverItem");
    qRegisterMetaType<sak::UninstallReport>("sak::UninstallReport");
    qRegisterMetaType<sak::ScanLevel>("sak::ScanLevel");
    qRegisterMetaType<QVector<sak::ProgramInfo>>("QVector<sak::ProgramInfo>");
    qRegisterMetaType<QVector<sak::LeftoverItem>>("QVector<sak::LeftoverItem>");

    // Wire enumerator signals
    connect(m_enumerator.get(), &ProgramEnumerator::enumerationStarted, this, [this]() {
        if (m_state == State::Enumerating) {
            Q_EMIT enumerationStarted();
        }
    });
    connect(m_enumerator.get(),
            &ProgramEnumerator::enumerationProgress,
            this,
            [this](int current, int total) {
                if (m_state == State::Enumerating) {
                    Q_EMIT enumerationProgress(current, total);
                }
            });
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

    if (!loadSettings()) {
        sak::logError("Advanced Uninstall settings load failed; safe values are active");
    }
}

AdvancedUninstallController::~AdvancedUninstallController() {
    cleanupWorkers();
    if (!saveSettings()) {
        sak::logError("Advanced Uninstall settings save failed during controller teardown");
    }
}

AdvancedUninstallController::State AdvancedUninstallController::currentState() const {
    return m_state;
}

// -- Program Enumeration -----------------------------------------------------

void AdvancedUninstallController::refreshPrograms() {
    if (m_enumerationStopping) {
        Q_EMIT statusMessage("Waiting for cancelled enumeration to stop.", kStatusTimeoutShortMs);
        return;
    }
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
    if ((m_enumThread == nullptr) || !m_enumThread->isRunning()) {
        setState(State::Idle);
        Q_EMIT statusMessage("Enumeration thread unavailable.", kStatusTimeoutShortMs);
        return;
    }
    m_enumerator->resetCancel();
    // Bump the generation so a still-running prior enumeration (cancelled, but whose cooperative
    // stop was cleared by resetCancel above) cannot pass its stale completion off as this run's.
    const int generation = ++m_enumGeneration;
    QMetaObject::invokeMethod(
        m_enumerator.get(),
        [enumerator = m_enumerator.get(), generation]() { enumerator->enumerateAll(generation); },
        Qt::QueuedConnection);
}

QVector<ProgramInfo> AdvancedUninstallController::programs() const {
    return m_programs;
}

bool sameQueuedProgram(const ProgramInfo& lhs, const ProgramInfo& rhs) {
    if (lhs.source != rhs.source) {
        return false;
    }
    if (!lhs.packageFullName.isEmpty() || !rhs.packageFullName.isEmpty()) {
        return !lhs.packageFullName.isEmpty() && !rhs.packageFullName.isEmpty() &&
               lhs.packageFullName.compare(rhs.packageFullName, Qt::CaseInsensitive) == 0;
    }
    if (!lhs.registryKeyPath.isEmpty() || !rhs.registryKeyPath.isEmpty()) {
        return !lhs.registryKeyPath.isEmpty() && !rhs.registryKeyPath.isEmpty() &&
               lhs.registryKeyPath.compare(rhs.registryKeyPath, Qt::CaseInsensitive) == 0;
    }
    const std::array same_fallback_identity = {
        lhs.displayName.compare(rhs.displayName, Qt::CaseInsensitive) == 0,
        lhs.publisher.compare(rhs.publisher, Qt::CaseInsensitive) == 0,
        lhs.displayVersion.compare(rhs.displayVersion, Qt::CaseInsensitive) == 0,
        lhs.installLocation.compare(rhs.installLocation, Qt::CaseInsensitive) == 0,
        lhs.uninstallString.compare(rhs.uninstallString, Qt::CaseInsensitive) == 0,
        lhs.quietUninstallString.compare(rhs.quietUninstallString, Qt::CaseInsensitive) == 0};
    return std::ranges::all_of(same_fallback_identity, [](bool same) { return same; });
}

bool AdvancedUninstallController::refuseWhileEnumerationStopping(const QString& operation) {
    if (!m_enumerationStopping) {
        return false;
    }
    Q_EMIT statusMessage(QStringLiteral(
                             "Cannot %1 while cancelled program enumeration is still stopping.")
                             .arg(operation),
                         kStatusTimeoutShortMs);
    return true;
}

// -- Uninstall Operations ----------------------------------------------------

void AdvancedUninstallController::uninstallProgram(const ProgramInfo& program,
                                                   ScanLevel scan_level,
                                                   bool create_restore_point,
                                                   bool auto_clean_safe) {
    if (refuseWhileEnumerationStopping(QStringLiteral("uninstall"))) {
        return;
    }
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Another operation is already in progress.", kStatusTimeoutShortMs);
        return;
    }

    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage("Cannot uninstall: program name is empty.", kStatusTimeoutShortMs);
        return;
    }

    if (!isValidScanLevel(scan_level)) {
        Q_EMIT statusMessage("Cannot uninstall: scan level is invalid.", kStatusTimeoutShortMs);
        return;
    }

    m_currentAutoCleanSafe = auto_clean_safe;
    setState(State::Uninstalling);

    // Safely dispose of any prior worker whose thread may still be exiting before the
    // unique_ptr is overwritten (B10-09).
    const int generation = ++m_uninstallGeneration;
    retireWorker(m_uninstallWorker, this);
    m_uninstallWorker = makeWorker<UninstallWorker>(
        program, uninstallModeForProgram(program), scan_level, create_restore_point, this);
    connectUninstallWorkerSignals(true, generation);

    Q_EMIT uninstallStarted(program.displayName);
    Q_EMIT statusMessage(QString("Uninstalling %1...").arg(program.displayName), 0);

    m_uninstallWorker->start();
}

void AdvancedUninstallController::forceUninstall(const ProgramInfo& program,
                                                 ScanLevel scan_level,
                                                 bool create_restore_point) {
    if (refuseWhileEnumerationStopping(QStringLiteral("force uninstall"))) {
        return;
    }
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Another operation is already in progress.", kStatusTimeoutShortMs);
        return;
    }

    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage("Cannot uninstall: program name is empty.", kStatusTimeoutShortMs);
        return;
    }

    if (!isValidScanLevel(scan_level)) {
        Q_EMIT statusMessage("Cannot force uninstall: scan level is invalid.",
                             kStatusTimeoutShortMs);
        return;
    }

    m_currentAutoCleanSafe = false;  // Force uninstall always shows results for review
    setState(State::Uninstalling);

    const int generation = ++m_uninstallGeneration;
    retireWorker(m_uninstallWorker, this);
    m_uninstallWorker = makeWorker<UninstallWorker>(
        program, UninstallWorker::Mode::ForcedUninstall, scan_level, create_restore_point, this);

    connectUninstallWorkerSignals(false, generation);

    Q_EMIT uninstallStarted(program.displayName);
    Q_EMIT statusMessage(QString("Forced uninstall: %1...").arg(program.displayName), 0);

    m_uninstallWorker->start();
}

void AdvancedUninstallController::removeRegistryEntry(const ProgramInfo& program) {
    if (refuseWhileEnumerationStopping(QStringLiteral("remove a registry entry"))) {
        return;
    }
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Another operation is already in progress.", kStatusTimeoutShortMs);
        return;
    }

    if (program.registryKeyPath.isEmpty()) {
        Q_EMIT statusMessage("Cannot remove: no registry key path.", kStatusTimeoutShortMs);
        return;
    }

    // Fail closed on a malformed (non-hive-rooted) key path before spawning a destructive
    // RegistryOnly worker, so an unscreened string can never drive a registry delete.
    if (!isHiveRootedKeyPath(program.registryKeyPath)) {
        Q_EMIT statusMessage("Cannot remove: registry key path is not hive-rooted.",
                             kStatusTimeoutShortMs);
        return;
    }

    setState(State::Uninstalling);

    m_currentAutoCleanSafe = false;
    const int generation = ++m_uninstallGeneration;
    retireWorker(m_uninstallWorker, this);
    m_uninstallWorker = makeWorker<UninstallWorker>(
        program, UninstallWorker::Mode::RegistryOnly, ScanLevel::Safe, false, this);

    connectUninstallWorkerSignals(false, generation);
    Q_EMIT uninstallStarted(program.displayName);
    m_uninstallWorker->start();
}

AdvancedUninstallController::CleanupScreenResult AdvancedUninstallController::screenCleanupItems(
    const QVector<LeftoverItem>& selected_items) {
    CleanupScreenResult result;
    result.allowed.reserve(selected_items.size());
    result.refused.reserve(selected_items.size());
    for (const auto& item : selected_items) {
        if (!item.selected) {
            continue;
        }
        // A report-only item carries no deletion authority however it got selected: the
        // scanner surfaced it for visibility because it could not tie the path to this
        // program (an unverifiable registry InstallLocation). Refuse it with a reason
        // rather than dropping it silently.
        const QString refusal = item.deletable
                                    ? cleanupItemRefusal(item)
                                    : QStringLiteral(
                                          "reported for review only -- this path could not be "
                                          "tied to the program being uninstalled");
        if (refusal.isEmpty()) {
            result.allowed.append(item);
        } else {
            result.refused.append({item, refusal});
            Q_EMIT statusMessage(QString("Cleanup refused item %1: %2").arg(item.path, refusal),
                                 kStatusTimeoutShortMs);
        }
    }
    return result;
}

void AdvancedUninstallController::cleanLeftovers(const QVector<LeftoverItem>& selected_items) {
    if (refuseWhileEnumerationStopping(QStringLiteral("clean leftovers"))) {
        return;
    }
    if (selected_items.isEmpty()) {
        Q_EMIT statusMessage("No leftover items selected.", kStatusTimeoutShortMs);
        return;
    }

    // Idle-only: a manual clean never runs concurrently with an uninstall/enumeration/another
    // cleanup. (Auto-clean does NOT go through here; it starts from the controlled Uninstalling
    // state via startCleanupWorker, so it cannot be triggered re-entrantly.)
    if (m_state != State::Idle) {
        Q_EMIT statusMessage("Cannot clean while another operation is in progress.",
                             kStatusTimeoutShortMs);
        return;
    }

    // Second, independent safety layer (the same fail-closed screens the AI clean-leftovers op
    // uses): refuse OS-critical, malformed, report-only, or injection-shaped targets before any
    // item reaches CleanupWorker. Irreversible registry/service/task/firewall targets are valid
    // here because this MANUAL path runs only after explicit user authorization; automatic cleanup
    // applies its separate recoverability policy before reaching this screen.
    const CleanupScreenResult screen = screenCleanupItems(selected_items);

    if (screen.allowed.isEmpty()) {
        if (screen.refused.isEmpty()) {
            Q_EMIT statusMessage("No selected leftover items were eligible for cleanup.",
                                 kStatusTimeoutShortMs);
            return;
        }
        setState(State::Cleaning);
        Q_EMIT cleanupStarted(static_cast<int>(screen.refused.size()));
        reportCleanupRefusals(screen.refused);
        setState(State::Idle);
        Q_EMIT cleanupFinished(0, static_cast<int>(screen.refused.size()), 0);
        Q_EMIT statusMessage(QStringLiteral(
                                 "Cleanup refused %1 item(s) during safety screening; nothing was "
                                 "removed.")
                                 .arg(screen.refused.size()),
                             kStatusTimeoutLongMs);
        return;
    }

    // Say so in aggregate too: the per-item "Skipped protected item" notices scroll past, and a
    // clean that quietly does less than was asked for must be visible as such.
    if (!screen.refused.isEmpty()) {
        Q_EMIT statusMessage(
            QString("%1 selected item(s) were refused by safety screening and were not cleaned.")
                .arg(screen.refused.size()),
            kStatusTimeoutLongMs);
    }

    // Manual clean honors the user's recycle preference. When that preference is the Recycle Bin,
    // recoverability is REQUIRED as well: forcing the mode alone still let a recycle FAILURE fall
    // through to a permanent delete, turning the recoverable removal the user asked for into an
    // irreversible one behind their back. A user who chose permanent deletion (m_useRecycleBin
    // false) reviewed the items and gets exactly that.
    startCleanupWorker(screen.allowed,
                       screen.refused,
                       m_useRecycleBin,
                       /*require_recoverable=*/m_useRecycleBin);
}

void AdvancedUninstallController::startCleanupWorker(const QVector<LeftoverItem>& screened_items,
                                                     const QVector<CleanupRefusal>& refused_items,
                                                     bool use_recycle_bin,
                                                     bool require_recoverable) {
    setState(State::Cleaning);

    const int total = static_cast<int>(screened_items.size() + refused_items.size());
    Q_EMIT cleanupStarted(total);
    Q_EMIT statusMessage(QString("Cleaning %1 leftover items...").arg(total), 0);
    m_pendingCleanupRefused = static_cast<int>(refused_items.size());
    reportCleanupRefusals(refused_items);

    const int generation = ++m_cleanupGeneration;
    retireWorker(m_cleanupWorker, this);
    m_cleanupWorker = makeWorker<CleanupWorker>(screened_items, use_recycle_bin, this);
    m_cleanupWorker->setRequireRecoverable(require_recoverable);
    connectCleanupWorkerSignals(m_cleanupWorker.get(), screened_items, generation);
    m_cleanupWorker->start();
}

void AdvancedUninstallController::reportCleanupRefusals(
    const QVector<CleanupRefusal>& refused_items) {
    for (const CleanupRefusal& refusal : refused_items) {
        Q_EMIT itemCleanupError(refusal.item, refusal.reason);
        Q_EMIT itemCleaned(refusal.item, false);
    }
}

bool AdvancedUninstallController::sameCleanupItemIdentity(const LeftoverItem& lhs,
                                                          const LeftoverItem& rhs) {
    return lhs.type == rhs.type && lhs.path == rhs.path &&
           lhs.registryValueName == rhs.registryValueName &&
           lhs.firewallDirection == rhs.firewallDirection &&
           lhs.firewallProfile == rhs.firewallProfile && lhs.firewallProgram == rhs.firewallProgram;
}

void AdvancedUninstallController::appendUniqueBatchAutoCleanItems(
    const QVector<LeftoverItem>& items) {
    for (const LeftoverItem& item : items) {
        const bool already_queued =
            std::ranges::any_of(m_batchAutoCleanItems, [&item](const LeftoverItem& queued) {
                return sameCleanupItemIdentity(item, queued);
            });
        if (!already_queued) {
            m_batchAutoCleanItems.append(item);
        }
    }
}

bool AdvancedUninstallController::isRecoverableLeftoverType(const LeftoverItem& item) {
    switch (item.type) {
    case LeftoverItem::Type::File:
    case LeftoverItem::Type::Folder:
        return true;
    case LeftoverItem::Type::StartupEntry:
        // A file-shortcut startup entry recycles; a registry Run VALUE does not.
        return item.registryValueName.isEmpty();
    default:
        return false;  // registry key/value, service, task, firewall, shell ext -> irreversible
    }
}

QVector<LeftoverItem> AdvancedUninstallController::autoCleanableLeftovers(
    const QVector<LeftoverItem>& items) {
    QVector<LeftoverItem> out;
    for (const LeftoverItem& item : items) {
        if (item.deletable && item.risk == LeftoverItem::RiskLevel::Safe &&
            isRecoverableLeftoverType(item) && cleanupItemRefusal(item).isEmpty()) {
            LeftoverItem copy = item;
            copy.selected = true;
            out.append(copy);
        }
    }
    return out;
}

QVector<LeftoverItem> AdvancedUninstallController::leftoversStillPresent(
    const QVector<LeftoverItem>& all) const {
    QVector<LeftoverItem> out;
    for (const LeftoverItem& item : all) {
        const bool removed = std::ranges::any_of(m_autoCleanedItems, [&item](const auto& cleaned) {
            return sameCleanupItemIdentity(item, cleaned);
        });
        if (!removed) {
            out.append(item);
        }
    }
    return out;
}

void AdvancedUninstallController::finalizeDeferredAutoClean() {
    if (!m_autoCleanInProgress) {
        return;
    }
    m_autoCleanInProgress = false;
    const UninstallReport report = m_pendingAutoCleanReport;
    m_pendingAutoCleanReport = UninstallReport{};
    m_pendingAutoCleanItemCount = 0;
    // Re-show everything still on disk (nonSafe + any Safe item screened out or not confirmed
    // removed), then surface the deferred uninstall completion.
    Q_EMIT leftoversUpdated(leftoversStillPresent(report.foundLeftovers));
    m_autoCleanedItems.clear();
    Q_EMIT uninstallFinished(report);
}

void AdvancedUninstallController::mergeDeferredAutoCleanResult(int succeeded,
                                                               int failed,
                                                               qint64 bytes_recovered) {
    if (!m_autoCleanInProgress) {
        return;
    }
    int recorded_successes = 0;
    for (const LeftoverItem& item : m_pendingAutoCleanReport.foundLeftovers) {
        const bool removed = std::ranges::any_of(m_autoCleanedItems, [&item](const auto& cleaned) {
            return sameCleanupItemIdentity(item, cleaned);
        });
        if (!removed) {
            continue;
        }
        if (recordCleanupSuccess(m_pendingAutoCleanReport, item.type)) {
            ++recorded_successes;
        }
    }
    m_pendingAutoCleanReport.failedDeletions += failed;
    m_pendingAutoCleanReport.totalSpaceRecovered += bytes_recovered;
    if (recorded_successes != succeeded) {
        const QString mismatch =
            QStringLiteral(
                "Automatic cleanup reported %1 success(es), but %2 item identity "
                "record(s) matched the deferred report.")
                .arg(succeeded)
                .arg(recorded_successes);
        m_pendingAutoCleanReport.errorLog.append(mismatch);
        sak::logError(mismatch.toStdString());
    }
}

void AdvancedUninstallController::failDeferredAutoClean(const QString& error) {
    if (!m_autoCleanInProgress) {
        return;
    }
    int recorded_successes = 0;
    qint64 recovered_bytes = 0;
    for (const LeftoverItem& item : m_pendingAutoCleanReport.foundLeftovers) {
        const bool removed = std::ranges::any_of(m_autoCleanedItems, [&item](const auto& cleaned) {
            return sameCleanupItemIdentity(item, cleaned);
        });
        if (!removed) {
            continue;
        }
        if (recordCleanupSuccess(m_pendingAutoCleanReport, item.type)) {
            ++recorded_successes;
            recovered_bytes += item.sizeBytes;
        }
    }
    m_pendingAutoCleanReport.failedDeletions +=
        std::max(0, m_pendingAutoCleanItemCount - recorded_successes);
    m_pendingAutoCleanReport.totalSpaceRecovered += recovered_bytes;
    m_pendingAutoCleanReport.errorLog.append(error);
}

bool AdvancedUninstallController::maybeAutoCleanSafeLeftovers(UninstallReport& report,
                                                              bool auto_clean) {
    if (!auto_clean) {
        return false;
    }
    // ONLY recoverable-type Safe items are auto-deleted: registry keys/values, services, tasks,
    // firewall rules and shell extensions have no Recycle Bin (irreversible), so even when Safe
    // they stay for human review -- an AUTOMATIC delete must always be undoable.
    const QVector<LeftoverItem> cleanable = autoCleanableLeftovers(report.foundLeftovers);
    if (cleanable.isEmpty()) {
        return false;
    }
    // Re-screen against the OS-critical denylist (defense in depth) before anything reaches the
    // worker.
    const CleanupScreenResult screen = screenCleanupItems(cleanable);
    if (screen.allowed.isEmpty()) {
        report.failedDeletions += static_cast<int>(screen.refused.size());
        for (const CleanupRefusal& refusal : screen.refused) {
            report.errorLog.append(QStringLiteral("Automatic cleanup refused %1: %2")
                                       .arg(refusal.item.path, refusal.reason));
            Q_EMIT itemCleanupError(refusal.item, refusal.reason);
            Q_EMIT itemCleaned(refusal.item, false);
        }
        return false;
    }
    // Defer uninstallFinished until this cleanup completes (onCleanupComplete) so the panel's
    // refreshPrograms() cannot flip the controller to Enumerating and refuse the clean; remember
    // the report so onCleanupComplete can then surface it + the leftovers still present.
    m_autoCleanInProgress = true;
    m_pendingAutoCleanReport = report;
    m_pendingAutoCleanItemCount = static_cast<int>(cleanable.size());
    m_autoCleanedItems.clear();
    Q_EMIT autoCleanSafeStarted(static_cast<int>(cleanable.size()));
    Q_EMIT statusMessage(QString("Auto-cleaning %1 safe leftover item(s) from %2 to the Recycle "
                                 "Bin (Review/Risky items kept for your review)...")
                             .arg(cleanable.size())
                             .arg(report.programName),
                         0);
    // Auto-clean is automatic (no per-item human review), so it ALWAYS uses the Recycle Bin AND
    // requires recoverability -- if an item cannot be recycled it is left in place (reported
    // failed), never permanently deleted. Started from the active uninstall-pipeline state via
    // startCleanupWorker so it is not subject to the manual clean's Idle-only reentrancy guard.
    startCleanupWorker(screen.allowed,
                       screen.refused,
                       /*use_recycle_bin=*/true,
                       /*require_recoverable=*/true);
    return true;
}

void AdvancedUninstallController::connectCleanupWorkerSignals(CleanupWorker* worker,
                                                              const QVector<LeftoverItem>& items,
                                                              int generation) {
    connectCleanupItemSignals(worker, items, generation);
    connectCleanupTerminalSignals(worker, generation);
}

void AdvancedUninstallController::connectCleanupItemSignals(CleanupWorker* worker,
                                                            const QVector<LeftoverItem>& items,
                                                            int generation) {
    auto outcome_state = std::make_shared<CleanupOutcomeState>();
    outcome_state->items = items;
    connectCleanupErrorSignal(worker, outcome_state, generation);
    connectCleanupOutcomeSignal(worker, outcome_state, generation);
}

void AdvancedUninstallController::connectCleanupErrorSignal(
    CleanupWorker* worker,
    const std::shared_ptr<CleanupOutcomeState>& outcome_state,
    int generation) {
    connect(worker,
            &CleanupWorker::itemCleanupError,
            this,
            [this, outcome_state, generation](const QString& path, const QString& reason) {
                if (generation != m_cleanupGeneration ||
                    outcome_state->index >= outcome_state->items.size()) {
                    return;
                }
                const LeftoverItem item = outcome_state->items.at(outcome_state->index);
                if (item.path != path) {
                    outcome_state->identityFailed = true;
                    sak::logError("Cleanup error path did not match current typed item");
                    Q_EMIT itemCleanupError(
                        item,
                        QStringLiteral("Cleanup outcome identity mismatch; result was refused."));
                    return;
                }
                Q_EMIT itemCleanupError(item, reason);
            });
}

void AdvancedUninstallController::connectCleanupOutcomeSignal(
    CleanupWorker* worker,
    const std::shared_ptr<CleanupOutcomeState>& outcome_state,
    int generation) {
    connect(worker,
            &CleanupWorker::itemCleaned,
            this,
            [this, outcome_state, generation](const QString& path, bool success) {
                if (generation != m_cleanupGeneration ||
                    outcome_state->index >= outcome_state->items.size()) {
                    return;
                }
                const LeftoverItem item = outcome_state->items.at(outcome_state->index);
                const bool identity_matches = item.path == path && !outcome_state->identityFailed;
                if (!identity_matches && !outcome_state->identityFailed) {
                    sak::logError("Cleanup completion path did not match current typed item");
                    Q_EMIT itemCleanupError(
                        item,
                        QStringLiteral("Cleanup outcome identity mismatch; result was refused."));
                }
                const bool outcome_success = success && identity_matches;
                if (outcome_success && m_autoCleanInProgress) {
                    m_autoCleanedItems.append(item);
                }
                Q_EMIT itemCleaned(item, outcome_success);
                outcome_state->identityFailed = false;
                ++outcome_state->index;
            });
}

void AdvancedUninstallController::connectCleanupTerminalSignals(CleanupWorker* worker,
                                                                int generation) {
    connect(worker,
            &CleanupWorker::cleanupComplete,
            this,
            [this, generation](int succeeded, int failed, qint64 bytes) {
                if (generation == m_cleanupGeneration) {
                    ++m_cleanupGeneration;
                    onCleanupComplete(succeeded, failed, bytes);
                }
            });
    connect(worker,
            &CleanupWorker::failed,
            this,
            [this, generation](int error_code, const QString& message) {
                if (generation == m_cleanupGeneration) {
                    ++m_cleanupGeneration;
                    onCleanupWorkerFailed(error_code, message);
                }
            });
    connect(worker, &CleanupWorker::cancelled, this, [this, generation]() {
        if (generation == m_cleanupGeneration) {
            ++m_cleanupGeneration;
            onCleanupWorkerCancelled();
        }
    });
    connect(worker,
            &CleanupWorker::progress,
            this,
            [this, generation](int current, int total, const QString&) {
                if (generation == m_cleanupGeneration) {
                    Q_EMIT progressUpdate(current, total);
                }
            });
}

void AdvancedUninstallController::cancelOperation() {
    // Cancel enumeration if running; the persistent worker thread stays alive for reuse and is
    // torn down only in cleanupWorkers().
    if (m_state == State::Enumerating && m_enumerator) {
        m_enumerator->requestCancel();
        m_enumerationStopping = true;
        Q_EMIT statusMessage("Cancelling program enumeration...", 0);
    }

    if (m_uninstallWorker && (m_uninstallWorker->isExecuting() || m_uninstallWorker->isRunning())) {
        m_uninstallWorker->requestStop();
    }
    if (m_cleanupWorker && (m_cleanupWorker->isExecuting() || m_cleanupWorker->isRunning())) {
        m_cleanupWorker->requestStop();
    }
    if (m_batchRestorePointPending) {
        // The restore point cannot be interrupted mid-checkpoint; the batch stops once it lands.
        m_batchCancelledBeforeStart = true;
        Q_EMIT statusMessage("Cancelling batch uninstall after the restore point...", 0);
    }
}

// -- Batch Uninstall ---------------------------------------------------------

bool AdvancedUninstallController::addToQueue(const ProgramInfo& program,
                                             ScanLevel scan_level,
                                             bool auto_clean_safe) {
    // Refuse mid-batch mutation: processNextQueueItem / onUninstallComplete index m_queue by
    // m_batchIndex, so appending while a batch runs (m_batchIndex >= 0) would shift indices and
    // misattribute a completion to the wrong program.
    if (m_batchIndex >= 0) {
        Q_EMIT statusMessage("Cannot modify the queue while a batch uninstall is running.",
                             kStatusTimeoutShortMs);
        return false;
    }

    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage("Cannot add to queue: program name is empty.", kStatusTimeoutShortMs);
        return false;
    }

    if (!isValidScanLevel(scan_level)) {
        Q_EMIT statusMessage("Cannot add to queue: scan level is invalid.", kStatusTimeoutShortMs);
        return false;
    }

    if (std::ranges::any_of(m_queue, [&program](const UninstallQueueItem& queued) {
            return sameQueuedProgram(queued.program, program);
        })) {
        Q_EMIT statusMessage("Cannot add to queue: program is already queued.",
                             kStatusTimeoutShortMs);
        return false;
    }
    if (m_queue.size() >= kMaxBatchQueueItems) {
        Q_EMIT statusMessage(QStringLiteral("Cannot add to queue: maximum of %1 programs reached.")
                                 .arg(kMaxBatchQueueItems),
                             kStatusTimeoutShortMs);
        return false;
    }

    UninstallQueueItem item;
    item.program = program;
    item.scanLevel = scan_level;
    item.autoCleanSafeLeftovers = auto_clean_safe;
    item.status = UninstallQueueItem::Status::Queued;
    m_queue.append(item);
    return true;
}

bool AdvancedUninstallController::removeFromQueue(int index) {
    // Mid-batch removal would shift m_batchIndex-based indexing so a completion is applied to (or
    // an uninstall is run against) the wrong queued program -- refuse it while a batch is running.
    if (m_batchIndex >= 0) {
        Q_EMIT statusMessage("Cannot modify the queue while a batch uninstall is running.",
                             kStatusTimeoutShortMs);
        return false;
    }
    if (index < 0 || index >= m_queue.size()) {
        Q_EMIT statusMessage("Cannot remove queue item: index is invalid.", kStatusTimeoutShortMs);
        return false;
    }
    m_queue.removeAt(index);
    return true;
}

QVector<UninstallQueueItem> AdvancedUninstallController::queue() const {
    return m_queue;
}

bool AdvancedUninstallController::clearQueue() {
    // Clearing the queue mid-batch would leave m_batchIndex pointing past the emptied vector and
    // strand the running item's completion -- refuse it while a batch is running.
    if (m_batchIndex >= 0) {
        Q_EMIT statusMessage("Cannot modify the queue while a batch uninstall is running.",
                             kStatusTimeoutShortMs);
        return false;
    }
    m_queue.clear();
    return true;
}

bool AdvancedUninstallController::startBatchUninstall(bool create_restore_point) {
    if (refuseWhileEnumerationStopping(QStringLiteral("start a batch uninstall"))) {
        Q_EMIT batchFailed(
            QStringLiteral("Cannot start batch while cancelled enumeration is still stopping."));
        return false;
    }
    if (m_state != State::Idle) {
        const QString error = QStringLiteral("Another operation is already in progress.");
        Q_EMIT statusMessage(error, kStatusTimeoutShortMs);
        Q_EMIT batchFailed(error);
        return false;
    }

    if (m_queue.isEmpty()) {
        const QString error = QStringLiteral("Batch queue is empty.");
        Q_EMIT statusMessage(error, kStatusTimeoutShortMs);
        Q_EMIT batchFailed(error);
        return false;
    }

    // Own the busy state for the WHOLE batch: without this the controller stays Idle between queue
    // items, letting a concurrent refresh / manual clean / second uninstall run mid-batch.
    // processNextQueueItem keeps it Uninstalling; finishBatchUninstall (or a cancel) sets Idle.
    setState(State::Uninstalling);

    m_batchIndex = -1;
    m_batchAutoCleanItems.clear();
    m_batchCompletionPending = false;
    m_pendingBatchSucceeded = 0;
    m_pendingBatchFailed = 0;

    if (!create_restore_point) {
        processNextQueueItem();
        return true;
    }

    // One restore point for the whole batch, made on a pool thread: Checkpoint-Computer can take
    // up to two minutes, which used to freeze the window. The batch starts when it lands.
    const QString desc = QString("SAK: Before batch uninstall (%1 programs)").arg(m_queue.size());
    m_batchRestorePointPending = true;
    m_batchCancelledBeforeStart = false;
    Q_EMIT statusMessage(QStringLiteral("Creating system restore point..."), 0);
    auto* watcher = new QFutureWatcher<RestorePointManager::Outcome>(this);
    connect(watcher,
            &QFutureWatcher<RestorePointManager::Outcome>::finished,
            this,
            [this, watcher, desc]() {
                const RestorePointManager::Outcome outcome = watcher->result();
                watcher->deleteLater();
                continueBatchAfterRestorePoint(outcome, desc);
            });
    watcher->setFuture(
        QtConcurrent::run([maker = m_batchRestorePointMaker, desc]() { return maker(desc); }));
    return true;
}

void AdvancedUninstallController::continueBatchAfterRestorePoint(
    const RestorePointManager::Outcome& outcome, const QString& desc) {
    m_batchRestorePointPending = false;
    if (m_batchCancelledBeforeStart) {
        m_batchCancelledBeforeStart = false;
        setState(State::Idle);
        Q_EMIT statusMessage("Batch uninstall cancelled.", kStatusTimeoutLongMs);
        Q_EMIT batchFailed(QStringLiteral("Batch uninstall cancelled before it started."));
        return;
    }
    // FAIL CLOSED: the user asked for a rollback point. With none -- not created, and no recent
    // one to reuse -- abort the (potentially destructive) batch rather than run with no way back.
    if (!outcome.usable()) {
        setState(State::Idle);
        Q_EMIT statusMessage("Cannot create system restore point; batch uninstall aborted.",
                             kStatusTimeoutLongMs);
        Q_EMIT batchFailed(QStringLiteral("Required system restore point could not be created: %1")
                               .arg(outcome.message));
        return;
    }
    if (outcome.kind == RestorePointManager::Outcome::Kind::Created) {
        Q_EMIT restorePointCreated(desc);
    } else {
        // Reused: the message names the restore point in use and why no new one was made.
        Q_EMIT restorePointCreated(outcome.message);
    }
    processNextQueueItem();
}

// -- Settings ----------------------------------------------------------------

void AdvancedUninstallController::deferSettingsError(const QString& error) {
    QTimer::singleShot(0, this, [this, error]() { Q_EMIT settingsError(error); });
}

bool AdvancedUninstallController::loadSettings() {
    auto& cfg = ConfigManager::instance();

    if (!cfg.isHealthy()) {
        m_defaultScanLevel = ScanLevel::Moderate;
        m_autoRestorePoint = true;
        m_autoCleanSafePreference = false;
        m_showSystemComponents = false;
        m_useRecycleBin = true;
        m_selectAllByDefault = false;
        const QString error = QStringLiteral(
            "Advanced Uninstall settings store is unhealthy; safe values are active.");
        sak::logError(error.toStdString());
        deferSettingsError(error);
        return false;
    }

    bool scan_valid = false;
    int raw_scan_level =
        cfg.getValue("advuninstall/default_scan_level", kDefaultScanLevelValue).toInt(&scan_valid);
    if (!scan_valid || raw_scan_level < 0 || raw_scan_level > kMaxScanLevelValue) {
        scan_valid = false;
        raw_scan_level = kDefaultScanLevelValue;
        sak::logError("Malformed Advanced Uninstall scan level; using Moderate");
    }
    m_defaultScanLevel = static_cast<ScanLevel>(raw_scan_level);
    // The boolean prefs are range-validated like the scan level above rather than coerced with
    // toBool(): saveSettings() writes them as typed bools, so anything else in the file is a
    // truncated write or an edit from outside the app, and toBool() would turn any such string
    // into "true". Each one names the value to keep when the stored text is not a boolean -- the
    // conservative choice for THAT preference (recycle rather than permanently delete, do not
    // auto-clean, do not pre-select), which is not always the shipped default.
    bool restore_valid = false;
    bool clean_valid = false;
    bool system_valid = false;
    bool recycle_valid = false;
    bool select_valid = false;
    m_autoRestorePoint =
        loadBoolSetting("advuninstall/auto_restore_point", true, true, restore_valid);
    m_autoCleanSafePreference =
        loadBoolSetting("advuninstall/auto_clean_safe", true, false, clean_valid);
    m_showSystemComponents =
        loadBoolSetting("advuninstall/show_system_components", false, false, system_valid);
    m_useRecycleBin = loadBoolSetting("advuninstall/use_recycle_bin", false, true, recycle_valid);
    m_selectAllByDefault =
        loadBoolSetting("advuninstall/select_all_by_default", false, false, select_valid);
    const std::array validity = {
        scan_valid, restore_valid, clean_valid, system_valid, recycle_valid, select_valid};
    const bool valid = std::ranges::all_of(validity, [](bool item) { return item; });
    if (!valid) {
        const QString error = QStringLiteral(
            "One or more Advanced Uninstall settings were invalid; safe values replaced them.");
        deferSettingsError(error);
    }
    return valid;
}

bool AdvancedUninstallController::saveSettings() {
    auto& cfg = ConfigManager::instance();

    if (!cfg.isHealthy()) {
        const QString error =
            QStringLiteral("Advanced Uninstall settings were not saved: store is unhealthy.");
        sak::logError(error.toStdString());
        deferSettingsError(error);
        return false;
    }

    cfg.setValue("advuninstall/default_scan_level", static_cast<int>(m_defaultScanLevel));
    cfg.setValue("advuninstall/auto_restore_point", m_autoRestorePoint);
    cfg.setValue("advuninstall/auto_clean_safe", m_autoCleanSafePreference);
    cfg.setValue("advuninstall/show_system_components", m_showSystemComponents);
    cfg.setValue("advuninstall/use_recycle_bin", m_useRecycleBin);
    cfg.setValue("advuninstall/select_all_by_default", m_selectAllByDefault);
    if (!cfg.sync()) {
        const QString error =
            QStringLiteral("Advanced Uninstall settings could not be written to disk.");
        sak::logError(error.toStdString());
        deferSettingsError(error);
        return false;
    }
    return true;
}

bool AdvancedUninstallController::autoRestorePoint() const {
    return m_autoRestorePoint;
}

void AdvancedUninstallController::setAutoRestorePoint(bool enabled) {
    m_autoRestorePoint = enabled;
}

bool AdvancedUninstallController::autoCleanSafe() const {
    return m_autoCleanSafePreference;
}

void AdvancedUninstallController::setAutoCleanSafe(bool enabled) {
    m_autoCleanSafePreference = enabled;
}

ScanLevel AdvancedUninstallController::defaultScanLevel() const {
    return m_defaultScanLevel;
}

void AdvancedUninstallController::setDefaultScanLevel(ScanLevel level) {
    // Fail closed on an out-of-range enum (e.g. a bad numeric cast): keep the current level rather
    // than store a value that would later drive an invalid scan.
    const int raw = static_cast<int>(level);
    if (raw < 0 || raw > kMaxScanLevelValue) {
        Q_EMIT statusMessage("Ignored invalid default scan level.", kStatusTimeoutShortMs);
        return;
    }
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

// -- Private Slots -----------------------------------------------------------

void AdvancedUninstallController::onLeftoverScanStarted(ScanLevel level) {
    setState(State::LeftoverScanning);
    Q_EMIT leftoverScanStarted(level);
}

void AdvancedUninstallController::onEnumerationFinished(int generation,
                                                        QVector<ProgramInfo> programs) {
    if (generation == m_enumGeneration && m_enumerationStopping) {
        m_enumerationStopping = false;
        setState(State::Idle);
        Q_EMIT enumerationCancelled();
        return;
    }
    // Drop a stale completion: a run cancelled then restarted would otherwise apply the OLD (or an
    // un-cancelled superseded) result to the NEW run. Also guard the post-cancel Idle state.
    if (generation != m_enumGeneration || m_state != State::Enumerating) {
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
    if (generation == m_enumGeneration && m_enumerationStopping) {
        m_enumerationStopping = false;
        setState(State::Idle);
        Q_EMIT enumerationCancelled();
        return;
    }
    // Drop a stale failure (a superseded run's cancel) so it cannot force the NEW run's state to
    // Idle.
    if (generation != m_enumGeneration || m_state != State::Enumerating) {
        return;
    }

    // The enumerator stays on its persistent worker thread; nothing to move back.
    setState(State::Idle);
    Q_EMIT statusMessage(QString("Enumeration failed: %1").arg(error), kStatusTimeoutLongMs);
    Q_EMIT enumerationFailed(error);
}

void AdvancedUninstallController::onUninstallComplete(UninstallReport report) {
    // If in batch mode, process next
    if (m_batchIndex >= 0 && m_batchIndex < m_queue.size()) {
        auto& qi = m_queue[m_batchIndex];
        qi.report = report;
        qi.status = (report.uninstallResult == UninstallReport::UninstallResult::Success ||
                     report.uninstallResult == UninstallReport::UninstallResult::Skipped)
                        ? UninstallQueueItem::Status::Completed
                        : UninstallQueueItem::Status::Failed;
        Q_EMIT queueItemStatusChanged(m_batchIndex, qi.status);

        // Accumulate this item's auto-cleanable (Safe AND recoverable-type) leftovers for a single
        // pass at batch end (only if the item opted in). Running a cleanup now would race the next
        // queued uninstall, so it is deferred until the whole batch is done.
        if (qi.autoCleanSafeLeftovers) {
            appendUniqueBatchAutoCleanItems(autoCleanableLeftovers(report.foundLeftovers));
        }

        // Process next queued item
        processNextQueueItem();
        return;
    }

    // Auto-clean the SAFE leftovers if enabled (Review/Risky always stay for human review). This
    // must run BEFORE uninstallFinished is emitted: the panel's uninstallFinished slot calls
    // refreshPrograms() synchronously, flipping the controller to Enumerating, which would make the
    // clean refuse. When an auto-clean starts, uninstallFinished + the completion status are
    // DEFERRED to onCleanupComplete (active uninstall state -> Cleaning). No-op when
    // disabled/nothing safe.
    const bool auto_clean_safe = m_currentAutoCleanSafe;
    m_currentAutoCleanSafe = false;
    if (maybeAutoCleanSafeLeftovers(report, auto_clean_safe)) {
        return;
    }

    setState(State::Idle);
    Q_EMIT uninstallFinished(report);
    Q_EMIT statusMessage(QString("Uninstall of %1 complete. Found %2 leftovers.")
                             .arg(report.programName)
                             .arg(report.foundLeftovers.size()),
                         kStatusTimeoutLongMs);
}

void AdvancedUninstallController::onUninstallWorkerFailed(int error_code, const QString& message) {
    m_currentAutoCleanSafe = false;
    if (m_batchIndex >= 0 && m_batchIndex < m_queue.size()) {
        UninstallQueueItem& item = m_queue[m_batchIndex];
        item.status = UninstallQueueItem::Status::Failed;
        item.errorCode = error_code;
        item.errorMessage = message;
        Q_EMIT queueItemStatusChanged(m_batchIndex, UninstallQueueItem::Status::Failed);
        Q_EMIT batchItemFailed(m_batchIndex, error_code, message);
        processNextQueueItem();
        return;
    }

    setState(State::Idle);
    Q_EMIT uninstallFailed(error_code, message);
    Q_EMIT statusMessage(QString("Uninstall failed: %1").arg(message), kStatusTimeoutLongMs);
}

void AdvancedUninstallController::onUninstallWorkerCancelled() {
    m_currentAutoCleanSafe = false;
    if (m_batchIndex >= 0 && m_batchIndex < m_queue.size()) {
        m_queue[m_batchIndex].status = UninstallQueueItem::Status::Cancelled;
        Q_EMIT queueItemStatusChanged(m_batchIndex, UninstallQueueItem::Status::Cancelled);
        m_batchIndex = -1;
        m_batchAutoCleanItems.clear();
        setState(State::Idle);
        Q_EMIT uninstallCancelled();
        Q_EMIT statusMessage("Batch cancelled.", kStatusTimeoutShortMs);
        Q_EMIT batchFailed(QStringLiteral("Batch uninstall was cancelled."));
        return;
    }

    setState(State::Idle);
    Q_EMIT uninstallCancelled();
    Q_EMIT statusMessage("Uninstall cancelled.", kStatusTimeoutShortMs);
}

void AdvancedUninstallController::onCleanupComplete(int succeeded,
                                                    int failed,
                                                    qint64 bytes_recovered) {
    failed += m_pendingCleanupRefused;
    m_pendingCleanupRefused = 0;
    const bool deferred_batch = m_batchCompletionPending;
    mergeDeferredAutoCleanResult(succeeded, failed, bytes_recovered);
    setState(State::Idle);
    Q_EMIT cleanupFinished(succeeded, failed, bytes_recovered);

    QString msg = QString("Cleanup complete: %1 items removed").arg(succeeded);
    if (failed > 0) {
        msg += QString(", %1 failed").arg(failed);
    }
    if (bytes_recovered > 0) {
        const double mb = static_cast<double>(bytes_recovered) / kBytesPerMBf;
        msg += QString(" (%1 MB recovered)").arg(mb, 0, 'f', kRecoveredSizeDisplayPrecision);
    }
    Q_EMIT statusMessage(msg, kStatusTimeoutLongMs);

    if (deferred_batch) {
        finishDeferredBatchCleanup(succeeded, failed, bytes_recovered);
        return;
    }

    // If this was the deferred SINGLE-uninstall auto-clean, NOW surface the uninstall completion
    // and re-show the leftovers still on disk (the successfully-cleaned Safe ones are dropped;
    // anything screened out or failed stays visible). No-op for a manual/batch cleanup.
    finalizeDeferredAutoClean();
}

void AdvancedUninstallController::onCleanupWorkerFailed(int error_code, const QString& message) {
    m_pendingCleanupRefused = 0;
    setState(State::Idle);
    Q_EMIT cleanupFailed(error_code, message);
    if (m_batchCompletionPending) {
        failDeferredBatchCleanup(
            QStringLiteral("Batch uninstall completed, but automatic cleanup failed: %1")
                .arg(message));
        return;
    }
    failDeferredAutoClean(QStringLiteral("Automatic cleanup failed: %1").arg(message));
    Q_EMIT statusMessage(QString("Cleanup failed: %1").arg(message), kStatusTimeoutLongMs);
    // The UNINSTALL succeeded; only the deferred auto-clean failed. Still surface the uninstall
    // completion + all leftovers (nothing was reliably removed), and clear the deferred state so a
    // later cleanup cannot re-emit this stale report.
    finalizeDeferredAutoClean();
}

void AdvancedUninstallController::onCleanupWorkerCancelled() {
    // A cancelled cleanup is NOT a completed one: surface it as cancelled rather
    // than emitting cleanupFinished with partial (success-looking) counts.
    m_pendingCleanupRefused = 0;
    setState(State::Idle);
    Q_EMIT cleanupCancelled();
    if (m_batchCompletionPending) {
        failDeferredBatchCleanup(
            QStringLiteral("Batch uninstall completed, but automatic cleanup was cancelled."));
        return;
    }
    failDeferredAutoClean(QStringLiteral("Automatic cleanup was cancelled."));
    Q_EMIT statusMessage("Cleanup cancelled.", kStatusTimeoutShortMs);
    // Same as the failure path: finish the deferred uninstall + re-show leftovers, clear the flag.
    finalizeDeferredAutoClean();
}

// -- Private -----------------------------------------------------------------

void AdvancedUninstallController::setState(State new_state) {
    if (m_state != new_state) {
        m_state = new_state;
        Q_EMIT stateChanged(new_state);
    }
}

void AdvancedUninstallController::stopEnumThread() {
    if (m_enumThread == nullptr) {
        return;
    }
    m_enumerator->requestCancel();
    m_enumThread->quit();
    if (!m_enumThread->wait(kThreadWaitMs)) {
        ProgramEnumerator* stale_enumerator = m_enumerator.release();
        QThread* stale_thread = m_enumThread;
        m_enumThread = nullptr;
        stale_thread->setParent(nullptr);
        QObject::disconnect(stale_enumerator, nullptr, this, nullptr);
        QObject::connect(stale_thread, &QThread::finished, stale_enumerator, &QObject::deleteLater);
        QObject::connect(stale_thread, &QThread::finished, stale_thread, &QObject::deleteLater);
        sak::logError("Enumeration thread refused the bounded cooperative stop; detached safely");
        return;
    }
    // Join confirmed: delete here, before ~AdvancedUninstallController destroys the
    // m_enumerator unique_ptr that a running enumerateAll() would otherwise use.
    delete m_enumThread;
    m_enumThread = nullptr;
}

void AdvancedUninstallController::cleanupWorkers() {
    stopEnumThread();
    // Cooperative stop + detach-if-refuses, never terminate()+destroy a live QThread.
    retireWorker(m_uninstallWorker, this);
    retireWorker(m_cleanupWorker, this);
}

void AdvancedUninstallController::connectUninstallWorkerSignals(bool include_native_signals,
                                                                int generation) {
    if (include_native_signals) {
        connectUninstallNativeSignals(generation);
    }
    connectUninstallScanSignals(generation);
    connectUninstallTerminalSignals(generation);
}

void AdvancedUninstallController::connectUninstallNativeSignals(int generation) {
    connect(m_uninstallWorker.get(),
            &UninstallWorker::nativeUninstallerStarted,
            this,
            [this, generation](const QString& name) {
                if (generation == m_uninstallGeneration) {
                    Q_EMIT nativeUninstallerStarted(name);
                }
            });
    connect(m_uninstallWorker.get(),
            &UninstallWorker::nativeUninstallerFinished,
            this,
            [this, generation](int exit_code) {
                if (generation == m_uninstallGeneration) {
                    Q_EMIT nativeUninstallerFinished(exit_code);
                }
            });
}

void AdvancedUninstallController::connectUninstallScanSignals(int generation) {
    connect(m_uninstallWorker.get(),
            &UninstallWorker::restorePointCreated,
            this,
            [this, generation](const QString& name) {
                if (generation == m_uninstallGeneration) {
                    Q_EMIT restorePointCreated(name);
                }
            });
    connect(m_uninstallWorker.get(),
            &UninstallWorker::leftoverScanStarted,
            this,
            [this, generation](ScanLevel level) {
                if (generation == m_uninstallGeneration) {
                    onLeftoverScanStarted(level);
                }
            });
    connect(m_uninstallWorker.get(),
            &UninstallWorker::leftoverScanProgress,
            this,
            [this, generation](const QString& path, int found) {
                if (generation == m_uninstallGeneration) {
                    Q_EMIT leftoverScanProgress(path, found);
                }
            });
    connect(m_uninstallWorker.get(),
            &UninstallWorker::leftoverScanFinished,
            this,
            [this, generation](const QVector<LeftoverItem>& leftovers) {
                if (generation == m_uninstallGeneration) {
                    Q_EMIT leftoverScanFinished(leftovers);
                }
            });
    connect(m_uninstallWorker.get(),
            &UninstallWorker::progress,
            this,
            [this, generation](int current, int total, const QString& msg) {
                if (generation == m_uninstallGeneration) {
                    Q_EMIT uninstallProgress(total > 0 ? (current * kPercentMax / total) : 0, msg);
                    Q_EMIT progressUpdate(current, total);
                }
            });
}

void AdvancedUninstallController::connectUninstallTerminalSignals(int generation) {
    connect(m_uninstallWorker.get(),
            &UninstallWorker::uninstallComplete,
            this,
            [this, generation](const UninstallReport& report) {
                if (generation == m_uninstallGeneration) {
                    ++m_uninstallGeneration;
                    onUninstallComplete(report);
                }
            });
    connect(m_uninstallWorker.get(),
            &UninstallWorker::failed,
            this,
            [this, generation](int error_code, const QString& message) {
                if (generation == m_uninstallGeneration) {
                    ++m_uninstallGeneration;
                    onUninstallWorkerFailed(error_code, message);
                }
            });
    connect(m_uninstallWorker.get(), &UninstallWorker::cancelled, this, [this, generation]() {
        if (generation == m_uninstallGeneration) {
            ++m_uninstallGeneration;
            onUninstallWorkerCancelled();
        }
    });
}

void AdvancedUninstallController::finishDeferredBatchCleanup(int cleanup_succeeded,
                                                             int cleanup_failed,
                                                             qint64 cleanup_bytes_recovered) {
    if (!m_batchCompletionPending) {
        return;
    }
    const int succeeded = m_pendingBatchSucceeded;
    const int failed = m_pendingBatchFailed;
    m_batchCompletionPending = false;
    m_pendingBatchSucceeded = 0;
    m_pendingBatchFailed = 0;
    Q_EMIT batchFinished(
        succeeded, failed, cleanup_succeeded, cleanup_failed, cleanup_bytes_recovered);
    Q_EMIT statusMessage(
        QString("Batch pipeline complete: %1 uninstall(s) succeeded, %2 failed; cleanup removed "
                "%3 item(s), %4 failed.")
            .arg(succeeded)
            .arg(failed)
            .arg(cleanup_succeeded)
            .arg(cleanup_failed),
        kStatusTimeoutLongMs);
}

void AdvancedUninstallController::failDeferredBatchCleanup(const QString& error) {
    if (!m_batchCompletionPending) {
        return;
    }
    m_batchCompletionPending = false;
    m_pendingBatchSucceeded = 0;
    m_pendingBatchFailed = 0;
    Q_EMIT batchFailed(error);
    Q_EMIT statusMessage(error, kStatusTimeoutLongMs);
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

    // Screen accumulated SAFE items before deciding the terminal path. When cleanup is needed,
    // batchFinished remains deferred until that cleanup completes; no subscriber can observe a
    // final batch result while work is still pending.
    const QVector<LeftoverItem> auto_clean = m_batchAutoCleanItems;
    m_batchAutoCleanItems.clear();
    const CleanupScreenResult screen = auto_clean.isEmpty() ? CleanupScreenResult{}
                                                            : screenCleanupItems(auto_clean);

    m_batchIndex = -1;
    if (screen.allowed.isEmpty()) {
        setState(State::Idle);
        reportCleanupRefusals(screen.refused);
        Q_EMIT batchFinished(succeeded, failed, 0, static_cast<int>(screen.refused.size()), 0);
        const QString cleanup_summary =
            screen.refused.isEmpty()
                ? QStringLiteral("no automatic cleanup was required")
                : QStringLiteral("automatic cleanup refused %1 protected item(s)")
                      .arg(screen.refused.size());
        Q_EMIT statusMessage(
            QString("Batch pipeline complete: %1 uninstall(s) succeeded, %2 failed; %3.")
                .arg(succeeded)
                .arg(failed)
                .arg(cleanup_summary),
            kStatusTimeoutLongMs);
        return;
    }

    m_batchCompletionPending = true;
    m_pendingBatchSucceeded = succeeded;
    m_pendingBatchFailed = failed;
    // Auto-clean the SAFE leftovers accumulated from every opted-in batch item in one RECOVERABLE
    // (Recycle Bin, forced), denylist-screened pass. batchFinished is deferred until cleanup's
    // terminal result, and m_batchIndex is already -1 so queue mutation is no longer falsely
    // locked.
    Q_EMIT autoCleanSafeStarted(static_cast<int>(screen.allowed.size() + screen.refused.size()));
    Q_EMIT statusMessage(QString("Auto-cleaning %1 safe leftover item(s) from the batch to the "
                                 "Recycle Bin...")
                             .arg(screen.allowed.size() + screen.refused.size()),
                         0);
    startCleanupWorker(screen.allowed,
                       screen.refused,
                       /*use_recycle_bin=*/true,
                       /*require_recoverable=*/true);
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
    qi.errorCode = 0;
    qi.errorMessage.clear();
    Q_EMIT queueItemStatusChanged(m_batchIndex, UninstallQueueItem::Status::InProgress);

    // Don't create individual restore points in batch (already done).
    // Ensure the previous worker is safely retired (detach if it refuses to stop)
    // before its unique_ptr is overwritten, so we never destroy a live QThread.
    setState(State::Uninstalling);
    const int generation = ++m_uninstallGeneration;
    retireWorker(m_uninstallWorker, this);
    m_uninstallWorker = makeWorker<UninstallWorker>(
        qi.program, uninstallModeForProgram(qi.program), qi.scanLevel, false, this);
    connectUninstallWorkerSignals(true, generation);

    Q_EMIT uninstallStarted(qi.program.displayName);
    Q_EMIT statusMessage(QString("Batch %1/%2: Uninstalling %3...")
                             .arg(m_batchIndex + 1)
                             .arg(m_queue.size())
                             .arg(qi.program.displayName),
                         0);

    m_uninstallWorker->start();
}

}  // namespace sak
