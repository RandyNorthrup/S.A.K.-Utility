// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/flash_coordinator.h"

#include "sak/drive_unmounter.h"
#include "sak/flash_worker.h"
#include "sak/flasher_policy.h"
#include "sak/input_validator.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <vector>

#include <windows.h>

#include <winioctl.h>

namespace {
constexpr qint64 kDefaultFlashBufferSizeMb = 256;
// Per-worker I/O buffer ceiling. Each running worker allocates one, and the size can
// come from an attacker-writable config file, so it is bounded rather than passed
// through to an allocation. Well above the 512 MB the settings dialog offers.
constexpr qint64 kMaxFlashBufferSizeMb = 1024;
// Upper bound on a parsed \\.\PhysicalDriveN index. It exists to catch a garbage
// parse, not to cap real hardware, so it is high enough for an enterprise JBOD -- a
// lower bound silently refused legitimate high-numbered disks after they had already
// passed validation.
constexpr int kMaxPhysicalDriveNumber = 1023;
// Ceiling on targets in one run. Every target costs a worker, a thread and a device
// handle, and they are all created AFTER the disks have been taken offline -- an
// unbounded list turns an allocation failure there into offline disks with no writer.
constexpr int kMaxTargetDrives = 64;
// One drive at a time unless a caller raises it. Matches the ConfigManager default
// the settings dialog shows, so an untouched install behaves the way the dialog says.
constexpr int kDefaultMaxConcurrentWrites = 1;
constexpr int kWorkerShutdownTimeoutMs = sak::kTimeoutThreadShutdownMs;

// ZIP local-file-header signature "PK\x03\x04": four bytes, with 0x03 at index 2 and 0x04 at
// index 3 (index 0/1 hold 'P'/'K').
constexpr int kZipSignatureLength = 4;
constexpr int kZipSignatureThirdByteIndex = 2;
constexpr int kZipSignatureFourthByteIndex = 3;

// Milliseconds in one second, for converting an elapsed-timer reading to fractional seconds.
constexpr double kMillisecondsPerSecond = 1000.0;

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
    if (DeviceIoControl(handle,
                        IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        nullptr,
                        0,
                        &deviceNumber,
                        sizeof(deviceNumber),
                        &bytesReturned,
                        nullptr) == 0) {
        return -1;
    }
    // A "successful" short reply would leave the zero-initialized struct in place and
    // let a zeroed DeviceNumber pass for a real answer. Require the whole record.
    if (bytesReturned < sizeof(deviceNumber)) {
        return -1;
    }
    if (deviceNumber.DeviceType != FILE_DEVICE_DISK) {
        return -1;
    }
    return static_cast<int>(deviceNumber.DeviceNumber);
}

// True when the file really begins with a ZIP local-file-header signature
// ("PK\x03\x04"), whatever its extension says. Returns false when the file cannot be
// opened or is shorter than the signature; the caller pairs this with the
// extension-based check and the image source's own open(), which both fail closed.
bool hasZipContainerMagic(const QString& imagePath) {
    QFile file(imagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    char magic[kZipSignatureLength] = {};
    const qint64 read = file.read(magic, static_cast<qint64>(sizeof(magic)));
    return read == static_cast<qint64>(sizeof(magic)) && magic[0] == 'P' && magic[1] == 'K' &&
           magic[kZipSignatureThirdByteIndex] == '\x03' &&
           magic[kZipSignatureFourthByteIndex] == '\x04';
}

// A file the flasher recognizes as a compressed container but the streaming
// DecompressorFactory cannot turn into a raw disk image (currently .zip -- a
// multi-member archive). Such a file MUST be refused, never raw-written: writing
// the archive bytes verbatim produces a corrupt, unbootable target.
bool isUnsupportedCompressedContainer(const QString& imagePath) {
    if (CompressedImageSource::isCompressed(imagePath)) {
        return false;  // The streaming decompressor turns this one into raw bytes.
    }
    // detectFormat() reads the EXTENSION only, so a .zip renamed to .img would sail
    // past it and be written to the disk verbatim. Sniff the container signature too
    // and refuse on either signal.
    return FileImageSource::detectFormat(imagePath) == sak::ImageFormat::ZIP ||
           hasZipContainerMagic(imagePath);
}

// True only when a SUCCESSFUL IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS reply actually
// carries the extents it claims. A short or empty "success" would otherwise be read
// straight out of the zero-initialized buffer, walk no extents at all, and clear the
// OS disk for a raw write on no evidence.
bool volumeExtentsReplyIsComplete(const VOLUME_DISK_EXTENTS* extents,
                                  DWORD bytesReturned,
                                  DWORD maxExtents) {
    if (extents->NumberOfDiskExtents == 0 || extents->NumberOfDiskExtents > maxExtents) {
        return false;
    }
    const size_t headerSize = offsetof(VOLUME_DISK_EXTENTS, Extents);
    const size_t returned = bytesReturned;
    return returned >= headerSize + (extents->NumberOfDiskExtents * sizeof(DISK_EXTENT));
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
    const DWORD bufSize = sizeof(VOLUME_DISK_EXTENTS) + ((kMaxExtents - 1) * sizeof(DISK_EXTENT));
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
    if (ok == 0) {
        return OsDiskCheck::Undetermined;
    }
    // Fail closed on a reply that does not carry the extents it claims.
    if (!volumeExtentsReplyIsComplete(extents, bytesReturned, kMaxExtents)) {
        return OsDiskCheck::Undetermined;
    }
    for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < kMaxExtents; ++i) {
        if (static_cast<int>(extents->Extents[i].DiskNumber) == driveNumber) {
            return OsDiskCheck::IsSystem;
        }
    }
    return OsDiskCheck::NotSystem;
}

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
    // A run torn down by destruction (owner shutdown, a timeout, a cancel that the
    // workers answer after we are gone) never reaches emitTerminalOutcome, so nothing
    // has cleared the PERSISTENT OFFLINE attribute the unmount step set. Clear it here
    // or the targets stay offline until someone runs `diskpart online disk` by hand.
    const QStringList stranded = reonlineOfflinedDrives();
    if (!stranded.isEmpty()) {
        sak::logError(QString("Drives left OFFLINE at shutdown (need `diskpart online disk`): %1")
                          .arg(stranded.join(QStringLiteral(", ")))
                          .toStdString());
    }
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
        // Say so on the error channel too: a caller that only listens to flashError
        // (the AI action path does) would otherwise see a bare false with no reason.
        Q_EMIT flashError("A flash run is already in progress");
        return false;
    }

    sak::logInfo(QString("Starting flash: %1 to %2 drives")
                     .arg(imagePath)
                     .arg(targetDrives.size())
                     .toStdString());

    // Before the first step that can fail, so a rejected image cannot leave the
    // PREVIOUS run's totals, operation text and results on display under the new state.
    resetRunState(targetDrives);

    if (!validateImagePath(imagePath)) {
        return false;
    }

    // Validate targets
    Q_EMIT stateChanged(sak::FlashState::Validating, "Validating targets...");

    if (!validateTargets(targetDrives)) {
        sak::logError("Target validation failed");
        failRun("Validation failed", "Target validation failed");
        return false;
    }

    if (!prepareImageSource(imagePath)) {
        return false;
    }

    if (!recordTotalBytes(imagePath, targetDrives.size())) {
        return false;
    }

    // Source checksum is calculated by each FlashWorker on its own
    // thread, not here on the UI thread (avoids freezing the GUI
    // for minutes with large images).

    return unmountAndFlash(imagePath, targetDrives);
}

