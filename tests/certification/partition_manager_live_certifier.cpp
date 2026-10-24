// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_live_certifier.cpp
/// @brief Developer-only, disposable-VHD proof through the real Partition Manager controller.
/// No disk number is accepted from the command line. The fixture harness creates and seeds two
/// fresh VHDs; Virtual Disk API handles independently bind their exact files to live attachments.
/// Only PartitionManagerController executes CreateImage and RestoreImage. The supervisor contains
/// the worker and its descendants in a Windows Job before the worker's first instruction runs.

#include "sak/elevation_manager.h"
#include "sak/file_hash.h"
#include "sak/partition_file_system_detector.h"
#include "sak/partition_manager_controller.h"
#include "sak/partition_raw_device_io.h"
#include "sak/partition_safety_validator.h"
#include "sak/partition_script_builder.h"
#include "sak/storage_inventory_worker.h"
#include "sak/windows_path_policy.h"

#include <QBuffer>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include <windows.h>

#include <initguid.h>
#include <tlhelp32.h>
#include <virtdisk.h>
#include <winioctl.h>

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
static_assert(false, "The live certifier must never be built with production test hooks");
#endif

namespace {

template <typename Value>
using Result = std::expected<Value, QString>;

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kMinimumVhdBytes = 512ULL * kMiB;
constexpr uint64_t kMaximumVhdBytes = 1024ULL * kMiB;
constexpr uint64_t kWorkspaceSizeMultiplier = 4;
constexpr int kStageTimeoutMs = 180'000;
constexpr int kCancellationGraceMs = 30'000;
constexpr int kWorkerTimeoutMs = 900'000;
constexpr int kProcessStartTimeoutMs = 5000;
constexpr int kProcessExitTimeoutMs = 5000;
constexpr int kJobQuiescencePollMs = 25;
constexpr int kMaximumPathAncestors = 128;
constexpr size_t kPhysicalPathChars = 64;
constexpr DWORD kMaximumVolumeExtents = 32;
constexpr qsizetype kMaximumMarkerBytes = 256;
constexpr qsizetype kMaximumWorkerOutputBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaximumExecutionReportBytes = 512 * 1024;
constexpr qsizetype kMaximumLogCharacters = 128 * 1024;
constexpr int kFailureExit = 2;
constexpr int kTimeoutExit = 3;
constexpr char kWorkerFramePrefix[] = "SAK_PARTITION_CERTIFIER_RESULT:";
constexpr char kToolName[] = "partition-manager-controller-live-certifier";
constexpr char kSourceDigest[] = SAK_PARTITION_CERTIFIER_SOURCE_SHA256;
static_assert(sizeof(kSourceDigest) == 65, "CMake must bind a 64-hex runtime source digest");

struct HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using NativeHandle = std::unique_ptr<void, HandleCloser>;

enum class ProofMode {
    Restore,
    OfflineRestore,
    OfflineReadOnlyRestore,
    OfflinePartitionImage,
    BusySourceRefusal
};

Result<ProofMode> parseProofMode(const QString& value) {
    if (value == QStringLiteral("restore")) {
        return ProofMode::Restore;
    }
    if (value == QStringLiteral("offline-restore")) {
        return ProofMode::OfflineRestore;
    }
    if (value == QStringLiteral("offline-readonly-restore")) {
        return ProofMode::OfflineReadOnlyRestore;
    }
    if (value == QStringLiteral("offline-partition-image")) {
        return ProofMode::OfflinePartitionImage;
    }
    if (value == QStringLiteral("busy-source-refusal")) {
        return ProofMode::BusySourceRefusal;
    }
    return std::unexpected(
        QStringLiteral("Unknown --proof-mode; no implicit proof mode is accepted"));
}

QString proofModeName(ProofMode mode) {
    switch (mode) {
    case ProofMode::Restore:
        return QStringLiteral("restore");
    case ProofMode::OfflineRestore:
        return QStringLiteral("offline-restore");
    case ProofMode::OfflineReadOnlyRestore:
        return QStringLiteral("offline-readonly-restore");
    case ProofMode::OfflinePartitionImage:
        return QStringLiteral("offline-partition-image");
    case ProofMode::BusySourceRefusal:
        return QStringLiteral("busy-source-refusal");
    }
    return {};
}

bool offlineProof(ProofMode mode) {
    return mode == ProofMode::OfflineRestore || mode == ProofMode::OfflineReadOnlyRestore ||
           mode == ProofMode::OfflinePartitionImage;
}

bool restoreProof(ProofMode mode) {
    return mode == ProofMode::Restore || mode == ProofMode::OfflineRestore ||
           mode == ProofMode::OfflineReadOnlyRestore;
}

QString proofScenario(ProofMode mode) {
    switch (mode) {
    case ProofMode::Restore:
        return QStringLiteral("vhd.image-restore");
    case ProofMode::OfflineRestore:
        return QStringLiteral("vhd.image-restore-offline");
    case ProofMode::OfflineReadOnlyRestore:
        return QStringLiteral("vhd.image-restore-offline-readonly");
    case ProofMode::OfflinePartitionImage:
        return QStringLiteral("vhd.partition-image-create-offline");
    case ProofMode::BusySourceRefusal:
        return QStringLiteral("vhd.image-create-busy-refusal");
    }
    return {};
}

struct Config {
    QString source_vhd{};
    QString target_vhd{};
    QString output_dir{};
    QString run_id{};
    QString containment_job{};
    ProofMode proof_mode{ProofMode::Restore};
    bool confirmed{false};
    bool validate_only{false};
    bool worker{false};
};

QString proofReportFile(ProofMode mode) {
    return mode == ProofMode::Restore ? QStringLiteral("controller-restore-report.json")
                                      : QStringLiteral("controller-image-proof-report.json");
}

QString successfulProofStatus(const Config& config) {
    if (config.validate_only) {
        return QStringLiteral("Validated");
    }
    return config.proof_mode == ProofMode::BusySourceRefusal ? QStringLiteral("RefusalVerified")
                                                             : QStringLiteral("Passed");
}

struct Attachment {
    QString file_path{};
    QString physical_path{};
    QString vhd_id{};
    uint32_t disk_number{0};
    uint64_t virtual_size{0};
    ULONG storage_type{VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN};
    NativeHandle handle{};
};

struct FixtureState {
    Attachment source;
    Attachment target;
    sak::PartitionDiskInfo source_disk;
    sak::PartitionDiskInfo target_disk;
    sak::PartitionInfoEx source_partition;
    QString source_marker_hash{};
    QString target_marker{};
    QString image_path{};
    QString image_hash{};
    QString source_range_hash{};
    QString source_lock_path{};
    uint64_t image_offset{0};
    uint64_t image_size{0};
    uint64_t source_attributes{0};
    NativeHandle source_control{};
    NativeHandle source_holder{};
};

auto refused(const QString& error) {
    return std::unexpected(error);
}

QString windowsFailure(const QString& operation, DWORD code = GetLastError()) {
    return QStringLiteral("%1 failed (Windows error %2)").arg(operation).arg(code);
}

bool samePath(const QString& left, const QString& right) {
    return sak::screeningPathForm(left).compare(sak::screeningPathForm(right),
                                                Qt::CaseInsensitive) == 0;
}

Result<void> validateVhdPathText(const Config& config) {
    for (const auto& path : {config.source_vhd, config.target_vhd}) {
        const auto suffix = QFileInfo(path).suffix().toLower();
        if (suffix != QStringLiteral("vhd") && suffix != QStringLiteral("vhdx")) {
            return refused(QStringLiteral("Only .vhd/.vhdx fixture paths are accepted"));
        }
        if (!samePath(QFileInfo(path).absolutePath(), config.output_dir)) {
            return refused(
                QStringLiteral("Both VHD files must be direct children of --output-dir"));
        }
    }
    return {};
}

Result<void> validateConfigText(const Config& config) {
    if (proofModeName(config.proof_mode).isEmpty()) {
        return refused(QStringLiteral("Unknown proof mode"));
    }
    if (!config.validate_only && !config.confirmed) {
        return refused(QStringLiteral("--confirm-disposable-vhds is required for real execution"));
    }
    for (const auto& path : {config.source_vhd, config.target_vhd, config.output_dir}) {
        if (!sak::literalLocalPathTrusted(path)) {
            return refused(QStringLiteral("Every path must be literal, local and drive-qualified"));
        }
    }
    if (samePath(config.source_vhd, config.target_vhd)) {
        return refused(QStringLiteral("Source and target VHD paths must be distinct"));
    }
    const QUuid run_id(config.run_id);
    if (run_id.isNull() || run_id.toString(QUuid::WithoutBraces) != config.run_id) {
        return refused(QStringLiteral("--run-id must be a non-null lowercase UUID without braces"));
    }
    return validateVhdPathText(config);
}

Result<Config> parseConfig(const QStringList& arguments) {
    QCommandLineParser parser;
    parser.addOptions(
        {QCommandLineOption{QStringLiteral("source-vhd"), "Exact source VHD file", "path"},
         QCommandLineOption{QStringLiteral("target-vhd"), "Exact target VHD file", "path"},
         QCommandLineOption{
             QStringLiteral("output-dir"), "Existing guest-local fixture directory", "path"},
         QCommandLineOption{QStringLiteral("run-id"), "Fixture nonce", "uuid"},
         QCommandLineOption{
             QStringLiteral("proof-mode"), "Explicit fixture proof workflow", "mode"},
         QCommandLineOption{QStringLiteral("confirm-disposable-vhds"),
                            "Authorize only the named disposable VHDs"},
         QCommandLineOption{QStringLiteral("validate-only"),
                            "Validate without queueing or detaching anything"},
         QCommandLineOption{QStringLiteral("contained-worker"), "Internal supervised worker"},
         QCommandLineOption{
             QStringLiteral("containment-job"), "Internal containment identity", "name"}});
    if (!parser.parse(arguments)) {
        return refused(parser.errorText());
    }
    if (!parser.positionalArguments().isEmpty()) {
        return refused(QStringLiteral("Positional arguments and disk numbers are not accepted"));
    }
    const auto names = parser.optionNames();
    const auto duplicate = std::ranges::find_if(names, [&](const QString& name) {
        return names.count(name) != 1 || parser.values(name).size() > 1;
    });
    if (duplicate != names.cend()) {
        return refused(QStringLiteral("Duplicate option refused: %1").arg(*duplicate));
    }
    const auto mode = parser.isSet("proof-mode") ? parseProofMode(parser.value("proof-mode"))
                                                 : Result<ProofMode>(ProofMode::Restore);
    if (!mode) {
        return refused(mode.error());
    }
    Config config{.source_vhd = parser.value("source-vhd"),
                  .target_vhd = parser.value("target-vhd"),
                  .output_dir = parser.value("output-dir"),
                  .run_id = parser.value("run-id"),
                  .containment_job = parser.value("containment-job"),
                  .proof_mode = *mode,
                  .confirmed = parser.isSet("confirm-disposable-vhds"),
                  .validate_only = parser.isSet("validate-only"),
                  .worker = parser.isSet("contained-worker")};
    if (config.worker != !config.containment_job.isEmpty()) {
        return refused(QStringLiteral("Internal worker requires its exact containment job"));
    }
    if (const auto valid = validateConfigText(config); !valid) {
        return refused(valid.error());
    }
    return config;
}

Result<void> rejectReparseChain(const QString& path) {
    QString current = QDir::toNativeSeparators(path);
    for (int depth = 0; depth < kMaximumPathAncestors; ++depth) {
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return refused(windowsFailure(QStringLiteral("Path attributes: %1").arg(current)));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return refused(QStringLiteral("Reparse-point path refused: %1").arg(current));
        }
        const QString parent = QFileInfo(current).dir().absolutePath();
        if (samePath(parent, current)) {
            return {};
        }
        current = QDir::toNativeSeparators(parent);
    }
    return refused(QStringLiteral("Path ancestor bound exceeded"));
}

Result<void> validateFixturePaths(const Config& config) {
    if (!QFileInfo(config.output_dir).isDir()) {
        return refused(QStringLiteral("Fixture output directory must already exist"));
    }
    for (const auto& path : {config.output_dir, config.source_vhd, config.target_vhd}) {
        if (const auto valid = rejectReparseChain(path); !valid) {
            return valid;
        }
        if (!samePath(QFileInfo(path).canonicalFilePath(), path)) {
            return refused(
                QStringLiteral("Path is not its canonical literal identity: %1").arg(path));
        }
    }
    for (const auto& path : {config.source_vhd, config.target_vhd}) {
        if (!QFileInfo(path).isFile()) {
            return refused(QStringLiteral("VHD input must be an existing regular file"));
        }
        NativeHandle file(CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info)) {
            return refused(windowsFailure(QStringLiteral("VHD file identity")));
        }
        if (info.nNumberOfLinks != 1) {
            return refused(QStringLiteral("Hard-linked VHD inputs are refused"));
        }
    }
    return {};
}

