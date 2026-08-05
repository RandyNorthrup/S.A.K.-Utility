// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/flash_coordinator.h"

#include "sak/drive_unmounter.h"
#include "sak/flash_worker.h"
#include "sak/flasher_policy.h"
#include "sak/input_validator.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

#include <vector>

#include <windows.h>

#include <winioctl.h>

namespace {
constexpr qint64 kDefaultFlashBufferSizeMb = 256;
constexpr int kMaxPhysicalDriveNumber = 99;
// One drive at a time unless a caller raises it. Matches the ConfigManager default
// the settings dialog shows, so an untouched install behaves the way the dialog says.
constexpr int kDefaultMaxConcurrentWrites = 1;
constexpr int kWorkerShutdownTimeoutMs = sak::kTimeoutThreadShutdownMs;

// Tri-state result of the OS-disk identity probe. Undetermined MUST be treated
// as unsafe by callers (fail closed) -- a fallback to "not system" would let a
// raw write proceed when protection could not be established.
enum class OsDiskCheck {
    NotSystem,
    IsSystem,
    Undetermined
};

// The flash run is "active" (busy) from the moment targets start validating
// through verification. A second startFlash arriving in ANY of these states must
// be refused -- not just during Flashing/Verifying -- or two runs race the shared
// worker/state members.
bool isActiveFlashState(sak::FlashState state) {
    return state == sak::FlashState::Validating || state == sak::FlashState::Unmounting ||
           state == sak::FlashState::Decompressing || state == sak::FlashState::Flashing ||
           state == sak::FlashState::Verifying;
}

// Reads the actual physical-disk number the given open device handle refers to
// (IOCTL_STORAGE_GET_DEVICE_NUMBER). Returns -1 on failure or a non-disk device.
// Used to confirm a "\\.\PhysicalDriveN" path really opens disk N, closing a
// DosDevice-alias bypass where the parsed number differs from the opened device.
int queryHandleDriveNumber(HANDLE handle) {
    STORAGE_DEVICE_NUMBER deviceNumber = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(handle,
                         IOCTL_STORAGE_GET_DEVICE_NUMBER,
                         nullptr,
                         0,
                         &deviceNumber,
                         sizeof(deviceNumber),
                         &bytesReturned,
                         nullptr)) {
        return -1;
    }
    if (deviceNumber.DeviceType != FILE_DEVICE_DISK) {
        return -1;
    }
    return static_cast<int>(deviceNumber.DeviceNumber);
}

// A file the flasher recognizes as a compressed container but the streaming
// DecompressorFactory cannot turn into a raw disk image (currently .zip -- a
// multi-member archive). Such a file MUST be refused, never raw-written: writing
// the archive bytes verbatim produces a corrupt, unbootable target.
bool isUnsupportedCompressedContainer(const QString& imagePath) {
    return FileImageSource::detectFormat(imagePath) == sak::ImageFormat::ZIP &&
           !CompressedImageSource::isCompressed(imagePath);
}

// Returns whether the given physical-drive number backs the current OS (system)
// volume. Used as an engine-level guard so a bad caller cannot raw write the
// running OS disk. Determined natively (no elevation needed). If the OS disk
// cannot be identified at any step this returns Undetermined; the caller then
// refuses the write rather than assuming the disk is safe.
OsDiskCheck physicalDriveOsDiskCheck(int driveNumber) {
    wchar_t winDir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH || winDir[1] != L':') {
        return OsDiskCheck::Undetermined;
    }
    const wchar_t volumePath[] = {L'\\', L'\\', L'.', L'\\', winDir[0], L':', L'\0'};
    HANDLE hVol = CreateFileW(
        volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        return OsDiskCheck::Undetermined;
    }
    constexpr DWORD kMaxExtents = 16;
    const DWORD bufSize = sizeof(VOLUME_DISK_EXTENTS) + (kMaxExtents - 1) * sizeof(DISK_EXTENT);
    std::vector<unsigned char> buffer(bufSize, 0);
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buffer.data());
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(hVol,
                                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                    nullptr,
                                    0,
                                    extents,
                                    bufSize,
                                    &bytesReturned,
                                    nullptr);
    CloseHandle(hVol);
    if (!ok) {
        return OsDiskCheck::Undetermined;
    }
    for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < kMaxExtents; ++i) {
        if (static_cast<int>(extents->Extents[i].DiskNumber) == driveNumber) {
            return OsDiskCheck::IsSystem;
        }
    }
    return OsDiskCheck::NotSystem;
}

// Returns the first target path that appears more than once (case-insensitive,
// since Windows device paths are not case-sensitive), or an empty string if all
// targets are distinct.
}  // namespace