void FlashCoordinator::resetRunState(const QStringList& targetDrives) {
    // The coordinator is long-lived and reused across flashes; without this a stale
    // non-zero completedDrives/failedDrives from a prior run satisfies the finalize
    // predicate on the first completion of the next run, aborting still-active workers.
    // Everything here is read by the locked cross-thread getters, so it is written
    // under the same lock rather than raced against them.
    const QMutexLocker locker(&m_mutex);
    m_isCancelled = false;
    m_targetDrives = targetDrives;
    m_progress.completedDrives = 0;
    m_progress.failedDrives = 0;
    m_progress.activeDrives = 0;
    m_progress.bytesWritten = 0;
    m_progress.totalBytes = 0;
    m_progress.speedMBps = 0.0;
    m_progress.percentage = 0.0;
    m_progress.currentOperation.clear();
    m_progress.state = m_state;
    m_result = sak::FlashResult{};
    m_reportedWorkers.clear();
    m_flashTimer.start();
}

bool FlashCoordinator::recordTotalBytes(const QString& imagePath, qsizetype targetCount) {
    // The opened source must report a positive, representable size BEFORE any disk is
    // taken offline. A container whose header does not yield a decompressed size
    // reports 0 or -1 here; accepting it would offline every target and then hand the
    // workers an image they reject, and it would make every progress figure meaningless.
    const qint64 imageBytes = m_imageSource->size();
    if (imageBytes <= 0) {
        sak::logError(QString("Refusing %1: the image source reports a size of %2 bytes")
                          .arg(imagePath)
                          .arg(imageBytes)
                          .toStdString());
        failRun("Unknown image size",
                QString("Refusing to flash %1: its uncompressed size could not be determined")
                    .arg(imagePath));
        return false;
    }
    if (targetCount <= 0 || imageBytes > std::numeric_limits<qint64>::max() / targetCount) {
        sak::logError(QString("Refusing %1: %2 bytes across %3 targets overflows the byte total")
                          .arg(imagePath)
                          .arg(imageBytes)
                          .arg(targetCount)
                          .toStdString());
        failRun(
            "Image too large",
            QString("Refusing to flash %1: the reported image size is not usable").arg(imagePath));
        return false;
    }
    const QMutexLocker locker(&m_mutex);
    m_progress.totalBytes = imageBytes * targetCount;
    return true;
}

void FlashCoordinator::setState(sak::FlashState state) {
    const QMutexLocker locker(&m_mutex);
    m_state = state;
    // FlashProgress carries its own copy of the state and callers read it from the
    // progress snapshot; leaving it at Idle for the whole run misreports the operation.
    m_progress.state = state;
}

void FlashCoordinator::failRun(const QString& stateMessage, const QString& errorMessage) {
    setState(sak::FlashState::Failed);
    Q_EMIT stateChanged(sak::FlashState::Failed, stateMessage);
    if (!errorMessage.isEmpty()) {
        Q_EMIT flashError(errorMessage);
    }
    releaseImageSource();
}

void FlashCoordinator::releaseImageSource() {
    // A run that ends before the workers start must not leave the coordinator's own
    // image handle open until the next run or destruction reclaims it.
    if (m_imageSource) {
        m_imageSource->close();
        m_imageSource.reset();
    }
}