Result<GET_VIRTUAL_DISK_INFO> virtualDiskInfo(HANDLE handle,
                                              GET_VIRTUAL_DISK_INFO_VERSION version) {
    GET_VIRTUAL_DISK_INFO info{};
    info.Version = version;
    ULONG bytes = sizeof(info);
    const DWORD result = GetVirtualDiskInformation(handle, &bytes, &info, nullptr);
    if (result != ERROR_SUCCESS) {
        return refused(windowsFailure(QStringLiteral("GetVirtualDiskInformation"), result));
    }
    return info;
}

Result<QString> attachedPhysicalPath(HANDLE handle) {
    std::array<wchar_t, kPhysicalPathChars> path{};
    ULONG bytes = static_cast<ULONG>(sizeof(path));
    const DWORD result = GetVirtualDiskPhysicalPath(handle, &bytes, path.data());
    if (result != ERROR_SUCCESS) {
        return refused(windowsFailure(QStringLiteral("GetVirtualDiskPhysicalPath"), result));
    }
    const auto end = std::find(path.cbegin(), path.cend(), L'\0');
    if (end == path.cend()) {
        return refused(QStringLiteral("Virtual disk returned an unterminated physical path"));
    }
    const QString physical =
        QString::fromWCharArray(path.data(), static_cast<int>(std::distance(path.cbegin(), end)));
    if (!sak::rawDevicePhysicalDriveNumber(physical)) {
        return refused(QStringLiteral("Virtual disk did not resolve to one PhysicalDrive"));
    }
    return sak::canonicalRawDevicePath(physical);
}

bool supportedVirtualStorageType(ULONG format) {
    return format == VIRTUAL_STORAGE_TYPE_DEVICE_VHD || format == VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
}

Result<Attachment> describeAttachment(NativeHandle handle, const QString& path) {
    const HANDLE raw = handle.get();
    const auto storage = virtualDiskInfo(raw, GET_VIRTUAL_DISK_INFO_VIRTUAL_STORAGE_TYPE);
    if (!storage) {
        return refused(storage.error());
    }
    if (!supportedVirtualStorageType(storage->VirtualStorageType.DeviceId) ||
        !IsEqualGUID(storage->VirtualStorageType.VendorId, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT)) {
        return refused(QStringLiteral("Only Microsoft VHD/VHDX attachments are permitted"));
    }
    const auto size = virtualDiskInfo(raw, GET_VIRTUAL_DISK_INFO_SIZE);
    if (!size) {
        return refused(size.error());
    }
    const auto id = virtualDiskInfo(raw, GET_VIRTUAL_DISK_INFO_IDENTIFIER);
    if (!id) {
        return refused(id.error());
    }
    const auto physical = attachedPhysicalPath(raw);
    if (!physical) {
        return refused(physical.error());
    }
    const QString identifier = QUuid(id->Identifier).toString(QUuid::WithoutBraces);
    if (QUuid(identifier).isNull()) {
        return refused(QStringLiteral("VHD identifier is unavailable"));
    }
    return Attachment{.file_path = path,
                      .physical_path = *physical,
                      .vhd_id = identifier,
                      .disk_number = *sak::rawDevicePhysicalDriveNumber(*physical),
                      .virtual_size = size->Size.VirtualSize,
                      .storage_type = storage->VirtualStorageType.DeviceId,
                      .handle = std::move(handle)};
}

struct AttachmentOpenOptions {
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
    VIRTUAL_DISK_ACCESS_MASK access{};
};

AttachmentOpenOptions attachmentOpenOptions() {
    AttachmentOpenOptions options;
    options.parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    options.parameters.Version1.RWDepth = 0;
    // Windows requires DETACH when reopening a permanently attached VHD, even for
    // observation. This grants no backing-store write access and does not detach it.
    // Actual detach remains restricted to the validated source after CreateImage.
    options.access = static_cast<VIRTUAL_DISK_ACCESS_MASK>(VIRTUAL_DISK_ACCESS_GET_INFO |
                                                           VIRTUAL_DISK_ACCESS_DETACH);
    return options;
}

Result<Attachment> openAttachment(const QString& path) {
    VIRTUAL_STORAGE_TYPE type{VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN,
                              VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    auto options = attachmentOpenOptions();
    HANDLE raw = nullptr;
    const DWORD opened = OpenVirtualDisk(&type,
                                         reinterpret_cast<LPCWSTR>(path.utf16()),
                                         options.access,
                                         OPEN_VIRTUAL_DISK_FLAG_NONE,
                                         &options.parameters,
                                         &raw);
    if (opened != ERROR_SUCCESS) {
        return refused(windowsFailure(QStringLiteral("OpenVirtualDisk: %1").arg(path), opened));
    }
    return describeAttachment(NativeHandle(raw), path);
}

bool protectedInventoryDisk(const sak::PartitionDiskInfo& disk) {
    return disk.is_system || disk.is_boot || disk.is_dynamic || disk.is_storage_spaces ||
           disk.partition_enumeration_failed || disk.unique_id.trimmed().isEmpty();
}

Result<void> validateDiskFacts(const Attachment& attachment, const sak::PartitionDiskInfo& disk) {
    if (attachment.virtual_size < kMinimumVhdBytes || attachment.virtual_size > kMaximumVhdBytes ||
        disk.size_bytes != attachment.virtual_size) {
        return refused(
            QStringLiteral("Disposable VHD size must match inventory and be 512-1024 MiB"));
    }
    if (disk.disk_number.value() != attachment.disk_number ||
        !samePath(sak::canonicalRawDevicePath(disk.device_path), attachment.physical_path)) {
        return refused(QStringLiteral("VHD attachment and inventory device identity disagree"));
    }
    if (protectedInventoryDisk(disk)) {
        return refused(QStringLiteral("Protected, incomplete or identity-less disk refused"));
    }
    if (std::ranges::any_of(disk.partitions, [](const auto& partition) {
            return partition.is_boot || partition.is_system || partition.is_efi;
        })) {
        return refused(QStringLiteral("VHD contains a boot/system/EFI partition"));
    }
    return {};
}

Result<sak::PartitionDiskInfo> resolveInventoryDisk(const Attachment& attachment,
                                                    const sak::PartitionInventory& inventory) {
    const auto matches = std::ranges::count_if(inventory.disks, [&](const auto& disk) {
        return disk.disk_number.value() == attachment.disk_number;
    });
    if (matches != 1) {
        return refused(QStringLiteral("VHD disk is absent or ambiguous in inventory"));
    }
    const auto found = std::ranges::find_if(inventory.disks, [&](const auto& disk) {
        return disk.disk_number.value() == attachment.disk_number;
    });
    if (const auto valid = validateDiskFacts(attachment, *found); !valid) {
        return refused(valid.error());
    }
    return *found;
}

Result<void> sufficientOutputSpace(const QString& path, uint64_t vhd_size) {
    const QStorageInfo storage(path);
    const uint64_t required_space = vhd_size * kWorkspaceSizeMultiplier;
    if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < 0 ||
        static_cast<uint64_t>(storage.bytesAvailable()) < required_space) {
        return refused(QStringLiteral("Fixture volume free space is unknown or insufficient"));
    }
    return {};
}

Result<QVector<uint32_t>> volumeDiskNumbers(const QString& path) {
    const auto identity = sak::resolvePartitionPathVolumeIdentity(path);
    if (!identity.valid()) {
        return refused(identity.error);
    }
    QString volume = identity.volume_guid;
    while (volume.endsWith(QLatin1Char('\\'))) {
        volume.chop(1);
    }
    NativeHandle handle(CreateFileW(reinterpret_cast<LPCWSTR>(volume.utf16()),
                                    0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    0,
                                    nullptr));
    constexpr DWORD kBufferBytes = sizeof(VOLUME_DISK_EXTENTS) +
                                   (kMaximumVolumeExtents - 1) * sizeof(DISK_EXTENT);
    alignas(VOLUME_DISK_EXTENTS) std::array<std::byte, kBufferBytes> storage{};
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(storage.data());
    DWORD received = 0;
    if (handle.get() == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(handle.get(),
                         IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr,
                         0,
                         extents,
                         kBufferBytes,
                         &received,
                         nullptr)) {
        return refused(windowsFailure(QStringLiteral("Output volume disk extents")));
    }
    if (extents->NumberOfDiskExtents == 0 || extents->NumberOfDiskExtents > kMaximumVolumeExtents) {
        return refused(QStringLiteral("Output volume extents are absent or exceed the bound"));
    }
    const size_t required_bytes = offsetof(VOLUME_DISK_EXTENTS, Extents) +
                                  extents->NumberOfDiskExtents * sizeof(DISK_EXTENT);
    if (received < required_bytes) {
        return refused(QStringLiteral("Output volume returned truncated extent information"));
    }
    QVector<uint32_t> numbers;
    for (DWORD index = 0; index < extents->NumberOfDiskExtents; ++index) {
        numbers.append(extents->Extents[index].DiskNumber);
    }
    return numbers;
}

Result<void> outputVolumeIsSeparate(const Config& config, const FixtureState& fixture) {
    if (const auto space = sufficientOutputSpace(config.output_dir, fixture.source.virtual_size);
        !space) {
        return space;
    }
    const auto numbers = volumeDiskNumbers(config.output_dir);
    if (!numbers) {
        return refused(numbers.error());
    }
    if (numbers->contains(fixture.source.disk_number) ||
        numbers->contains(fixture.target.disk_number)) {
        return refused(
            QStringLiteral("Fixture directory is on a VHD that this run would read/write"));
    }
    return {};
}

Result<QString> fixtureVolumeRoot(const sak::PartitionDiskInfo& disk,
                                  const QString& label,
                                  sak::PartitionInfoEx* selected = nullptr) {
    QStringList matches;
    for (const auto& partition : disk.partitions) {
        if (partition.volume && partition.volume->label == label &&
            partition.volume->file_system.compare(QStringLiteral("NTFS"), Qt::CaseInsensitive) ==
                0 &&
            partition.volume->drive_letter.size() == 1) {
            const auto& volume = *partition.volume;
            const QString root = volume.drive_letter + QStringLiteral(":/");
            const auto actual = sak::resolvePartitionPathVolumeIdentity(root);
            if (!actual.valid() || !samePath(actual.volume_guid, volume.volume_guid)) {
                return refused(
                    QStringLiteral("Fixture drive letter no longer matches its volume GUID"));
            }
            matches.append(root);
            if (selected != nullptr) {
                *selected = partition;
            }
        }
    }
    if (matches.size() != 1) {
        return refused(
            QStringLiteral("Expected exactly one mounted NTFS fixture volume: %1").arg(label));
    }
    return matches.constFirst();
}

Result<QString> verifyMarker(const QString& path, const QString& expected) {
    if (const auto safe = rejectReparseChain(path); !safe) {
        return refused(safe.error());
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return refused(file.errorString());
    }
    const QByteArray bytes = file.read(kMaximumMarkerBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > kMaximumMarkerBytes ||
        QString::fromUtf8(bytes).trimmed() != expected) {
        return refused(QStringLiteral("Fixture marker content/size mismatch: %1").arg(path));
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

Result<QString> hashFile(const QString& path) {
    const sak::file_hasher hasher;
    const auto digest = hasher.calculateHash(std::filesystem::path(path.toStdWString()));
    if (!digest) {
        return refused(QStringLiteral("SHA-256 failed for %1").arg(path));
    }
    return QString::fromStdString(*digest).toLower();
}

QJsonObject attachmentJson(const Attachment& attachment, const sak::PartitionDiskInfo& disk) {
    return {{"vhd_path", attachment.file_path},
            {"vhd_identifier", attachment.vhd_id},
            {"physical_path", attachment.physical_path},
            {"disk_number", static_cast<int>(attachment.disk_number)},
            {"disk_unique_id", disk.unique_id},
            {"disk_serial_number", disk.serial_number},
            {"size_bytes", QString::number(disk.size_bytes)},
            {"bus_type", disk.bus_type},
            {"is_boot", disk.is_boot},
            {"is_system", disk.is_system}};
}

void emitWorkerFrame(const QJsonObject& report) {
    QTextStream stream(stdout);
    stream << kWorkerFramePrefix
           << QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Compact)) << Qt::endl;
}

