// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file flash_live_certifier.cpp
/// @brief LIVE, DESTRUCTIVE raw-disk certifier for the Batch 4 FlashWorker
///        memory-safety fixes (B4-01/04/05/06/07). It writes a real image to a
///        real removable USB disk and reads it back through the actual
///        FlashWorker verify paths -- the only way to exercise the fixed code on
///        hardware.
///
/// SAFETY: this program raw-writes a physical drive and DESTROYS its contents.
/// It refuses to touch anything that is not, at runtime:
///   - a USB bus device, AND
///   - removable / hot-pluggable, AND
///   - NOT the boot disk, NOT the system disk, AND
///   - smaller than kMaxAllowedBytes (a guard against hitting a large internal
///     or external storage disk by mistake).
/// The physical-drive number MUST be passed explicitly with --drive N, and the
/// destructive intent MUST be confirmed with --yes-destroy. Even then every
/// runtime guard above must pass or the program aborts before opening the disk
/// for write.
///
/// Usage (elevated):
///   flash_live_certifier --drive 4 --yes-destroy [--size-mb 64]
///
/// Exit code 0 = all cases certified; non-zero = a guard tripped or a case
/// failed.

#include "sak/flash_worker.h"
#include "sak/image_source.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QString>
#include <QTemporaryFile>

#include <cstdio>

#include <windows.h>

#include <winioctl.h>

namespace {

// Any target at or above this size is refused: a disposable USB test stick is
// small; a half-terabyte match means we probably grabbed the wrong disk.
constexpr qint64 kMaxAllowedBytes = 512LL * 1024 * 1024 * 1024;  // 512 GiB

QString physicalDrivePath(int driveNumber) {
    return QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
}

// Returns the byte length of a physical drive, or -1 on failure.
qint64 physicalDriveLength(int driveNumber) {
    // IOCTL_DISK_GET_LENGTH_INFO requires READ access to the device: opening with
    // dwDesiredAccess = 0 (metadata-only, which IOCTL_STORAGE_QUERY_PROPERTY does
    // tolerate) makes the length IOCTL fail with ERROR_ACCESS_DENIED on real USB
    // drives, so the pre-flight wrongly refused a perfectly good disk. Open for
    // GENERIC_READ; the share flags keep other readers/writers unaffected.
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(physicalDrivePath(driveNumber).utf16()),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr,
                           OPEN_EXISTING,
                           0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    GET_LENGTH_INFORMATION li{};
    DWORD ret = 0;
    const bool ok =
        DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &li, sizeof(li), &ret, nullptr);
    CloseHandle(h);
    return ok ? static_cast<qint64>(li.Length.QuadPart) : -1;
}

// True when the drive reports the USB bus and removable/hot-plug media.
bool isRemovableUsb(int driveNumber) {
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(physicalDrivePath(driveNumber).utf16()),
                           0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr,
                           OPEN_EXISTING,
                           0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    std::vector<unsigned char> buffer(1024, 0);
    DWORD ret = 0;
    const bool ok = DeviceIoControl(h,
                                    IOCTL_STORAGE_QUERY_PROPERTY,
                                    &query,
                                    sizeof(query),
                                    buffer.data(),
                                    static_cast<DWORD>(buffer.size()),
                                    &ret,
                                    nullptr);
    if (!ok) {
        CloseHandle(h);
        return false;
    }
    const auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    const bool usb = desc->BusType == BusTypeUsb;
    const bool removable = desc->RemovableMedia != FALSE;

    // Also require the media to be hot-pluggable per IOCTL_STORAGE_GET_HOTPLUG_INFO
    // -- a USB SSD can report RemovableMedia=FALSE, so treat USB+hotplug as removable.
    STORAGE_HOTPLUG_INFO hotplug{};
    DWORD hret = 0;
    const bool hok = DeviceIoControl(
        h, IOCTL_STORAGE_GET_HOTPLUG_INFO, nullptr, 0, &hotplug, sizeof(hotplug), &hret, nullptr);
    CloseHandle(h);
    const bool hotpluggable = hok && hotplug.MediaRemovable;
    return usb && (removable || hotpluggable);
}

// True when the given physical drive backs the current OS (system) volume.
bool isSystemDisk(int driveNumber) {
    wchar_t winDir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH || winDir[1] != L':') {
        return true;  // fail closed: cannot prove it is safe -> treat as system
    }
    const wchar_t volumePath[] = {L'\\', L'\\', L'.', L'\\', winDir[0], L':', L'\0'};
    HANDLE hVol = CreateFileW(
        volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        return true;  // fail closed
    }
    constexpr DWORD kMaxExtents = 16;
    const DWORD bufSize = sizeof(VOLUME_DISK_EXTENTS) + (kMaxExtents - 1) * sizeof(DISK_EXTENT);
    std::vector<unsigned char> buffer(bufSize, 0);
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buffer.data());
    DWORD ret = 0;
    const BOOL ok = DeviceIoControl(
        hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, extents, bufSize, &ret, nullptr);
    CloseHandle(hVol);
    if (!ok) {
        return true;  // fail closed
    }
    for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < kMaxExtents; ++i) {
        if (static_cast<int>(extents->Extents[i].DiskNumber) == driveNumber) {
            return true;
        }
    }
    return false;
}