bool FlashCoordinator::validateImagePath(const QString& imagePath) {
    sak::path_validation_config img_cfg;
    img_cfg.must_exist = true;
    img_cfg.must_be_file = true;
    img_cfg.check_read_permission = true;
    // Build the filesystem path from the WIDE string. QString::toStdString() is UTF-8,
    // but std::filesystem::path reads a narrow string in the active code page on
    // Windows, so a non-ASCII path would be validated as a DIFFERENT path than the one
    // every later Qt call opens.
    auto path_result = sak::input_validator::validatePath(
        std::filesystem::path(imagePath.toStdWString()), img_cfg);
    if (!path_result) {
        sak::logError("Image path validation failed: {}", path_result.error_message);
        failRun("Invalid image path",
                QString::fromStdString("Image path validation failed: " +
                                       path_result.error_message));
        return false;
    }
    // Reject a zero-length image (fail closed). It would write nothing yet pass:
    // imageFitsDevice(0,cap) is true, the write loop is skipped, and a full verify
    // hashes 0 bytes so source==target==SHA512(empty) -> a false "success". The
    // path validator only checks existence/type, not that there is data to flash.
    if (QFileInfo(imagePath).size() == 0) {
        sak::logError("Refusing to flash a zero-length image");
        failRun("Empty image", "Image file is empty (0 bytes); nothing to flash");
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
        failRun("Unsupported compressed image",
                "Unsupported compressed image; extract the disk image before flashing");
        return false;
    } else {
        m_imageSource = std::make_unique<FileImageSource>(imagePath);
    }

    // Capture the source's own reason for a failed open instead of replacing it with
    // generic text. The connection is direct (same thread) and is dropped again right
    // away, so it cannot outlive this scope or catch a later read error.
    QString openError;
    auto captureError = [&openError](const QString& error) {
        openError = error;
    };
    const QMetaObject::Connection errorConnection =
        connect(m_imageSource.get(), &ImageSource::readError, this, captureError);
    const bool opened = m_imageSource->open();
    disconnect(errorConnection);
    if (!opened) {
        sak::logError(QString("Failed to open image source: %1").arg(openError).toStdString());
        failRun("Failed to open image",
                openError.isEmpty()
                    ? QString("Failed to open image file %1").arg(imagePath)
                    : QString("Failed to open image file %1: %2").arg(imagePath, openError));
        return false;
    }
    return true;
}

bool FlashCoordinator::unmountAndFlash(const QString& imagePath, const QStringList& targetDrives) {
    // Sole caller startFlash() rejects an empty drive list up front and has already run
    // validateImagePath() on the image, which no empty path survives.
    Q_ASSERT(!imagePath.isEmpty());
    Q_ASSERT(!targetDrives.isEmpty());

    // A cancel that arrived during the synchronous validation phase must stop the run
    // HERE. Without this check the next state assignment simply overwrites Cancelled
    // and the run goes on to take every target offline for a write that
    // startPendingWorkers() will then refuse to start -- disks offline, nothing
    // written, no terminal outcome ever emitted.
    if (abortIfCancelled("Cancelled before unmounting; no drive was taken offline")) {
        return false;
    }

    setState(sak::FlashState::Unmounting);
    Q_EMIT stateChanged(sak::FlashState::Unmounting, "Unmounting volumes...");

    if (!unmountVolumes(targetDrives)) {
        // unmountVolumes() already emitted flashError() with details
        failRun("Failed to unmount target volumes", QString());
        return false;
    }

    if (abortIfCancelled("Cancelled during unmount")) {
        return false;
    }

    setState(sak::FlashState::Flashing);
    Q_EMIT stateChanged(sak::FlashState::Flashing,
                        QString("Writing to %1 drives...").arg(targetDrives.size()));

    // Build every worker up front, but start only as many as the concurrent-write
    // ceiling allows; the rest are queued and started as earlier drives finish.
    // Construction opens nothing -- FlashWorker opens its image source and its
    // device handle on its own thread inside execute() -- so a queued worker costs
    // no handle while it waits.
    // Point the queue at the first worker THIS run creates rather than at index 0,
    // so a leftover finished worker from a previous run can never be restarted.
    m_nextWorkerIndex = m_workers.size();
    if (!buildWorkerQueue(imagePath, targetDrives)) {
        return false;
    }

    startPendingWorkers();

    return true;
}

// Constructs one queued worker per target. Opens nothing: FlashWorker opens its image source
// and device handle on its own thread inside execute(), so a queued worker holds no handle
// while it waits.
bool FlashCoordinator::buildWorkerQueue(const QString& imagePath, const QStringList& targetDrives) {
    try {
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
    } catch (const std::exception& e) {
        // Every target is already OFFLINE at this point. Letting an allocation failure escape
        // startFlash would leave them that way with no writer and no terminal outcome, so
        // retire the partial queue, put the disks back and fail closed.
        const QString reason = QString::fromUtf8(e.what());
        sak::logError(QString("Failed to build the flash workers: %1").arg(reason).toStdString());
        cleanupWorkers();
        rollbackAndReport();
        failRun("Failed to start the flash", QString("Could not start the flash: %1").arg(reason));
        return false;
    }
    return true;
}