Result<std::optional<sak::PartitionExecutionResult>> waitForController(
    sak::PartitionManagerController& controller,
    const std::function<bool()>& start,
    QJsonObject* report) {
    QEventLoop loop;
    QTimer deadline;
    QTimer grace;
    deadline.setSingleShot(true);
    grace.setSingleShot(true);
    bool timed_out = false;
    std::optional<sak::PartitionExecutionResult> execution;
    QObject::connect(&controller,
                     &sak::PartitionManagerController::executionFinished,
                     &loop,
                     [&](const auto& result) { execution = result; });
    QObject::connect(&controller, &sak::PartitionManagerController::stateChanged, &loop, [&]() {
        if (!controller.interactionBlocked()) {
            loop.quit();
        }
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        timed_out = true;
        controller.cancel();
        grace.start(kCancellationGraceMs);
    });
    QObject::connect(&grace, &QTimer::timeout, &loop, [&]() {
        report->insert("status", "Failed");
        report->insert("error", "Controller failed to settle after bounded cancellation");
        report->insert("timed_out", true);
        emitWorkerFrame(*report);
        // The supervisor still owns our kill-on-close job and reaps descendants before reporting.
        ExitProcess(kTimeoutExit);
    });
    deadline.start(kStageTimeoutMs);
    if (!start()) {
        return refused(QStringLiteral("Controller refused the requested transition"));
    }
    if (controller.interactionBlocked()) {
        loop.exec();
    }
    if (timed_out) {
        return refused(QStringLiteral("Controller stage exceeded its deadline"));
    }
    return execution;
}

Result<void> refreshController(sak::PartitionManagerController& controller, QJsonObject* report) {
    const auto refreshed = waitForController(
        controller,
        [&]() {
            controller.refreshInventory();
            return true;
        },
        report);
    if (!refreshed) {
        return refused(refreshed.error());
    }
    const auto& inventory = controller.inventory();
    if (inventory.isEmpty() || inventory.layout_hash.isEmpty() ||
        inventory.hasPartitionEnumerationFailure() || !inventory.warnings.isEmpty()) {
        return refused(QStringLiteral("Controller inventory is not authoritative: %1")
                           .arg(inventory.warnings.join(QStringLiteral("; "))));
    }
    return {};
}

Result<void> attachmentHandleUnchanged(const Attachment& attachment) {
    const auto physical = attachedPhysicalPath(attachment.handle.get());
    if (!physical) {
        return refused(physical.error());
    }
    const auto identifier = virtualDiskInfo(attachment.handle.get(),
                                            GET_VIRTUAL_DISK_INFO_IDENTIFIER);
    if (!identifier) {
        return refused(identifier.error());
    }
    if (!samePath(*physical, attachment.physical_path) ||
        QUuid(identifier->Identifier).toString(QUuid::WithoutBraces) != attachment.vhd_id) {
        return refused(QStringLiteral("Virtual Disk handle attachment identity changed"));
    }
    return {};
}

Result<void> revalidateAttachment(const Attachment& attachment,
                                  const sak::PartitionDiskInfo& expected,
                                  const sak::PartitionInventory& inventory) {
    if (const auto valid = attachmentHandleUnchanged(attachment); !valid) {
        return valid;
    }
    const auto current = resolveInventoryDisk(attachment, inventory);
    if (!current) {
        return refused(current.error());
    }
    if (current->unique_id != expected.unique_id ||
        current->serial_number != expected.serial_number) {
        return refused(QStringLiteral("Stable disk identity changed before operation"));
    }
    return {};
}

bool executionFlagsPassed(const sak::PartitionExecutionResult& result) {
    return result.success && !result.dry_run && !result.cancelled && !result.timed_out;
}

bool operationExecutionPassed(const sak::PartitionExecutionResult& result,
                              const sak::PartitionOperation& operation,
                              const sak::PartitionManagerController& controller) {
    return executionFlagsPassed(result) && result.steps.size() == 1 &&
           result.steps.constFirst().success && !result.steps.constFirst().skipped &&
           result.steps.constFirst().operation_id == operation.id && controller.queue().isEmpty();
}

Result<void> recordExecution(const sak::PartitionOperation& operation,
                             const sak::PartitionQueueReviewToken& reviewed,
                             const sak::PartitionScript& script,
                             const sak::PartitionExecutionResult& result,
                             QJsonObject* report) {
    const QByteArray raw_report = result.report_json.toUtf8();
    if (raw_report.size() > kMaximumExecutionReportBytes) {
        return refused(QStringLiteral("Execution evidence exceeded its size bound"));
    }
    QJsonParseError parse_error;
    const auto execution_report = QJsonDocument::fromJson(raw_report, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !execution_report.isObject()) {
        return refused(QStringLiteral("Controller emitted invalid execution-report JSON"));
    }
    QJsonArray runs = report->value("operations").toArray();
    runs.append(QJsonObject{
        {"type", sak::toDisplayString(operation.type)},
        {"operation_id", operation.id},
        {"review_revision", QString::number(reviewed.revision)},
        {"review_fingerprint", reviewed.fingerprint},
        {"layout_hash", reviewed.layout_hash},
        {"target_unique_id", operation.target.disk_unique_id},
        {"accepted_payload", operation.payload},
        {"target_kind",
         operation.target.kind == sak::PartitionTargetKind::Disk ? "Disk" : "Partition"},
        {"target_partition_number", static_cast<int>(operation.target.partition_number.value())},
        {"target_partition_guid", operation.target.partition_guid},
        {"target_offset_bytes", QString::number(operation.target.offset_bytes)},
        {"target_size_bytes", QString::number(operation.target.size_bytes)},
        {"execution_cancelled", result.cancelled},
        {"execution_timed_out", result.timed_out},
        {"queued_script_sha256",
         QString::fromLatin1(
             QCryptographicHash::hash(script.script.toUtf8(), QCryptographicHash::Sha256).toHex())},
        {"execution", execution_report.object()}});
    report->insert("operations", runs);
    return {};
}

struct PreparedOperation {
    sak::PartitionOperation operation{};
    sak::PartitionQueueReviewToken reviewed{};
    sak::PartitionScript script{};
};

bool acceptedRequestMatches(const sak::PartitionOperation& operation,
                            const sak::PartitionTarget& target,
                            const QJsonObject& payload) {
    return operation.target.kind == target.kind &&
           operation.target.disk_number == target.disk_number &&
           operation.target.partition_number == target.partition_number &&
           operation.target.partition_guid == target.partition_guid &&
           operation.target.offset_bytes == target.offset_bytes &&
           operation.target.size_bytes == target.size_bytes &&
           std::ranges::all_of(payload.keys(), [&](const auto& key) {
               return operation.payload.value(key) == payload.value(key);
           });
}

Result<PreparedOperation> prepareControllerOperation(sak::PartitionManagerController& controller,
                                                     const sak::PartitionDiskInfo& disk,
                                                     sak::PartitionOperationType type,
                                                     const QJsonObject& payload,
                                                     const sak::PartitionTarget& target) {
    if (!controller.queue().isEmpty()) {
        return refused(
            QStringLiteral("Certifier requires an empty controller queue per operation"));
    }
    if (!controller.queueOperation(type, target, payload) ||
        controller.queue().operations().size() != 1) {
        return refused(QStringLiteral("Controller did not admit exactly one operation"));
    }
    PreparedOperation prepared;
    prepared.operation = controller.queue().operations().constFirst();
    prepared.reviewed = controller.queueReviewToken();
    const sak::PartitionScriptBuilder builder;
    prepared.script = builder.buildScript(prepared.operation);
    if (!prepared.script.valid() || prepared.operation.target.disk_unique_id != disk.unique_id ||
        !acceptedRequestMatches(prepared.operation, target, payload)) {
        return refused(
            QStringLiteral("Queued operation has no valid script/stable target binding"));
    }
    return prepared;
}

bool ownedSourceLockStepRefused(const sak::PartitionExecutionStep& step,
                                const sak::PartitionOperation& operation) {
    const QString errors = step.stderr_text + step.error_message;
    const bool owned_lock_error =
        errors.contains(
            QStringLiteral("Create Image cannot open the source volume for exclusive capture:")) ||
        errors.contains(QStringLiteral("Create Image cannot lock the source volume."));
    return !step.success && !step.skipped && step.operation_id == operation.id &&
           owned_lock_error &&
           !step.stdout_text.contains(QStringLiteral("Running full clone verification")) &&
           !step.stdout_text.contains(QStringLiteral("Running sample clone verification"));
}

bool ownedSourceLockRefused(const sak::PartitionExecutionResult& result,
                            const sak::PartitionOperation& operation) {
    if (operation.type != sak::PartitionOperationType::CreateImage || result.success ||
        result.dry_run || result.cancelled || result.timed_out || result.steps.size() != 1) {
        return false;
    }
    return ownedSourceLockStepRefused(result.steps.constFirst(), operation);
}

Result<void> runControllerOperation(sak::PartitionManagerController& controller,
                                    const sak::PartitionDiskInfo& disk,
                                    const sak::PartitionOperationRequest& request,
                                    QJsonObject* report,
                                    bool expect_lock_refusal = false) {
    const auto prepared =
        prepareControllerOperation(controller, disk, request.type, request.payload, request.target);
    if (!prepared) {
        return refused(prepared.error());
    }
    const auto execution = waitForController(
        controller,
        [&]() {
            const bool accepted = controller.applyQueue(prepared->reviewed, false, false);
            if (accepted) {
                report->insert("mutations_may_have_started", true);
                report->insert("controller_attempted", true);
            }
            return accepted;
        },
        report);
    if (!execution) {
        return refused(execution.error());
    }
    if (!execution->has_value()) {
        return refused(QStringLiteral("Controller emitted no execution result"));
    }
    const auto& result = **execution;
    if (const auto recorded = recordExecution(
            prepared->operation, prepared->reviewed, prepared->script, result, report);
        !recorded) {
        return recorded;
    }
    if (expect_lock_refusal) {
        if (!ownedSourceLockRefused(result, prepared->operation)) {
            return refused(QStringLiteral("Busy proof did not fail at owned source acquisition: %1")
                               .arg(result.message));
        }
        report->insert("owned_source_lock_refusal", true);
        return {};
    }
    if (!operationExecutionPassed(result, prepared->operation, controller)) {
        return refused(QStringLiteral("Real controller execution did not fully succeed: %1")
                           .arg(result.message));
    }
    report->insert("controller_executed", true);
    return {};
}

Result<void> resolveFixtureDisks(FixtureState* fixture, const sak::PartitionInventory& inventory) {
    const auto source = resolveInventoryDisk(fixture->source, inventory);
    if (!source) {
        return refused(source.error());
    }
    const auto target = resolveInventoryDisk(fixture->target, inventory);
    if (!target) {
        return refused(target.error());
    }
    if (source->unique_id == target->unique_id || target->is_read_only) {
        return refused(QStringLiteral("Target is read-only or duplicates source disk identity"));
    }
    fixture->source_disk = *source;
    fixture->target_disk = *target;
    return {};
}

Result<void> admitMarkers(const Config& config, FixtureState* fixture) {
    const auto source_root = fixtureVolumeRoot(fixture->source_disk,
                                               QStringLiteral("SAKRESTSRC"),
                                               &fixture->source_partition);
    if (!source_root) {
        return refused(source_root.error());
    }
    const auto target_root = fixtureVolumeRoot(fixture->target_disk, QStringLiteral("SAKRESTDST"));
    if (!target_root) {
        return refused(target_root.error());
    }
    const auto sentinel = verifyMarker(*source_root + "sak-partition-manager-restore.txt",
                                       "partition-manager-restore-certification:" + config.run_id);
    if (!sentinel) {
        return refused(sentinel.error());
    }
    const auto marker = verifyMarker(*target_root + "sak-partition-manager-pre-restore.txt",
                                     "target-before-restore:" + config.run_id);
    if (!marker) {
        return refused(marker.error());
    }
    fixture->source_marker_hash = *sentinel;
    fixture->target_marker = "target-before-restore:" + config.run_id;
    fixture->image_path = QDir(config.output_dir).filePath("controller-source.img");
    if (QFileInfo::exists(fixture->image_path)) {
        return refused(QStringLiteral("Raw image output already exists; use a fresh fixture run"));
    }
    return {};
}