FlashCoordinator::FlashCoordinator(QObject* parent)
    : QObject(parent)
    , m_state(sak::FlashState::Idle)
    , m_verificationEnabled(true)
    , m_bufferSize(kDefaultFlashBufferSizeMb * sak::kBytesPerMB)
    , m_maxConcurrentWrites(kDefaultMaxConcurrentWrites)
    , m_ejectOnCompletion(false)
    , m_nextWorkerIndex(0)
    // Most thorough by default. A caller that never calls setValidationMode gets the same
    // behaviour FlashWorker had on its own, so this wiring cannot weaken an existing run.
    , m_validationMode(sak::ValidationMode::Full)
    , m_isCancelled(false) {
    m_progress.state = sak::FlashState::Idle;
    m_progress.percentage = 0.0;
    m_progress.bytesWritten = 0;
    m_progress.totalBytes = 0;
    m_progress.speedMBps = 0.0;
    m_progress.activeDrives = 0;
    m_progress.failedDrives = 0;
    m_progress.completedDrives = 0;
}

FlashCoordinator::~FlashCoordinator() {
    if (isFlashing()) {
        cancel();
    }
    cleanupWorkers();
}

bool FlashCoordinator::startFlash(const QString& imagePath, const QStringList& targetDrives) {
    // m_imageSource is null on entry and created by prepareImageSource() below; do not assert it.
    // An empty imagePath is likewise not asserted: validateImagePath() below rejects it.
    if (targetDrives.isEmpty()) {
        sak::logError("No target drives specified");
        Q_EMIT flashError("No target drives specified");
        return false;
    }

    // Atomically refuse re-entry AND claim the run (-> Validating). Closes the window
    // where a second startFlash slips past a plain isFlashing() check while the first
    // is still in its synchronous Validating/Unmounting phase.
    if (!beginFlashClaim()) {
        sak::logError("Flash already in progress");
        return false;
    }

    sak::logInfo(QString("Starting flash: %1 to %2 drives")
                     .arg(imagePath)
                     .arg(targetDrives.size())
                     .toStdString());

    if (!validateImagePath(imagePath)) {
        return false;
    }

    m_isCancelled = false;
    m_targetDrives = targetDrives;

    // Reset per-run progress and result. The coordinator is long-lived and reused across flashes;
    // without this a stale non-zero completedDrives/failedDrives from a prior run satisfies the
    // finalize predicate on the first completion of the next run, aborting still-active workers.
    m_progress.completedDrives = 0;
    m_progress.failedDrives = 0;
    m_progress.activeDrives = 0;
    m_progress.bytesWritten = 0;
    m_progress.speedMBps = 0.0;
    m_progress.percentage = 0.0;
    m_result = sak::FlashResult{};
    m_reportedWorkers.clear();
    m_flashTimer.start();

    // Validate targets
    m_state = sak::FlashState::Validating;
    Q_EMIT stateChanged(m_state, "Validating targets...");

    if (!validateTargets(targetDrives)) {
        sak::logError("Target validation failed");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Validation failed");
        Q_EMIT flashError("Target validation failed");
        return false;
    }

    if (!prepareImageSource(imagePath)) {
        return false;
    }

    m_progress.totalBytes = m_imageSource->size() * targetDrives.size();

    // Source checksum is calculated by each FlashWorker on its own
    // thread, not here on the UI thread (avoids freezing the GUI
    // for minutes with large images).

    return unmountAndFlash(imagePath, targetDrives);
}

bool FlashCoordinator::validateImagePath(const QString& imagePath) {
    sak::path_validation_config img_cfg;
    img_cfg.must_exist = true;
    img_cfg.must_be_file = true;
    img_cfg.check_read_permission = true;
    auto path_result =
        sak::input_validator::validatePath(std::filesystem::path(imagePath.toStdString()), img_cfg);
    if (!path_result) {
        sak::logError("Image path validation failed: {}", path_result.error_message);
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Invalid image path");
        Q_EMIT flashError(
            QString::fromStdString("Image path validation failed: " + path_result.error_message));
        return false;
    }
    // Reject a zero-length image (fail closed). It would write nothing yet pass:
    // imageFitsDevice(0,cap) is true, the write loop is skipped, and a full verify
    // hashes 0 bytes so source==target==SHA512(empty) -> a false "success". The
    // path validator only checks existence/type, not that there is data to flash.
    if (QFileInfo(imagePath).size() == 0) {
        sak::logError("Refusing to flash a zero-length image");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Empty image");
        Q_EMIT flashError("Image file is empty (0 bytes); nothing to flash");
        return false;
    }
    return true;
}

bool FlashCoordinator::prepareImageSource(const QString& imagePath) {
    // m_imageSource is created here; asserting it non-null on entry is inverted (it is null).
    // Sole caller startFlash() runs validateImagePath() (must_exist + must_be_file) first, so an
    // empty path cannot reach this.
    Q_ASSERT(!imagePath.isEmpty());
    if (CompressedImageSource::isCompressed(imagePath)) {
        m_imageSource = std::make_unique<CompressedImageSource>(imagePath);
    } else if (isUnsupportedCompressedContainer(imagePath)) {
        sak::logError("Refusing to flash an unsupported compressed container (e.g. .zip)");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Unsupported compressed image");
        Q_EMIT flashError("Unsupported compressed image; extract the disk image before flashing");
        return false;
    } else {
        m_imageSource = std::make_unique<FileImageSource>(imagePath);
    }

    if (!m_imageSource->open()) {
        sak::logError("Failed to open image source");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Failed to open image");
        Q_EMIT flashError("Failed to open image file");
        return false;
    }
    return true;
}