bool runningElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return elevated != FALSE;
}

// A plain uncompressed image source over a real file. Uses the same interface
// FlashWorker consumes; deterministic content makes the verify meaningful.
class RawFileImageSource : public ImageSource {
public:
    explicit RawFileImageSource(const QString& path) : m_file(path) {}

    bool open() override {
        if (m_file.isOpen()) {
            return true;
        }
        return m_file.open(QIODevice::ReadOnly);
    }
    void close() override { m_file.close(); }
    bool isOpen() const override { return m_file.isOpen(); }
    qint64 read(char* data, qint64 maxSize) override { return m_file.read(data, maxSize); }
    qint64 size() const override { return m_file.size(); }
    qint64 position() const override { return m_file.pos(); }
    bool seek(qint64 pos) override { return m_file.seek(pos); }
    bool atEnd() const override { return m_file.atEnd(); }
    sak::ImageMetadata metadata() const override {
        sak::ImageMetadata md;
        md.path = m_file.fileName();
        md.size = m_file.size();
        md.format = sak::ImageFormat::IMG;
        return md;
    }
    QString calculateChecksum() override {
        const bool wasOpen = m_file.isOpen();
        if (!wasOpen && !m_file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        const qint64 savedPos = m_file.pos();
        m_file.seek(0);
        QCryptographicHash hash(QCryptographicHash::Sha512);
        if (!hash.addData(&m_file)) {
            return QString();
        }
        m_file.seek(savedPos);
        if (!wasOpen) {
            m_file.close();
        }
        return QString(hash.result().toHex());
    }

private:
    mutable QFile m_file;
};

// Build a deterministic (non-trivial) test image of the requested size.
bool writeTestImage(const QString& path, qint64 bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    constexpr qint64 kChunk = 1LL * 1024 * 1024;
    QByteArray chunk(static_cast<int>(qMin(kChunk, bytes)), Qt::Uninitialized);
    qint64 written = 0;
    quint64 state = 0x9E'37'79'B9'7F'4A'7C'15ULL;  // fixed seed -> reproducible content
    while (written < bytes) {
        const qint64 thisChunk = qMin(static_cast<qint64>(chunk.size()), bytes - written);
        for (qint64 i = 0; i < thisChunk; ++i) {
            state = state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
            chunk[static_cast<int>(i)] = static_cast<char>((state >> 33) & 0xFF);
        }
        if (f.write(chunk.constData(), thisChunk) != thisChunk) {
            return false;
        }
        written += thisChunk;
    }
    return true;
}

struct CaseResult {
    QString name;
    bool passed = false;
    QString detail;
};

// Run one flash+verify case against the (already guarded) target.
CaseResult runCase(const QString& name,
                   const QString& targetDevice,
                   const QString& imagePath,
                   sak::ValidationMode mode) {
    CaseResult cr;
    cr.name = name;

    auto source = std::make_unique<RawFileImageSource>(imagePath);
    const qint64 imageBytes = QFile(imagePath).size();

    FlashWorker worker(std::move(source), targetDevice);
    worker.setVerificationEnabled(true);
    worker.setValidationMode(mode);

    bool gotVerification = false;
    sak::ValidationResult result;
    qint64 bytesWritten = 0;
    QString errorText;

    QObject::connect(&worker,
                     &FlashWorker::verificationCompleted,
                     [&](const sak::ValidationResult& r) {
                         result = r;
                         gotVerification = true;
                     });
    QObject::connect(&worker, &FlashWorker::writeCompleted, [&](qint64 n) { bytesWritten = n; });
    QObject::connect(&worker, &FlashWorker::error, [&](const QString& e) { errorText = e; });

    worker.start();
    while (worker.isRunning()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    // The busy-loop above only exits once the thread has stopped, so this join returns at once;
    // still check it rather than discard the result, so a future refactor cannot silently leave a
    // running QThread to be destroyed under us.
    if (!worker.wait(5000)) {
        cr.detail = "worker thread did not join after completion";
        return cr;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);  // drain queued signals

    if (!errorText.isEmpty()) {
        cr.detail = QString("worker error: %1").arg(errorText);
        return cr;
    }
    if (bytesWritten < imageBytes) {
        cr.detail = QString("short write: %1 < %2 image bytes").arg(bytesWritten).arg(imageBytes);
        return cr;
    }
    if (!gotVerification) {
        cr.detail = "no verificationCompleted signal";
        return cr;
    }
    if (!result.passed) {
        cr.detail = QString("verification failed: %1")
                        .arg(result.errors.isEmpty() ? QString("(no detail)")
                                                     : result.errors.first());
        return cr;
    }
    cr.passed = true;
    cr.detail = QString("wrote %1 bytes, verify OK").arg(bytesWritten);
    return cr;
}

int argInt(const QStringList& args, const QString& flag, int fallback) {
    const int idx = args.indexOf(flag);
    if (idx >= 0 && idx + 1 < args.size()) {
        bool ok = false;
        const int v = args.at(idx + 1).toInt(&ok);
        if (ok) {
            return v;
        }
    }
    return fallback;
}

}  // namespace

// Validates args + runtime safety guards. Returns 0 when it is safe to proceed
// (and prints the confirmed target), or a non-zero exit code on any refusal.
int checkArgsAndGuards(int driveNumber, bool confirmed) {
    if (driveNumber < 0) {
        std::fprintf(stderr, "FATAL: --drive N is required\n");
        return 2;
    }
    if (!confirmed) {
        std::fprintf(stderr, "FATAL: refusing to write without --yes-destroy\n");
        return 2;
    }
    if (!runningElevated()) {
        std::fprintf(stderr, "FATAL: must run elevated (admin) to open a physical drive\n");
        return 2;
    }
    // Runtime safety guards (independent of the number passed).
    if (driveNumber == 0) {
        std::fprintf(stderr, "FATAL: drive 0 is refused outright\n");
        return 3;
    }
    if (isSystemDisk(driveNumber)) {
        std::fprintf(stderr,
                     "FATAL: PhysicalDrive%d backs the system/boot volume -- refused\n",
                     driveNumber);
        return 3;
    }
    if (!isRemovableUsb(driveNumber)) {
        std::fprintf(stderr,
                     "FATAL: PhysicalDrive%d is not a removable USB device -- refused\n",
                     driveNumber);
        return 3;
    }
    const qint64 diskBytes = physicalDriveLength(driveNumber);
    if (diskBytes <= 0) {
        std::fprintf(stderr,
                     "FATAL: could not read PhysicalDrive%d length -- refused\n",
                     driveNumber);
        return 3;
    }
    if (diskBytes >= kMaxAllowedBytes) {
        std::fprintf(stderr,
                     "FATAL: PhysicalDrive%d is %.1f GiB (>= %lld GiB cap) -- refused\n",
                     driveNumber,
                     diskBytes / (1024.0 * 1024.0 * 1024.0),
                     kMaxAllowedBytes / (1024LL * 1024 * 1024));
        return 3;
    }
    std::fprintf(stderr,
                 "Target: PhysicalDrive%d  (%.2f GiB, removable USB, not system) -- proceeding\n",
                 driveNumber,
                 diskBytes / (1024.0 * 1024.0 * 1024.0));
    return 0;
}

// Builds the test images, runs the verify cases, prints results.
// Returns 0 when every case passed, 1 otherwise.
int runAllCases(const QString& target, int sizeMb) {
    const QString tmpDir = QDir::tempPath();
    const QString bigImg = tmpDir + "/sak_flash_cert_big.img";
    const QString smallImg = tmpDir + "/sak_flash_cert_small.img";
    const qint64 bigBytes = static_cast<qint64>(sizeMb) * 1024 * 1024;
    const qint64 smallBytes = 256 * 1024;  // < 1 MiB sample block -> B4-06 whole-image path

    if (!writeTestImage(bigImg, bigBytes) || !writeTestImage(smallImg, smallBytes)) {
        std::fprintf(stderr, "FATAL: failed to build test images\n");
        return 4;
    }

    QList<CaseResult> results;
    results << runCase("full-verify (large)", target, bigImg, sak::ValidationMode::Full);
    results << runCase(
        "sample-verify (large, B4-01/07)", target, bigImg, sak::ValidationMode::Sample);
    results << runCase(
        "sample-verify (small, B4-06 whole-image)", target, smallImg, sak::ValidationMode::Sample);

    QFile::remove(bigImg);
    QFile::remove(smallImg);

    int failed = 0;
    std::fprintf(stderr, "\n== Results ==\n");
    for (const CaseResult& cr : results) {
        std::fprintf(stderr,
                     "  [%s] %s -- %s\n",
                     cr.passed ? "PASS" : "FAIL",
                     cr.name.toStdString().c_str(),
                     cr.detail.toStdString().c_str());
        if (!cr.passed) {
            ++failed;
        }
    }
    std::fprintf(stderr,
                 "\n%d/%d cases passed\n",
                 static_cast<int>(results.size()) - failed,
                 static_cast<int>(results.size()));
    return failed == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    const int driveNumber = argInt(args, "--drive", -1);
    const int sizeMb = argInt(args, "--size-mb", 64);
    const bool confirmed = args.contains("--yes-destroy");

    std::fprintf(stderr, "== SAK flash live certifier (DESTRUCTIVE) ==\n");

    const int guard = checkArgsAndGuards(driveNumber, confirmed);
    if (guard != 0) {
        return guard;
    }
    return runAllCases(physicalDrivePath(driveNumber), sizeMb);
}