Result<FixtureState> admitFixture(const Config& config,
                                  sak::PartitionManagerController& controller,
                                  QJsonObject* report) {
    auto source = openAttachment(config.source_vhd);
    if (!source) {
        return refused(source.error());
    }
    auto target = openAttachment(config.target_vhd);
    if (!target) {
        return refused(target.error());
    }
    if (source->disk_number == target->disk_number || source->vhd_id == target->vhd_id ||
        source->virtual_size != target->virtual_size) {
        return refused(
            QStringLiteral("Distinct, equal-sized source/target VHD identities required"));
    }
    if (const auto refreshed = refreshController(controller, report); !refreshed) {
        return refused(refreshed.error());
    }
    FixtureState fixture;
    fixture.source = std::move(*source);
    fixture.target = std::move(*target);
    if (const auto resolved = resolveFixtureDisks(&fixture, controller.inventory()); !resolved) {
        return refused(resolved.error());
    }
    if (const auto separate = outputVolumeIsSeparate(config, fixture); !separate) {
        return refused(separate.error());
    }
    if (const auto markers = admitMarkers(config, &fixture); !markers) {
        return refused(markers.error());
    }
    report->insert("source", attachmentJson(fixture.source, fixture.source_disk));
    report->insert("target_before", attachmentJson(fixture.target, fixture.target_disk));
    return fixture;
}

Result<void> verifyNativeSource(HANDLE handle,
                                const Attachment& source,
                                std::optional<uint32_t> partition = std::nullopt) {
    STORAGE_DEVICE_NUMBER actual{};
    DWORD received = 0;
    if (!DeviceIoControl(handle,
                         IOCTL_STORAGE_GET_DEVICE_NUMBER,
                         nullptr,
                         0,
                         &actual,
                         sizeof(actual),
                         &received,
                         nullptr)) {
        return refused(windowsFailure(QStringLiteral("Identify held fixture source")));
    }
    if (received != sizeof(actual) || actual.DeviceType != FILE_DEVICE_DISK ||
        actual.DeviceNumber != source.disk_number ||
        (partition && actual.PartitionNumber != *partition)) {
        return refused(QStringLiteral("Held fixture source identity mismatch"));
    }
    return attachmentHandleUnchanged(source);
}

Result<NativeHandle> openFixtureSource(const FixtureState& fixture,
                                       const QString& path,
                                       DWORD access,
                                       std::optional<uint32_t> partition = std::nullopt) {
    if (const auto pinned = attachmentHandleUnchanged(fixture.source); !pinned) {
        return refused(pinned.error());
    }
    NativeHandle handle(CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                    access,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    0,
                                    nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        return refused(windowsFailure(QStringLiteral("Open admitted fixture source")));
    }
    if (const auto valid = verifyNativeSource(handle.get(), fixture.source, partition); !valid) {
        return refused(valid.error());
    }
    return handle;
}

Result<uint64_t> fixtureDiskAttributes(HANDLE handle) {
    GET_DISK_ATTRIBUTES attributes{};
    DWORD received = 0;
    if (!DeviceIoControl(handle,
                         IOCTL_DISK_GET_DISK_ATTRIBUTES,
                         nullptr,
                         0,
                         &attributes,
                         sizeof(attributes),
                         &received,
                         nullptr)) {
        return refused(windowsFailure(QStringLiteral("Read fixture disk attributes")));
    }
    if (received != sizeof(attributes) || attributes.Version != sizeof(attributes)) {
        return refused(QStringLiteral("Fixture disk attributes reply is incomplete"));
    }
    return attributes.Attributes;
}

Result<void> setOfflineFixtureAttributes(const Config& config, FixtureState* fixture) {
    auto setter =
        openFixtureSource(*fixture, fixture->source.physical_path, GENERIC_READ | GENERIC_WRITE);
    if (!setter) {
        return refused(setter.error());
    }
    SET_DISK_ATTRIBUTES attributes{};
    attributes.Version = sizeof(attributes);
    attributes.Persist = FALSE;
    attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
    attributes.Attributes = fixture->source_attributes | DISK_ATTRIBUTE_OFFLINE;
    if (config.proof_mode == ProofMode::OfflineReadOnlyRestore) {
        attributes.Attributes |= DISK_ATTRIBUTE_READ_ONLY;
    }
    DWORD received = 0;
    if (!DeviceIoControl(setter->get(),
                         IOCTL_DISK_SET_DISK_ATTRIBUTES,
                         &attributes,
                         sizeof(attributes),
                         nullptr,
                         0,
                         &received,
                         nullptr)) {
        return refused(windowsFailure(QStringLiteral("Prepare admitted offline source fixture")));
    }
    const auto actual = fixtureDiskAttributes(setter->get());
    if (!actual) {
        return refused(actual.error());
    }
    if (*actual != attributes.Attributes) {
        return refused(QStringLiteral("Prepared source attributes do not match requested fixture"));
    }
    fixture->source_attributes = *actual;
    return {};
}

Result<void> acquireFixtureSourceLock(FixtureState* fixture) {
    if (!fixture->source_partition.volume) {
        return refused(QStringLiteral("Admitted source volume identity is unavailable"));
    }
    QString volume = fixture->source_partition.volume->volume_guid;
    while (volume.endsWith(QLatin1Char('\\'))) {
        volume.chop(1);
    }
    if (sak::canonicalRawDevicePath(volume).isEmpty()) {
        return refused(QStringLiteral("Admitted source volume path is not canonical"));
    }
    auto holder = openFixtureSource(
        *fixture, volume, GENERIC_READ, fixture->source_partition.partition_number.value());
    if (!holder) {
        return refused(holder.error());
    }
    DWORD received = 0;
    if (!DeviceIoControl(
            holder->get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &received, nullptr)) {
        return refused(windowsFailure(QStringLiteral("Acquire independent fixture source lock")));
    }
    fixture->source_holder = std::move(*holder);
    fixture->source_lock_path = volume;
    return {};
}

bool matchingImagePartition(const sak::PartitionInfoEx& actual,
                            const sak::PartitionInfoEx& expected) {
    return actual.disk_number == expected.disk_number &&
           actual.partition_number == expected.partition_number &&
           actual.partition_guid == expected.partition_guid &&
           actual.gpt_type == expected.gpt_type && actual.offset_bytes == expected.offset_bytes &&
           actual.size_bytes == expected.size_bytes;
}

bool isInventoryMsrAnnotation(const sak::PartitionVolumeInfo& volume) {
    // The inventory's MSR display annotation is not an observed Windows volume.
    // Accept its exact representation only; any identity, geometry, protection,
    // probe detail or error requires refusal, not a guessed nonmountable state.
    const sak::PartitionVolumeInfo annotation{
        .file_system = QStringLiteral("Other"),
        .file_system_source = sak::PartitionFileSystemDetector::inferredProtectedSource(),
        .health_status = QStringLiteral("Reserved")};
    const auto fields = [](const sak::PartitionVolumeInfo& value) {
        return std::tie(value.volume_guid,
                        value.drive_letter,
                        value.label,
                        value.file_system,
                        value.file_system_source,
                        value.file_system_details,
                        value.health_status,
                        value.total_bytes,
                        value.free_bytes,
                        value.bitlocker_enabled,
                        value.bitlocker_locked,
                        value.dirty_bit_known,
                        value.dirty_bit_set,
                        value.dirty_bit_error,
                        value.bitlocker_state_known,
                        value.bitlocker_state_error);
    };
    return fields(volume) == fields(annotation);
}

bool knownOfflineFixtureMsr(const sak::PartitionInfoEx& partition) {
    return partition.is_msr &&
           QUuid(partition.gpt_type) ==
               QUuid(QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae")) &&
           !partition.volume_lookup_failed && partition.volume_lookup_error.isEmpty() &&
           (!partition.volume || isInventoryMsrAnnotation(*partition.volume));
}

bool offlineFixturePartitionsKnown(const FixtureState& fixture) {
    const auto source_matches =
        std::ranges::count_if(fixture.source_disk.partitions, [&](const auto& partition) {
            return matchingImagePartition(partition, fixture.source_partition);
        });
    return source_matches == 1 &&
           std::ranges::all_of(fixture.source_disk.partitions, [&](const auto& partition) {
               return matchingImagePartition(partition, fixture.source_partition) ||
                      knownOfflineFixtureMsr(partition);
           });
}

Result<void> closeProofHandle(NativeHandle* handle, const QString& label) {
    if (!*handle) {
        return {};
    }
    if (!CloseHandle(handle->get())) {
        return refused(windowsFailure(label));
    }
    (void)handle->release();
    return {};
}

Result<void> prepareOfflineFixture(const Config& config,
                                   FixtureState* fixture,
                                   QJsonObject* report) {
    // The native offline bit alone does not reject open files. Quiesce this admitted
    // fixture first; never force-close another process or infer an unenumerated volume.
    if (!offlineFixturePartitionsKnown(*fixture)) {
        return refused(
            QStringLiteral("Offline fixture preparation requires only its admitted NTFS source and "
                           "known MSR partitions"));
    }
    if (const auto held = acquireFixtureSourceLock(fixture); !held) {
        return held;
    }
    report->insert("offline_preparation_source_locked", true);
    DWORD received = 0;
    if (!DeviceIoControl(fixture->source_holder.get(),
                         FSCTL_DISMOUNT_VOLUME,
                         nullptr,
                         0,
                         nullptr,
                         0,
                         &received,
                         nullptr)) {
        return refused(windowsFailure(QStringLiteral("Dismount admitted locked source fixture")));
    }
    report->insert("offline_preparation_source_dismounted", true);
    if (const auto prepared = setOfflineFixtureAttributes(config, fixture); !prepared) {
        return prepared;
    }
    // FSCTL_LOCK_VOLUME is released when its handle closes. Explicit UNLOCK can no
    // longer address an offline volume; successful CloseHandle is the native lease end.
    if (const auto closed = closeProofHandle(
            &fixture->source_holder, QStringLiteral("Close offline preparation source lock"));
        !closed) {
        return closed;
    }
    report->insert("offline_preparation_lock_released", true);
    return {};
}

Result<void> refreshPreparedFixture(FixtureState* fixture,
                                    sak::PartitionManagerController& controller,
                                    QJsonObject* report) {
    if (const auto refreshed = refreshController(controller, report); !refreshed) {
        return refreshed;
    }
    if (const auto valid =
            revalidateAttachment(fixture->source, fixture->source_disk, controller.inventory());
        !valid) {
        return valid;
    }
    const auto current = resolveInventoryDisk(fixture->source, controller.inventory());
    if (!current) {
        return refused(current.error());
    }
    const auto matches = std::ranges::count_if(current->partitions, [&](const auto& partition) {
        return matchingImagePartition(partition, fixture->source_partition);
    });
    if (matches != 1 || !current->is_offline.has_value() ||
        *current->is_offline != ((fixture->source_attributes & DISK_ATTRIBUTE_OFFLINE) != 0) ||
        current->is_read_only != ((fixture->source_attributes & DISK_ATTRIBUTE_READ_ONLY) != 0)) {
        return refused(
            QStringLiteral("Prepared fixture partition/state does not match live inventory"));
    }
    fixture->source_disk = *current;
    return {};
}

Result<void> prepareSourceProof(const Config& config,
                                FixtureState* fixture,
                                sak::PartitionManagerController& controller,
                                QJsonObject* report) {
    auto control = openFixtureSource(*fixture, fixture->source.physical_path, 0);
    if (!control) {
        return refused(control.error());
    }
    fixture->source_control = std::move(*control);
    const auto attributes = fixtureDiskAttributes(fixture->source_control.get());
    if (!attributes) {
        return refused(attributes.error());
    }
    fixture->source_attributes = *attributes;
    report->insert("source_attributes_before_preparation", QString::number(*attributes));
    if (offlineProof(config.proof_mode)) {
        if (const auto prepared = prepareOfflineFixture(config, fixture, report); !prepared) {
            return prepared;
        }
    } else if ((*attributes & DISK_ATTRIBUTE_OFFLINE) != 0) {
        return refused(QStringLiteral("Locked source fixture must be online"));
    }
    if (config.proof_mode == ProofMode::BusySourceRefusal) {
        if (const auto held = acquireFixtureSourceLock(fixture); !held) {
            return held;
        }
        report->insert("independent_source_lock_held", true);
        report->insert("independent_source_lock_path", fixture->source_lock_path);
        report->insert("independent_source_lock_partition",
                       static_cast<int>(fixture->source_partition.partition_number.value()));
    }
    report->insert("source_attributes_before_capture", QString::number(fixture->source_attributes));
    fixture->image_offset = config.proof_mode == ProofMode::OfflinePartitionImage
                                ? fixture->source_partition.offset_bytes
                                : 0;
    fixture->image_size = config.proof_mode == ProofMode::OfflinePartitionImage
                              ? fixture->source_partition.size_bytes
                              : fixture->source.virtual_size;
    return refreshPreparedFixture(fixture, controller, report);
}

bool fixtureImageRangeValid(const FixtureState& fixture) {
    return fixture.image_size != 0 && fixture.image_offset <= fixture.source.virtual_size &&
           fixture.image_size <= fixture.source.virtual_size - fixture.image_offset;
}

Result<QString> hashExactRange(QIODevice* input, uint64_t byte_count) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QElapsedTimer elapsed;
    elapsed.start();
    uint64_t remaining = byte_count;
    while (remaining != 0) {
        const auto take = static_cast<qint64>(std::min(remaining, kMiB));
        const QByteArray chunk = input->read(take);
        if (chunk.size() != take || elapsed.elapsed() > kStageTimeoutMs) {
            return refused(
                QStringLiteral("Pinned source range hash read failed or exceeded its bound"));
        }
        hash.addData(chunk);
        remaining -= static_cast<uint64_t>(chunk.size());
    }
    return QString::fromLatin1(hash.result().toHex());
}