bool FlashCoordinator::unmountAndFlash(const QString& imagePath, const QStringList& targetDrives) {
    // Sole caller startFlash() rejects an empty drive list up front and has already run
    // validateImagePath() on the image, which no empty path survives.
    Q_ASSERT(!imagePath.isEmpty());
    Q_ASSERT(!targetDrives.isEmpty());
    m_state = sak::FlashState::Unmounting;
    Q_EMIT stateChanged(m_state, "Unmounting volumes...");

    if (!unmountVolumes(targetDrives)) {
        // unmountVolumes() already emitted flashError() with details
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Failed to unmount target volumes");
        return false;
    }

    m_state = sak::FlashState::Flashing;
    Q_EMIT stateChanged(m_state, QString("Writing to %1 drives...").arg(targetDrives.size()));

    // Build every worker up front, but start only as many as the concurrent-write
    // ceiling allows; the rest are queued and started as earlier drives finish.
    // Construction opens nothing -- FlashWorker opens its image source and its
    // device handle on its own thread inside execute() -- so a queued worker costs
    // no handle while it waits.
    // Point the queue at the first worker THIS run creates rather than at index 0,
    // so a leftover finished worker from a previous run can never be restarted.
    m_nextWorkerIndex = m_workers.size();
    for (const QString& drive : targetDrives) {
        std::unique_ptr<ImageSource> workerSource;
        if (CompressedImageSource::isCompressed(imagePath)) {
            workerSource = std::make_unique<CompressedImageSource>(imagePath);
        } else {
            workerSource = std::make_unique<FileImageSource>(imagePath);
        }

        auto worker = std::make_unique<FlashWorker>(std::move(workerSource), drive);
        worker->setVerificationEnabled(m_verificationEnabled);
        worker->setBufferSize(m_bufferSize);
        worker->setValidationMode(m_validationMode);
        connectWorkerSignals(worker.get());
        m_workers.push_back(std::move(worker));
    }

    startPendingWorkers();

    return true;
}

void FlashCoordinator::startPendingWorkers() {
    std::vector<FlashWorker*> toStart;
    {
        QMutexLocker locker(&m_mutex);
        // A cancelled or already-finished run must not start another drive. Without
        // this, cancelling a queued multi-drive run would tear down the running
        // worker and then start the next queued one on the back of its failure
        // report -- the run would keep writing drives after the user stopped it.
        if (m_isCancelled.load() || m_state != sak::FlashState::Flashing) {
            return;
        }
        // Saturate rather than subtract blind: m_workers is cleared at teardown while
        // m_nextWorkerIndex keeps its value, and an unsigned underflow here would turn
        // "nothing queued" into a huge pending count.
        const size_t queued =
            m_nextWorkerIndex < m_workers.size() ? m_workers.size() - m_nextWorkerIndex : 0;
        const int pending = static_cast<int>(queued);
        const int startable =
            startableWorkerCount(m_maxConcurrentWrites, m_progress.activeDrives, pending);
        for (int i = 0; i < startable; ++i) {
            toStart.push_back(m_workers[m_nextWorkerIndex].get());
            ++m_nextWorkerIndex;
        }
        // Count them as active before releasing the lock so a completion arriving
        // between here and start() cannot see a stale slot as free.
        m_progress.activeDrives += startable;
    }

    for (FlashWorker* worker : toStart) {
        worker->start();
    }
}

int FlashCoordinator::startableWorkerCount(int maxConcurrent, int running, int pending) {
    return sak::startableWorkerCount(maxConcurrent, running, pending);
}

void FlashCoordinator::cancel() {
    if (!isFlashing()) {
        return;
    }

    sak::logInfo("Cancelling flash operation");
    m_isCancelled = true;

    // Cancel all workers
    for (auto& worker : m_workers) {
        worker->requestStop();
    }

    markQueuedWorkersCancelled();

    m_state = sak::FlashState::Cancelled;
    Q_EMIT stateChanged(m_state, "Cancelled by user");
}

void FlashCoordinator::markQueuedWorkersCancelled() {
    // Workers still queued behind the concurrent-write ceiling were never started,
    // so they will never emit a terminal signal of their own. Count them as failed
    // now: without this the finalize predicate (completed + failed == targets) can
    // never be satisfied after a cancel, and the run would wait forever for drives
    // that are never going to be written.
    QMutexLocker locker(&m_mutex);
    while (m_nextWorkerIndex < m_workers.size()) {
        const QString devicePath = m_workers[m_nextWorkerIndex]->targetDevice();
        ++m_nextWorkerIndex;
        m_progress.failedDrives++;
        m_result.failedDrives.append(devicePath);
        m_result.errorMessages.append(
            QString("%1: cancelled before writing started").arg(devicePath));
        sak::logInfo(
            QString("Cancelled queued drive %1 before it started").arg(devicePath).toStdString());
    }
}

bool FlashCoordinator::isFlashing() const {
    QMutexLocker locker(&m_mutex);
    // Reports "busy" for the WHOLE active run (including the synchronous Validating/
    // Unmounting phases), so the re-entry guard cannot be slipped during them.
    return isActiveFlashState(m_state);
}