bool FlashCoordinator::abortIfCancelled(const QString& reason) {
    if (!m_isCancelled.load()) {
        return false;
    }
    sak::logInfo(QString("Flash cancelled: %1").arg(reason).toStdString());
    // Bring back anything this run already took offline -- a cancelled run must not
    // leave a disk carrying the persistent OFFLINE attribute.
    const QStringList stranded = reonlineOfflinedDrives();
    setState(sak::FlashState::Cancelled);
    Q_EMIT stateChanged(sak::FlashState::Cancelled, reason);
    if (!stranded.isEmpty()) {
        Q_EMIT flashError(QString("Cancelled, but these drives are still offline and need a "
                                  "manual `diskpart online disk`: %1")
                              .arg(stranded.join(QStringLiteral(", "))));
    }
    releaseImageSource();
    return true;
}

void FlashCoordinator::startPendingWorkers() {
    std::vector<FlashWorker*> toStart;
    bool leftVerifying = false;
    {
        const QMutexLocker locker(&m_mutex);
        // A cancelled or already-finished run must not start another drive. Without
        // this, cancelling a queued multi-drive run would tear down the running
        // worker and then start the next queued one on the back of its failure
        // report -- the run would keep writing drives after the user stopped it.
        // Verifying counts as live: an earlier drive reading itself back does not mean
        // the queue behind it is abandoned.
        if (m_isCancelled.load() ||
            (m_state != sak::FlashState::Flashing && m_state != sak::FlashState::Verifying)) {
            return;
        }
        // Saturate rather than subtract blind: m_workers is cleared at teardown while
        // m_nextWorkerIndex keeps its value, and an unsigned underflow here would turn
        // "nothing queued" into a huge pending count.
        const size_t queued =
            m_nextWorkerIndex < m_workers.size() ? m_workers.size() - m_nextWorkerIndex : 0;
        // Clamp instead of narrowing blind: the cast is only safe because the target
        // ceiling bounds m_workers, and saying so beats relying on it.
        const int pending = queued > static_cast<size_t>(std::numeric_limits<int>::max())
                                ? std::numeric_limits<int>::max()
                                : static_cast<int>(queued);
        const int startable =
            startableWorkerCount(m_maxConcurrentWrites, m_progress.activeDrives, pending);
        for (int i = 0; i < startable; ++i) {
            toStart.push_back(m_workers[m_nextWorkerIndex].get());
            ++m_nextWorkerIndex;
        }
        // Count them as active before releasing the lock so a completion arriving
        // between here and start() cannot see a stale slot as free.
        m_progress.activeDrives += startable;
        if (startable > 0 && m_state == sak::FlashState::Verifying) {
            m_state = sak::FlashState::Flashing;
            m_progress.state = m_state;
            leftVerifying = true;
        }
    }

    if (leftVerifying) {
        Q_EMIT stateChanged(sak::FlashState::Flashing, "Writing to the next drive...");
    }

    startWorkers(toStart);
}