Result<QString> hashFixtureSourceRange(const FixtureState& fixture) {
    if (!fixtureImageRangeValid(fixture)) {
        return refused(QStringLiteral("Fixture hash range exceeds pinned physical source"));
    }
    if (const auto valid = verifyNativeSource(fixture.source_control.get(), fixture.source);
        !valid) {
        return refused(valid.error());
    }
    QString error;
    auto input = sak::openFileOrRawDeviceReadOnly(fixture.source.physical_path, &error);
    if (!input || input->size() != static_cast<qint64>(fixture.source.virtual_size) ||
        !input->seek(static_cast<qint64>(fixture.image_offset))) {
        return refused(QStringLiteral("Cannot read exact pinned source range: %1").arg(error));
    }
    const auto digest = hashExactRange(input.get(), fixture.image_size);
    if (!digest) {
        return digest;
    }
    if (const auto valid = verifyNativeSource(fixture.source_control.get(), fixture.source);
        !valid) {
        return refused(valid.error());
    }
    return digest;
}

sak::PartitionTarget imageProofTarget(const Config& config, const FixtureState& fixture) {
    sak::PartitionTarget target;
    target.disk_number = fixture.source_disk.disk_number;
    target.size_bytes = fixture.source_disk.size_bytes;
    if (config.proof_mode == ProofMode::OfflinePartitionImage) {
        target.kind = sak::PartitionTargetKind::Partition;
        target.partition_number = fixture.source_partition.partition_number;
        target.partition_guid = fixture.source_partition.partition_guid;
        target.offset_bytes = fixture.source_partition.offset_bytes;
        target.size_bytes = fixture.source_partition.size_bytes;
    }
    return target;
}

Result<void> verifyControllerImageOutput(const Config& config,
                                         FixtureState* fixture,
                                         QJsonObject* report) {
    if (QFileInfo(fixture->image_path).size() != static_cast<qint64>(fixture->image_size)) {
        return refused(
            QStringLiteral("Created raw image length does not match selected source range"));
    }
    const auto digest = hashFile(fixture->image_path);
    if (!digest) {
        return refused(digest.error());
    }
    fixture->image_hash = *digest;
    report->insert("source_image", fixture->image_path);
    report->insert("source_image_sha256", *digest);
    report->insert("source_range_offset_bytes", QString::number(fixture->image_offset));
    report->insert("source_range_size_bytes", QString::number(fixture->image_size));
    if (offlineProof(config.proof_mode)) {
        const auto final_hash = hashFixtureSourceRange(*fixture);
        if (!final_hash) {
            return refused(final_hash.error());
        }
        if (*final_hash != fixture->source_range_hash || *final_hash != fixture->image_hash) {
            return refused(QStringLiteral("Full offline source range and image hashes differ"));
        }
        report->insert("source_range_sha256_after", *final_hash);
        report->insert("source_range_verified", true);
    }
    return {};
}


Result<void> createControllerImage(const Config& config,
                                   FixtureState* fixture,
                                   sak::PartitionManagerController& controller,
                                   QJsonObject* report) {
    if (const auto valid =
            revalidateAttachment(fixture->source, fixture->source_disk, controller.inventory());
        !valid) {
        return valid;
    }
    if (offlineProof(config.proof_mode)) {
        const auto source_hash = hashFixtureSourceRange(*fixture);
        if (!source_hash) {
            return refused(source_hash.error());
        }
        fixture->source_range_hash = *source_hash;
        report->insert("source_range_sha256_before", *source_hash);
    }
    const QJsonObject payload{{"source_path", fixture->source.physical_path},
                              {"target_path", fixture->image_path},
                              {"source_offset_bytes", QString::number(fixture->image_offset)},
                              {"source_size_bytes", QString::number(fixture->image_size)},
                              {"source_capture_mode",
                               offlineProof(config.proof_mode) ? "OfflineDisk" : "LockedVolumes"},
                              {"verify_mode", "Full"}};
    if (const auto applied =
            runControllerOperation(controller,
                                   fixture->source_disk,
                                   {.type = sak::PartitionOperationType::CreateImage,
                                    .target = imageProofTarget(config, *fixture),
                                    .payload = payload},
                                   report,
                                   config.proof_mode == ProofMode::BusySourceRefusal);
        !applied) {
        return applied;
    }
    if (config.proof_mode == ProofMode::BusySourceRefusal) {
        if (QFileInfo::exists(fixture->image_path)) {
            return refused(QStringLiteral("Busy-source refusal created an image output"));
        }
        report->insert("image_output_absent", true);
        return {};
    }
    return verifyControllerImageOutput(config, fixture, report);
}

Result<void> finishSourceProof(FixtureState* fixture, QJsonObject* report) {
    if (const auto valid = verifyNativeSource(fixture->source_control.get(), fixture->source);
        !valid) {
        return valid;
    }
    const auto attributes = fixtureDiskAttributes(fixture->source_control.get());
    if (!attributes) {
        return refused(attributes.error());
    }
    report->insert("source_attributes_after_capture", QString::number(*attributes));
    if (*attributes != fixture->source_attributes) {
        return refused(QStringLiteral("Create Image changed full source disk attributes"));
    }
    report->insert("source_attributes_preserved", true);
    if (fixture->source_holder) {
        if (const auto valid =
                verifyNativeSource(fixture->source_holder.get(),
                                   fixture->source,
                                   fixture->source_partition.partition_number.value());
            !valid) {
            return valid;
        }
        DWORD received = 0;
        if (!DeviceIoControl(fixture->source_holder.get(),
                             FSCTL_UNLOCK_VOLUME,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             &received,
                             nullptr)) {
            return refused(
                windowsFailure(QStringLiteral("Release independent fixture source lock")));
        }
        if (const auto closed = closeProofHandle(
                &fixture->source_holder, QStringLiteral("Close independent fixture source lock"));
            !closed) {
            return closed;
        }
        report->insert("independent_source_lock_released", true);
    }
    return closeProofHandle(&fixture->source_control,
                            QStringLiteral("Close fixture source control"));
}

Result<void> detachSourceForRestore(const Config& config,
                                    FixtureState* fixture,
                                    const sak::PartitionInventory& inventory) {
    if (const auto paths = validateFixturePaths(config); !paths) {
        return paths;
    }
    if (const auto valid = revalidateAttachment(fixture->source, fixture->source_disk, inventory);
        !valid) {
        return valid;
    }
    const DWORD detached =
        DetachVirtualDisk(fixture->source.handle.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    if (detached != ERROR_SUCCESS) {
        return refused(windowsFailure(QStringLiteral("Detach validated source VHD"), detached));
    }
    return {};
}

Result<void> verifyRestoredFixture(const Config& config,
                                   const FixtureState& fixture,
                                   const sak::PartitionInventory& inventory,
                                   QJsonObject* report) {
    if (const auto native = attachmentHandleUnchanged(fixture.target); !native) {
        return native;
    }
    const auto disk = resolveInventoryDisk(fixture.target, inventory);
    if (!disk) {
        return refused(disk.error());
    }
    const auto root = fixtureVolumeRoot(*disk, QStringLiteral("SAKRESTSRC"));
    if (!root) {
        return refused(root.error());
    }
    const auto sentinel = verifyMarker(*root + "sak-partition-manager-restore.txt",
                                       "partition-manager-restore-certification:" + config.run_id);
    if (!sentinel) {
        return refused(sentinel.error());
    }
    if (*sentinel != fixture.source_marker_hash ||
        QFileInfo::exists(*root + "sak-partition-manager-pre-restore.txt")) {
        return refused(
            QStringLiteral("Restored sentinel/hash or target-overwrite verification failed"));
    }
    const auto final_hash = hashFile(fixture.image_path);
    if (!final_hash) {
        return refused(final_hash.error());
    }
    if (*final_hash != fixture.image_hash) {
        return refused(QStringLiteral("Source image changed"));
    }
    report->insert("target_after", attachmentJson(fixture.target, *disk));
    report->insert("sentinel_sha256", *sentinel);
    report->insert("sentinel_verified", true);
    report->insert("target_overwritten", true);
    report->insert("pre_restore_marker", fixture.target_marker);
    return {};
}

Result<void> restoreControllerImage(const Config& config,
                                    FixtureState* fixture,
                                    sak::PartitionManagerController& controller,
                                    QJsonObject* report) {
    if (const auto paths = validateFixturePaths(config); !paths) {
        return paths;
    }
    if (const auto refreshed = refreshController(controller, report); !refreshed) {
        return refreshed;
    }
    if (std::ranges::any_of(controller.inventory().disks, [&](const auto& disk) {
            return disk.unique_id == fixture->source_disk.unique_id;
        })) {
        return refused(QStringLiteral("Source VHD remains attached after its detach"));
    }
    report->insert("source_dismounted_before_restore", true);
    if (const auto valid =
            revalidateAttachment(fixture->target, fixture->target_disk, controller.inventory());
        !valid) {
        return valid;
    }
    if (const auto separate = outputVolumeIsSeparate(config, *fixture); !separate) {
        return separate;
    }
    const QJsonObject payload{{"source_path", fixture->image_path},
                              {"target_path", fixture->target.physical_path},
                              {"source_size_bytes", QString::number(fixture->source.virtual_size)},
                              {"target_size_bytes", QString::number(fixture->target.virtual_size)},
                              {"source_sha256", fixture->image_hash},
                              {"target_wipe_confirmed", true},
                              {"verify_mode", "Full"}};
    sak::PartitionTarget target;
    target.disk_number = fixture->target_disk.disk_number;
    target.size_bytes = fixture->target_disk.size_bytes;
    if (const auto applied =
            runControllerOperation(controller,
                                   fixture->target_disk,
                                   {.type = sak::PartitionOperationType::RestoreImage,
                                    .target = target,
                                    .payload = payload},
                                   report);
        !applied) {
        return applied;
    }
    return verifyRestoredFixture(config, *fixture, controller.inventory(), report);
}

Result<void> verifyWorkerContainment(const QString& name) {
    if (!name.startsWith(QStringLiteral("Local\\SAK-Partition-Cert-"))) {
        return refused(QStringLiteral("Worker containment identity is invalid"));
    }
    NativeHandle job(
        OpenJobObjectW(JOB_OBJECT_QUERY, FALSE, reinterpret_cast<LPCWSTR>(name.utf16())));
    BOOL contained = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!job || !IsProcessInJob(GetCurrentProcess(), job.get(), &contained) || !contained ||
        !QueryInformationJobObject(
            job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) ||
        (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) == 0) {
        return refused(QStringLiteral("Worker is not inside its kill-on-close containment job"));
    }
    // Do not retain a worker job handle: the supervisor must remain its last handle owner.
    return {};
}

QJsonObject reportBase(const Config& config) {
    return {{"tool", kToolName},
            {"schema_version", config.proof_mode == ProofMode::Restore ? 1 : 2},
            {"scenario_id", proofScenario(config.proof_mode)},
            {"proof_mode", proofModeName(config.proof_mode)},
            {"run_id", config.run_id},
            {"status", "Failed"},
            {"controller_executed", false},
            {"controller_attempted", false},
            {"source_preparation_started", false},
            {"operations", QJsonArray{}},
            {"mutations_may_have_started", false},
            {"execution_engine", "PartitionManagerController"},
            {"validation_only", config.validate_only},
            {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"source_digest", kSourceDigest},
            {"qt_version", qVersion()},
            {"source_vhd", config.source_vhd},
            {"target_vhd", config.target_vhd}};
}

Result<void> validateWorkerAdmission(const Config& config) {
    if (const auto contained = verifyWorkerContainment(config.containment_job); !contained) {
        return contained;
    }
    if (!sak::ElevationManager::isElevated()) {
        return refused(QStringLiteral("An already-elevated VM token is required"));
    }
    return validateFixturePaths(config);
}