bool FlashCoordinator::beginFlashClaim() {
    QMutexLocker locker(&m_mutex);
    if (isActiveFlashState(m_state)) {
        return false;  // A run is already active -- refuse re-entry atomically.
    }
    // Claim the run under the lock so a concurrent startFlash cannot also pass the
    // gate before the first one has moved the state out of Idle/Completed/Failed.
    m_state = sak::FlashState::Validating;
    return true;
}

sak::FlashState FlashCoordinator::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

sak::FlashProgress FlashCoordinator::progress() const {
    QMutexLocker locker(&m_mutex);
    return m_progress;
}

void FlashCoordinator::setVerificationEnabled(bool enabled) {
    m_verificationEnabled = enabled;
}

bool FlashCoordinator::isVerificationEnabled() const {
    return m_verificationEnabled;
}

void FlashCoordinator::setBufferSize(qint64 sizeBytes) {
    m_bufferSize = sizeBytes;
}

void FlashCoordinator::setValidationMode(sak::ValidationMode mode) {
    m_validationMode = mode;
}

void FlashCoordinator::setMaxConcurrentWrites(int maxWrites) {
    if (maxWrites < 1) {
        // A ceiling below one would start nothing and hang the run. Refuse it and
        // say so rather than silently substituting a number the caller did not ask
        // for.
        sak::logError("Refusing a concurrent-write ceiling of {}; keeping {}",
                      maxWrites,
                      m_maxConcurrentWrites);
        return;
    }
    m_maxConcurrentWrites = maxWrites;
}

int FlashCoordinator::maxConcurrentWrites() const {
    return m_maxConcurrentWrites;
}

void FlashCoordinator::setEjectOnCompletion(bool eject) {
    m_ejectOnCompletion = eject;
}

bool FlashCoordinator::ejectOnCompletion() const {
    return m_ejectOnCompletion;
}

void FlashCoordinator::onWorkerProgress(double percentage, qint64 bytesWritten) {
    Q_UNUSED(percentage);
    Q_UNUSED(bytesWritten);
    updateProgress();
}

void FlashCoordinator::connectWorkerSignals(FlashWorker* worker) {
    connect(worker, &FlashWorker::progressUpdated, this, &FlashCoordinator::onWorkerProgress);
    connect(
        worker, &FlashWorker::verificationCompleted, this, &FlashCoordinator::onWorkerCompleted);
    connect(worker, &FlashWorker::error, this, &FlashCoordinator::onWorkerFailed);

    // Safety net: an exception inside FlashWorker::execute() (e.g. std::bad_alloc)
    // surfaces ONLY as WorkerBase::failed/cancelled, which none of the FlashWorker
    // signals above cover. Without this the run would never finalize and the caller
    // would block until its timeout. Deduped by onWorkerFailedFor so a normal handled
    // error (which emits BOTH FlashWorker::error AND WorkerBase::failed) is not counted
    // twice.
    connect(worker, &WorkerBase::failed, this, [this, worker](int, const QString& msg) {
        onWorkerFailedFor(worker,
                          msg.isEmpty() ? QStringLiteral("Worker aborted unexpectedly") : msg);
    });
    connect(worker, &WorkerBase::cancelled, this, [this, worker]() {
        onWorkerFailedFor(worker, QStringLiteral("Worker cancelled before completion"));
    });

    // When verification is disabled, verificationCompleted never fires. Use
    // writeCompleted as the completion signal in that case.
    if (!m_verificationEnabled) {
        connect(worker, &FlashWorker::writeCompleted, this, [this](qint64 /*bytesWritten*/) {
            sak::ValidationResult result;
            result.passed = true;
            onWorkerCompleted(result);
        });
    }
}

QString FlashCoordinator::recordDriveCompletion(const QString& devicePath,
                                                const sak::ValidationResult& result) {
    // Called with m_mutex held. completedDrives counts SUCCESSES only (failures bump
    // failedDrives); a failed verification must not bump both counters or it would
    // contribute 2 to the finalize sum and tear down still-active workers early.
    // Returns the error message on failure (empty on success).
    if (!result.passed) {
        sak::logError(QString("Verification failed for drive: %1").arg(devicePath).toStdString());
        m_progress.failedDrives++;
        m_result.failedDrives.append(devicePath);
        const QString errorMsg = result.errors.isEmpty() ? QStringLiteral("Verification failed")
                                                         : result.errors.first();
        m_result.errorMessages.append(QString("%1: %2").arg(devicePath).arg(errorMsg));
        return errorMsg;
    }
    m_progress.completedDrives++;
    m_result.successfulDrives.append(devicePath);
    // The verified device checksum (== source when verification is on).
    if (m_result.sourceChecksum.isEmpty() && !result.targetChecksum.isEmpty()) {
        m_result.sourceChecksum = result.targetChecksum;
    }
    return QString();
}