void FlashCoordinator::startWorkers(const std::vector<FlashWorker*>& workers) {
    std::vector<FlashWorker*> failedToStart;
    for (FlashWorker* worker : workers) {
        worker->start();
        // QThread::start() returns void and only warns when the thread cannot be
        // created. It marks the thread running synchronously before run() begins, so a
        // worker that is neither running nor finished never started -- and the run
        // would otherwise wait forever for a terminal signal that cannot come.
        if (!worker->isRunning() && !worker->isFinished()) {
            sak::logError(QString("Could not start the writer thread for %1")
                              .arg(worker->targetDevice())
                              .toStdString());
            failedToStart.push_back(worker);
        }
    }

    // Report AFTER the start loop: onWorkerFailedFor() can finalize the run and tear
    // the workers down, which would invalidate the pointers still being iterated above.
    for (const FlashWorker* worker : failedToStart) {
        onWorkerFailedFor(worker, QStringLiteral("Could not start the writer thread"));
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

    // A cancel that lands before any worker started leaves nothing that will ever emit
    // a terminal signal, so the run would sit in Cancelled forever with every target
    // still carrying the persistent OFFLINE attribute. Finish it here when every drive
    // is already accounted for; otherwise the running workers report and the usual
    // finalize path in onWorkerFailedFor() takes over.
    bool terminal = false;
    sak::FlashResult terminalResult;
    QString terminalMessage;
    {
        const QMutexLocker locker(&m_mutex);
        m_state = sak::FlashState::Cancelled;
        m_progress.state = m_state;
        if (m_progress.activeDrives <= 0 &&
            m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
            m_result.success = false;
            finalizeResultMetrics();
            terminal = true;
            terminalResult = m_result;
            terminalMessage = QString("Cancelled: %1 successful, %2 failed")
                                  .arg(m_result.successfulDrives.size())
                                  .arg(m_result.failedDrives.size());
        }
    }

    Q_EMIT stateChanged(sak::FlashState::Cancelled, "Cancelled by user");
    if (terminal) {
        emitTerminalOutcome(sak::FlashState::Cancelled, terminalResult, terminalMessage);
    }
}

void FlashCoordinator::markQueuedWorkersCancelled() {
    // Workers still queued behind the concurrent-write ceiling were never started,
    // so they will never emit a terminal signal of their own. Count them as failed
    // now: without this the finalize predicate (completed + failed == targets) can
    // never be satisfied after a cancel, and the run would wait forever for drives
    // that are never going to be written.
    const QMutexLocker locker(&m_mutex);
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
    const QMutexLocker locker(&m_mutex);
    // Reports "busy" for the WHOLE active run (including the synchronous Validating/
    // Unmounting phases), so the re-entry guard cannot be slipped during them.
    return isActiveFlashState(m_state);
}

bool FlashCoordinator::beginFlashClaim() {
    const QMutexLocker locker(&m_mutex);
    if (isActiveFlashState(m_state)) {
        return false;  // A run is already active -- refuse re-entry atomically.
    }
    // Cancelled/Failed is NOT the same as "no writers left". requestStop() is
    // cooperative and a worker wedged in a long raw WriteFile keeps writing sectors
    // well after cancel() published Cancelled. Claiming a new run now would reset the
    // shared counters, target list and dedup set out from under those live writers and
    // let their late terminal signals be counted against the new run. Fail closed until
    // the previous run's threads are actually gone.
    if (std::ranges::any_of(m_workers, [](const std::unique_ptr<FlashWorker>& worker) {
            return worker && worker->isRunning();
        })) {
        return false;
    }
    // Claim the run under the lock so a concurrent startFlash cannot also pass the
    // gate before the first one has moved the state out of Idle/Completed/Failed.
    m_state = sak::FlashState::Validating;
    m_progress.state = m_state;
    return true;
}

sak::FlashState FlashCoordinator::state() const {
    const QMutexLocker locker(&m_mutex);
    return m_state;
}

sak::FlashProgress FlashCoordinator::progress() const {
    const QMutexLocker locker(&m_mutex);
    return m_progress;
}

void FlashCoordinator::setVerificationEnabled(bool enabled) {
    m_verificationEnabled = enabled;
}

bool FlashCoordinator::isVerificationEnabled() const {
    return m_verificationEnabled;
}

void FlashCoordinator::setBufferSize(qint64 sizeBytes) {
    // Same contract as setMaxConcurrentWrites: refuse a value that cannot work and say
    // so, rather than passing it down to a worker that quietly substitutes its own.
    // Zero/negative starts no I/O at all, and an oversized value is an allocation
    // request the caller (config file, settings dialog) does not get to make unbounded.
    const qint64 maxBytes = kMaxFlashBufferSizeMb * sak::kBytesPerMB;
    if (sizeBytes <= 0 || sizeBytes > maxBytes) {
        sak::logError("Refusing a flash buffer size of {} bytes (allowed 1..{}); keeping {}",
                      sizeBytes,
                      maxBytes,
                      m_bufferSize);
        return;
    }
    m_bufferSize = sizeBytes;
}

qint64 FlashCoordinator::bufferSize() const {
    return m_bufferSize;
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
    // The coordinator otherwise advertised Flashing for the whole read-back stage and
    // never entered Verifying at all, so its reported state described the wrong
    // operation for the entire second half of a verified run.
    connect(worker, &FlashWorker::verificationProgress, this, [this](double, qint64) {
        noteVerificationStarted();
    });
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

void FlashCoordinator::noteVerificationStarted() {
    // Only the Flashing -> Verifying edge publishes, so the periodic verification
    // progress signal does not turn into a stateChanged storm.
    {
        const QMutexLocker locker(&m_mutex);
        if (m_state != sak::FlashState::Flashing) {
            return;
        }
        m_state = sak::FlashState::Verifying;
        m_progress.state = m_state;
    }
    Q_EMIT stateChanged(sak::FlashState::Verifying, "Verifying written data...");
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
        // Keep EVERY reported reason, not just the first: the later entries are the
        // ones that say where and how the verification diverged.
        const QString errorMsg = result.errors.isEmpty() ? QStringLiteral("Verification failed")
                                                         : result.errors.join(QStringLiteral("; "));
        m_result.errorMessages.append(QString("%1: %2").arg(devicePath).arg(errorMsg));
        return errorMsg;
    }

    // The checksum of the image this drive was written FROM. The worker's own source
    // hash is the honest provenance; on a passing full verify the device read-back
    // hash equals it, and it is all there is in Sample/Skip mode.
    const QString reportedSource = result.sourceChecksum.isEmpty() ? result.targetChecksum
                                                                   : result.sourceChecksum;

    // Every drive in a run must have been written from the SAME image. Each worker
    // opens the image file itself, so an image replaced mid-run gives different drives
    // different bytes and each one still verifies against the copy it read. Pin the
    // first reported source hash and refuse any drive that reports a different one.
    if (!reportedSource.isEmpty() && !m_result.sourceChecksum.isEmpty() &&
        reportedSource != m_result.sourceChecksum) {
        const QString errorMsg = QStringLiteral(
            "Source image changed during the run: this drive was written from "
            "different bytes than the first drive");
        sak::logError(QString("%1: %2").arg(devicePath, errorMsg).toStdString());
        m_progress.failedDrives++;
        m_result.failedDrives.append(devicePath);
        m_result.errorMessages.append(QString("%1: %2").arg(devicePath).arg(errorMsg));
        return errorMsg;
    }

    m_progress.completedDrives++;
    m_result.successfulDrives.append(devicePath);
    if (m_result.sourceChecksum.isEmpty() && !reportedSource.isEmpty()) {
        m_result.sourceChecksum = reportedSource;
    }
    return QString();
}

bool FlashCoordinator::finalizeRunIfLastDrive(sak::FlashState* terminalState,
                                              sak::FlashResult* terminalResult,
                                              QString* terminalMessage) {
    if (m_progress.completedDrives + m_progress.failedDrives < m_targetDrives.size()) {
        return false;
    }
    // The terminal state must reflect the RUN outcome, not which handler fired last: if any
    // drive failed (e.g. the last worker to report failed its verification), the run FAILED
    // even though the success handler is what got us here.
    const bool anyFailed = m_progress.failedDrives > 0;
    // A cancelled run ends Cancelled, not Failed: the outcome has to be able to say the user
    // stopped it rather than blaming the flash.
    const bool cancelled = m_isCancelled.load();
    m_state = cancelled ? sak::FlashState::Cancelled
                        : (anyFailed ? sak::FlashState::Failed : sak::FlashState::Completed);
    m_progress.state = m_state;
    m_result.success = !anyFailed && !cancelled;
    finalizeResultMetrics();
    *terminalState = m_state;
    *terminalResult = m_result;
    *terminalMessage =
        QString("%1: %2 successful, %3 failed")
            .arg(cancelled ? QStringLiteral("Cancelled")
                           : (anyFailed ? QStringLiteral("Failed") : QStringLiteral("Completed")))
            .arg(m_result.successfulDrives.size())
            .arg(m_result.failedDrives.size());
    return true;
}

void FlashCoordinator::onWorkerCompleted(const sak::ValidationResult& result) {
    // m_targetDrives is assigned only in startFlash(), after its empty-list rejection, and is
    // never cleared; no worker exists to signal this slot before that assignment.
    Q_ASSERT(!m_targetDrives.isEmpty());
    const FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (worker == nullptr) {
        // Not expected: this slot is private and only connectWorkerSignals() wires it.
        // Do not swallow it silently -- an unattributable completion leaves the run
        // waiting for a drive that has already finished, with its disk still offline.
        sak::logError(
            "Flash completion arrived with no FlashWorker sender; it cannot be "
            "attributed to a drive");
        return;
    }

    const QString devicePath = worker->targetDevice();
    sak::logInfo(QString("Drive completed: %1").arg(devicePath).toStdString());

    // Mutate coordinator state under the lock, but capture everything the signals
    // need into locals and EMIT AFTER releasing it. Emitting under m_mutex deadlocks
    // a same-thread (direct-connected) slot that calls a locked getter -- state(),
    // progress(), isFlashing() (B10-07). cleanupWorkers() (which can wait() up to 15s)
    // likewise runs unlocked so it never stalls those getters.
    QString errorMsg;
    const bool passed = result.passed;
    bool terminal = false;
    sak::FlashState terminalState{};
    sak::FlashResult terminalResult;
    QString terminalMessage;
    {
        const QMutexLocker locker(&m_mutex);
        // Mark this worker as having reported a terminal outcome so a later WorkerBase
        // failed()/cancelled() for the same drive is not double-counted as a failure.
        m_reportedWorkers.insert(worker);
        m_progress.activeDrives--;
        errorMsg = recordDriveCompletion(devicePath, result);

        terminal = finalizeRunIfLastDrive(&terminalState, &terminalResult, &terminalMessage);
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
    if (worker == nullptr) {
        // See onWorkerCompleted: an unattributable failure must not vanish quietly, or
        // the run waits forever for a drive that has already given up.
        sak::logError(QString("Flash failure arrived with no FlashWorker sender: %1")
                          .arg(reportedError)
                          .toStdString());
        return;
    }
    onWorkerFailedFor(worker, reportedError);
}

void FlashCoordinator::onWorkerFailedFor(const FlashWorker* worker, const QString& error) {
    // Reached only from a worker signal, and workers are created after startFlash() assigns the
    // (non-empty) target list.
    Q_ASSERT(!m_targetDrives.isEmpty());
    if (worker == nullptr) {
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
        const QMutexLocker locker(&m_mutex);
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
            // A cancelled run ends Cancelled: overwriting it with Failed would make a
            // user-stopped run indistinguishable from a flash that went wrong.
            const bool cancelled = m_isCancelled.load();
            m_state = cancelled ? sak::FlashState::Cancelled : sak::FlashState::Failed;
            m_progress.state = m_state;
            m_result.success = false;
            finalizeResultMetrics();
            terminal = true;
            terminalState = m_state;
            terminalResult = m_result;
            terminalMessage = QString("%1: %2 successful, %3 failed")
                                  .arg(cancelled ? QStringLiteral("Cancelled")
                                                 : QStringLiteral("Failed"))
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

    // Take THIS run's workers and image handle out of the coordinator BEFORE emitting
    // anything. A direct-connected stateChanged/flashCompleted handler is allowed to
    // start the next run, and a teardown running afterwards against the member would
    // stop and destroy the NEW run's workers and close its image instead of ours.
    std::vector<std::unique_ptr<FlashWorker>> finished;
    {
        const QMutexLocker locker(&m_mutex);
        finished.swap(m_workers);
        m_nextWorkerIndex = 0;
    }
    releaseImageSource();

    sak::FlashResult finalResult = result;
    finalResult.reonlineFailedDrives = reonlineOfflinedDrives();
    if (!finalResult.reonlineFailedDrives.isEmpty()) {
        // A log line would tell the user the run finished while leaving disks carrying
        // the persistent OFFLINE attribute. Put it in the reported result instead.
        finalResult.errorMessages.append(
            QString("Still offline after the run (run `diskpart online disk`): %1")
                .arg(finalResult.reonlineFailedDrives.join(QStringLiteral(", "))));
    }

    // Eject AFTER re-onlining, never instead of it. The OFFLINE attribute is
    // persistent, so ejecting while it is still set would leave a drive that comes
    // back offline the next time it is plugged in -- the eject must remove a normal,
    // online disk.
    // ejectCompletedDrives opens devices, so it runs here with m_mutex released.
    if (m_ejectOnCompletion) {
        ejectCompletedDrives(finalResult);
    }

    Q_EMIT stateChanged(state, statusMessage);
    Q_EMIT flashCompleted(finalResult);
    retireWorkers(finished);
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

QStringList FlashCoordinator::reonlineDrives(const QStringList& drivePaths) {
    QStringList stillOffline;
    if (drivePaths.isEmpty()) {
        return stillOffline;
    }
    DriveUnmounter unmounter;
    for (const QString& devicePath : drivePaths) {
        const int driveNumber = parsePhysicalDriveNumber(devicePath);
        if (driveNumber < 0 || driveNumber > kMaxPhysicalDriveNumber) {
            sak::logError(QString("Cannot re-online %1: unparseable drive number")
                              .arg(devicePath)
                              .toStdString());
            stillOffline.append(devicePath);
            continue;
        }
        if (!unmounter.allowAutoMount(driveNumber)) {
            sak::logError(QString("Failed to bring drive %1 back online: %2")
                              .arg(devicePath, unmounter.lastError())
                              .toStdString());
            stillOffline.append(devicePath);
        }
    }
    return stillOffline;
}

QStringList FlashCoordinator::reonlineOfflinedDrives() {
    const QStringList offlined = m_offlinedDrives;
    m_offlinedDrives.clear();
    const QStringList stillOffline = reonlineDrives(offlined);
    // Keep the ones that refused so a later teardown tries again instead of losing
    // track of a disk left with a persistent OFFLINE attribute.
    m_offlinedDrives = stillOffline;
    return stillOffline;
}

QStringList FlashCoordinator::rollbackOfflinedDrives(const QStringList& offlinedPaths) {
    // Clear the persistent OFFLINE attribute on drives we already offlined this run,
    // so a mid-list unmount failure does not strand the earlier targets.
    if (offlinedPaths.isEmpty()) {
        return QStringList();
    }
    sak::logInfo(QString("Rolling back %1 drive(s) taken offline before the failure")
                     .arg(offlinedPaths.size())
                     .toStdString());
    return reonlineDrives(offlinedPaths);
}

void FlashCoordinator::rollbackAndReport() {
    const QStringList offlined = m_offlinedDrives;
    m_offlinedDrives = rollbackOfflinedDrives(offlined);
    if (m_offlinedDrives.isEmpty()) {
        return;
    }
    sak::logError(QString("Rollback left these drives offline: %1")
                      .arg(m_offlinedDrives.join(QStringLiteral(", ")))
                      .toStdString());
    Q_EMIT flashError(QString("These drives are still offline and need a manual "
                              "`diskpart online disk`: %1")
                          .arg(m_offlinedDrives.join(QStringLiteral(", "))));
}

int FlashCoordinator::parsePhysicalDriveNumber(const QString& devicePath) {
    const QString prefix = QStringLiteral("PhysicalDrive");
    const int idx = static_cast<int>(devicePath.lastIndexOf(prefix));
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

QString FlashCoordinator::firstDuplicatePhysicalDrive(const QStringList& targetDrives) {
    QSet<int> seen;
    for (const QString& devicePath : targetDrives) {
        const int driveNumber = parsePhysicalDriveNumber(devicePath);
        if (driveNumber < 0) {
            // No parseable disk identity. validateSingleTarget() refuses such a path
            // outright (it cannot prove the path opens the disk it names), so it is not
            // folded into the identity set here.
            continue;
        }
        if (seen.contains(driveNumber)) {
            return devicePath;
        }
        seen.insert(driveNumber);
    }
    return QString();
}

bool FlashCoordinator::validateTargets(const QStringList& targetDrives) {
    // Sole caller startFlash() returns false on an empty drive list before reaching here.
    Q_ASSERT(!targetDrives.isEmpty());

    // Bound the run. Every target costs a worker, a thread and a device handle, and
    // they are all created AFTER the disks are offline -- an unbounded list turns a
    // failed allocation there into offline disks with nothing writing to them.
    if (targetDrives.size() > kMaxTargetDrives) {
        sak::logError(QString("Refusing %1 targets in one run (ceiling %2)")
                          .arg(targetDrives.size())
                          .arg(kMaxTargetDrives)
                          .toStdString());
        Q_EMIT flashError(QString("Refusing %1 target drives: at most %2 may be flashed in one run")
                              .arg(targetDrives.size())
                              .arg(kMaxTargetDrives));
        return false;
    }

    // Reject duplicate target paths: two workers writing the SAME physical disk
    // concurrently interleave their raw writes and corrupt each other.
    const QString duplicate = firstDuplicateTarget(targetDrives);
    if (!duplicate.isEmpty()) {
        sak::logError(QString("Duplicate target device rejected: %1").arg(duplicate).toStdString());
        Q_EMIT flashError(QString("Duplicate target device: %1").arg(duplicate));
        return false;
    }

    // String equality is not device identity. "\\.\PhysicalDrive1" and
    // "\\?\PhysicalDrive1" are different strings naming the SAME disk and sail past the
    // check above, putting two raw writers on one target. validateSingleTarget() below
    // proves each path really opens the physical disk its name claims, so the parsed
    // number IS the disk identity -- reject a repeat of it.
    const QString duplicateDisk = firstDuplicatePhysicalDrive(targetDrives);
    if (!duplicateDisk.isEmpty()) {
        sak::logError(QString("Duplicate target physical disk rejected: %1")
                          .arg(duplicateDisk)
                          .toStdString());
        Q_EMIT flashError(QString("Duplicate target device: %1 names a physical disk that is "
                                  "already in the target list")
                              .arg(duplicateDisk));
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
        DWORD const error = GetLastError();
        sak::logError(
            QString("Failed to open device %1: Error %2").arg(devicePath).arg(error).toStdString());
        Q_EMIT flashError(QString("Cannot access device %1. Error: %2").arg(devicePath).arg(error));
        return false;
    }

    // Get device geometry to verify it's a valid disk
    DISK_GEOMETRY geometry;
    DWORD bytesReturned = 0;
    if (DeviceIoControl(hDevice,
                        IOCTL_DISK_GET_DRIVE_GEOMETRY,
                        nullptr,
                        0,
                        &geometry,
                        sizeof(geometry),
                        &bytesReturned,
                        nullptr) == 0) {
        DWORD const error = GetLastError();
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
    // Track drives we successfully took offline (in m_offlinedDrives) so we can roll
    // them back online if a LATER target fails to unmount -- otherwise earlier targets
    // are stranded offline -- and so a teardown that never reaches emitTerminalOutcome
    // can still clear the attribute.
    m_offlinedDrives.clear();

    for (const QString& devicePath : targetDrives) {
        sak::logInfo(QString("Unmounting volumes on %1").arg(devicePath).toStdString());

        // Extract drive number from path (e.g., "\\.\PhysicalDrive1" -> 1)
        const int driveNumber = parsePhysicalDriveNumber(devicePath);

        if (driveNumber < 0 || driveNumber > kMaxPhysicalDriveNumber) {
            sak::logError(QString("Invalid device path format or drive number out of range: %1")
                              .arg(devicePath)
                              .toStdString());
            Q_EMIT flashError(QString("Invalid device path format: %1").arg(devicePath));
            rollbackAndReport();
            return false;
        }

        if (!unmounter.unmountDrive(driveNumber)) {
            // Carry the unmounter's own reason: "failed to unmount" without it hides
            // which handle or which API call actually refused.
            sak::logError(QString("Failed to unmount volumes on %1: %2")
                              .arg(devicePath, unmounter.lastError())
                              .toStdString());
            Q_EMIT flashError(
                QString("Failed to unmount volumes on %1: %2. "
                        "Please close any applications using this drive and try again.")
                    .arg(devicePath, unmounter.lastError()));
            rollbackAndReport();
            return false;
        }

        m_offlinedDrives.append(devicePath);
        sak::logInfo(QString("Successfully unmounted volumes on %1").arg(devicePath).toStdString());
    }

    return true;
}

void FlashCoordinator::updateProgress() {
    // Compute under the lock, emit the snapshot after releasing it (B10-07): a
    // direct-connected progressUpdated slot that reads progress()/state() would
    // otherwise re-lock the non-recursive m_mutex and deadlock.
    sak::FlashProgress snapshot;
    {
        const QMutexLocker locker(&m_mutex);
        m_progress.bytesWritten = 0;
        double totalSpeed = 0.0;

        for (const auto& worker : m_workers) {
            m_progress.bytesWritten += worker->bytesWritten();
            totalSpeed += worker->speedMBps();
        }

        m_progress.speedMBps = totalSpeed;
        m_progress.percentage = m_progress.getOverallProgress();
        m_progress.state = m_state;
        m_progress.currentOperation =
            m_state == sak::FlashState::Verifying
                ? QString("Verifying %1 drives...").arg(m_progress.activeDrives)
                : QString("Writing to %1 drives...").arg(m_progress.activeDrives);
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
    m_result.elapsedSeconds = static_cast<double>(m_flashTimer.elapsed()) / kMillisecondsPerSecond;
    // Say whether anything actually read the drives back. With verification off a
    // "successful" drive means the write succeeded and nothing checked it, and the
    // result must report that rather than let a skipped verify read as a passed one.
    m_result.verified = m_verificationEnabled;
}

void FlashCoordinator::cleanupWorkers() {
    // m_imageSource may legitimately be null (called from the dtor when no flash ran, and after a
    // reset); releaseImageSource() handles it. No entry assert.
    std::vector<std::unique_ptr<FlashWorker>> workers;
    {
        const QMutexLocker locker(&m_mutex);
        workers.swap(m_workers);
        // The queue index is an index into m_workers; leaving it behind would make it
        // point past the end of an empty vector.
        m_nextWorkerIndex = 0;
    }
    retireWorkers(workers);
    releaseImageSource();
}

void FlashCoordinator::retireWorkers(std::vector<std::unique_ptr<FlashWorker>>& workers) {
    // Wait for all workers to finish with cooperative stop
    for (auto& worker : workers) {
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
        // Sever every signal back to the coordinator BEFORE letting go. A detached
        // worker outlives this run, and its eventual terminal signal would be counted
        // against the NEXT run -- whose startFlash cleared the dedup set -- corrupting
        // that run's drive counters and possibly finalizing it early.
        disconnect(detached, nullptr, this, nullptr);
        connect(detached, &QThread::finished, detached, &QObject::deleteLater);
        detached->setParent(nullptr);
        static_cast<void>(worker.release());
    }

    workers.clear();
}