int finishWorkerReport(QJsonObject* report, const Config& config, const Result<void>& outcome) {
    report->insert("status", outcome ? successfulProofStatus(config) : QStringLiteral("Failed"));
    if (!outcome) {
        report->insert("error", outcome.error());
    }
    report->insert("completed_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    emitWorkerFrame(*report);
    return outcome ? 0 : kFailureExit;
}

Result<void> executeFixtureProof(const Config& config,
                                 FixtureState* fixture,
                                 sak::PartitionManagerController& controller,
                                 QJsonObject* report) {
    report->insert("source_preparation_started", true);
    auto outcome = prepareSourceProof(config, fixture, controller, report);
    if (outcome) {
        outcome = createControllerImage(config, fixture, controller, report);
    }
    if (outcome) {
        outcome = finishSourceProof(fixture, report);
    }
    if (outcome && restoreProof(config.proof_mode)) {
        outcome = detachSourceForRestore(config, fixture, controller.inventory());
    }
    if (outcome && restoreProof(config.proof_mode)) {
        outcome = restoreControllerImage(config, fixture, controller, report);
    }
    return outcome;
}


int runWorker(const Config& config) {
    auto report = reportBase(config);
    if (const auto admitted = validateWorkerAdmission(config); !admitted) {
        return finishWorkerReport(&report, config, admitted);
    }
    sak::PartitionManagerController controller;
    QStringList logs;
    qsizetype log_characters = 0;
    bool log_overflow = false;
    QObject::connect(&controller,
                     &sak::PartitionManagerController::logOutput,
                     &controller,
                     [&](const QString& text) {
                         if (text.size() > kMaximumLogCharacters - log_characters) {
                             log_overflow = true;
                             controller.cancel();
                             return;
                         }
                         log_characters += text.size();
                         logs.append(text);
                     });
    auto fixture = admitFixture(config, controller, &report);
    if (!fixture) {
        return finishWorkerReport(&report, config, refused(fixture.error()));
    }
    if (config.validate_only) {
        return finishWorkerReport(&report, config, {});
    }
    auto outcome = executeFixtureProof(config, &*fixture, controller, &report);
    report.insert("logs", QJsonArray::fromStringList(logs));
    if (log_overflow) {
        outcome = refused(QStringLiteral("Controller log evidence exceeded its bound"));
    }
    return finishWorkerReport(&report, config, outcome);
}

Result<NativeHandle> createContainmentJob(const QString& name) {
    NativeHandle job(CreateJobObjectW(nullptr, reinterpret_cast<LPCWSTR>(name.utf16())));
    if (!job || GetLastError() == ERROR_ALREADY_EXISTS) {
        return refused(windowsFailure(QStringLiteral("Create unique containment job")));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return refused(windowsFailure(QStringLiteral("Set containment limits")));
    }
    return job;
}

Result<void> resumeSingleWorkerThread(DWORD process_id) {
    NativeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (snapshot.get() == INVALID_HANDLE_VALUE || !Thread32First(snapshot.get(), &entry)) {
        return refused(windowsFailure(QStringLiteral("Enumerate suspended worker thread")));
    }
    DWORD thread_id = 0;
    int matches = 0;
    do {
        if (entry.th32OwnerProcessID == process_id) {
            thread_id = entry.th32ThreadID;
            ++matches;
        }
    } while (Thread32Next(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES || matches != 1) {
        return refused(QStringLiteral("Suspended worker thread is absent or ambiguous"));
    }
    NativeHandle thread(OpenThread(THREAD_SUSPEND_RESUME, FALSE, thread_id));
    if (!thread || ResumeThread(thread.get()) != 1) {
        return refused(windowsFailure(QStringLiteral("Resume contained worker")));
    }
    return {};
}

Result<void> launchContainedWorker(QProcess& worker, HANDLE job, const QStringList& arguments) {
    worker.setProgram(QCoreApplication::applicationFilePath());
    worker.setArguments(arguments);
    worker.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_SUSPENDED | CREATE_NO_WINDOW;
    });
    worker.start();
    if (!worker.waitForStarted(kProcessStartTimeoutMs)) {
        return refused(
            QStringLiteral("Contained worker did not start: %1").arg(worker.errorString()));
    }
    NativeHandle process(OpenProcess(
        PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(worker.processId())));
    if (!process || !AssignProcessToJobObject(job, process.get())) {
        const DWORD assignment_error = GetLastError();
        worker.kill();  // Suspended worker has executed no code and spawned no descendant.
        if (!worker.waitForFinished(kProcessExitTimeoutMs)) {
            return refused(
                QStringLiteral("Uncontained suspended worker termination was not confirmed"));
        }
        return refused(
            windowsFailure(QStringLiteral("Assign suspended worker to job"), assignment_error));
    }
    return resumeSingleWorkerThread(static_cast<DWORD>(worker.processId()));
}

struct WorkerOutput {
    QByteArray stdout_bytes;
    QByteArray stderr_bytes;
    QString failure;
};

void failWorkerJob(HANDLE job, int code, WorkerOutput* output, const QString& reason) {
    output->failure = reason;
    if (!TerminateJobObject(job, static_cast<UINT>(code))) {
        output->failure += QStringLiteral("; %1").arg(
            windowsFailure(QStringLiteral("Terminate contained worker")));
    }
}

bool appendWorkerOutput(QByteArray* destination, const QByteArray& chunk, qsizetype current_total) {
    if (chunk.size() > kMaximumWorkerOutputBytes - current_total) {
        return false;
    }
    destination->append(chunk);
    return true;
}

WorkerOutput waitForWorker(QProcess& worker, HANDLE job) {
    WorkerOutput output;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    const auto drain = [&]() {
        const auto total = [&]() {
            return output.stdout_bytes.size() + output.stderr_bytes.size();
        };
        const bool stdout_ok =
            appendWorkerOutput(&output.stdout_bytes, worker.readAllStandardOutput(), total());
        const bool stderr_ok =
            appendWorkerOutput(&output.stderr_bytes, worker.readAllStandardError(), total());
        if (!stdout_ok || !stderr_ok) {
            failWorkerJob(
                job, kFailureExit, &output, QStringLiteral("Worker output exceeded its bound"));
            loop.quit();
        }
    };
    QObject::connect(&worker, &QProcess::readyReadStandardOutput, &loop, drain);
    QObject::connect(&worker, &QProcess::readyReadStandardError, &loop, drain);
    QObject::connect(&worker, &QProcess::finished, &loop, [&]() { loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        failWorkerJob(job,
                      kTimeoutExit,
                      &output,
                      QStringLiteral("Contained worker exceeded overall deadline"));
        loop.quit();
    });
    deadline.start(kWorkerTimeoutMs);
    if (worker.state() != QProcess::NotRunning) {
        loop.exec();
    }
    drain();
    if (worker.state() != QProcess::NotRunning && !worker.waitForFinished(kProcessExitTimeoutMs)) {
        output.failure += QStringLiteral("; worker termination was not confirmed");
    }
    return output;
}

bool workerProofIdentityMatches(const QJsonObject& report, const Config& config) {
    return report.value("schema_version").toInt() ==
               (config.proof_mode == ProofMode::Restore ? 1 : 2) &&
           report.value("scenario_id").toString() == proofScenario(config.proof_mode) &&
           report.value("proof_mode").toString() == proofModeName(config.proof_mode) &&
           report.value("validation_only").isBool() &&
           report.value("validation_only").toBool() == config.validate_only;
}

Result<QJsonObject> workerReport(const WorkerOutput& output, const Config& config) {
    if (!output.failure.isEmpty()) {
        return refused(output.failure);
    }
    QJsonObject report;
    int frames = 0;
    for (const auto& line : output.stdout_bytes.split('\n')) {
        if (!line.startsWith(kWorkerFramePrefix)) {
            continue;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line.mid(sizeof(kWorkerFramePrefix) - 1),
                                                      &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return refused(QStringLiteral("Worker emitted malformed evidence JSON"));
        }
        report = document.object();
        ++frames;
    }
    if (frames != 1 || report.value("run_id").toString() != config.run_id ||
        report.value("tool").toString() != QLatin1String(kToolName) ||
        !workerProofIdentityMatches(report, config)) {
        return refused(QStringLiteral("Worker evidence frame identity/count mismatch"));
    }
    return report;
}

Result<DWORD> activeJobProcessCount(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(
            job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
        return refused(windowsFailure(QStringLiteral("Query worker containment completion")));
    }
    return accounting.ActiveProcesses;
}

Result<void> confirmJobEmpty(HANDLE job, QJsonObject* evidence) {
    const auto initial = activeJobProcessCount(job);
    if (!initial) {
        return refused(initial.error());
    }
    evidence->insert("initial_active_processes", QString::number(*initial));
    evidence->insert("last_active_processes", QString::number(*initial));
    evidence->insert("wait_ms", 0);
    if (*initial == 0) {
        return {};
    }
    // The process exit notification and job accounting can settle on different turns.
    // Wait for observed zero; a live descendant never becomes an accepted result by timeout.
    QEventLoop loop;
    QTimer poll;
    QTimer deadline;
    QElapsedTimer elapsed;
    bool empty = false;
    QString query_error;
    deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        const auto count = activeJobProcessCount(job);
        if (!count) {
            query_error = count.error();
            loop.quit();
            return;
        }
        evidence->insert("last_active_processes", QString::number(*count));
        empty = *count == 0;
        if (empty) {
            loop.quit();
        }
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() { loop.quit(); });
    elapsed.start();
    poll.start(kJobQuiescencePollMs);
    deadline.start(kProcessExitTimeoutMs);
    loop.exec();
    evidence->insert("wait_ms", elapsed.elapsed());
    if (!query_error.isEmpty()) {
        return refused(query_error);
    }
    if (empty) {
        return {};
    }
    const BOOL killed = TerminateJobObject(job, kFailureExit);
    evidence->insert("termination_requested", killed != FALSE);
    return refused(killed
                       ? QStringLiteral("Worker descendants did not settle within the bounded wait")
                       : windowsFailure(QStringLiteral("Terminate remaining descendants")));
}

Result<void> writeFinalReport(const Config& config, const QJsonObject& report) {
    const QString path = QDir(config.output_dir).filePath(proofReportFile(config.proof_mode));
    if (QFileInfo::exists(path)) {
        return refused(QStringLiteral("Evidence report already exists"));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return refused(file.errorString());
    }
    const QByteArray data = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        return refused(QStringLiteral("Could not commit complete certification evidence"));
    }
    return {};
}

struct SupervisorResult {
    QJsonObject report{};
    bool success{false};
};

bool workerPassed(const Config& config, const QProcess& worker, const QJsonObject& report) {
    const QString expected = successfulProofStatus(config);
    return worker.exitStatus() == QProcess::NormalExit && worker.exitCode() == 0 &&
           report.value("status").toString() == expected;
}

SupervisorResult gatherWorkerResult(const Config& config,
                                    const QProcess& worker,
                                    HANDLE job,
                                    const WorkerOutput& output) {
    QJsonObject quiescence;
    const auto empty = confirmJobEmpty(job, &quiescence);
    const auto decoded = workerReport(output, config);
    SupervisorResult result;
    result.report = decoded ? *decoded : reportBase(config);
    result.report.insert("job_quiescence", quiescence);
    const QString original_error = result.report.value("error").toString();
    if (!original_error.isEmpty()) {
        result.report.insert("worker_error", original_error);
    }
    result.success = empty && decoded && workerPassed(config, worker, result.report);
    if (!result.success) {
        result.report.insert("status", "Failed");
        // Without a normal, complete worker result we cannot claim that no write began.
        result.report.insert("mutations_may_have_started", !config.validate_only);
        if (!empty) {
            result.report.insert("containment_error", empty.error());
            result.report.insert("error",
                                 original_error.isEmpty()
                                     ? empty.error()
                                     : original_error + QStringLiteral("; ") + empty.error());
        } else if (!decoded) {
            result.report.insert("error", decoded.error());
        } else if (!result.report.contains("error")) {
            result.report.insert("error", "Worker exited unsuccessfully");
        }
    }
    result.report.insert("worker_exit_code", worker.exitCode());
    result.report.insert("worker_descendants_finished", empty.has_value());
    result.report.insert("worker_stderr", QString::fromUtf8(output.stderr_bytes));
    return result;
}

int publishSupervisorResult(const Config& config,
                            SupervisorResult result,
                            const QString& binary_hash) {
    result.report.insert("certifier_binary_sha256", binary_hash);
    if (const auto saved = writeFinalReport(config, result.report); !saved) {
        QTextStream(stderr) << saved.error() << Qt::endl;
        return kFailureExit;
    }
    QTextStream(stdout) << QString::fromUtf8(
                               QJsonDocument(result.report).toJson(QJsonDocument::Compact))
                        << Qt::endl;
    return result.success ? 0 : kFailureExit;
}