void FlashCoordinator::onWorkerCompleted(const sak::ValidationResult& result) {
    // m_targetDrives is assigned only in startFlash(), after its empty-list rejection, and is
    // never cleared; no worker exists to signal this slot before that assignment.
    Q_ASSERT(!m_targetDrives.isEmpty());
    const FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }

    QString devicePath = worker->targetDevice();
    sak::logInfo(QString("Drive completed: %1").arg(devicePath).toStdString());

    // Mutate coordinator state under the lock, but capture everything the signals
    // need into locals and EMIT AFTER releasing it. Emitting under m_mutex deadlocks
    // a same-thread (direct-connected) slot that calls a locked getter -- state(),
    // progress(), isFlashing() (B10-07). cleanupWorkers() (which can wait() up to 15s)
    // likewise runs unlocked so it never stalls those getters.
    QString errorMsg;
    bool passed = result.passed;
    bool terminal = false;
    sak::FlashState terminalState{};
    sak::FlashResult terminalResult;
    QString terminalMessage;
    {
        QMutexLocker locker(&m_mutex);
        // Mark this worker as having reported a terminal outcome so a later WorkerBase
        // failed()/cancelled() for the same drive is not double-counted as a failure.
        m_reportedWorkers.insert(worker);
        m_progress.activeDrives--;
        errorMsg = recordDriveCompletion(devicePath, result);

        if (m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
            // The terminal state must reflect the RUN outcome, not which handler fired
            // last: if any drive failed (e.g. the last worker to report failed its
            // verification), the run FAILED even though this is the success handler.
            const bool anyFailed = m_progress.failedDrives > 0;
            m_state = anyFailed ? sak::FlashState::Failed : sak::FlashState::Completed;
            m_result.success = !anyFailed;
            finalizeResultMetrics();
            terminal = true;
            terminalState = m_state;
            terminalResult = m_result;
            terminalMessage = QString("%1: %2 successful, %3 failed")
                                  .arg(anyFailed ? QStringLiteral("Failed")
                                                 : QStringLiteral("Completed"))
                                  .arg(m_result.successfulDrives.size())
                                  .arg(m_result.failedDrives.size());
        }
    }

    if (!passed) {
        Q_EMIT driveFailed(devicePath, errorMsg);
    } else {
        Q_EMIT driveCompleted(devicePath, result.targetChecksum);
    }

    if (terminal) {
        emitTerminalOutcome(terminalState, terminalResult, terminalMessage);
        return;
    }

    // A slot freed up: let the next queued drive start.
    startPendingWorkers();
}

void FlashCoordinator::onWorkerFailed(const QString& error) {
    // See onWorkerCompleted: m_targetDrives is set before any worker can signal, never cleared.
    Q_ASSERT(!m_targetDrives.isEmpty());
    // `error` crosses a signal/slot boundary from FlashWorker, so it is not an invariant of
    // this class and must not abort the process in Debug while Release carries on. An empty
    // message is not a reason to drop the failure - the drive still failed - so substitute a
    // usable one and keep going.
    const QString reportedError =
        error.isEmpty() ? tr("Flash failed (the worker reported no reason)") : error;
    const FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }
    onWorkerFailedFor(worker, reportedError);
}

void FlashCoordinator::onWorkerFailedFor(const FlashWorker* worker, const QString& error) {
    // Reached only from a worker signal, and workers are created after startFlash() assigns the
    // (non-empty) target list.
    Q_ASSERT(!m_targetDrives.isEmpty());
    if (!worker) {
        return;
    }

    // Mutate under the lock; emit + cleanupWorkers() AFTER releasing it (see
    // onWorkerCompleted for the deadlock rationale, B10-07).
    QString devicePath;
    bool terminal = false;
    sak::FlashState terminalState{};
    sak::FlashResult terminalResult;
    QString terminalMessage;
    {
        QMutexLocker locker(&m_mutex);
        // Dedup BEFORE dereferencing the worker. A normal handled error emits BOTH
        // FlashWorker::error and (as execute() returns unexpected) WorkerBase::failed; the
        // first finalizes the run and cleanupWorkers() DESTROYS the worker, so by the time
        // the second (deduped) call arrives the captured pointer may dangle. contains()
        // only compares the pointer value (no deref), so it safely rejects the stale
        // pointer here; calling worker->targetDevice() before this check would be a UAF.
        if (m_reportedWorkers.contains(worker)) {
            return;
        }
        m_reportedWorkers.insert(worker);

        devicePath = worker->targetDevice();
        sak::logError(QString("Drive failed: %1 - %2").arg(devicePath, error).toStdString());
        m_progress.failedDrives++;
        m_progress.activeDrives--;

        m_result.failedDrives.append(devicePath);
        m_result.errorMessages.append(QString("%1: %2").arg(devicePath, error));

        if (m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
            m_state = sak::FlashState::Failed;
            m_result.success = false;
            finalizeResultMetrics();
            terminal = true;
            terminalState = m_state;
            terminalResult = m_result;
            terminalMessage = QString("Failed: %1 successful, %2 failed")
                                  .arg(m_result.successfulDrives.size())
                                  .arg(m_result.failedDrives.size());
        }
    }

    Q_EMIT driveFailed(devicePath, error);

    if (terminal) {
        emitTerminalOutcome(terminalState, terminalResult, terminalMessage);
        return;
    }

    // One drive failing does not abandon the rest of the queue -- the remaining
    // targets are independent disks. startPendingWorkers() refuses to start
    // anything once the run has been cancelled.
    startPendingWorkers();
}

void FlashCoordinator::emitTerminalOutcome(sak::FlashState state,
                                           const sak::FlashResult& result,
                                           const QString& statusMessage) {
    // Re-online every drive we took offline -- BOTH successful AND failed targets.
    // The unmount step set a PERSISTENT OFFLINE attribute (so Windows could not
    // auto-mount and corrupt the write); it survives the flash. Leaving a FAILED
    // target offline would strand it, requiring a manual `diskpart online disk`.
    // Re-onlining a half-written disk is safe: it cannot corrupt anything further,
    // and the user can immediately re-partition or re-flash it.
    reonlineDrives(result.successfulDrives + result.failedDrives);

    // Eject AFTER re-onlining, never instead of it. The OFFLINE attribute is
    // persistent, so ejecting while it is still set would leave a drive that comes
    // back offline the next time it is plugged in -- the eject must remove a normal,
    // online disk.
    // ejectCompletedDrives opens devices, so it runs here with m_mutex released.
    sak::FlashResult finalResult = result;
    if (m_ejectOnCompletion) {
        ejectCompletedDrives(finalResult);
    }

    Q_EMIT stateChanged(state, statusMessage);
    Q_EMIT flashCompleted(finalResult);
    cleanupWorkers();
}

void FlashCoordinator::ejectCompletedDrives(sak::FlashResult& result) {
    DriveUnmounter unmounter;
    for (const QString& devicePath : result.successfulDrives) {
        const int driveNumber = parsePhysicalDriveNumber(devicePath);
        if (driveNumber < 0 || driveNumber > kMaxPhysicalDriveNumber) {
            result.ejectFailedDrives.append(devicePath);
            sak::logWarning(
                QString("Cannot eject %1: unparseable drive number").arg(devicePath).toStdString());
            continue;
        }
        if (unmounter.ejectDrive(driveNumber)) {
            result.ejectedDrives.append(devicePath);
            continue;
        }
        // Not a flash failure: the image is written and verified. It is a removal
        // failure, and it is reported as one so the user does not unplug a drive
        // Windows still has mounted.
        result.ejectFailedDrives.append(devicePath);
        sak::logWarning(QString("Flashed %1 but could not eject it: %2")
                            .arg(devicePath, unmounter.lastError())
                            .toStdString());
    }
}

void FlashCoordinator::reonlineDrives(const QStringList& drivePaths) {
    if (drivePaths.isEmpty()) {
        return;
    }
    DriveUnmounter unmounter;
    for (const QString& devicePath : drivePaths) {
        const int driveNumber = parsePhysicalDriveNumber(devicePath);
        if (driveNumber < 0 || driveNumber > kMaxPhysicalDriveNumber) {
            sak::logWarning(QString("Cannot re-online %1: unparseable drive number")
                                .arg(devicePath)
                                .toStdString());
            continue;
        }
        if (!unmounter.allowAutoMount(driveNumber)) {
            sak::logWarning(QString("Failed to bring drive %1 back online: %2")
                                .arg(devicePath, unmounter.lastError())
                                .toStdString());
        }
    }
}

int FlashCoordinator::parsePhysicalDriveNumber(const QString& devicePath) {
    const QString prefix = QStringLiteral("PhysicalDrive");
    const int idx = devicePath.lastIndexOf(prefix);
    if (idx < 0) {
        return -1;
    }
    bool ok = false;
    const int number = devicePath.mid(idx + prefix.size()).toInt(&ok);
    return ok ? number : -1;
}

QString FlashCoordinator::firstDuplicateTarget(const QStringList& targetDrives) {
    QSet<QString> seen;
    for (const QString& devicePath : targetDrives) {
        const QString key = devicePath.trimmed().toLower();
        if (seen.contains(key)) {
            return devicePath;
        }
        seen.insert(key);
    }
    return QString();
}

bool FlashCoordinator::validateTargets(const QStringList& targetDrives) {
    // Sole caller startFlash() returns false on an empty drive list before reaching here.
    Q_ASSERT(!targetDrives.isEmpty());

    // Reject duplicate target paths: two workers writing the SAME physical disk
    // concurrently interleave their raw writes and corrupt each other.
    const QString duplicate = firstDuplicateTarget(targetDrives);
    if (!duplicate.isEmpty()) {
        sak::logError(QString("Duplicate target device rejected: %1").arg(duplicate).toStdString());
        Q_EMIT flashError(QString("Duplicate target device: %1").arg(duplicate));
        return false;
    }

    // cppcheck-suppress useStlAlgorithm ; early-return with per-device error reporting
    for (const QString& devicePath : targetDrives) {
        if (!validateSingleTarget(devicePath)) {
            return false;
        }
    }

    return true;
}