Result<void> stopFailedLaunch(QProcess& worker, HANDLE job, const QString& cause) {
    QString failure = cause;
    if (!TerminateJobObject(job, kFailureExit)) {
        failure +=
            QStringLiteral("; %1").arg(windowsFailure(QStringLiteral("Terminate failed launch")));
    }
    if (worker.state() != QProcess::NotRunning && !worker.waitForFinished(kProcessExitTimeoutMs)) {
        failure += QStringLiteral("; failed launch termination was not confirmed");
    }
    return refused(failure);
}

int supervise(const Config& config, const QStringList& original_arguments) {
    if (const auto paths = validateFixturePaths(config); !paths) {
        QTextStream(stderr) << paths.error() << Qt::endl;
        return kFailureExit;
    }
    const QString report_path =
        QDir(config.output_dir).filePath(proofReportFile(config.proof_mode));
    if (QFileInfo::exists(report_path)) {
        QTextStream(stderr) << "Evidence report already exists" << Qt::endl;
        return kFailureExit;
    }
    const QString binary_path = QCoreApplication::applicationFilePath();
    NativeHandle binary_pin(CreateFileW(reinterpret_cast<LPCWSTR>(binary_path.utf16()),
                                        GENERIC_READ,
                                        FILE_SHARE_READ,
                                        nullptr,
                                        OPEN_EXISTING,
                                        0,
                                        nullptr));
    if (binary_pin.get() == INVALID_HANDLE_VALUE) {
        QTextStream(stderr) << windowsFailure(QStringLiteral("Pin certifier executable"))
                            << Qt::endl;
        return kFailureExit;
    }
    const auto binary_hash = hashFile(binary_path);
    if (!binary_hash) {
        QTextStream(stderr) << binary_hash.error() << Qt::endl;
        return kFailureExit;
    }
    const QString job_name = "Local\\SAK-Partition-Cert-" +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto job = createContainmentJob(job_name);
    if (!job) {
        QTextStream(stderr) << job.error() << Qt::endl;
        return kFailureExit;
    }
    QStringList arguments = original_arguments.mid(1);
    arguments << "--contained-worker" << "--containment-job" << job_name;
    QProcess worker;
    const auto launched = launchContainedWorker(worker, job->get(), arguments);
    if (!launched) {
        const auto stopped = stopFailedLaunch(worker, job->get(), launched.error());
        QTextStream(stderr) << stopped.error() << Qt::endl;
        return kFailureExit;
    }
    const auto output = waitForWorker(worker, job->get());
    return publishSupervisorResult(config,
                                   gatherWorkerResult(config, worker, job->get(), output),
                                   *binary_hash);
}

void addSelfTestCheck(QJsonArray* checks, const QString& name, bool passed) {
    checks->append(QJsonObject{{"name", name}, {"passed", passed}});
}

void selfTestConfigChecks(QJsonArray* checks) {
    const auto check = [&](const QString& name, bool passed) {
        addSelfTestCheck(checks, name, passed);
    };
    Config valid{.source_vhd = "C:/SAK-Cert/run/source.vhdx",
                 .target_vhd = "C:/SAK-Cert/run/target.vhdx",
                 .output_dir = "C:/SAK-Cert/run",
                 .run_id = "b6bca989-a1d3-4d4d-a307-b567c38f2d74",
                 .confirmed = true};
    check("literal-config-positive", validateConfigText(valid).has_value());
    auto bad = valid;
    bad.confirmed = false;
    check("confirmation-required", !validateConfigText(bad));
    bad.validate_only = true;
    check("validation-only-is-nondestructive", validateConfigText(bad).has_value());
    bad = valid;
    bad.target_vhd = "\\\\.\\PhysicalDrive0";
    check("raw-target-cli-refused", !validateConfigText(bad));
    bad = valid;
    bad.target_vhd = valid.source_vhd.toUpper();
    check("aliased-paths-refused", !validateConfigText(bad));
    bad = valid;
    bad.run_id.clear();
    check("missing-run-identity-refused", !validateConfigText(bad));
    bad = valid;
    bad.source_vhd = "C:/SAK-Cert/run/../source.vhdx";
    check("traversal-refused", !validateConfigText(bad));
    bad = valid;
    bad.source_vhd = "C:/elsewhere/source.vhdx";
    check("outside-fixture-root-refused", !validateConfigText(bad));
}

void selfTestDiskChecks(QJsonArray* checks) {
    const auto check = [&](const QString& name, bool passed) {
        addSelfTestCheck(checks, name, passed);
    };
    Attachment attachment{.physical_path = "\\\\.\\PhysicalDrive2",
                          .disk_number = 2,
                          .virtual_size = kMinimumVhdBytes};
    sak::PartitionDiskInfo disk;
    disk.disk_number = sak::DiskNumber{2};
    disk.device_path = attachment.physical_path;
    disk.unique_id = "known-fixture";
    disk.size_bytes = kMinimumVhdBytes;
    check("disk-facts-positive", validateDiskFacts(attachment, disk).has_value());
    disk.is_boot = true;
    check("boot-disk-refused", !validateDiskFacts(attachment, disk));
    disk.is_boot = false;
    disk.is_system = true;
    check("system-disk-refused", !validateDiskFacts(attachment, disk));
    disk.is_system = false;
    disk.unique_id.clear();
    check("missing-disk-identity-refused", !validateDiskFacts(attachment, disk));
    disk.unique_id = "known-fixture";
    attachment.virtual_size = kMaximumVhdBytes + 1;
    check("large-disk-refused", !validateDiskFacts(attachment, disk));
    attachment.virtual_size = kMinimumVhdBytes;
    disk.disk_number = sak::DiskNumber{3};
    check("attachment-disk-mismatch-refused", !validateDiskFacts(attachment, disk));
    check("vhd-type-positive", supportedVirtualStorageType(VIRTUAL_STORAGE_TYPE_DEVICE_VHD));
    check("vhdx-type-positive", supportedVirtualStorageType(VIRTUAL_STORAGE_TYPE_DEVICE_VHDX));
    check("iso-type-refused", !supportedVirtualStorageType(VIRTUAL_STORAGE_TYPE_DEVICE_ISO));
    check("unknown-type-refused",
          !supportedVirtualStorageType(VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN));
}

void selfTestProofModes(QJsonArray* checks) {
    const std::array modes{ProofMode::Restore,
                           ProofMode::OfflineRestore,
                           ProofMode::OfflineReadOnlyRestore,
                           ProofMode::OfflinePartitionImage,
                           ProofMode::BusySourceRefusal};
    for (const auto mode : modes) {
        const QString name = proofModeName(mode);
        const auto parsed = parseProofMode(name);
        Config config{.proof_mode = mode};
        const auto report = reportBase(config);
        addSelfTestCheck(checks, "proof-mode-roundtrip-" + name, parsed && *parsed == mode);
        addSelfTestCheck(
            checks,
            "proof-report-isolation-" + name,
            report.value("schema_version").toInt() == (mode == ProofMode::Restore ? 1 : 2) &&
                (proofScenario(mode) == QStringLiteral("vhd.image-restore")) ==
                    (mode == ProofMode::Restore) &&
                (proofReportFile(mode) == QStringLiteral("controller-restore-report.json")) ==
                    (mode == ProofMode::Restore));
        config.validate_only = true;
        addSelfTestCheck(checks,
                         "proof-validation-only-" + name,
                         successfulProofStatus(config) == QStringLiteral("Validated") &&
                             !reportBase(config).value("source_preparation_started").toBool());
    }
    addSelfTestCheck(checks,
                     "unknown-proof-mode-refused",
                     !parseProofMode(QStringLiteral("legacy")));
    addSelfTestCheck(checks, "empty-proof-mode-refused", !parseProofMode(QString{}));
    addSelfTestCheck(checks,
                     "busy-proof-status-isolated",
                     successfulProofStatus(Config{.proof_mode = ProofMode::BusySourceRefusal}) ==
                         QStringLiteral("RefusalVerified"));
}

void selfTestWorkflowRouting(QJsonArray* checks) {
    const std::array modes{ProofMode::Restore,
                           ProofMode::OfflineRestore,
                           ProofMode::OfflineReadOnlyRestore,
                           ProofMode::OfflinePartitionImage,
                           ProofMode::BusySourceRefusal};
    constexpr std::array expected_offline{false, true, true, true, false};
    constexpr std::array expected_restore{true, true, true, false, false};
    for (size_t index = 0; index < modes.size(); ++index) {
        addSelfTestCheck(checks,
                         "offline-workflow-" + proofModeName(modes[index]),
                         offlineProof(modes[index]) == expected_offline[index]);
        addSelfTestCheck(checks,
                         "restore-workflow-" + proofModeName(modes[index]),
                         restoreProof(modes[index]) == expected_restore[index]);
    }
}

void selfTestBusyRefusal(QJsonArray* checks) {
    sak::PartitionOperation operation;
    operation.id = QStringLiteral("owned-lock-proof");
    operation.type = sak::PartitionOperationType::CreateImage;
    sak::PartitionExecutionResult result;
    sak::PartitionExecutionStep step;
    step.operation_id = operation.id;
    step.stderr_text =
        QStringLiteral("Create Image cannot open the source volume for exclusive capture: fixture");
    result.steps.append(step);
    addSelfTestCheck(checks,
                     "busy-owned-open-refusal-positive",
                     ownedSourceLockRefused(result, operation));
    result.steps.first().stderr_text =
        QStringLiteral("Create Image cannot lock the source volume.");
    addSelfTestCheck(checks,
                     "busy-owned-lock-refusal-positive",
                     ownedSourceLockRefused(result, operation));
    const auto check_changed =
        [&](const QString& name,
            const std::function<void(sak::PartitionExecutionResult&)>& change) {
            auto changed = result;
            change(changed);
            addSelfTestCheck(checks, name, !ownedSourceLockRefused(changed, operation));
        };
    check_changed("busy-success-refused", [](auto& value) { value.success = true; });
    check_changed("busy-dry-run-refused", [](auto& value) { value.dry_run = true; });
    check_changed("busy-cancelled-refused", [](auto& value) { value.cancelled = true; });
    check_changed("busy-timeout-refused", [](auto& value) { value.timed_out = true; });
    check_changed("busy-no-step-refused", [](auto& value) { value.steps.clear(); });
    check_changed("busy-multiple-steps-refused", [](auto& value) {
        const auto extra_step = value.steps.constFirst();
        value.steps.append(extra_step);
    });
    check_changed("busy-skipped-step-refused",
                  [](auto& value) { value.steps.first().skipped = true; });
    check_changed("busy-passed-step-refused",
                  [](auto& value) { value.steps.first().success = true; });
    check_changed("busy-other-operation-refused",
                  [](auto& value) { value.steps.first().operation_id = "other"; });
    check_changed("busy-other-error-refused",
                  [](auto& value) { value.steps.first().stderr_text = "unrelated failure"; });
    check_changed("busy-verification-witness-refused", [](auto& value) {
        value.steps.first().stdout_text = "Running full clone verification";
    });
    check_changed("busy-sample-verification-witness-refused", [](auto& value) {
        value.steps.first().stdout_text = "Running sample clone verification";
    });
    operation.type = sak::PartitionOperationType::RestoreImage;
    addSelfTestCheck(checks,
                     "busy-restore-operation-refused",
                     !ownedSourceLockRefused(result, operation));
}

void selfTestImageScope(QJsonArray* checks) {
    FixtureState fixture;
    fixture.source.virtual_size = kMinimumVhdBytes;
    fixture.source_disk.disk_number = sak::DiskNumber{7};
    fixture.source_disk.size_bytes = fixture.source.virtual_size;
    fixture.source_partition.partition_number = sak::PartitionNumber{2};
    fixture.source_partition.offset_bytes = kMiB;
    fixture.source_partition.size_bytes = kMinimumVhdBytes - kMiB;
    fixture.image_size = fixture.source.virtual_size;
    addSelfTestCheck(checks, "image-range-whole-positive", fixtureImageRangeValid(fixture));
    fixture.image_size = 0;
    addSelfTestCheck(checks, "image-range-empty-refused", !fixtureImageRangeValid(fixture));
    fixture.image_offset = fixture.source.virtual_size + 1;
    fixture.image_size = 1;
    addSelfTestCheck(checks,
                     "image-range-offset-overflow-refused",
                     !fixtureImageRangeValid(fixture));
    fixture.image_offset = fixture.source.virtual_size;
    addSelfTestCheck(checks, "image-range-end-overflow-refused", !fixtureImageRangeValid(fixture));
    const auto target = imageProofTarget(Config{.proof_mode = ProofMode::OfflinePartitionImage},
                                         fixture);
    addSelfTestCheck(checks,
                     "partition-image-scope-pinned",
                     target.kind == sak::PartitionTargetKind::Partition &&
                         target.partition_number == fixture.source_partition.partition_number &&
                         target.offset_bytes == fixture.source_partition.offset_bytes &&
                         target.size_bytes == fixture.source_partition.size_bytes);
    const auto whole = imageProofTarget(Config{}, fixture);
    addSelfTestCheck(checks,
                     "whole-image-scope-pinned",
                     whole.kind == sak::PartitionTargetKind::Disk && whole.offset_bytes == 0 &&
                         whole.size_bytes == fixture.source_disk.size_bytes);
}