bool FlashCoordinator::validateSingleTarget(const QString& devicePath) {
    // Open device handle to verify it exists and is accessible
    HANDLE hDevice = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()),
                                 GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 0,
                                 nullptr);

    if (hDevice == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        sak::logError(
            QString("Failed to open device %1: Error %2").arg(devicePath).arg(error).toStdString());
        Q_EMIT flashError(QString("Cannot access device %1. Error: %2").arg(devicePath).arg(error));
        return false;
    }

    // Get device geometry to verify it's a valid disk
    DISK_GEOMETRY geometry;
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice,
                         IOCTL_DISK_GET_DRIVE_GEOMETRY,
                         nullptr,
                         0,
                         &geometry,
                         sizeof(geometry),
                         &bytesReturned,
                         nullptr)) {
        DWORD error = GetLastError();
        CloseHandle(hDevice);
        sak::logError(QString("Failed to get geometry for %1: Error %2")
                          .arg(devicePath)
                          .arg(error)
                          .toStdString());
        Q_EMIT flashError(
            QString("Device %1 is not a valid disk. Error: %2").arg(devicePath).arg(error));
        return false;
    }

    // Confirm the path really opens the physical disk its name claims. A crafted
    // DosDevice alias could make "\\.\PhysicalDriveN" open a DIFFERENT device than
    // disk N, so the parsed-number OS guard below would probe the wrong disk. Read
    // the ACTUAL device number from THIS handle and require it to match (fail
    // closed on any mismatch or on a query failure).
    const int actualNumber = queryHandleDriveNumber(hDevice);
    CloseHandle(hDevice);

    const int parsedNumber = parsePhysicalDriveNumber(devicePath);
    if (actualNumber < 0 || parsedNumber < 0 || actualNumber != parsedNumber) {
        sak::logError(QString("Refusing %1: device number mismatch (path %2, actual %3)")
                          .arg(devicePath)
                          .arg(parsedNumber)
                          .arg(actualNumber)
                          .toStdString());
        Q_EMIT flashError(
            QString("Refusing to write %1: it does not resolve to the named physical disk")
                .arg(devicePath));
        return false;
    }

    // Engine-level safety gate: never raw-write the current OS disk (fail closed).
    if (!passesOsDiskGuard(devicePath)) {
        return false;
    }

    sak::logInfo(QString("Validated device: %1").arg(devicePath).toStdString());
    return true;
}

bool FlashCoordinator::passesOsDiskGuard(const QString& devicePath) {
    // Parse the physical drive number and refuse if it backs the system volume OR
    // if the OS-disk identity cannot be established -- no fallback to "assume safe",
    // which would allow a write when protection is unproven.
    const int driveNumber = parsePhysicalDriveNumber(devicePath);
    if (driveNumber < 0) {
        // Cannot parse the physical-drive number -> cannot run the OS-disk guard.
        sak::logError(QString("Refusing raw write to %1: cannot parse a PhysicalDrive number "
                              "for the OS-disk safety check")
                          .arg(devicePath)
                          .toStdString());
        Q_EMIT flashError(QString("Refusing to write %1: unable to verify it is not the OS disk")
                              .arg(devicePath));
        return false;
    }
    const OsDiskCheck osCheck = physicalDriveOsDiskCheck(driveNumber);
    if (osCheck != OsDiskCheck::NotSystem) {
        const bool undetermined = (osCheck == OsDiskCheck::Undetermined);
        sak::logError(QString("Refusing raw write to %1: %2")
                          .arg(devicePath,
                               undetermined ? QStringLiteral("OS system-disk identity could not be "
                                                             "established")
                                            : QStringLiteral("it backs the OS system volume"))
                          .toStdString());
        Q_EMIT flashError(
            undetermined
                ? QString("Refusing to write %1: could not verify it is not the OS disk")
                      .arg(devicePath)
                : QString("Refusing to write %1: it is the current OS disk").arg(devicePath));
        return false;
    }
    return passesBootDiskGuard(devicePath, driveNumber);
}

bool FlashCoordinator::passesBootDiskGuard(const QString& devicePath, int driveNumber) {
    // The OS-disk check above only covers the disk backing the running %WINDIR%
    // volume. On split-boot hardware the boot manager / ESP that actually boots the
    // OS can live on a SEPARATE physical disk; erasing it makes the system
    // unbootable. This independent, fail-closed probe protects that disk too.
    const sak::DiskProbe boot = DriveScanner::physicalDriveBootProbe(driveNumber);
    if (boot == sak::DiskProbe::No) {
        return true;
    }
    const bool undetermined = (boot == sak::DiskProbe::Undetermined);
    sak::logError(QString("Refusing raw write to %1: %2")
                      .arg(devicePath,
                           undetermined
                               ? QStringLiteral("boot-disk status could not be established")
                               : QStringLiteral("it carries the Windows boot loader"))
                      .toStdString());
    Q_EMIT flashError(
        undetermined
            ? QString("Refusing to write %1: could not verify it is not a boot disk")
                  .arg(devicePath)
            : QString("Refusing to write %1: it is a Windows boot/system disk").arg(devicePath));
    return false;
}

bool FlashCoordinator::unmountVolumes(const QStringList& targetDrives) {
    // Sole caller unmountAndFlash() forwards startFlash()'s list, already rejected if empty.
    Q_ASSERT(!targetDrives.isEmpty());
    DriveUnmounter unmounter;
    // Track drives we successfully took offline so we can roll them back online if a
    // LATER target fails to unmount -- otherwise earlier targets are stranded offline.
    std::vector<int> offlined;

    for (const QString& devicePath : targetDrives) {
        sak::logInfo(QString("Unmounting volumes on %1").arg(devicePath).toStdString());

        // Extract drive number from path (e.g., "\\.\PhysicalDrive1" -> 1)
        const int driveNumber = parsePhysicalDriveNumber(devicePath);

        if (driveNumber < 0 || driveNumber > kMaxPhysicalDriveNumber) {
            sak::logError(QString("Invalid device path format or drive number out of range: %1")
                              .arg(devicePath)
                              .toStdString());
            Q_EMIT flashError(QString("Invalid device path format: %1").arg(devicePath));
            rollbackOfflinedDrives(unmounter, offlined);
            return false;
        }

        if (!unmounter.unmountDrive(driveNumber)) {
            sak::logError(QString("Failed to unmount volumes on %1").arg(devicePath).toStdString());
            Q_EMIT flashError(
                QString("Failed to unmount volumes on %1. "
                        "Please close any applications using this drive and try again.")
                    .arg(devicePath));
            rollbackOfflinedDrives(unmounter, offlined);
            return false;
        }

        offlined.push_back(driveNumber);
        sak::logInfo(QString("Successfully unmounted volumes on %1").arg(devicePath).toStdString());
    }

    return true;
}

void FlashCoordinator::rollbackOfflinedDrives(DriveUnmounter& unmounter,
                                              const std::vector<int>& offlined) {
    // Best-effort: clear the persistent OFFLINE attribute on drives we already
    // offlined this run, so a mid-list unmount failure does not strand them.
    for (const int driveNumber : offlined) {
        if (!unmounter.allowAutoMount(driveNumber)) {
            sak::logWarning(QString("Rollback: failed to bring drive %1 back online: %2")
                                .arg(driveNumber)
                                .arg(unmounter.lastError())
                                .toStdString());
        }
    }
}

void FlashCoordinator::updateProgress() {
    // Compute under the lock, emit the snapshot after releasing it (B10-07): a
    // direct-connected progressUpdated slot that reads progress()/state() would
    // otherwise re-lock the non-recursive m_mutex and deadlock.
    sak::FlashProgress snapshot;
    {
        QMutexLocker locker(&m_mutex);
        m_progress.bytesWritten = 0;
        double totalSpeed = 0.0;

        for (const auto& worker : m_workers) {
            m_progress.bytesWritten += worker->bytesWritten();
            totalSpeed += worker->speedMBps();
        }

        m_progress.speedMBps = totalSpeed;
        m_progress.percentage = m_progress.getOverallProgress();
        m_progress.currentOperation =
            QString("Writing to %1 drives...").arg(m_progress.activeDrives);
        snapshot = m_progress;
    }

    Q_EMIT progressUpdated(snapshot);
}

void FlashCoordinator::finalizeResultMetrics() {
    // m_mutex is already held by the caller (the finalize branch of a worker handler).
    qint64 total_bytes = 0;
    for (const std::unique_ptr<FlashWorker>& worker : m_workers) {
        if (worker) {
            total_bytes += worker->bytesWritten();
        }
    }
    m_result.bytesWritten = total_bytes;
    m_result.elapsedSeconds = static_cast<double>(m_flashTimer.elapsed()) / 1000.0;
}

void FlashCoordinator::cleanupWorkers() {
    // m_imageSource may legitimately be null (called from the dtor when no flash ran, and after a
    // reset); the `if (m_imageSource)` guard below handles it. No entry assert.
    // Wait for all workers to finish with cooperative stop
    for (auto& worker : m_workers) {
        if (!worker->isRunning()) {
            continue;
        }
        sak::logInfo("Requesting worker thread to stop...");
        worker->requestStop();

        if (worker->wait(kWorkerShutdownTimeoutMs)) {
            sak::logInfo("Worker thread stopped gracefully");
            continue;
        }

        // Refuse-to-stop (wedged in a long WriteFile). Do NOT destroy it here:
        // ~FlashWorker would terminate() a live raw-disk write -> corrupted media
        // and a leaked handle. Detach it as a bounded, intentional leak so it
        // finishes its current write and self-cleans, instead of being killed
        // mid-write.
        sak::logError(
            "Worker thread did not stop within 15s -- detaching "
            "to avoid terminating a live disk write");
        // Self-delete once the wedged write finally finishes, so a repeated wedge
        // does not leak a QThread + FlashWorker each time. Connect BEFORE releasing
        // ownership; QThread::finished fires on every exit path once run() returns,
        // and the queued deleteLater reclaims the detached worker on the owning
        // thread's event loop.
        FlashWorker* detached = worker.get();
        connect(detached, &QThread::finished, detached, &QObject::deleteLater);
        detached->setParent(nullptr);
        static_cast<void>(worker.release());
    }

    m_workers.clear();
    // The queue index is an index into m_workers; leaving it behind would make it
    // point past the end of an empty vector.
    m_nextWorkerIndex = 0;

    if (m_imageSource) {
        m_imageSource->close();
        m_imageSource.reset();
    }
}