void selfTestAcceptedRequest(QJsonArray* checks) {
    sak::PartitionOperation operation;
    operation.target.disk_number = sak::DiskNumber{7};
    operation.target.size_bytes = kMinimumVhdBytes;
    operation.payload = {{"verify_mode", "Full"}, {"source_capture_mode", "OfflineDisk"}};
    const auto target = operation.target;
    const auto payload = operation.payload;
    addSelfTestCheck(checks,
                     "accepted-request-positive",
                     acceptedRequestMatches(operation, target, payload));
    operation.payload.insert("source_capture_mode", "LockedVolumes");
    addSelfTestCheck(checks,
                     "accepted-request-mode-drift-refused",
                     !acceptedRequestMatches(operation, target, payload));
    operation.payload = payload;
    operation.target.kind = sak::PartitionTargetKind::Partition;
    addSelfTestCheck(checks,
                     "accepted-request-kind-drift-refused",
                     !acceptedRequestMatches(operation, target, payload));
    operation.target = target;
    operation.target.offset_bytes = 1;
    addSelfTestCheck(checks,
                     "accepted-request-offset-drift-refused",
                     !acceptedRequestMatches(operation, target, payload));
    operation.target = target;
    operation.target.size_bytes = 1;
    addSelfTestCheck(checks,
                     "accepted-request-size-drift-refused",
                     !acceptedRequestMatches(operation, target, payload));
    operation.target = target;
    operation.target.disk_number = sak::DiskNumber{8};
    addSelfTestCheck(checks,
                     "accepted-request-disk-drift-refused",
                     !acceptedRequestMatches(operation, target, payload));
    operation.target = target;
    operation.target.partition_number = sak::PartitionNumber{2};
    addSelfTestCheck(checks,
                     "accepted-request-partition-drift-refused",
                     !acceptedRequestMatches(operation, target, payload));
    operation.target = target;
    operation.target.partition_guid = QStringLiteral("unexpected-guid");
    addSelfTestCheck(checks,
                     "accepted-request-guid-drift-refused",
                     !acceptedRequestMatches(operation, target, payload));
}

void selfTestExactRangeHash(QJsonArray* checks) {
    QByteArray bytes("abcdef");
    QBuffer input(&bytes);
    const bool opened = input.open(QIODevice::ReadOnly);
    const bool positioned = input.seek(1);
    constexpr uint64_t kSelectedHashBytes = 3;
    const auto digest = hashExactRange(&input, kSelectedHashBytes);
    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(QByteArray("bcd"), QCryptographicHash::Sha256).toHex());
    addSelfTestCheck(checks,
                     "exact-range-hash-positive",
                     opened && positioned && digest && *digest == expected);
    const auto short_read = hashExactRange(&input, static_cast<uint64_t>(bytes.size()));
    addSelfTestCheck(checks, "exact-range-short-read-refused", !short_read);
}

void selfTestWorkerProofIdentity(QJsonArray* checks) {
    Config config{.proof_mode = ProofMode::OfflineRestore};
    const auto report = reportBase(config);
    addSelfTestCheck(checks,
                     "worker-proof-identity-positive",
                     workerProofIdentityMatches(report, config));
    for (const auto& key : {"schema_version", "scenario_id", "proof_mode", "validation_only"}) {
        auto altered = report;
        altered.remove(key);
        addSelfTestCheck(checks,
                         QStringLiteral("worker-proof-missing-%1-refused").arg(key),
                         !workerProofIdentityMatches(altered, config));
    }
}

void selfTestOfflineFixtureScope(QJsonArray* checks) {
    FixtureState fixture;
    fixture.source_partition.partition_number = sak::PartitionNumber{2};
    fixture.source_partition.size_bytes = kMinimumVhdBytes;
    sak::PartitionInfoEx msr;
    msr.partition_number = sak::PartitionNumber{1};
    msr.is_msr = true;
    msr.gpt_type = QStringLiteral("{e3c9e316-0b5c-4db8-817d-f92df00215ae}");
    fixture.source_disk.partitions = {msr, fixture.source_partition};
    addSelfTestCheck(checks,
                     "offline-fixture-scope-positive",
                     offlineFixturePartitionsKnown(fixture));
    const auto valid = fixture.source_disk.partitions;
    fixture.source_disk.partitions.first().is_msr = false;
    addSelfTestCheck(checks,
                     "offline-fixture-unknown-partition-refused",
                     !offlineFixturePartitionsKnown(fixture));
    fixture.source_disk.partitions = valid;
    fixture.source_disk.partitions.first().gpt_type.clear();
    addSelfTestCheck(checks,
                     "offline-fixture-unknown-type-refused",
                     !offlineFixturePartitionsKnown(fixture));
    fixture.source_disk.partitions = valid;
    fixture.source_disk.partitions.first().volume.emplace();
    addSelfTestCheck(checks,
                     "offline-fixture-msr-volume-refused",
                     !offlineFixturePartitionsKnown(fixture));
    fixture.source_disk.partitions = {msr};
    addSelfTestCheck(checks,
                     "offline-fixture-missing-source-refused",
                     !offlineFixturePartitionsKnown(fixture));
    fixture.source_disk.partitions = {fixture.source_partition, fixture.source_partition};
    addSelfTestCheck(checks,
                     "offline-fixture-duplicate-source-refused",
                     !offlineFixturePartitionsKnown(fixture));
}

void selfTestOfflineInventoryMsr(QJsonArray* checks) {
    // Use the real inventory parser: its protected-partition display annotation is
    // not a Windows volume. The original hand-built fixture omitted that annotation.
    const QJsonObject partition{{"PartitionNumber", 1},
                                {"GptType", "{e3c9e316-0b5c-4db8-817d-f92df00215ae}"},
                                {"Volume", QJsonValue::Null}};
    const QJsonObject disk{{"Number", 0}, {"Partitions", QJsonArray{partition}}};
    const auto inventory = sak::StorageInventoryWorker::parseInventoryJson(
        QJsonDocument(disk).toJson(QJsonDocument::Compact));
    FixtureState fixture;
    fixture.source_partition.partition_number = sak::PartitionNumber{2};
    fixture.source_partition.size_bytes = kMinimumVhdBytes;
    fixture.source_disk = inventory.disks.first();
    fixture.source_disk.partitions.append(fixture.source_partition);
    addSelfTestCheck(checks,
                     "offline-fixture-inventory-msr-positive",
                     offlineFixturePartitionsKnown(fixture));
    using Change = std::pair<const char*, std::function<void(sak::PartitionVolumeInfo&)>>;
    const std::array changes{
        Change{"guid", [](auto& v) { v.volume_guid = "unexpected"; }},
        Change{"letter", [](auto& v) { v.drive_letter = "Z"; }},
        Change{"label", [](auto& v) { v.label = "unexpected"; }},
        Change{"filesystem", [](auto& v) { v.file_system = "NTFS"; }},
        Change{"source", [](auto& v) { v.file_system_source = "WindowsVolume"; }},
        Change{"details", [](auto& v) { v.file_system_details.append("unexpected"); }},
        Change{"health", [](auto& v) { v.health_status = "Healthy"; }},
        Change{"total", [](auto& v) { v.total_bytes = 1; }},
        Change{"free", [](auto& v) { v.free_bytes = 1; }},
        Change{"bitlocker-enabled", [](auto& v) { v.bitlocker_enabled = true; }},
        Change{"bitlocker-locked", [](auto& v) { v.bitlocker_locked = true; }},
        Change{"dirty-known", [](auto& v) { v.dirty_bit_known = true; }},
        Change{"dirty-set", [](auto& v) { v.dirty_bit_set = true; }},
        Change{"dirty-error", [](auto& v) { v.dirty_bit_error = "unexpected"; }},
        Change{"bitlocker-known", [](auto& v) { v.bitlocker_state_known = true; }},
        Change{"bitlocker-error", [](auto& v) { v.bitlocker_state_error = "unexpected"; }}};
    const auto valid = fixture.source_disk.partitions;
    for (const auto& [name, change] : changes) {
        fixture.source_disk.partitions = valid;
        change(*fixture.source_disk.partitions.first().volume);
        addSelfTestCheck(checks,
                         QStringLiteral("offline-fixture-advisory-%1-refused").arg(name),
                         !offlineFixturePartitionsKnown(fixture));
    }
    fixture.source_disk.partitions = valid;
    fixture.source_disk.partitions.first().volume_lookup_failed = true;
    addSelfTestCheck(checks,
                     "offline-fixture-msr-lookup-failure-refused",
                     !offlineFixturePartitionsKnown(fixture));
    fixture.source_disk.partitions = valid;
    fixture.source_disk.partitions.first().volume_lookup_error = "unexpected";
    addSelfTestCheck(checks,
                     "offline-fixture-msr-lookup-error-refused",
                     !offlineFixturePartitionsKnown(fixture));
}

void selfTestExecutionFlagSerialization(QJsonArray* checks) {
    const sak::PartitionOperation operation;
    const sak::PartitionQueueReviewToken reviewed;
    const sak::PartitionScript script;
    for (const bool cancelled : {false, true}) {
        for (const bool timed_out : {false, true}) {
            sak::PartitionExecutionResult result;
            result.cancelled = cancelled;
            result.timed_out = timed_out;
            result.report_json = QStringLiteral("{\"schema_version\":1}");
            QJsonObject report{{"operations", QJsonArray{}}};
            const auto recorded = recordExecution(operation, reviewed, script, result, &report);
            const auto decoded = QJsonDocument::fromJson(QJsonDocument(report).toJson()).object();
            const auto operations = decoded.value("operations").toArray();
            const auto row = operations.size() == 1 ? operations.at(0).toObject() : QJsonObject{};
            const auto cancelled_value = row.value("execution_cancelled");
            const auto timed_out_value = row.value("execution_timed_out");
            const QString suffix = QStringLiteral("-%1-%2")
                                       .arg(static_cast<int>(cancelled))
                                       .arg(static_cast<int>(timed_out));
            addSelfTestCheck(checks,
                             "execution-cancelled-boolean" + suffix,
                             recorded && cancelled_value.isBool() &&
                                 cancelled_value.toBool() == cancelled);
            addSelfTestCheck(checks,
                             "execution-timed-out-boolean" + suffix,
                             recorded && timed_out_value.isBool() &&
                                 timed_out_value.toBool() == timed_out);
        }
    }
}

int selfTest() {
    QJsonArray checks;
    selfTestConfigChecks(&checks);
    selfTestDiskChecks(&checks);
    selfTestProofModes(&checks);
    selfTestWorkflowRouting(&checks);
    selfTestBusyRefusal(&checks);
    selfTestImageScope(&checks);
    selfTestAcceptedRequest(&checks);
    selfTestExactRangeHash(&checks);
    selfTestWorkerProofIdentity(&checks);
    selfTestOfflineFixtureScope(&checks);
    selfTestOfflineInventoryMsr(&checks);
    selfTestExecutionFlagSerialization(&checks);
    const auto open_options = attachmentOpenOptions();
    addSelfTestCheck(&checks,
                     "permanent-attachment-access",
                     open_options.access ==
                         (VIRTUAL_DISK_ACCESS_GET_INFO | VIRTUAL_DISK_ACCESS_DETACH));
    addSelfTestCheck(&checks,
                     "attachment-backing-store-read-only",
                     open_options.parameters.Version == OPEN_VIRTUAL_DISK_VERSION_1 &&
                         open_options.parameters.Version1.RWDepth == 0);
    const bool passed = std::ranges::all_of(checks, [](const QJsonValue& value) {
        return value.toObject().value("passed").toBool();
    });
    QTextStream(stdout) << QString::fromUtf8(
                               QJsonDocument(QJsonObject{{"tool", kToolName},
                                                         {"self_test", true},
                                                         {"storage_accessed", false},
                                                         {"status", passed ? "Passed" : "Failed"},
                                                         {"checks", checks}})
                                   .toJson(QJsonDocument::Compact))
                        << Qt::endl;
    return passed ? 0 : kFailureExit;
}


}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() == 2 && arguments.at(1) == "--self-test") {
        return selfTest();
    }
    const auto config = parseConfig(arguments);
    if (!config) {
        QTextStream(stderr) << config.error() << Qt::endl;
        return kFailureExit;
    }
    return config->worker ? runWorker(*config) : supervise(*config, arguments);
}
