// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/partition_file_system_detector.h"
#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_apfs_writer.h"
#include "sak/partition_hfs_file_system_reader.h"
#include "sak/partition_raw_device_io.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIODevice>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr uint64_t kDefaultReadMaxBytes = 1024ULL * 1024ULL;
constexpr uint64_t kDefaultApfsExportMaxEntries = 10000;
constexpr uint64_t kDefaultApfsExportMaxFileBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kDefaultApfsExportMaxTotalBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxIntValue = static_cast<uint64_t>(std::numeric_limits<int>::max());
constexpr qsizetype kFixtureHfsHeaderOffset = 1024;
constexpr qsizetype kFixtureHfsVersionOffset = 2;
constexpr qsizetype kFixtureHfsAttributesOffset = 4;
constexpr qsizetype kFixtureHfsFileCountOffset = 32;
constexpr qsizetype kFixtureHfsFolderCountOffset = 36;
constexpr qsizetype kFixtureHfsBlockSizeOffset = 40;
constexpr qsizetype kFixtureHfsTotalBlocksOffset = 44;
constexpr qsizetype kFixtureHfsFreeBlocksOffset = 48;
constexpr qsizetype kFixtureHfsAllocationForkOffset = 112;
constexpr qsizetype kFixtureHfsCatalogForkOffset = 272;
constexpr qsizetype kFixtureHfsAttributesForkOffset = 352;
constexpr qsizetype kFixtureHfsForkLogicalSizeOffset = 0;
constexpr qsizetype kFixtureHfsForkTotalBlocksOffset = 12;
constexpr qsizetype kFixtureHfsForkExtentsOffset = 16;
constexpr qsizetype kFixtureHfsExtentStartBlockOffset = 0;
constexpr qsizetype kFixtureHfsExtentBlockCountOffset = 4;
constexpr qsizetype kFixtureHfsExtentBytes = 8;
constexpr int kFixtureHfsExtentCount = 8;
constexpr qsizetype kFixtureHfsCatalogFileDataForkOffset = 88;
constexpr qsizetype kFixtureHfsForkDataBytes =
    kFixtureHfsForkExtentsOffset + kFixtureHfsExtentBytes * kFixtureHfsExtentCount;
constexpr qsizetype kFixtureHfsCatalogFileResourceForkOffset =
    kFixtureHfsCatalogFileDataForkOffset + kFixtureHfsForkDataBytes;
constexpr qsizetype kFixtureHfsBTreeKindOffset = 8;
constexpr qsizetype kFixtureHfsBTreeHeightOffset = 9;
constexpr qsizetype kFixtureHfsBTreeNumRecordsOffset = 10;
constexpr qsizetype kFixtureHfsBTreeHeaderRecordOffset = 14;
constexpr qsizetype kFixtureHfsBTreeHeaderTreeDepthOffset = 0;
constexpr qsizetype kFixtureHfsBTreeHeaderRootNodeOffset = 2;
constexpr qsizetype kFixtureHfsBTreeHeaderLeafRecordsOffset = 6;
constexpr qsizetype kFixtureHfsBTreeHeaderFirstLeafNodeOffset = 10;
constexpr qsizetype kFixtureHfsBTreeHeaderLastLeafNodeOffset = 14;
constexpr qsizetype kFixtureHfsBTreeHeaderNodeSizeOffset = 18;
constexpr qsizetype kFixtureHfsBTreeHeaderMaxKeyLengthOffset = 20;
constexpr qsizetype kFixtureHfsBTreeHeaderTotalNodesOffset = 22;
constexpr qsizetype kFixtureHfsBTreeHeaderFreeNodesOffset = 26;
constexpr qsizetype kFixtureHfsBTreeHeaderKeyCompareTypeOffset = 37;
constexpr qsizetype kFixtureHfsBTreeHeaderAttributesOffset = 38;
constexpr uint16_t kFixtureHfsCatalogMaxKeyLength = 516;
constexpr uint32_t kFixtureHfsBTreeBigKeysMask = 0x00000002;
constexpr uint32_t kFixtureHfsBTreeVariableIndexKeysMask = 0x00000004;
constexpr qsizetype kFixtureHfsCatalogRecordIdOffset = 8;
constexpr qsizetype kFixtureHfsFolderRecordBytes = 88;
constexpr qsizetype kFixtureHfsFileRecordBytes = 248;
constexpr qsizetype kFixtureHfsRootRecordOffset = 14;
constexpr qsizetype kFixtureHfsHelloRecordOffset = 120;
constexpr qsizetype kFixtureHfsNoteRecordOffset = 248;
constexpr qsizetype kFixtureHfsHeaderFreeOffset = 256;
constexpr uint32_t kFixtureHfsJournaledMask = 0x00002000;
constexpr uint32_t kFixtureHfsBlockSize = 4096;
constexpr uint32_t kFixtureHfsImageBlockCount = 64;
constexpr uint32_t kFixtureHfsFreeBlockCount = 40;
constexpr uint32_t kFixtureHfsCatalogStartBlock = 2;
constexpr uint32_t kFixtureHfsCatalogNodeSize = 4096;
constexpr uint32_t kFixtureHfsCatalogTotalNodes = 2;
constexpr uint32_t kFixtureHfsHelloFileBlock = 4;
constexpr uint32_t kFixtureHfsNoteFileBlock = 5;
constexpr uint32_t kFixtureHfsAttributesStartBlock = 6;
constexpr uint32_t kFixtureHfsHelloResourceForkBlock = 8;
constexpr uint32_t kFixtureHfsAttributeForkValueBlock = 9;
constexpr uint32_t kFixtureHfsAllocationStartBlock = 60;
constexpr uint32_t kFixtureHfsAttributesNodeSize = 4096;
constexpr uint32_t kFixtureHfsAttributesTotalNodes = 2;
constexpr uint32_t kFixtureHfsRootFolderId = 2;
constexpr uint32_t kFixtureHfsDocsFolderId = 16;
constexpr uint32_t kFixtureHfsHelloFileId = 17;
constexpr uint32_t kFixtureHfsNoteFileId = 18;
constexpr uint16_t kFixtureHfsFolderRecordType = 1;
constexpr uint16_t kFixtureHfsFileRecordType = 2;
constexpr uint32_t kFixtureHfsAttributeInlineRecord = 0x10;
constexpr uint32_t kFixtureHfsAttributeForkRecord = 0x20;
constexpr qsizetype kFixtureHfsAttributeRecordTypeBytes = 4;
constexpr qsizetype kFixtureHfsAttributeInlineReservedBytes = 8;
constexpr qsizetype kFixtureHfsAttributeInlineSizeOffset =
    kFixtureHfsAttributeRecordTypeBytes + kFixtureHfsAttributeInlineReservedBytes;
constexpr qsizetype kFixtureHfsAttributeInlineDataOffset =
    kFixtureHfsAttributeInlineSizeOffset + sizeof(uint32_t);
constexpr qsizetype kFixtureHfsAttributeForkDataOffset =
    kFixtureHfsAttributeRecordTypeBytes + sizeof(uint32_t);
constexpr char kFixtureHfsBTreeHeaderKind = 1;
constexpr char kFixtureHfsBTreeLeafKind = static_cast<char>(0xFF);
constexpr char kFixtureHfsBTreeLeafHeight = 1;
constexpr char kFixtureHfsBTreeKeyCompareType = static_cast<char>(0xCF);
constexpr const char* kFixtureHfsHelloPath = "/hello.txt";
constexpr const char* kFixtureHfsHelloText = "hello from hfs\n";
constexpr const char* kFixtureHfsHelloResourceText = "resource from hfs\n";
constexpr const char* kFixtureHfsNoteText = "nested hfs note\n";
constexpr const char* kFixtureHfsAttributeForkText = "large hfs attribute payload from fork";

struct Config {
    QString inputPath;
    QString outputPath;
    QString expectedFileSystem;
    uint64_t inputOffsetBytes{0};
    bool inputOffsetProvided{false};
    bool inputOffsetInvalid{false};
    uint64_t inputSizeOverride{0};
    bool inputSizeOverrideProvided{false};
    bool requireSane{false};
    QString hfsBuildWriterFixturePath;
    bool hfsCheck{false};
    QString hfsListPath;
    QString hfsReadFilePath;
    QString hfsReadResourceForkPath;
    QString hfsReadAttributeName;
    uint64_t hfsReadAttributeFileId{0};
    bool hfsReadAttributeFileIdProvided{false};
    bool hfsReadAttributeFileIdInvalid{false};
    uint64_t hfsReadMaxBytes{kDefaultReadMaxBytes};
    bool hfsReadMaxBytesProvided{false};
    bool hfsReadMaxBytesInvalid{false};
    QString apfsListPath;
    QString apfsReadFilePath;
    QString apfsExportPath;
    QString apfsExportOutputDirectory;
    QString apfsBuildFormatImagePath;
    QString apfsFormatExistingTargetPath;
    QString apfsWriteRootFileImagePath;
    QString apfsWriteRootFileTargetPath;
    QString apfsWriteRootFileName;
    QString apfsWriteRootFileText;
    QString apfsPatchRootFileImagePath;
    QString apfsPatchRootFileTargetPath;
    QString apfsPatchRootFileName;
    QString apfsPatchRootFileText;
    QString apfsDeleteRootFileImagePath;
    QString apfsDeleteRootFileTargetPath;
    QString apfsDeleteRootFileName;
    QString apfsRepairObjectChecksumsImagePath;
    QString apfsRepairObjectChecksumsTargetPath;
    QString apfsRepairReadFilePath;
    QString apfsFormatVolumeName{QStringLiteral("SAK APFS")};
    QString apfsFormatSeedFileName;
    QString apfsFormatSeedFileText;
    bool apfsFormatTargetWipeConfirmed{false};
    bool apfsFormatAllowRawTarget{false};
    bool apfsFormatRawHardwareProof{false};
    bool apfsWriteTargetConfirmed{false};
    bool apfsWriteAllowRawTarget{false};
    bool apfsWriteRawHardwareProof{false};
    bool apfsRepairTargetConfirmed{false};
    bool apfsRepairAllowRawTarget{false};
    bool apfsRepairRawHardwareProof{false};
    uint64_t apfsRepairCorruptMetadataBlock{0};
    bool apfsRepairCorruptMetadataBlockProvided{false};
    bool apfsRepairCorruptMetadataBlockInvalid{false};
    uint64_t apfsFormatSizeBytes{64ULL * 1024ULL * 1024ULL};
    bool apfsFormatSizeBytesProvided{false};
    bool apfsFormatSizeBytesInvalid{false};
    uint64_t apfsWriteTargetSizeBytes{0};
    bool apfsWriteTargetSizeBytesProvided{false};
    bool apfsWriteTargetSizeBytesInvalid{false};
    uint64_t apfsPatchRootFileOffsetBytes{0};
    bool apfsPatchRootFileOffsetBytesProvided{false};
    bool apfsPatchRootFileOffsetBytesInvalid{false};
    uint64_t apfsRepairTargetSizeBytes{0};
    bool apfsRepairTargetSizeBytesProvided{false};
    bool apfsRepairTargetSizeBytesInvalid{false};
    uint64_t apfsFormatBlockSizeBytes{4096};
    bool apfsFormatBlockSizeBytesProvided{false};
    bool apfsFormatBlockSizeBytesInvalid{false};
    uint64_t apfsReadMaxBytes{kDefaultReadMaxBytes};
    bool apfsReadMaxBytesProvided{false};
    bool apfsReadMaxBytesInvalid{false};
    uint64_t apfsRepairReadMaxBytes{kDefaultReadMaxBytes};
    bool apfsRepairReadMaxBytesProvided{false};
    bool apfsRepairReadMaxBytesInvalid{false};
    uint64_t apfsExportMaxEntries{kDefaultApfsExportMaxEntries};
    bool apfsExportMaxEntriesProvided{false};
    bool apfsExportMaxEntriesInvalid{false};
    uint64_t apfsExportMaxFileBytes{kDefaultApfsExportMaxFileBytes};
    bool apfsExportMaxFileBytesProvided{false};
    bool apfsExportMaxFileBytesInvalid{false};
    uint64_t apfsExportMaxTotalBytes{kDefaultApfsExportMaxTotalBytes};
    bool apfsExportMaxTotalBytesProvided{false};
    bool apfsExportMaxTotalBytesInvalid{false};
};

QString utcNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString argValue(const QStringList& args, const QString& name, const QString& fallback = {}) {
    const int index = args.indexOf(name);
    if (index < 0 || index + 1 >= args.size()) {
        return fallback;
    }
    return args.at(index + 1);
}

bool hasSwitch(const QStringList& args, const QString& name) {
    return args.contains(name);
}

std::optional<uint64_t> argUInt64(const QStringList& args, const QString& name) {
    const QString value = argValue(args, name);
    if (value.trimmed().isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const auto parsed = value.toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(parsed);
}

QJsonArray stringArray(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
}

QJsonArray blockersArray(const QStringList& blockers, const QStringList& warnings) {
    QJsonArray array = stringArray(blockers);
    for (const auto& warning : warnings) {
        array.append(QStringLiteral("Warning: %1").arg(warning));
    }
    return array;
}

QJsonObject hfsEntryObject(const sak::PartitionHfsFileEntry& entry) {
    return QJsonObject{{QStringLiteral("path"), entry.path},
                       {QStringLiteral("name"), entry.name},
                       {QStringLiteral("type"), entry.type},
                       {QStringLiteral("catalog_id"),
                        QString::number(entry.catalog_id)},
                       {QStringLiteral("size_bytes"),
                        QString::number(entry.size_bytes)},
                       {QStringLiteral("resource_fork_size_bytes"),
                        QString::number(entry.resource_fork_size_bytes)},
                       {QStringLiteral("directory"), entry.directory},
                       {QStringLiteral("regular_file"), entry.regular_file}};
}

QJsonObject apfsEntryObject(const sak::PartitionApfsFileEntry& entry) {
    return QJsonObject{{QStringLiteral("path"), entry.path},
                       {QStringLiteral("name"), entry.name},
                       {QStringLiteral("type"), entry.type},
                       {QStringLiteral("object_id"), QString::number(entry.object_id)},
                       {QStringLiteral("size_bytes"), QString::number(entry.size_bytes)},
                       {QStringLiteral("directory"), entry.directory},
                       {QStringLiteral("regular_file"), entry.regular_file},
                       {QStringLiteral("symlink"), entry.symlink}};
}

QJsonArray hfsEntriesArray(const QVector<sak::PartitionHfsFileEntry>& entries) {
    QJsonArray array;
    for (const auto& entry : entries) {
        array.append(hfsEntryObject(entry));
    }
    return array;
}

QJsonObject hfsAttributeMetadataObject(const sak::PartitionHfsAttributeMetadata& record) {
    return QJsonObject{{QStringLiteral("file_id"), QString::number(record.file_id)},
                       {QStringLiteral("start_block"), QString::number(record.start_block)},
                       {QStringLiteral("name"), record.name},
                       {QStringLiteral("storage"), record.storage},
                       {QStringLiteral("size_bytes"), QString::number(record.size_bytes)},
                       {QStringLiteral("extent_count"), record.extent_count},
                       {QStringLiteral("readable"), record.readable}};
}

QJsonArray hfsAttributeMetadataArray(
    const QVector<sak::PartitionHfsAttributeMetadata>& records) {
    QJsonArray array;
    for (const auto& record : records) {
        array.append(hfsAttributeMetadataObject(record));
    }
    return array;
}

QJsonArray apfsEntriesArray(const QVector<sak::PartitionApfsFileEntry>& entries) {
    QJsonArray array;
    for (const auto& entry : entries) {
        array.append(apfsEntryObject(entry));
    }
    return array;
}

void setErrorMessage(QString* errorMessage, const QString& message) {
    if (errorMessage) {
        *errorMessage = message;
    }
}

uint64_t remainingSizeAfterOffset(uint64_t fullSize, uint64_t offsetBytes) {
    return offsetBytes < fullSize ? fullSize - offsetBytes : 0ULL;
}

#ifdef Q_OS_WIN
class ScopedWin32Handle {
public:
    explicit ScopedWin32Handle(HANDLE handle) : handle_(handle) {}
    ~ScopedWin32Handle() {
        if (isValid()) {
            CloseHandle(handle_);
        }
    }

    ScopedWin32Handle(const ScopedWin32Handle&) = delete;
    ScopedWin32Handle& operator=(const ScopedWin32Handle&) = delete;

    HANDLE get() const { return handle_; }
    bool isValid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

QString win32ErrorMessage(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags,
                                        nullptr,
                                        errorCode,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer),
                                        0,
                                        nullptr);
    QString message;
    if (length > 0 && buffer != nullptr) {
        message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    if (message.isEmpty()) {
        message = QStringLiteral("Win32 error %1").arg(errorCode);
    }
    return message;
}

bool isWindowsDevicePath(const QString& path) {
    return path.startsWith(QStringLiteral("\\\\.\\")) ||
           path.startsWith(QStringLiteral("\\\\?\\"));
}

ScopedWin32Handle openWindowsReadHandle(const QString& path) {
    return ScopedWin32Handle(CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                         GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr));
}

void updateWindowsInputSize(HANDLE handle, uint64_t inputOffsetBytes, uint64_t* inputSizeBytes) {
    if (!inputSizeBytes) {
        return;
    }

    LARGE_INTEGER fileSize{};
    if (GetFileSizeEx(handle, &fileSize) && fileSize.QuadPart > 0) {
        const auto fullSize = static_cast<uint64_t>(fileSize.QuadPart);
        *inputSizeBytes = remainingSizeAfterOffset(fullSize, inputOffsetBytes);
    }
}

bool seekWindowsHandle(HANDLE handle, uint64_t inputOffsetBytes, QString* errorMessage) {
    if (inputOffsetBytes == 0) {
        return true;
    }

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(inputOffsetBytes);
    if (SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN)) {
        return true;
    }

    setErrorMessage(errorMessage,
                    QStringLiteral("Seek failed: %1").arg(win32ErrorMessage(GetLastError())));
    return false;
}

std::optional<QByteArray> readWindowsProbeBytesFromHandle(HANDLE handle, QString* errorMessage) {
    QByteArray bytes;
    const auto readLimit = sak::PartitionFileSystemDetector::maxProbeBytes();
    const auto clampedReadLimit =
        std::min<uint64_t>(readLimit, static_cast<uint64_t>(std::numeric_limits<DWORD>::max()));
    bytes.resize(static_cast<int>(clampedReadLimit));

    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(handle,
                             bytes.data(),
                             static_cast<DWORD>(bytes.size()),
                             &bytesRead,
                             nullptr);
    const DWORD readError = GetLastError();
    if (!ok) {
        setErrorMessage(errorMessage,
                        QStringLiteral("Read failed: %1").arg(win32ErrorMessage(readError)));
        return std::nullopt;
    }

    bytes.resize(static_cast<int>(bytesRead));
    return bytes;
}

std::optional<QByteArray> readWindowsDeviceProbeBytes(const QString& path,
                                                      uint64_t inputOffsetBytes,
                                                      uint64_t* inputSizeBytes,
                                                      QString* errorMessage) {
    if (!isWindowsDevicePath(path)) {
        return std::nullopt;
    }

    const ScopedWin32Handle handle = openWindowsReadHandle(path);
    if (!handle.isValid()) {
        setErrorMessage(errorMessage,
                        QStringLiteral("Open failed: %1").arg(win32ErrorMessage(GetLastError())));
        return std::nullopt;
    }

    updateWindowsInputSize(handle.get(), inputOffsetBytes, inputSizeBytes);
    if (!seekWindowsHandle(handle.get(), inputOffsetBytes, errorMessage)) {
        return std::nullopt;
    }
    return readWindowsProbeBytesFromHandle(handle.get(), errorMessage);
}
#endif

QString reportInputPath(const QString& path) {
#ifdef Q_OS_WIN
    if (isWindowsDevicePath(path)) {
        return path;
    }
#endif
    return QFileInfo(path).absoluteFilePath();
}

bool writeJsonFile(const QString& path, const QJsonObject& object) {
    if (path.trimmed().isEmpty()) {
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

bool writeBinaryFile(const QString& path, const QByteArray& bytes) {
    if (path.trimmed().isEmpty()) {
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

void writeAscii(QByteArray* bytes, qsizetype offset, const char* value) {
    Q_ASSERT(bytes);
    const QByteArray text(value);
    for (qsizetype index = 0; index < text.size(); ++index) {
        (*bytes)[offset + index] = text.at(index);
    }
}

void writeBe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>(value & 0xFF);
}

void writeBe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>(value & 0xFF);
}

void writeBe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 56) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 48) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 40) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 32) & 0xFF);
    (*bytes)[offset + 4] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 5] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 6] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 7] = static_cast<char>(value & 0xFF);
}

void writeHfsExtent(QByteArray* bytes,
                    qsizetype offset,
                    uint32_t startBlock,
                    uint32_t blockCount) {
    writeBe32(bytes, offset + kFixtureHfsExtentStartBlockOffset, startBlock);
    writeBe32(bytes, offset + kFixtureHfsExtentBlockCountOffset, blockCount);
}

void writeHfsFork(QByteArray* bytes,
                  qsizetype offset,
                  uint64_t logicalSize,
                  uint32_t totalBlocks,
                  uint32_t firstBlock) {
    writeBe64(bytes, offset + kFixtureHfsForkLogicalSizeOffset, logicalSize);
    writeBe32(bytes, offset + kFixtureHfsForkTotalBlocksOffset, totalBlocks);
    writeHfsExtent(bytes, offset + kFixtureHfsForkExtentsOffset, firstBlock, totalBlocks);
}

void setHfsAllocationBit(QByteArray* bytes, uint32_t block) {
    const qsizetype offset = static_cast<qsizetype>(
        kFixtureHfsAllocationStartBlock * kFixtureHfsBlockSize + block / 8U);
    const char mask = static_cast<char>(0x80U >> (block % 8U));
    (*bytes)[offset] = static_cast<char>((*bytes)[offset] | mask);
}

void writeHfsAllocationFork(QByteArray* image, const QVector<uint32_t>& allocatedBlocks) {
    writeHfsFork(image,
                 kFixtureHfsHeaderOffset + kFixtureHfsAllocationForkOffset,
                 kFixtureHfsBlockSize,
                 1,
                 kFixtureHfsAllocationStartBlock);
    for (uint32_t block : allocatedBlocks) {
        setHfsAllocationBit(image, block);
    }
}

QByteArray hfsCatalogKey(uint32_t parentId, const QString& name) {
    const uint16_t keyLength = static_cast<uint16_t>(
        sizeof(uint32_t) + sizeof(uint16_t) + name.size() * sizeof(uint16_t));
    QByteArray key(static_cast<qsizetype>(sizeof(uint16_t) + keyLength), '\0');
    writeBe16(&key, 0, keyLength);
    writeBe32(&key, sizeof(uint16_t), parentId);
    writeBe16(&key, sizeof(uint16_t) + sizeof(uint32_t),
              static_cast<uint16_t>(name.size()));
    for (qsizetype index = 0; index < name.size(); ++index) {
        writeBe16(&key,
                  sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint16_t) +
                      index * sizeof(uint16_t),
                  name.at(index).unicode());
    }
    return key;
}

QByteArray hfsFolderRecord(uint32_t folderId) {
    QByteArray record(kFixtureHfsFolderRecordBytes, '\0');
    writeBe16(&record, 0, kFixtureHfsFolderRecordType);
    writeBe32(&record, kFixtureHfsCatalogRecordIdOffset, folderId);
    return record;
}

QByteArray hfsFileRecord(uint32_t fileId,
                         const QByteArray& data,
                         uint32_t firstBlock,
                         const QByteArray& resource = {},
                         uint32_t resourceFirstBlock = 0) {
    QByteArray record(kFixtureHfsFileRecordBytes, '\0');
    writeBe16(&record, 0, kFixtureHfsFileRecordType);
    writeBe32(&record, kFixtureHfsCatalogRecordIdOffset, fileId);
    writeHfsFork(&record,
                 kFixtureHfsCatalogFileDataForkOffset,
                 static_cast<uint64_t>(data.size()),
                 1,
                 firstBlock);
    if (!resource.isEmpty() && resourceFirstBlock != 0) {
        writeHfsFork(&record,
                     kFixtureHfsCatalogFileResourceForkOffset,
                     static_cast<uint64_t>(resource.size()),
                     1,
                     resourceFirstBlock);
    }
    return record;
}

QByteArray hfsCatalogRecord(uint32_t parentId, const QString& name, const QByteArray& data) {
    QByteArray record = hfsCatalogKey(parentId, name);
    record.append(data);
    if ((record.size() % 2) != 0) {
        record.append('\0');
    }
    return record;
}

QByteArray hfsAttributeKey(uint32_t fileId, const QString& name) {
    const uint16_t keyLength = static_cast<uint16_t>(
        12 + name.size() * sizeof(uint16_t));
    QByteArray key(static_cast<qsizetype>(sizeof(uint16_t) + keyLength), '\0');
    writeBe16(&key, 0, keyLength);
    writeBe32(&key, 4, fileId);
    writeBe32(&key, 8, 0);
    writeBe16(&key, 12,
              static_cast<uint16_t>(name.size()));
    for (qsizetype index = 0; index < name.size(); ++index) {
        writeBe16(&key, 14 + index * sizeof(uint16_t),
                  name.at(index).unicode());
    }
    return key;
}

QByteArray hfsInlineAttributeRecord(uint32_t fileId,
                                    const QString& name,
                                    const QByteArray& value) {
    QByteArray record = hfsAttributeKey(fileId, name);
    const qsizetype dataOffset = record.size();
    record.resize(record.size() + kFixtureHfsAttributeInlineDataOffset + value.size());
    writeBe32(&record, dataOffset, kFixtureHfsAttributeInlineRecord);
    writeBe32(&record,
              dataOffset + kFixtureHfsAttributeInlineSizeOffset,
              static_cast<uint32_t>(value.size()));
    std::copy(value.cbegin(),
              value.cend(),
              record.begin() + dataOffset + kFixtureHfsAttributeInlineDataOffset);
    if ((record.size() % 2) != 0) {
        record.append('\0');
    }
    return record;
}

QByteArray hfsForkAttributeRecord(uint32_t fileId,
                                  const QString& name,
                                  const QByteArray& value,
                                  uint32_t firstBlock) {
    QByteArray record = hfsAttributeKey(fileId, name);
    const qsizetype dataOffset = record.size();
    record.resize(record.size() + kFixtureHfsAttributeForkDataOffset +
                  kFixtureHfsForkDataBytes);
    writeBe32(&record, dataOffset, kFixtureHfsAttributeForkRecord);
    writeHfsFork(&record,
                 dataOffset + kFixtureHfsAttributeForkDataOffset,
                 static_cast<uint64_t>(value.size()),
                 1,
                 firstBlock);
    if ((record.size() % 2) != 0) {
        record.append('\0');
    }
    return record;
}

void writeHfsNodeOffsets(QByteArray* node,
                         const QVector<qsizetype>& offsets,
                         qsizetype freeOffset) {
    for (int index = 0; index < offsets.size(); ++index) {
        writeBe16(node,
                  node->size() - ((index + 1) * static_cast<int>(sizeof(uint16_t))),
                  static_cast<uint16_t>(offsets.at(index)));
    }
    writeBe16(node,
              node->size() - ((offsets.size() + 1) * static_cast<int>(sizeof(uint16_t))),
              static_cast<uint16_t>(freeOffset));
}

void writeHfsCatalogHeaderNode(QByteArray* image) {
    const qsizetype nodeOffset =
        static_cast<qsizetype>(kFixtureHfsCatalogStartBlock * kFixtureHfsBlockSize);
    QByteArray node(kFixtureHfsCatalogNodeSize, '\0');
    node[kFixtureHfsBTreeKindOffset] = kFixtureHfsBTreeHeaderKind;
    writeBe16(&node, kFixtureHfsBTreeNumRecordsOffset, 3);

    const qsizetype header = kFixtureHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kFixtureHfsBTreeHeaderTreeDepthOffset, 1);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderRootNodeOffset, 1);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderLeafRecordsOffset, 3);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderLastLeafNodeOffset, 1);
    writeBe16(&node,
              header + kFixtureHfsBTreeHeaderNodeSizeOffset,
              kFixtureHfsCatalogNodeSize);
    writeBe16(&node,
              header + kFixtureHfsBTreeHeaderMaxKeyLengthOffset,
              kFixtureHfsCatalogMaxKeyLength);
    writeBe32(&node,
              header + kFixtureHfsBTreeHeaderTotalNodesOffset,
              kFixtureHfsCatalogTotalNodes);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderFreeNodesOffset, 0);
    node[header + kFixtureHfsBTreeHeaderKeyCompareTypeOffset] =
        kFixtureHfsBTreeKeyCompareType;
    writeBe32(&node,
              header + kFixtureHfsBTreeHeaderAttributesOffset,
              kFixtureHfsBTreeBigKeysMask | kFixtureHfsBTreeVariableIndexKeysMask);
    writeHfsNodeOffsets(&node,
                        {kFixtureHfsRootRecordOffset,
                         kFixtureHfsHelloRecordOffset,
                         kFixtureHfsNoteRecordOffset},
                        kFixtureHfsHeaderFreeOffset);
    // Mark the header node and the root leaf as allocated in the map record so
    // the catalog tree-mutation engine accepts the fixture.
    node[kFixtureHfsNoteRecordOffset] = static_cast<char>(0xC0);

    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

void writeHfsCatalogLeafNode(QByteArray* image) {
    const QByteArray hello(kFixtureHfsHelloText);
    const QByteArray helloResource(kFixtureHfsHelloResourceText);
    const QByteArray note(kFixtureHfsNoteText);
    const QVector<QByteArray> records{
        hfsCatalogRecord(kFixtureHfsRootFolderId,
                         QStringLiteral("Docs"),
                         hfsFolderRecord(kFixtureHfsDocsFolderId)),
        hfsCatalogRecord(kFixtureHfsRootFolderId,
                         QStringLiteral("hello.txt"),
                         hfsFileRecord(kFixtureHfsHelloFileId,
                                       hello,
                                       kFixtureHfsHelloFileBlock,
                                       helloResource,
                                       kFixtureHfsHelloResourceForkBlock)),
        hfsCatalogRecord(kFixtureHfsDocsFolderId,
                         QStringLiteral("note.txt"),
                         hfsFileRecord(kFixtureHfsNoteFileId,
                                       note,
                                       kFixtureHfsNoteFileBlock))};

    const qsizetype nodeOffset = static_cast<qsizetype>(
        (kFixtureHfsCatalogStartBlock + 1) * kFixtureHfsBlockSize);
    QByteArray node(kFixtureHfsCatalogNodeSize, '\0');
    node[kFixtureHfsBTreeKindOffset] = kFixtureHfsBTreeLeafKind;
    node[kFixtureHfsBTreeHeightOffset] = kFixtureHfsBTreeLeafHeight;
    writeBe16(&node, kFixtureHfsBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));

    QVector<qsizetype> offsets;
    qsizetype cursor = kFixtureHfsBTreeHeaderRecordOffset;
    for (const auto& record : records) {
        offsets.append(cursor);
        std::copy(record.cbegin(), record.cend(), node.begin() + cursor);
        cursor += record.size();
    }
    writeHfsNodeOffsets(&node, offsets, cursor);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
    std::copy(hello.cbegin(),
              hello.cend(),
              image->begin() +
                  static_cast<qsizetype>(kFixtureHfsHelloFileBlock * kFixtureHfsBlockSize));
    std::copy(note.cbegin(),
              note.cend(),
              image->begin() +
                  static_cast<qsizetype>(kFixtureHfsNoteFileBlock * kFixtureHfsBlockSize));
    std::copy(helloResource.cbegin(),
              helloResource.cend(),
              image->begin() +
                  static_cast<qsizetype>(kFixtureHfsHelloResourceForkBlock *
                                         kFixtureHfsBlockSize));
}

void writeHfsAttributesHeaderNode(QByteArray* image) {
    const qsizetype nodeOffset =
        static_cast<qsizetype>(kFixtureHfsAttributesStartBlock * kFixtureHfsBlockSize);
    QByteArray node(kFixtureHfsAttributesNodeSize, '\0');
    node[kFixtureHfsBTreeKindOffset] = kFixtureHfsBTreeHeaderKind;
    writeBe16(&node, kFixtureHfsBTreeNumRecordsOffset, 3);

    const qsizetype header = kFixtureHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kFixtureHfsBTreeHeaderTreeDepthOffset, 1);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderRootNodeOffset, 1);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderLeafRecordsOffset, 2);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&node, header + kFixtureHfsBTreeHeaderLastLeafNodeOffset, 1);
    writeBe16(&node,
              header + kFixtureHfsBTreeHeaderNodeSizeOffset,
              kFixtureHfsAttributesNodeSize);
    writeBe32(&node,
              header + kFixtureHfsBTreeHeaderTotalNodesOffset,
              kFixtureHfsAttributesTotalNodes);
    node[header + kFixtureHfsBTreeHeaderKeyCompareTypeOffset] =
        kFixtureHfsBTreeKeyCompareType;
    writeHfsNodeOffsets(&node, {kFixtureHfsRootRecordOffset}, kFixtureHfsHelloRecordOffset);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

void writeHfsAttributesLeafNode(QByteArray* image) {
    QByteArray finderInfo("finder-info-attribute");
    finderInfo.resize(32, '\0');
    const QByteArray forkValue(kFixtureHfsAttributeForkText);
    const QVector<QByteArray> records{
        hfsInlineAttributeRecord(kFixtureHfsHelloFileId,
                                 QStringLiteral("com.apple.FinderInfo"),
                                 finderInfo),
        hfsForkAttributeRecord(kFixtureHfsHelloFileId,
                               QStringLiteral("com.apple.ResourceFork"),
                               forkValue,
                               kFixtureHfsAttributeForkValueBlock)};

    const qsizetype nodeOffset = static_cast<qsizetype>(
        (kFixtureHfsAttributesStartBlock + 1) * kFixtureHfsBlockSize);
    QByteArray node(kFixtureHfsAttributesNodeSize, '\0');
    node[kFixtureHfsBTreeKindOffset] = kFixtureHfsBTreeLeafKind;
    node[kFixtureHfsBTreeHeightOffset] = kFixtureHfsBTreeLeafHeight;
    writeBe16(&node, kFixtureHfsBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));

    QVector<qsizetype> offsets;
    qsizetype cursor = kFixtureHfsBTreeHeaderRecordOffset;
    for (const auto& record : records) {
        offsets.append(cursor);
        std::copy(record.cbegin(), record.cend(), node.begin() + cursor);
        cursor += record.size();
    }
    writeHfsNodeOffsets(&node, offsets, cursor);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
    std::copy(forkValue.cbegin(),
              forkValue.cend(),
              image->begin() +
                  static_cast<qsizetype>(kFixtureHfsAttributeForkValueBlock *
                                         kFixtureHfsBlockSize));
}

QByteArray hfsWriterFixture() {
    QByteArray image(static_cast<qsizetype>(kFixtureHfsImageBlockCount * kFixtureHfsBlockSize),
                     '\0');
    writeAscii(&image, kFixtureHfsHeaderOffset, "H+");
    writeBe16(&image, kFixtureHfsHeaderOffset + kFixtureHfsVersionOffset, 4);
    writeBe32(&image,
              kFixtureHfsHeaderOffset + kFixtureHfsAttributesOffset,
              kFixtureHfsJournaledMask);
    writeBe32(&image, kFixtureHfsHeaderOffset + kFixtureHfsFileCountOffset, 2);
    writeBe32(&image, kFixtureHfsHeaderOffset + kFixtureHfsFolderCountOffset, 1);
    writeBe32(&image,
              kFixtureHfsHeaderOffset + kFixtureHfsBlockSizeOffset,
              kFixtureHfsBlockSize);
    writeBe32(&image,
              kFixtureHfsHeaderOffset + kFixtureHfsTotalBlocksOffset,
              kFixtureHfsImageBlockCount);
    writeBe32(&image,
              kFixtureHfsHeaderOffset + kFixtureHfsFreeBlocksOffset,
              kFixtureHfsFreeBlockCount);
    writeHfsAllocationFork(&image,
                           {kFixtureHfsCatalogStartBlock,
                            kFixtureHfsCatalogStartBlock + 1,
                            kFixtureHfsHelloFileBlock,
                            kFixtureHfsNoteFileBlock,
                            kFixtureHfsAttributesStartBlock,
                            kFixtureHfsAttributesStartBlock + 1,
                            kFixtureHfsHelloResourceForkBlock,
                            kFixtureHfsAttributeForkValueBlock,
                            kFixtureHfsAllocationStartBlock});
    writeHfsFork(&image,
                 kFixtureHfsHeaderOffset + kFixtureHfsCatalogForkOffset,
                 kFixtureHfsCatalogNodeSize * kFixtureHfsCatalogTotalNodes,
                 kFixtureHfsCatalogTotalNodes,
                 kFixtureHfsCatalogStartBlock);
    writeHfsFork(&image,
                 kFixtureHfsHeaderOffset + kFixtureHfsAttributesForkOffset,
                 kFixtureHfsAttributesNodeSize * kFixtureHfsAttributesTotalNodes,
                 kFixtureHfsAttributesTotalNodes,
                 kFixtureHfsAttributesStartBlock);
    writeHfsCatalogHeaderNode(&image);
    writeHfsCatalogLeafNode(&image);
    writeHfsAttributesHeaderNode(&image);
    writeHfsAttributesLeafNode(&image);
    return image;
}

bool openFileReadOnly(QFile* file, QString* errorMessage) {
    if (file->open(QIODevice::ReadOnly)) {
        return true;
    }

    setErrorMessage(errorMessage, QStringLiteral("Open failed: %1").arg(file->errorString()));
    return false;
}

void updateFileInputSize(const QString& path, uint64_t inputOffsetBytes, uint64_t* inputSizeBytes) {
    if (!inputSizeBytes) {
        return;
    }

    const QFileInfo inputInfo(path);
    const auto fullSize = inputInfo.size() > 0 ? static_cast<uint64_t>(inputInfo.size()) : 0ULL;
    *inputSizeBytes = remainingSizeAfterOffset(fullSize, inputOffsetBytes);
}

bool seekFileInput(QFile* file, uint64_t inputOffsetBytes, QString* errorMessage) {
    if (inputOffsetBytes == 0 || file->seek(static_cast<qint64>(inputOffsetBytes))) {
        return true;
    }

    setErrorMessage(errorMessage, QStringLiteral("Seek failed: %1").arg(file->errorString()));
    return false;
}

std::optional<QByteArray> readFileProbeBytes(QFile* file, QString* errorMessage) {
    const QByteArray bytes = file->read(sak::PartitionFileSystemDetector::maxProbeBytes());
    if (bytes.isEmpty() && file->error() != QFileDevice::NoError) {
        setErrorMessage(errorMessage, QStringLiteral("Read failed: %1").arg(file->errorString()));
        return std::nullopt;
    }
    return bytes;
}

std::optional<QByteArray> readProbeBytes(const QString& path,
                                         uint64_t inputOffsetBytes,
                                         uint64_t* inputSizeBytes,
                                         QString* errorMessage) {
#ifdef Q_OS_WIN
    if (isWindowsDevicePath(path)) {
        return readWindowsDeviceProbeBytes(path, inputOffsetBytes, inputSizeBytes, errorMessage);
    }
#endif

    QFile file(path);
    if (!openFileReadOnly(&file, errorMessage)) {
        return std::nullopt;
    }
    updateFileInputSize(path, inputOffsetBytes, inputSizeBytes);
    if (!seekFileInput(&file, inputOffsetBytes, errorMessage)) {
        return std::nullopt;
    }
    return readFileProbeBytes(&file, errorMessage);
}

bool hasSanityWarning(const QStringList& details) {
    for (const auto& detail : details) {
        if (detail.startsWith(QStringLiteral("Metadata sanity warning:"))) {
            return true;
        }
    }
    return false;
}

QJsonObject baseReport(const Config& config) {
    return QJsonObject{{QStringLiteral("schema_version"), 1},
                       {QStringLiteral("tool"),
                        QStringLiteral("partition-filesystem-probe-certifier")},
                       {QStringLiteral("created_utc"), utcNow()},
                       {QStringLiteral("input_path"), reportInputPath(config.inputPath)},
                       {QStringLiteral("input_offset_bytes"),
                        QString::number(config.inputOffsetBytes)},
                       {QStringLiteral("expected_file_system"), config.expectedFileSystem},
                       {QStringLiteral("require_sane_metadata"), config.requireSane}};
}

Config parseBaseConfig(const QStringList& args) {
    Config config;
    config.inputPath = argValue(args, QStringLiteral("--input"));
    config.outputPath = argValue(args, QStringLiteral("--output"));
    config.expectedFileSystem = argValue(args, QStringLiteral("--expect"));
    config.requireSane = hasSwitch(args, QStringLiteral("--require-sane"));
    const QString inputOffsetValue = argValue(args, QStringLiteral("--input-offset-bytes"));
    config.inputOffsetProvided = !inputOffsetValue.trimmed().isEmpty();
    const auto inputOffset = argUInt64(args, QStringLiteral("--input-offset-bytes"));
    config.inputOffsetInvalid = config.inputOffsetProvided && !inputOffset.has_value();
    config.inputOffsetBytes = inputOffset.value_or(0);
    const QString inputSizeValue = argValue(args, QStringLiteral("--input-size-bytes"));
    config.inputSizeOverrideProvided = !inputSizeValue.trimmed().isEmpty();
    config.inputSizeOverride = argUInt64(args, QStringLiteral("--input-size-bytes")).value_or(0);
    return config;
}

void parseHfsConfig(const QStringList& args, Config* config) {
    config->hfsBuildWriterFixturePath =
        argValue(args, QStringLiteral("--hfs-build-writer-fixture"));
    config->hfsCheck = hasSwitch(args, QStringLiteral("--hfs-check"));
    config->hfsListPath = argValue(args, QStringLiteral("--hfs-list-path"));
    config->hfsReadFilePath = argValue(args, QStringLiteral("--hfs-read-file"));
    config->hfsReadResourceForkPath = argValue(args, QStringLiteral("--hfs-read-resource-fork"));
    config->hfsReadAttributeName = argValue(args, QStringLiteral("--hfs-read-attribute-name"));
    const QString hfsReadAttributeFileIdValue =
        argValue(args, QStringLiteral("--hfs-read-attribute-file-id"));
    config->hfsReadAttributeFileIdProvided =
        !hfsReadAttributeFileIdValue.trimmed().isEmpty();
    const auto hfsReadAttributeFileId =
        argUInt64(args, QStringLiteral("--hfs-read-attribute-file-id"));
    config->hfsReadAttributeFileIdInvalid =
        config->hfsReadAttributeFileIdProvided && !hfsReadAttributeFileId.has_value();
    config->hfsReadAttributeFileId =
        hfsReadAttributeFileId.value_or(config->hfsReadAttributeFileId);
    const QString hfsReadMaxBytesValue = argValue(args, QStringLiteral("--hfs-read-max-bytes"));
    config->hfsReadMaxBytesProvided = !hfsReadMaxBytesValue.trimmed().isEmpty();
    const auto hfsReadMaxBytes = argUInt64(args, QStringLiteral("--hfs-read-max-bytes"));
    config->hfsReadMaxBytesInvalid =
        config->hfsReadMaxBytesProvided && !hfsReadMaxBytes.has_value();
    config->hfsReadMaxBytes = hfsReadMaxBytes.value_or(config->hfsReadMaxBytes);
}

void parseApfsReadConfig(const QStringList& args, Config* config) {
    config->apfsListPath = argValue(args, QStringLiteral("--apfs-list-path"));
    config->apfsReadFilePath = argValue(args, QStringLiteral("--apfs-read-file"));
    const QString apfsReadMaxBytesValue = argValue(args, QStringLiteral("--apfs-read-max-bytes"));
    config->apfsReadMaxBytesProvided = !apfsReadMaxBytesValue.trimmed().isEmpty();
    const auto apfsReadMaxBytes = argUInt64(args, QStringLiteral("--apfs-read-max-bytes"));
    config->apfsReadMaxBytesInvalid =
        config->apfsReadMaxBytesProvided && !apfsReadMaxBytes.has_value();
    config->apfsReadMaxBytes = apfsReadMaxBytes.value_or(config->apfsReadMaxBytes);
}

void parseApfsExportConfig(const QStringList& args, Config* config) {
    config->apfsExportPath = argValue(args, QStringLiteral("--apfs-export-path"));
    config->apfsExportOutputDirectory =
        argValue(args, QStringLiteral("--apfs-export-output"));
    const QString apfsExportMaxEntriesValue =
        argValue(args, QStringLiteral("--apfs-export-max-entries"));
    config->apfsExportMaxEntriesProvided = !apfsExportMaxEntriesValue.trimmed().isEmpty();
    const auto apfsExportMaxEntries =
        argUInt64(args, QStringLiteral("--apfs-export-max-entries"));
    config->apfsExportMaxEntriesInvalid =
        config->apfsExportMaxEntriesProvided && !apfsExportMaxEntries.has_value();
    config->apfsExportMaxEntries =
        apfsExportMaxEntries.value_or(config->apfsExportMaxEntries);
    const QString apfsExportMaxFileBytesValue =
        argValue(args, QStringLiteral("--apfs-export-max-file-bytes"));
    config->apfsExportMaxFileBytesProvided =
        !apfsExportMaxFileBytesValue.trimmed().isEmpty();
    const auto apfsExportMaxFileBytes =
        argUInt64(args, QStringLiteral("--apfs-export-max-file-bytes"));
    config->apfsExportMaxFileBytesInvalid =
        config->apfsExportMaxFileBytesProvided && !apfsExportMaxFileBytes.has_value();
    config->apfsExportMaxFileBytes =
        apfsExportMaxFileBytes.value_or(config->apfsExportMaxFileBytes);
    const QString apfsExportMaxTotalBytesValue =
        argValue(args, QStringLiteral("--apfs-export-max-total-bytes"));
    config->apfsExportMaxTotalBytesProvided =
        !apfsExportMaxTotalBytesValue.trimmed().isEmpty();
    const auto apfsExportMaxTotalBytes =
        argUInt64(args, QStringLiteral("--apfs-export-max-total-bytes"));
    config->apfsExportMaxTotalBytesInvalid =
        config->apfsExportMaxTotalBytesProvided && !apfsExportMaxTotalBytes.has_value();
    config->apfsExportMaxTotalBytes =
        apfsExportMaxTotalBytes.value_or(config->apfsExportMaxTotalBytes);
}

void parseApfsFormatConfig(const QStringList& args, Config* config) {
    config->apfsBuildFormatImagePath =
        argValue(args, QStringLiteral("--apfs-build-format-image"));
    config->apfsFormatExistingTargetPath =
        argValue(args, QStringLiteral("--apfs-format-existing-target"));
    config->apfsFormatVolumeName =
        argValue(args, QStringLiteral("--apfs-format-volume-name"), config->apfsFormatVolumeName);
    config->apfsFormatSeedFileName =
        argValue(args, QStringLiteral("--apfs-format-seed-file-name"));
    config->apfsFormatSeedFileText =
        argValue(args, QStringLiteral("--apfs-format-seed-file-text"));
    config->apfsFormatTargetWipeConfirmed =
        hasSwitch(args, QStringLiteral("--apfs-format-target-wipe-confirmed"));
    config->apfsFormatAllowRawTarget =
        hasSwitch(args, QStringLiteral("--apfs-format-allow-raw-target"));
    config->apfsFormatRawHardwareProof =
        hasSwitch(args, QStringLiteral("--apfs-format-raw-hardware-proof"));
    const QString sizeValue = argValue(args, QStringLiteral("--apfs-format-size-bytes"));
    config->apfsFormatSizeBytesProvided = !sizeValue.trimmed().isEmpty();
    const auto sizeBytes = argUInt64(args, QStringLiteral("--apfs-format-size-bytes"));
    config->apfsFormatSizeBytesInvalid =
        config->apfsFormatSizeBytesProvided && !sizeBytes.has_value();
    config->apfsFormatSizeBytes = sizeBytes.value_or(config->apfsFormatSizeBytes);
    const QString blockSizeValue = argValue(args, QStringLiteral("--apfs-format-block-size"));
    config->apfsFormatBlockSizeBytesProvided = !blockSizeValue.trimmed().isEmpty();
    const auto blockSize = argUInt64(args, QStringLiteral("--apfs-format-block-size"));
    config->apfsFormatBlockSizeBytesInvalid =
        config->apfsFormatBlockSizeBytesProvided && !blockSize.has_value();
    config->apfsFormatBlockSizeBytes = blockSize.value_or(config->apfsFormatBlockSizeBytes);
}

void parseApfsWriteConfig(const QStringList& args, Config* config) {
    config->apfsWriteRootFileImagePath =
        argValue(args, QStringLiteral("--apfs-write-root-file-image"));
    config->apfsWriteRootFileTargetPath =
        argValue(args, QStringLiteral("--apfs-write-root-file-target"));
    config->apfsWriteRootFileName =
        argValue(args, QStringLiteral("--apfs-write-root-file-name"));
    config->apfsWriteRootFileText =
        argValue(args, QStringLiteral("--apfs-write-root-file-text"));
    config->apfsWriteTargetConfirmed =
        hasSwitch(args, QStringLiteral("--apfs-write-target-confirmed"));
    config->apfsWriteAllowRawTarget =
        hasSwitch(args, QStringLiteral("--apfs-write-allow-raw-target"));
    config->apfsWriteRawHardwareProof =
        hasSwitch(args, QStringLiteral("--apfs-write-raw-hardware-proof"));
    const QString sizeValue = argValue(args, QStringLiteral("--apfs-write-target-size-bytes"));
    config->apfsWriteTargetSizeBytesProvided = !sizeValue.trimmed().isEmpty();
    const auto sizeBytes = argUInt64(args, QStringLiteral("--apfs-write-target-size-bytes"));
    config->apfsWriteTargetSizeBytesInvalid =
        config->apfsWriteTargetSizeBytesProvided && !sizeBytes.has_value();
    config->apfsWriteTargetSizeBytes = sizeBytes.value_or(config->apfsWriteTargetSizeBytes);
}

void parseApfsPatchConfig(const QStringList& args, Config* config) {
    config->apfsPatchRootFileImagePath =
        argValue(args, QStringLiteral("--apfs-patch-root-file-image"));
    config->apfsPatchRootFileTargetPath =
        argValue(args, QStringLiteral("--apfs-patch-root-file-target"));
    config->apfsPatchRootFileName =
        argValue(args, QStringLiteral("--apfs-patch-root-file-name"));
    config->apfsPatchRootFileText =
        argValue(args, QStringLiteral("--apfs-patch-root-file-text"));
    const QString offsetValue =
        argValue(args, QStringLiteral("--apfs-patch-root-file-offset-bytes"));
    config->apfsPatchRootFileOffsetBytesProvided = !offsetValue.trimmed().isEmpty();
    const auto offset =
        argUInt64(args, QStringLiteral("--apfs-patch-root-file-offset-bytes"));
    config->apfsPatchRootFileOffsetBytesInvalid =
        config->apfsPatchRootFileOffsetBytesProvided && !offset.has_value();
    config->apfsPatchRootFileOffsetBytes =
        offset.value_or(config->apfsPatchRootFileOffsetBytes);
}

void parseApfsDeleteConfig(const QStringList& args, Config* config) {
    config->apfsDeleteRootFileImagePath =
        argValue(args, QStringLiteral("--apfs-delete-root-file-image"));
    config->apfsDeleteRootFileTargetPath =
        argValue(args, QStringLiteral("--apfs-delete-root-file-target"));
    config->apfsDeleteRootFileName =
        argValue(args, QStringLiteral("--apfs-delete-root-file-name"));
}

void parseApfsRepairConfig(const QStringList& args, Config* config) {
    config->apfsRepairObjectChecksumsImagePath =
        argValue(args, QStringLiteral("--apfs-repair-object-checksums"));
    config->apfsRepairObjectChecksumsTargetPath =
        argValue(args, QStringLiteral("--apfs-repair-object-checksums-target"));
    config->apfsRepairReadFilePath =
        argValue(args, QStringLiteral("--apfs-repair-read-file"));
    config->apfsRepairTargetConfirmed =
        hasSwitch(args, QStringLiteral("--apfs-repair-target-confirmed"));
    config->apfsRepairAllowRawTarget =
        hasSwitch(args, QStringLiteral("--apfs-repair-allow-raw-target"));
    config->apfsRepairRawHardwareProof =
        hasSwitch(args, QStringLiteral("--apfs-repair-raw-hardware-proof"));
    const QString sizeValue = argValue(args, QStringLiteral("--apfs-repair-target-size-bytes"));
    config->apfsRepairTargetSizeBytesProvided = !sizeValue.trimmed().isEmpty();
    const auto sizeBytes = argUInt64(args, QStringLiteral("--apfs-repair-target-size-bytes"));
    config->apfsRepairTargetSizeBytesInvalid =
        config->apfsRepairTargetSizeBytesProvided && !sizeBytes.has_value();
    config->apfsRepairTargetSizeBytes =
        sizeBytes.value_or(config->apfsRepairTargetSizeBytes);
    const QString corruptBlockValue =
        argValue(args, QStringLiteral("--apfs-repair-corrupt-metadata-block"));
    config->apfsRepairCorruptMetadataBlockProvided = !corruptBlockValue.trimmed().isEmpty();
    const auto corruptBlock =
        argUInt64(args, QStringLiteral("--apfs-repair-corrupt-metadata-block"));
    config->apfsRepairCorruptMetadataBlockInvalid =
        config->apfsRepairCorruptMetadataBlockProvided && !corruptBlock.has_value();
    config->apfsRepairCorruptMetadataBlock =
        corruptBlock.value_or(config->apfsRepairCorruptMetadataBlock);
    const QString repairReadMaxBytesValue =
        argValue(args, QStringLiteral("--apfs-repair-read-max-bytes"));
    config->apfsRepairReadMaxBytesProvided =
        !repairReadMaxBytesValue.trimmed().isEmpty();
    const auto repairReadMaxBytes =
        argUInt64(args, QStringLiteral("--apfs-repair-read-max-bytes"));
    config->apfsRepairReadMaxBytesInvalid =
        config->apfsRepairReadMaxBytesProvided && !repairReadMaxBytes.has_value();
    config->apfsRepairReadMaxBytes =
        repairReadMaxBytes.value_or(config->apfsRepairReadMaxBytes);
}

Config parseConfig(const QStringList& args) {
    Config config = parseBaseConfig(args);
    parseHfsConfig(args, &config);
    parseApfsReadConfig(args, &config);
    parseApfsExportConfig(args, &config);
    parseApfsFormatConfig(args, &config);
    parseApfsWriteConfig(args, &config);
    parseApfsPatchConfig(args, &config);
    parseApfsDeleteConfig(args, &config);
    parseApfsRepairConfig(args, &config);
    return config;
}

void appendIf(bool condition, QStringList* errors, const QString& message) {
    if (condition && errors) {
        errors->append(message);
    }
}

struct RequestedApfsActions {
    bool formatBuild{false};
    bool existingFormat{false};
    bool imageWrite{false};
    bool rawWrite{false};
    bool imagePatch{false};
    bool rawPatch{false};
    bool imageDelete{false};
    bool rawDelete{false};
    bool imageRepair{false};
    bool rawRepair{false};
};

RequestedApfsActions requestedApfsActions(const Config& config) {
    return {.formatBuild = !config.apfsBuildFormatImagePath.trimmed().isEmpty(),
            .existingFormat = !config.apfsFormatExistingTargetPath.trimmed().isEmpty(),
            .imageWrite = !config.apfsWriteRootFileImagePath.trimmed().isEmpty(),
            .rawWrite = !config.apfsWriteRootFileTargetPath.trimmed().isEmpty(),
            .imagePatch = !config.apfsPatchRootFileImagePath.trimmed().isEmpty(),
            .rawPatch = !config.apfsPatchRootFileTargetPath.trimmed().isEmpty(),
            .imageDelete = !config.apfsDeleteRootFileImagePath.trimmed().isEmpty(),
            .rawDelete = !config.apfsDeleteRootFileTargetPath.trimmed().isEmpty(),
            .imageRepair = !config.apfsRepairObjectChecksumsImagePath.trimmed().isEmpty(),
            .rawRepair = !config.apfsRepairObjectChecksumsTargetPath.trimmed().isEmpty()};
}

void appendInputPathErrors(const Config& config,
                           const RequestedApfsActions& requested,
                           QStringList* errors) {
    const bool hfsFixtureRequested = !config.hfsBuildWriterFixturePath.trimmed().isEmpty();
    appendIf(!requested.formatBuild && !requested.existingFormat && !requested.rawWrite &&
                 !requested.rawPatch && !requested.rawDelete && !requested.imagePatch &&
                 !requested.rawRepair && !hfsFixtureRequested &&
                 config.inputPath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--input is required"));
    appendIf(config.outputPath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--output is required"));
}

void appendFormatPathErrors(const Config& config,
                            const RequestedApfsActions& requested,
                            QStringList* errors) {
    appendIf(requested.formatBuild && config.apfsBuildFormatImagePath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-build-format-image requires an image path"));
    appendIf(requested.formatBuild && requested.existingFormat,
             errors,
             QStringLiteral("Use either --apfs-build-format-image or --apfs-format-existing-target, not both"));
    appendIf(requested.existingFormat && config.apfsFormatExistingTargetPath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-format-existing-target requires a target path"));
}

int requestedRootMutationCount(bool write, bool patch, bool deleted) {
    return (write ? 1 : 0) + (patch ? 1 : 0) + (deleted ? 1 : 0);
}

bool imageRootMutationRequested(const RequestedApfsActions& requested) {
    return requested.imageWrite || requested.imagePatch || requested.imageDelete;
}

bool rawRootMutationRequested(const RequestedApfsActions& requested) {
    return requested.rawWrite || requested.rawPatch || requested.rawDelete;
}

void appendRootMutationConflictErrors(const RequestedApfsActions& requested,
                                      QStringList* errors) {
    const int imageMutationCount = requestedRootMutationCount(
        requested.imageWrite,
        requested.imagePatch,
        requested.imageDelete);
    const int rawMutationCount = requestedRootMutationCount(
        requested.rawWrite,
        requested.rawPatch,
        requested.rawDelete);
    appendIf(imageRootMutationRequested(requested) && rawRootMutationRequested(requested),
             errors,
             QStringLiteral("Use either APFS image root-file mutation or APFS raw root-file mutation, not both"));
    appendIf(imageMutationCount > 1,
             errors,
             QStringLiteral("Use only one APFS image root-file mutation at a time"));
    appendIf(rawMutationCount > 1,
             errors,
             QStringLiteral("Use only one APFS raw root-file mutation at a time"));
}

void appendRootMutationTargetPathErrors(const Config& config,
                                        const RequestedApfsActions& requested,
                                        QStringList* errors) {
    appendIf(requested.imageWrite && config.apfsWriteRootFileImagePath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-write-root-file-image requires an image path"));
    appendIf(requested.rawWrite && config.apfsWriteRootFileTargetPath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-write-root-file-target requires a target path"));
    appendIf(requested.imagePatch && config.apfsPatchRootFileImagePath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-patch-root-file-image requires an image path"));
    appendIf(requested.rawPatch && config.apfsPatchRootFileTargetPath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-patch-root-file-target requires a target path"));
    appendIf(requested.imageDelete && config.apfsDeleteRootFileImagePath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-delete-root-file-image requires an image path"));
    appendIf(requested.rawDelete && config.apfsDeleteRootFileTargetPath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-delete-root-file-target requires a target path"));
}

void appendWritePathErrors(const Config& config,
                           const RequestedApfsActions& requested,
                           QStringList* errors) {
    appendRootMutationConflictErrors(requested, errors);
    appendRootMutationTargetPathErrors(config, requested, errors);
}

void appendRepairPathErrors(const Config& config,
                            const RequestedApfsActions& requested,
                            QStringList* errors) {
    appendIf(requested.imageRepair && config.apfsRepairObjectChecksumsImagePath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-repair-object-checksums requires an image path"));
    appendIf(requested.imageRepair && requested.rawRepair,
             errors,
             QStringLiteral("Use either --apfs-repair-object-checksums or --apfs-repair-object-checksums-target, not both"));
    appendIf(requested.rawRepair && config.apfsRepairObjectChecksumsTargetPath.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-repair-object-checksums-target requires a target path"));
}

void appendRequiredPathErrors(const Config& config, QStringList* errors) {
    const auto requested = requestedApfsActions(config);
    appendInputPathErrors(config, requested, errors);
    appendFormatPathErrors(config, requested, errors);
    appendWritePathErrors(config, requested, errors);
    appendRepairPathErrors(config, requested, errors);
}

void appendCommonNumericConfigErrors(const Config& config, QStringList* errors) {
    appendIf(config.inputSizeOverrideProvided && config.inputSizeOverride == 0,
             errors,
             QStringLiteral("--input-size-bytes must be a positive integer when provided"));
    appendIf(config.inputOffsetInvalid,
             errors,
             QStringLiteral("--input-offset-bytes must be a non-negative integer"));
}

void appendHfsNumericConfigErrors(const Config& config, QStringList* errors) {
    appendIf(config.hfsReadMaxBytesInvalid,
             errors,
             QStringLiteral("--hfs-read-max-bytes must be a non-negative integer"));
    appendIf(config.hfsReadAttributeFileIdInvalid,
             errors,
             QStringLiteral("--hfs-read-attribute-file-id must be a positive integer"));
    appendIf(config.hfsReadAttributeFileIdProvided && config.hfsReadAttributeFileId == 0,
             errors,
             QStringLiteral("--hfs-read-attribute-file-id must be positive"));
    appendIf(config.hfsReadAttributeFileIdProvided &&
                 config.hfsReadAttributeName.trimmed().isEmpty(),
             errors,
             QStringLiteral("--hfs-read-attribute-name is required with --hfs-read-attribute-file-id"));
    appendIf(!config.hfsReadAttributeName.trimmed().isEmpty() &&
                 !config.hfsReadAttributeFileIdProvided,
             errors,
             QStringLiteral("--hfs-read-attribute-file-id is required with --hfs-read-attribute-name"));
}

void appendApfsReadLimitConfigErrors(const Config& config, QStringList* errors) {
    appendIf(config.apfsReadMaxBytesInvalid,
             errors,
             QStringLiteral("--apfs-read-max-bytes must be a non-negative integer"));
    appendIf(config.apfsRepairReadMaxBytesInvalid,
             errors,
             QStringLiteral("--apfs-repair-read-max-bytes must be a non-negative integer"));
    appendIf(config.apfsRepairCorruptMetadataBlockInvalid,
             errors,
             QStringLiteral("--apfs-repair-corrupt-metadata-block must be a non-negative integer"));
    appendIf(config.apfsExportMaxEntriesInvalid,
             errors,
             QStringLiteral("--apfs-export-max-entries must be a positive integer"));
    appendIf(config.apfsExportMaxFileBytesInvalid,
             errors,
             QStringLiteral("--apfs-export-max-file-bytes must be a positive integer"));
    appendIf(config.apfsExportMaxTotalBytesInvalid,
             errors,
             QStringLiteral("--apfs-export-max-total-bytes must be a positive integer"));
    appendIf(config.apfsExportMaxEntriesProvided && config.apfsExportMaxEntries == 0,
             errors,
             QStringLiteral("--apfs-export-max-entries must be positive"));
    appendIf(config.apfsExportMaxFileBytesProvided && config.apfsExportMaxFileBytes == 0,
             errors,
             QStringLiteral("--apfs-export-max-file-bytes must be positive"));
    appendIf(config.apfsExportMaxTotalBytesProvided && config.apfsExportMaxTotalBytes == 0,
             errors,
             QStringLiteral("--apfs-export-max-total-bytes must be positive"));
}

void appendApfsFormatConfigErrors(const Config& config, QStringList* errors) {
    appendIf(config.apfsFormatSizeBytesInvalid,
             errors,
             QStringLiteral("--apfs-format-size-bytes must be a positive integer"));
    appendIf(config.apfsFormatSizeBytesProvided && config.apfsFormatSizeBytes == 0,
             errors,
             QStringLiteral("--apfs-format-size-bytes must be positive"));
    appendIf(config.apfsFormatBlockSizeBytesInvalid,
             errors,
             QStringLiteral("--apfs-format-block-size must be a positive integer"));
    appendIf(config.apfsFormatBlockSizeBytesProvided && config.apfsFormatBlockSizeBytes == 0,
             errors,
             QStringLiteral("--apfs-format-block-size must be positive"));
    appendIf(!config.apfsFormatSeedFileName.trimmed().isEmpty() &&
                 config.apfsFormatSeedFileText.isEmpty(),
             errors,
             QStringLiteral("--apfs-format-seed-file-text is required with --apfs-format-seed-file-name"));
    appendIf(config.apfsFormatSeedFileName.trimmed().isEmpty() &&
                 !config.apfsFormatSeedFileText.isEmpty(),
             errors,
             QStringLiteral("--apfs-format-seed-file-name is required with --apfs-format-seed-file-text"));
}

void appendApfsRootFilePatchConfigErrors(const Config& config, QStringList* errors) {
    const bool patchRequested = !config.apfsPatchRootFileImagePath.trimmed().isEmpty() ||
                                !config.apfsPatchRootFileTargetPath.trimmed().isEmpty();
    appendIf(patchRequested && config.apfsPatchRootFileName.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-patch-root-file-name is required with APFS root-file patch"));
    appendIf(patchRequested && config.apfsPatchRootFileText.isEmpty(),
             errors,
             QStringLiteral("--apfs-patch-root-file-text is required with APFS root-file patch"));
    appendIf(patchRequested && !config.apfsPatchRootFileOffsetBytesProvided,
             errors,
             QStringLiteral("--apfs-patch-root-file-offset-bytes is required with APFS root-file patch"));
    appendIf(config.apfsPatchRootFileOffsetBytesInvalid,
             errors,
             QStringLiteral("--apfs-patch-root-file-offset-bytes must be a non-negative integer"));
}

bool apfsRootFileWriteConfigRequested(const Config& config) {
    return !config.apfsWriteRootFileImagePath.trimmed().isEmpty() ||
           !config.apfsWriteRootFileTargetPath.trimmed().isEmpty();
}

bool apfsRawRootMutationConfigRequested(const Config& config) {
    return !config.apfsWriteRootFileTargetPath.trimmed().isEmpty() ||
           !config.apfsPatchRootFileTargetPath.trimmed().isEmpty() ||
           !config.apfsDeleteRootFileTargetPath.trimmed().isEmpty();
}

bool apfsRootFileDeleteConfigRequested(const Config& config) {
    return !config.apfsDeleteRootFileImagePath.trimmed().isEmpty() ||
           !config.apfsDeleteRootFileTargetPath.trimmed().isEmpty();
}

void appendApfsRootFileWriteConfigErrors(const Config& config, QStringList* errors) {
    const bool writeRequested = apfsRootFileWriteConfigRequested(config);
    appendIf(writeRequested && config.apfsWriteRootFileName.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-write-root-file-name is required with APFS root-file write"));
    appendIf(writeRequested && config.apfsWriteRootFileText.isEmpty(),
             errors,
             QStringLiteral("--apfs-write-root-file-text is required with APFS root-file write"));
}

void appendApfsRootFileDeleteConfigErrors(const Config& config, QStringList* errors) {
    appendIf(apfsRootFileDeleteConfigRequested(config) &&
                 config.apfsDeleteRootFileName.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-delete-root-file-name is required with APFS root-file delete"));
}

void appendApfsRawRootMutationConfigErrors(const Config& config, QStringList* errors) {
    appendIf(config.apfsWriteTargetSizeBytesInvalid,
             errors,
             QStringLiteral("--apfs-write-target-size-bytes must be a positive integer"));
    appendIf(apfsRawRootMutationConfigRequested(config) && config.apfsWriteTargetSizeBytes == 0,
             errors,
             QStringLiteral("--apfs-write-target-size-bytes is required with APFS raw root-file mutation"));
}

void appendApfsRepairConfigErrors(const Config& config, QStringList* errors) {
    appendIf(config.apfsRepairTargetSizeBytesInvalid,
             errors,
             QStringLiteral("--apfs-repair-target-size-bytes must be a positive integer"));
    appendIf(!config.apfsRepairObjectChecksumsTargetPath.trimmed().isEmpty() &&
                 config.apfsRepairTargetSizeBytes == 0,
             errors,
             QStringLiteral("--apfs-repair-target-size-bytes is required with --apfs-repair-object-checksums-target"));
}

void appendApfsExportConfigErrors(const Config& config, QStringList* errors) {
    appendIf(!config.apfsExportPath.trimmed().isEmpty() &&
                 config.apfsExportOutputDirectory.trimmed().isEmpty(),
             errors,
             QStringLiteral("--apfs-export-output is required with --apfs-export-path"));
}

void appendApfsWriteConfigErrors(const Config& config, QStringList* errors) {
    appendApfsRootFileWriteConfigErrors(config, errors);
    appendApfsRootFilePatchConfigErrors(config, errors);
    appendApfsRootFileDeleteConfigErrors(config, errors);
    appendApfsRawRootMutationConfigErrors(config, errors);
    appendApfsRepairConfigErrors(config, errors);
    appendApfsExportConfigErrors(config, errors);
}

void appendNumericConfigErrors(const Config& config, QStringList* errors) {
    appendCommonNumericConfigErrors(config, errors);
    appendHfsNumericConfigErrors(config, errors);
    appendApfsReadLimitConfigErrors(config, errors);
    appendApfsFormatConfigErrors(config, errors);
    appendApfsWriteConfigErrors(config, errors);
}

void appendPlatformLimitErrors(const Config& config, QStringList* errors) {
    const auto maxQint64 = static_cast<uint64_t>(std::numeric_limits<qint64>::max());
    appendIf(config.inputOffsetBytes > maxQint64,
             errors,
             QStringLiteral("--input-offset-bytes is too large for this platform"));
    appendIf(config.apfsPatchRootFileOffsetBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-patch-root-file-offset-bytes is too large for this platform"));
    appendIf(config.hfsReadMaxBytes > maxQint64,
             errors,
             QStringLiteral("--hfs-read-max-bytes is too large for this platform"));
    appendIf(config.hfsReadAttributeFileId > std::numeric_limits<uint32_t>::max(),
             errors,
             QStringLiteral("--hfs-read-attribute-file-id is too large for this platform"));
    appendIf(config.apfsReadMaxBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-read-max-bytes is too large for this platform"));
    appendIf(config.apfsRepairReadMaxBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-repair-read-max-bytes is too large for this platform"));
    appendIf(config.apfsExportMaxEntries > kMaxIntValue,
             errors,
             QStringLiteral("--apfs-export-max-entries is too large for this platform"));
    appendIf(config.apfsExportMaxFileBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-export-max-file-bytes is too large for this platform"));
    appendIf(config.apfsExportMaxTotalBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-export-max-total-bytes is too large for this platform"));
    appendIf(config.apfsFormatSizeBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-format-size-bytes is too large for this platform"));
    appendIf(config.apfsFormatBlockSizeBytes > std::numeric_limits<uint32_t>::max(),
             errors,
             QStringLiteral("--apfs-format-block-size is too large for this platform"));
    appendIf(config.apfsWriteTargetSizeBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-write-target-size-bytes is too large for this platform"));
    appendIf(config.apfsRepairTargetSizeBytes > maxQint64,
             errors,
             QStringLiteral("--apfs-repair-target-size-bytes is too large for this platform"));
}

QStringList validateConfig(const Config& config) {
    QStringList errors;
    appendRequiredPathErrors(config, &errors);
    appendNumericConfigErrors(config, &errors);
    appendPlatformLimitErrors(config, &errors);
    return errors;
}

int fail(const Config& config, const QString& message) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"), QStringLiteral("Failed"));
    report.insert(QStringLiteral("error"), message);
    if (!config.outputPath.trimmed().isEmpty()) {
        writeJsonFile(config.outputPath, report);
    }
    QTextStream(stderr) << message << Qt::endl;
    return 1;
}

std::optional<sak::PartitionFileSystemDetection> detectInput(const Config& config,
                                                             uint64_t* inputSizeBytes,
                                                             QString* error) {
    const auto bytes =
        readProbeBytes(config.inputPath, config.inputOffsetBytes, inputSizeBytes, error);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    if (config.inputSizeOverride > 0) {
        *inputSizeBytes = config.inputSizeOverride;
    }
    const uint64_t inputSize = inputSizeBytes ? *inputSizeBytes : 0ULL;
    QString detectError;
    auto detection = sak::PartitionFileSystemDetector::detectFromDevicePath(config.inputPath,
                                                                            config.inputOffsetBytes,
                                                                            inputSize,
                                                                            &detectError);
    if (!detection.has_value()) {
        detection = sak::PartitionFileSystemDetector::detectBytes(*bytes, inputSize);
    }
    if (!detection.has_value() && error) {
        *error = detectError.isEmpty() ? QStringLiteral("No filesystem signature detected")
                                       : detectError;
    }
    return detection;
}

QStringList validationBlockers(const Config& config,
                               const sak::PartitionFileSystemDetection& detection) {
    QStringList blockers;
    if (!config.expectedFileSystem.trimmed().isEmpty() &&
        detection.file_system.compare(config.expectedFileSystem, Qt::CaseInsensitive) != 0) {
        blockers.append(QStringLiteral("Expected %1 but detected %2")
                            .arg(config.expectedFileSystem, detection.file_system));
    }
    if (config.requireSane && hasSanityWarning(detection.details)) {
        blockers.append(QStringLiteral("Metadata sanity warnings are present"));
    }
    return blockers;
}

QJsonObject detectionReport(const Config& config,
                            const sak::PartitionFileSystemDetection& detection,
                            uint64_t inputSizeBytes,
                            const QStringList& blockers) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("detected_file_system"), detection.file_system);
    report.insert(QStringLiteral("source"), detection.source);
    report.insert(QStringLiteral("input_size_bytes"), QString::number(inputSizeBytes));
    report.insert(QStringLiteral("total_bytes"), QString::number(detection.total_bytes));
    report.insert(QStringLiteral("free_bytes"), QString::number(detection.free_bytes));
    report.insert(QStringLiteral("details"), stringArray(detection.details));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    return report;
}

bool hfsOperationsRequested(const Config& config) {
    return config.hfsCheck ||
           !config.hfsListPath.trimmed().isEmpty() ||
           !config.hfsReadFilePath.trimmed().isEmpty() ||
           !config.hfsReadResourceForkPath.trimmed().isEmpty() ||
           config.hfsReadAttributeFileIdProvided ||
           !config.hfsReadAttributeName.trimmed().isEmpty();
}

bool hfsWriterFixtureRequested(const Config& config) {
    return !config.hfsBuildWriterFixturePath.trimmed().isEmpty();
}

bool apfsOperationsRequested(const Config& config) {
    return !config.apfsListPath.trimmed().isEmpty() ||
           !config.apfsReadFilePath.trimmed().isEmpty() ||
           !config.apfsExportPath.trimmed().isEmpty();
}

bool apfsFormatBuildRequested(const Config& config) {
    return !config.apfsBuildFormatImagePath.trimmed().isEmpty();
}

bool apfsExistingFormatRequested(const Config& config) {
    return !config.apfsFormatExistingTargetPath.trimmed().isEmpty();
}

bool apfsRootFileWriteRequested(const Config& config) {
    return !config.apfsWriteRootFileImagePath.trimmed().isEmpty();
}

bool apfsRawRootFileWriteRequested(const Config& config) {
    return !config.apfsWriteRootFileTargetPath.trimmed().isEmpty();
}

bool apfsRootFilePatchRequested(const Config& config) {
    return !config.apfsPatchRootFileImagePath.trimmed().isEmpty();
}

bool apfsRawRootFilePatchRequested(const Config& config) {
    return !config.apfsPatchRootFileTargetPath.trimmed().isEmpty();
}

bool apfsRootFileDeleteRequested(const Config& config) {
    return !config.apfsDeleteRootFileImagePath.trimmed().isEmpty();
}

bool apfsRawRootFileDeleteRequested(const Config& config) {
    return !config.apfsDeleteRootFileTargetPath.trimmed().isEmpty();
}

bool apfsRepairRequested(const Config& config) {
    return !config.apfsRepairObjectChecksumsImagePath.trimmed().isEmpty();
}

bool apfsRawRepairRequested(const Config& config) {
    return !config.apfsRepairObjectChecksumsTargetPath.trimmed().isEmpty();
}

QJsonArray apfsPlanStepsArray(const QVector<sak::PartitionApfsImageMutationStep>& steps) {
    QJsonArray array;
    for (const auto& step : steps) {
        array.append(QJsonObject{{QStringLiteral("name"), step.name},
                                 {QStringLiteral("description"), step.description},
                                 {QStringLiteral("writes_metadata"), step.writes_metadata},
                                 {QStringLiteral("requires_checkpoint"), step.requires_checkpoint},
                                 {QStringLiteral("required_evidence"),
                                  stringArray(step.required_evidence)}});
    }
    return array;
}

QStringList hfsOperationBlockers(const Config& config,
                                 const sak::PartitionFileSystemDetection& detection) {
    QStringList blockers;
    if (!hfsOperationsRequested(config)) {
        return blockers;
    }
    if (config.inputOffsetBytes != 0) {
        blockers.append(QStringLiteral("HFS check/browse/read/attribute proof does not support input offsets"));
    }
    if (detection.file_system.compare(QStringLiteral("HFS+"), Qt::CaseInsensitive) != 0 &&
        detection.file_system.compare(QStringLiteral("HFSX"), Qt::CaseInsensitive) != 0) {
        blockers.append(QStringLiteral("HFS check/browse/read/attribute proof requires detected HFS+ or HFSX"));
    }
    return blockers;
}

QStringList apfsOperationBlockers(const Config& config,
                                  const sak::PartitionFileSystemDetection& detection) {
    QStringList blockers;
    if (!apfsOperationsRequested(config)) {
        return blockers;
    }
    if (config.inputOffsetBytes != 0) {
        blockers.append(QStringLiteral("APFS browse/read proof does not support input offsets"));
    }
    if (detection.file_system.compare(QStringLiteral("APFS"), Qt::CaseInsensitive) != 0) {
        blockers.append(QStringLiteral("APFS browse/read proof requires detected APFS"));
    }
    return blockers;
}

QJsonObject hfsListingReport(const Config& config) {
    const auto result =
        sak::PartitionHfsFileSystemReader::listDirectoryFromImage(config.inputPath,
                                                                  config.hfsListPath,
                                                                  1000);
    QJsonObject report{{QStringLiteral("path"), config.hfsListPath},
                       {QStringLiteral("status"),
                        result.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
                       {QStringLiteral("file_system"), result.file_system},
                       {QStringLiteral("entry_count"), result.entries.size()},
                       {QStringLiteral("entries"), hfsEntriesArray(result.entries)},
                       {QStringLiteral("blockers"),
                        blockersArray(result.blockers, result.warnings)}};
    return report;
}

QJsonObject hfsConsistencyReport(const Config& config) {
    const auto result =
        sak::PartitionHfsFileSystemReader::checkConsistencyFromImage(config.inputPath);
    return QJsonObject{{QStringLiteral("status"),
                        result.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
                       {QStringLiteral("file_system"), result.file_system},
                       {QStringLiteral("records_scanned"), result.records_scanned},
                       {QStringLiteral("directories"), result.directories},
                       {QStringLiteral("files"), result.files},
                       {QStringLiteral("threads"), result.threads},
                       {QStringLiteral("other_records"), result.other_records},
                       {QStringLiteral("invalid_records_skipped"),
                        result.invalid_records_skipped},
                       {QStringLiteral("attributes_present"), result.attributes_present},
                       {QStringLiteral("attribute_records_scanned"),
                        result.attribute_records_scanned},
                       {QStringLiteral("inline_attribute_records"),
                        result.inline_attribute_records},
                       {QStringLiteral("fork_attribute_records"), result.fork_attribute_records},
                       {QStringLiteral("extent_attribute_records"),
                        result.extent_attribute_records},
                       {QStringLiteral("other_attribute_records"),
                        result.other_attribute_records},
                       {QStringLiteral("attribute_names"), stringArray(result.attribute_names)},
                       {QStringLiteral("attribute_metadata"),
                        stringArray(result.attribute_metadata)},
                       {QStringLiteral("attribute_records"),
                        hfsAttributeMetadataArray(result.attribute_records)},
                       {QStringLiteral("details"), stringArray(result.details)},
                       {QStringLiteral("blockers"),
                        blockersArray(result.blockers, result.warnings)}};
}

QJsonObject hfsReadReport(const Config& config, const QString& path, bool resourceFork) {
    const auto result =
        resourceFork
            ? sak::PartitionHfsFileSystemReader::readResourceForkFromImage(
                  config.inputPath,
                  path,
                  config.hfsReadMaxBytes)
            : sak::PartitionHfsFileSystemReader::readFileFromImage(config.inputPath,
                                                                   path,
                                                                   config.hfsReadMaxBytes);
    const QByteArray hash = QCryptographicHash::hash(result.data, QCryptographicHash::Sha256);
    return QJsonObject{{QStringLiteral("path"), path},
                       {QStringLiteral("resource_fork"), resourceFork},
                       {QStringLiteral("status"),
                        result.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
                       {QStringLiteral("file_system"), result.file_system},
                       {QStringLiteral("bytes_read"), result.data.size()},
                       {QStringLiteral("sha256"), QString::fromLatin1(hash.toHex())},
                       {QStringLiteral("blockers"),
                       blockersArray(result.blockers, result.warnings)}};
}

QJsonObject hfsAttributeReadReport(const Config& config) {
    const auto result = sak::PartitionHfsFileSystemReader::readAttributeValueFromImage(
        config.inputPath,
        static_cast<uint32_t>(config.hfsReadAttributeFileId),
        config.hfsReadAttributeName,
        config.hfsReadMaxBytes);
    const QByteArray hash = QCryptographicHash::hash(result.data, QCryptographicHash::Sha256);
    return QJsonObject{{QStringLiteral("file_id"),
                        QString::number(result.file_id)},
                       {QStringLiteral("attribute_name"), result.attribute_name},
                       {QStringLiteral("storage"), result.storage},
                       {QStringLiteral("status"),
                        result.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
                       {QStringLiteral("file_system"), result.file_system},
                       {QStringLiteral("bytes_read"), result.data.size()},
                       {QStringLiteral("sha256"), QString::fromLatin1(hash.toHex())},
                       {QStringLiteral("blockers"),
                        blockersArray(result.blockers, result.warnings)}};
}

QJsonObject apfsListingReport(const Config& config) {
    const auto result =
        sak::PartitionApfsFileSystemReader::listDirectoryFromImage(config.inputPath,
                                                                   config.apfsListPath,
                                                                   1000);
    QJsonObject report{{QStringLiteral("path"), config.apfsListPath},
                       {QStringLiteral("status"),
                        result.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
                       {QStringLiteral("file_system"), result.file_system},
                       {QStringLiteral("volume_name"), result.volume_name},
                       {QStringLiteral("entry_count"), result.entries.size()},
                       {QStringLiteral("entries"), apfsEntriesArray(result.entries)},
                       {QStringLiteral("blockers"),
                        blockersArray(result.blockers, result.warnings)}};
    return report;
}

QJsonObject apfsReadReport(const Config& config, const QString& path) {
    const auto result = sak::PartitionApfsFileSystemReader::readFileFromImage(
        config.inputPath,
        path,
        config.apfsReadMaxBytes);
    const QByteArray hash = QCryptographicHash::hash(result.data, QCryptographicHash::Sha256);
    return QJsonObject{{QStringLiteral("path"), path},
                       {QStringLiteral("status"),
                        result.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
                       {QStringLiteral("file_system"), result.file_system},
                       {QStringLiteral("volume_name"), result.volume_name},
                       {QStringLiteral("bytes_read"), result.data.size()},
                       {QStringLiteral("sha256"), QString::fromLatin1(hash.toHex())},
                       {QStringLiteral("blockers"),
                        blockersArray(result.blockers, result.warnings)}};
}

QJsonObject apfsExportReport(const Config& config) {
    const sak::PartitionApfsDirectoryExportOptions options{
        static_cast<int>(config.apfsExportMaxEntries),
        config.apfsExportMaxFileBytes,
        config.apfsExportMaxTotalBytes};
    const auto result = sak::PartitionApfsFileSystemReader::exportDirectoryFromImage(
        config.inputPath,
        config.apfsExportPath,
        config.apfsExportOutputDirectory,
        options);
    return QJsonObject{{QStringLiteral("path"), config.apfsExportPath},
                       {QStringLiteral("output_directory"), config.apfsExportOutputDirectory},
                       {QStringLiteral("status"),
                        result.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
                       {QStringLiteral("files_exported"), result.files_exported},
                       {QStringLiteral("directories_exported"), result.directories_exported},
                       {QStringLiteral("symlinks_skipped"), result.symlinks_skipped},
                       {QStringLiteral("entries_scanned"), result.entries_scanned},
                       {QStringLiteral("bytes_exported"),
                        QString::number(result.bytes_exported)},
                       {QStringLiteral("blockers"),
                        blockersArray(result.blockers, result.warnings)}};
}

QString jsonStringArraySummary(const QJsonArray& array) {
    QStringList values;
    for (const auto& item : array) {
        const QString value = item.toString();
        if (!value.trimmed().isEmpty()) {
            values.append(value);
        }
    }
    return values.join(QStringLiteral("; "));
}

QString hfsOperationFailureBlocker(const QString& label, const QJsonObject& operationReport) {
    if (operationReport.value(QStringLiteral("status")).toString() == QStringLiteral("Passed")) {
        return {};
    }

    const QString blockers =
        jsonStringArraySummary(operationReport.value(QStringLiteral("blockers")).toArray());
    return blockers.trimmed().isEmpty() ? QStringLiteral("%1 failed").arg(label)
                                        : QStringLiteral("%1 failed: %2").arg(label, blockers);
}

void appendIfNotEmpty(QStringList* values, const QString& value) {
    if (values && !value.trimmed().isEmpty()) {
        values->append(value);
    }
}

QStringList appendHfsOperationReports(QJsonObject* report,
                                      const Config& config,
                                      const sak::PartitionFileSystemDetection& detection) {
    QStringList operationBlockers;
    if (!report || !hfsOperationsRequested(config)) {
        return operationBlockers;
    }

    const QStringList blockers = hfsOperationBlockers(config, detection);
    if (!blockers.isEmpty()) {
        report->insert(QStringLiteral("hfs_operation_blockers"), stringArray(blockers));
        operationBlockers.append(blockers);
        return operationBlockers;
    }
    if (config.hfsCheck) {
        const QJsonObject check = hfsConsistencyReport(config);
        report->insert(QStringLiteral("hfs_check"), check);
        appendIfNotEmpty(&operationBlockers,
                         hfsOperationFailureBlocker(QStringLiteral("HFS consistency check"),
                                                    check));
    }
    if (!config.hfsListPath.trimmed().isEmpty()) {
        const QJsonObject listing = hfsListingReport(config);
        report->insert(QStringLiteral("hfs_listing"), listing);
        appendIfNotEmpty(&operationBlockers,
                         hfsOperationFailureBlocker(QStringLiteral("HFS listing"), listing));
    }
    if (!config.hfsReadFilePath.trimmed().isEmpty()) {
        const QJsonObject readFile = hfsReadReport(config, config.hfsReadFilePath, false);
        report->insert(QStringLiteral("hfs_read_file"), readFile);
        appendIfNotEmpty(&operationBlockers,
                         hfsOperationFailureBlocker(QStringLiteral("HFS file read"), readFile));
    }
    if (!config.hfsReadResourceForkPath.trimmed().isEmpty()) {
        const QJsonObject resourceFork =
            hfsReadReport(config, config.hfsReadResourceForkPath, true);
        report->insert(QStringLiteral("hfs_read_resource_fork"), resourceFork);
        appendIfNotEmpty(
            &operationBlockers,
            hfsOperationFailureBlocker(QStringLiteral("HFS resource-fork read"), resourceFork));
    }
    if (config.hfsReadAttributeFileIdProvided) {
        const QJsonObject attribute = hfsAttributeReadReport(config);
        report->insert(QStringLiteral("hfs_read_attribute"), attribute);
        appendIfNotEmpty(&operationBlockers,
                         hfsOperationFailureBlocker(QStringLiteral("HFS attribute read"),
                                                    attribute));
    }
    return operationBlockers;
}

QStringList appendApfsOperationReports(QJsonObject* report,
                                       const Config& config,
                                       const sak::PartitionFileSystemDetection& detection) {
    QStringList operationBlockers;
    if (!report || !apfsOperationsRequested(config)) {
        return operationBlockers;
    }

    const QStringList blockers = apfsOperationBlockers(config, detection);
    if (!blockers.isEmpty()) {
        report->insert(QStringLiteral("apfs_operation_blockers"), stringArray(blockers));
        operationBlockers.append(blockers);
        return operationBlockers;
    }
    if (!config.apfsListPath.trimmed().isEmpty()) {
        const QJsonObject listing = apfsListingReport(config);
        report->insert(QStringLiteral("apfs_listing"), listing);
        appendIfNotEmpty(&operationBlockers,
                         hfsOperationFailureBlocker(QStringLiteral("APFS listing"), listing));
    }
    if (!config.apfsReadFilePath.trimmed().isEmpty()) {
        const QJsonObject readFile = apfsReadReport(config, config.apfsReadFilePath);
        report->insert(QStringLiteral("apfs_read_file"), readFile);
        appendIfNotEmpty(&operationBlockers,
                         hfsOperationFailureBlocker(QStringLiteral("APFS file read"), readFile));
    }
    if (!config.apfsExportPath.trimmed().isEmpty()) {
        const QJsonObject exportReport = apfsExportReport(config);
        report->insert(QStringLiteral("apfs_export"), exportReport);
        appendIfNotEmpty(&operationBlockers,
                         hfsOperationFailureBlocker(QStringLiteral("APFS directory export"),
                                                    exportReport));
    }
    return operationBlockers;
}

int runHfsWriterFixtureCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const QByteArray image = hfsWriterFixture();
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("operation"), QStringLiteral("HFS writer fixture build"));
    report.insert(QStringLiteral("fixture_path"),
                  QFileInfo(config.hfsBuildWriterFixturePath).absoluteFilePath());
    report.insert(QStringLiteral("hfs_path"), QString::fromLatin1(kFixtureHfsHelloPath));
    report.insert(QStringLiteral("fixture_bytes"), QString::number(image.size()));
    report.insert(QStringLiteral("original_text"), QString::fromLatin1(kFixtureHfsHelloText));
    report.insert(QStringLiteral("original_sha256"),
                  QString::fromLatin1(
                      QCryptographicHash::hash(QByteArray(kFixtureHfsHelloText),
                                               QCryptographicHash::Sha256)
                          .toHex()));
    report.insert(QStringLiteral("original_resource_text"),
                  QString::fromLatin1(kFixtureHfsHelloResourceText));
    report.insert(QStringLiteral("original_resource_sha256"),
                  QString::fromLatin1(
                      QCryptographicHash::hash(QByteArray(kFixtureHfsHelloResourceText),
                                               QCryptographicHash::Sha256)
                          .toHex()));
    QStringList blockers;
    if (!writeBinaryFile(config.hfsBuildWriterFixturePath, image)) {
        blockers.append(QStringLiteral("Unable to write HFS writer fixture image"));
    }
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Built HFS writer fixture "
                        << QFileInfo(config.hfsBuildWriterFixturePath).fileName()
                        << Qt::endl;
    return 0;
}

sak::PartitionApfsWriteOptions apfsCertifierWriteOptions() {
    sak::PartitionApfsWriteOptions options;
    options.enable_experimental_writer = true;
    options.image_only = true;
    options.destructive_certification_evidence = true;
    options.allow_encrypted_or_protected_volume = true;
    options.allow_compressed_file_mutation = true;
    options.allow_snapshots = true;
    options.allow_multi_volume_container = true;
    options.max_payload_bytes = kDefaultApfsExportMaxFileBytes;
    options.evidence_id = QStringLiteral("certifier.apfs-image-only-format");
    return options;
}

QJsonObject apfsFormatBuildReportObject(
    const Config& config,
    const sak::PartitionApfsImageBuildResult& build) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  build.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    const bool rawTarget = sak::isWindowsRawDevicePath(build.image_path);
    report.insert(QStringLiteral("operation"),
                  rawTarget ? QStringLiteral("APFS raw target format")
                            : QStringLiteral("APFS image-only format build"));
    report.insert(QStringLiteral("image_path"),
                  rawTarget ? build.image_path : QFileInfo(build.image_path).absoluteFilePath());
    report.insert(QStringLiteral("image_sha256"), build.image_sha256);
    report.insert(QStringLiteral("target_container_bytes"),
                  QString::number(build.plan.target_container_bytes));
    report.insert(QStringLiteral("block_size_bytes"),
                  static_cast<int>(build.plan.block_size_bytes));
    report.insert(QStringLiteral("volume_name"), build.plan.volume_name);
    report.insert(QStringLiteral("seed_file_name"), config.apfsFormatSeedFileName);
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(build.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(build.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(build.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(build.blockers));
    return report;
}

QJsonObject apfsDetectionObject(const sak::PartitionFileSystemDetection& detection) {
    QJsonObject detectionObject;
    detectionObject.insert(QStringLiteral("file_system"), detection.file_system);
    detectionObject.insert(QStringLiteral("total_bytes"), QString::number(detection.total_bytes));
    detectionObject.insert(QStringLiteral("free_bytes"), QString::number(detection.free_bytes));
    detectionObject.insert(QStringLiteral("details"), stringArray(detection.details));
    return detectionObject;
}

struct ApfsDetectionValidation {
    QJsonObject* report{nullptr};
    QString imagePath;
    uint64_t imageSize{0};
    QString reportKey;
    QString missingMessage;
    QString wrongFileSystemMessage;
};

std::optional<sak::PartitionFileSystemDetection> appendApfsDetectionValidation(
    const ApfsDetectionValidation& validation,
    QStringList* blockers) {
    QString detectionError;
    const auto detection = sak::PartitionFileSystemDetector::detectFromDevicePath(
        validation.imagePath,
        0,
        validation.imageSize,
        &detectionError);
    if (!detection.has_value()) {
        blockers->append(detectionError.isEmpty() ? validation.missingMessage : detectionError);
        return std::nullopt;
    }
    validation.report->insert(validation.reportKey, apfsDetectionObject(*detection));
    if (detection->file_system.compare(QStringLiteral("APFS"), Qt::CaseInsensitive) != 0) {
        blockers->append(validation.wrongFileSystemMessage);
    }
    return detection;
}

QJsonObject apfsListingObject(const sak::PartitionApfsFileReadResult& listing) {
    return {{QStringLiteral("status"),
             listing.ok ? QStringLiteral("Passed") : QStringLiteral("Failed")},
            {QStringLiteral("volume_name"), listing.volume_name},
            {QStringLiteral("entry_count"), listing.entries.size()},
            {QStringLiteral("entries"), apfsEntriesArray(listing.entries)},
            {QStringLiteral("blockers"), blockersArray(listing.blockers, listing.warnings)}};
}

struct ApfsListingValidation {
    QJsonObject* report{nullptr};
    QString imagePath;
    QString reportKey;
    QString failureLabel;
};

sak::PartitionApfsFileReadResult appendApfsListingValidation(
    const ApfsListingValidation& validation,
    QStringList* blockers) {
    const auto listing = sak::PartitionApfsFileSystemReader::listDirectoryFromImage(
        validation.imagePath,
        QStringLiteral("/"),
        sak::kPartitionApfsDefaultBrowseEntryLimit);
    const QJsonObject listingObject = apfsListingObject(listing);
    validation.report->insert(validation.reportKey, listingObject);
    if (!listing.ok) {
        blockers->append(hfsOperationFailureBlocker(validation.failureLabel, listingObject));
    }
    return listing;
}

struct ApfsReadValidation {
    QJsonObject* report{nullptr};
    QString imagePath;
    QString path;
    uint64_t maxBytes{0};
    QString reportKey;
    QString failureLabel;
    QByteArray expectedData;
    QString mismatchMessage;
    bool compareExpected{false};
};

void appendApfsReadValidation(const ApfsReadValidation& validation, QStringList* blockers) {
    const auto readBack = sak::PartitionApfsFileSystemReader::readFileFromImage(
        validation.imagePath,
        validation.path,
        validation.maxBytes);
    const QByteArray hash = QCryptographicHash::hash(readBack.data, QCryptographicHash::Sha256);
    const QJsonObject readObject{{QStringLiteral("path"), validation.path},
                                 {QStringLiteral("status"),
                                  readBack.ok ? QStringLiteral("Passed")
                                              : QStringLiteral("Failed")},
                                 {QStringLiteral("bytes_read"), readBack.data.size()},
                                 {QStringLiteral("sha256"), QString::fromLatin1(hash.toHex())},
                                 {QStringLiteral("blockers"),
                                  blockersArray(readBack.blockers, readBack.warnings)}};
    validation.report->insert(validation.reportKey, readObject);
    if (!readBack.ok) {
        blockers->append(hfsOperationFailureBlocker(validation.failureLabel, readObject));
    } else if (validation.compareExpected && readBack.data != validation.expectedData) {
        blockers->append(validation.mismatchMessage);
    }
}

QStringList appendApfsFormatBuildValidation(QJsonObject* report,
                                            const Config& config,
                                            const sak::PartitionApfsImageBuildResult& build) {
    QStringList blockers;
    if (!report || !build.ok) {
        return blockers;
    }

    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = build.image_path,
         .imageSize = build.plan.target_container_bytes,
         .reportKey = QStringLiteral("generated_detection"),
         .missingMessage = QStringLiteral("Generated APFS image was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Generated image did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }

    const auto rootListing = appendApfsListingValidation(
        {.report = report,
         .imagePath = build.image_path,
         .reportKey = QStringLiteral("generated_apfs_listing"),
         .failureLabel = QStringLiteral("Generated APFS listing")},
        &blockers);
    if (rootListing.ok && rootListing.volume_name != build.plan.volume_name) {
        blockers.append(QStringLiteral("Generated APFS volume name did not round-trip"));
    }
    if (!config.apfsFormatSeedFileName.trimmed().isEmpty()) {
        const QString seedPath = QStringLiteral("/%1").arg(config.apfsFormatSeedFileName.trimmed());
        appendApfsReadValidation({.report = report,
                                  .imagePath = build.image_path,
                                  .path = seedPath,
                                  .maxBytes = kDefaultReadMaxBytes,
                                  .reportKey = QStringLiteral("generated_seed_file_read"),
                                  .failureLabel = QStringLiteral("Generated APFS seed read"),
                                  .expectedData = config.apfsFormatSeedFileText.toUtf8(),
                                  .mismatchMessage = QStringLiteral(
                                      "Generated APFS seed file contents did not round-trip"),
                                  .compareExpected = true},
                                 &blockers);
    }
    Q_UNUSED(config);
    return blockers;
}

int runApfsFormatBuildCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const sak::PartitionApfsImageFormatRequest request{
        .image_path = config.apfsBuildFormatImagePath,
        .target_container_bytes = config.apfsFormatSizeBytes,
        .block_size_bytes = static_cast<uint32_t>(config.apfsFormatBlockSizeBytes),
        .volume_name = config.apfsFormatVolumeName,
        .seed_file_name = config.apfsFormatSeedFileName,
        .seed_file_data = config.apfsFormatSeedFileText.toUtf8(),
        .options = apfsCertifierWriteOptions()};
    const auto build = config.apfsFormatSeedFileName.trimmed().isEmpty()
                           ? sak::PartitionApfsWriter::buildImageOnlyFormatImage(request)
                           : sak::PartitionApfsWriter::buildImageOnlyFormatImageWithSeedFile(
                                 request);
    QJsonObject report = apfsFormatBuildReportObject(config, build);
    QStringList blockers = build.blockers;
    blockers.append(appendApfsFormatBuildValidation(&report, config, build));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Built APFS image " << QFileInfo(build.image_path).fileName()
                        << Qt::endl;
    return 0;
}

sak::PartitionApfsWriteOptions apfsExistingFormatOptions(const Config& config) {
    auto options = apfsCertifierWriteOptions();
    if (config.apfsFormatAllowRawTarget) {
        options.image_only = false;
        options.raw_media_hardware_certification_evidence =
            config.apfsFormatRawHardwareProof;
        options.evidence_id = QStringLiteral("certifier.apfs-raw-existing-format");
    }
    return options;
}

sak::PartitionApfsWriteOptions apfsRawMutationOptions(const Config& config,
                                                      const QString& evidenceId) {
    auto options = apfsCertifierWriteOptions();
    options.image_only = false;
    options.raw_media_hardware_certification_evidence =
        config.apfsWriteRawHardwareProof || config.apfsRepairRawHardwareProof;
    options.evidence_id = evidenceId;
    return options;
}

int runApfsExistingFormatCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const auto build = sak::PartitionApfsWriter::formatExistingContainerTarget(
        {.image_path = config.apfsFormatExistingTargetPath,
         .target_container_bytes = config.apfsFormatSizeBytes,
         .block_size_bytes = static_cast<uint32_t>(config.apfsFormatBlockSizeBytes),
         .volume_name = config.apfsFormatVolumeName,
         .target_wipe_confirmed = config.apfsFormatTargetWipeConfirmed,
         .allow_raw_device_target = config.apfsFormatAllowRawTarget,
         .options = apfsExistingFormatOptions(config)});
    QJsonObject report = apfsFormatBuildReportObject(config, build);
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS existing-target format"));
    QStringList blockers = build.blockers;
    blockers.append(appendApfsFormatBuildValidation(&report, config, build));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Formatted APFS target "
                        << QFileInfo(build.image_path).fileName() << Qt::endl;
    return 0;
}

QJsonObject apfsFileWriteReportObject(
    const Config& config,
    const sak::PartitionApfsImageFileWriteResult& write) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  write.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS image-only root-file write"));
    report.insert(QStringLiteral("source_image_path"),
                  QFileInfo(write.source_image_path).absoluteFilePath());
    report.insert(QStringLiteral("written_image_path"),
                  QFileInfo(write.written_image_path).absoluteFilePath());
    report.insert(QStringLiteral("written_image_sha256"), write.written_image_sha256);
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(write.written_data_blocks));
    report.insert(QStringLiteral("file_name"), config.apfsWriteRootFileName);
    report.insert(QStringLiteral("plan_operation"), write.plan.operation);
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(write.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(write.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(write.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(write.blockers));
    return report;
}

QStringList appendApfsFileWriteValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsImageFileWriteResult& write) {
    QStringList blockers;
    if (!report || !write.ok) {
        return blockers;
    }

    const QFileInfo writtenInfo(write.written_image_path);
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = write.written_image_path,
         .imageSize = static_cast<uint64_t>(writtenInfo.size()),
         .reportKey = QStringLiteral("written_detection"),
         .missingMessage = QStringLiteral("Written APFS image was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Written image did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }

    appendApfsListingValidation({.report = report,
                                 .imagePath = write.written_image_path,
                                 .reportKey = QStringLiteral("written_apfs_listing"),
                                 .failureLabel = QStringLiteral("Written APFS listing")},
                                &blockers);

    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsWriteRootFileName.trimmed());
    appendApfsReadValidation({.report = report,
                              .imagePath = write.written_image_path,
                              .path = readPath,
                              .maxBytes = static_cast<uint64_t>(
                                  config.apfsWriteRootFileText.toUtf8().size()),
                              .reportKey = QStringLiteral("written_file_read"),
                              .failureLabel = QStringLiteral("Written APFS read"),
                              .expectedData = config.apfsWriteRootFileText.toUtf8(),
                              .mismatchMessage = QStringLiteral(
                                  "Written APFS file contents did not round-trip"),
                              .compareExpected = true},
                             &blockers);
    return blockers;
}

int runApfsRootFileWriteCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const auto write = sak::PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = config.inputPath,
         .written_image_path = config.apfsWriteRootFileImagePath,
         .file_name = config.apfsWriteRootFileName,
         .file_data = config.apfsWriteRootFileText.toUtf8(),
         .options = apfsCertifierWriteOptions()});
    QJsonObject report = apfsFileWriteReportObject(config, write);
    QStringList blockers = write.blockers;
    blockers.append(appendApfsFileWriteValidation(&report, config, write));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Wrote APFS image "
                        << QFileInfo(write.written_image_path).fileName() << Qt::endl;
    return 0;
}

QJsonObject apfsFilePatchReportObject(
    const Config& config,
    const sak::PartitionApfsImageFilePatchResult& patch) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  patch.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS image-only root-file patch"));
    report.insert(QStringLiteral("source_image_path"),
                  QFileInfo(patch.source_image_path).absoluteFilePath());
    report.insert(QStringLiteral("written_image_path"),
                  QFileInfo(patch.written_image_path).absoluteFilePath());
    report.insert(QStringLiteral("written_image_sha256"), patch.written_image_sha256);
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(patch.written_data_blocks));
    report.insert(QStringLiteral("file_name"), config.apfsPatchRootFileName);
    report.insert(QStringLiteral("file_bytes"), QString::number(patch.file_bytes));
    report.insert(QStringLiteral("patch_offset_bytes"),
                  QString::number(patch.patch_offset_bytes));
    report.insert(QStringLiteral("patch_bytes"), QString::number(patch.patch_bytes));
    report.insert(QStringLiteral("patch_sha256"), patch.patch_sha256);
    report.insert(QStringLiteral("readback_sha256"), patch.readback_sha256);
    report.insert(QStringLiteral("plan_operation"), patch.plan.operation);
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(patch.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(patch.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(patch.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(patch.blockers));
    return report;
}

QStringList appendApfsFilePatchValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsImageFilePatchResult& patch,
    const QByteArray& expectedData) {
    QStringList blockers;
    if (!report || !patch.ok) {
        return blockers;
    }

    const QFileInfo writtenInfo(patch.written_image_path);
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = patch.written_image_path,
         .imageSize = static_cast<uint64_t>(writtenInfo.size()),
         .reportKey = QStringLiteral("patched_detection"),
         .missingMessage = QStringLiteral("Patched APFS image was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Patched image did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }

    appendApfsListingValidation({.report = report,
                                 .imagePath = patch.written_image_path,
                                 .reportKey = QStringLiteral("patched_apfs_listing"),
                                 .failureLabel = QStringLiteral("Patched APFS listing")},
                                &blockers);

    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsPatchRootFileName.trimmed());
    appendApfsReadValidation({.report = report,
                              .imagePath = patch.written_image_path,
                              .path = readPath,
                              .maxBytes = static_cast<uint64_t>(expectedData.size()),
                              .reportKey = QStringLiteral("patched_file_read"),
                              .failureLabel = QStringLiteral("Patched APFS read"),
                              .expectedData = expectedData,
                              .mismatchMessage = QStringLiteral(
                                  "Patched APFS file contents did not match expected bytes"),
                              .compareExpected = true},
                             &blockers);
    return blockers;
}

std::optional<QByteArray> expectedApfsPatchData(const Config& config,
                                                QStringList* blockers) {
    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsPatchRootFileName.trimmed());
    const auto original = sak::PartitionApfsFileSystemReader::readFileFromImage(
        config.inputPath,
        readPath,
        kDefaultApfsExportMaxFileBytes);
    if (!original.ok) {
        blockers->append(QStringLiteral("APFS patch source read failed: %1")
                             .arg(original.blockers.join(QStringLiteral("; "))));
        return std::nullopt;
    }

    QByteArray expected = original.data;
    const QByteArray patchBytes = config.apfsPatchRootFileText.toUtf8();
    const uint64_t fileBytes = static_cast<uint64_t>(expected.size());
    const uint64_t patchBytesCount = static_cast<uint64_t>(patchBytes.size());
    if (config.apfsPatchRootFileOffsetBytes > fileBytes ||
        patchBytesCount > fileBytes - config.apfsPatchRootFileOffsetBytes) {
        blockers->append(QStringLiteral(
            "APFS patch expected range must stay inside existing file"));
        return std::nullopt;
    }
    std::copy(patchBytes.cbegin(),
              patchBytes.cend(),
              expected.begin() +
                  static_cast<qsizetype>(config.apfsPatchRootFileOffsetBytes));
    return expected;
}

int runApfsRootFilePatchCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    QStringList expectedBlockers;
    const auto expected = expectedApfsPatchData(config, &expectedBlockers);
    if (!expected.has_value()) {
        return fail(config, expectedBlockers.join(QStringLiteral("; ")));
    }

    const auto patch = sak::PartitionApfsWriter::patchImageOnlyRootFile(
        {.source_image_path = config.inputPath,
         .written_image_path = config.apfsPatchRootFileImagePath,
         .file_name = config.apfsPatchRootFileName,
         .patch_offset_bytes = config.apfsPatchRootFileOffsetBytes,
         .patch_data = config.apfsPatchRootFileText.toUtf8(),
         .options = apfsCertifierWriteOptions()});
    QJsonObject report = apfsFilePatchReportObject(config, patch);
    QStringList blockers = patch.blockers;
    blockers.append(appendApfsFilePatchValidation(&report, config, patch, *expected));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Patched APFS image "
                        << QFileInfo(patch.written_image_path).fileName() << Qt::endl;
    return 0;
}

QJsonObject apfsFileDeleteReportObject(
    const Config& config,
    const sak::PartitionApfsImageFileDeleteResult& deleted) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  deleted.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS image-only root-file delete"));
    report.insert(QStringLiteral("source_image_path"),
                  QFileInfo(deleted.source_image_path).absoluteFilePath());
    report.insert(QStringLiteral("written_image_path"),
                  QFileInfo(deleted.written_image_path).absoluteFilePath());
    report.insert(QStringLiteral("written_image_sha256"), deleted.written_image_sha256);
    report.insert(QStringLiteral("deleted_file_bytes"),
                  QString::number(deleted.deleted_file_bytes));
    report.insert(QStringLiteral("deleted_file_sha256"), deleted.deleted_file_sha256);
    report.insert(QStringLiteral("freed_data_blocks"),
                  QString::number(deleted.freed_data_blocks));
    report.insert(QStringLiteral("file_name"), config.apfsDeleteRootFileName);
    report.insert(QStringLiteral("plan_operation"), deleted.plan.operation);
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(deleted.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(deleted.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(deleted.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(deleted.blockers));
    return report;
}

QStringList appendApfsFileDeleteValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsImageFileDeleteResult& deleted) {
    QStringList blockers;
    if (!report || !deleted.ok) {
        return blockers;
    }

    const QFileInfo writtenInfo(deleted.written_image_path);
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = deleted.written_image_path,
         .imageSize = static_cast<uint64_t>(writtenInfo.size()),
         .reportKey = QStringLiteral("deleted_detection"),
         .missingMessage = QStringLiteral("Deleted APFS image was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Deleted image did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }

    appendApfsListingValidation({.report = report,
                                 .imagePath = deleted.written_image_path,
                                 .reportKey = QStringLiteral("deleted_apfs_listing"),
                                 .failureLabel = QStringLiteral("Deleted APFS listing")},
                                &blockers);

    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsDeleteRootFileName.trimmed());
    const auto readBack = sak::PartitionApfsFileSystemReader::readFileFromImage(
        deleted.written_image_path,
        readPath,
        1);
    report->insert(QStringLiteral("deleted_file_negative_read"),
                   QJsonObject{{QStringLiteral("status"),
                                readBack.ok ? QStringLiteral("Failed")
                                            : QStringLiteral("Passed")},
                               {QStringLiteral("path"), readPath},
                               {QStringLiteral("blockers"), stringArray(readBack.blockers)}});
    if (readBack.ok) {
        blockers.append(QStringLiteral("Deleted APFS file was still readable"));
    }
    return blockers;
}

int runApfsRootFileDeleteCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const auto deleted = sak::PartitionApfsWriter::deleteImageOnlyRootFile(
        {.source_image_path = config.inputPath,
         .written_image_path = config.apfsDeleteRootFileImagePath,
         .file_name = config.apfsDeleteRootFileName,
         .options = apfsCertifierWriteOptions()});
    QJsonObject report = apfsFileDeleteReportObject(config, deleted);
    QStringList blockers = deleted.blockers;
    blockers.append(appendApfsFileDeleteValidation(&report, config, deleted));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Deleted APFS root file into "
                        << QFileInfo(deleted.written_image_path).fileName() << Qt::endl;
    return 0;
}

QJsonObject apfsRawFileWriteReportObject(
    const Config& config,
    const sak::PartitionApfsRawFileWriteResult& write) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  write.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS raw root-file write"));
    report.insert(QStringLiteral("target_path"), write.target_path);
    report.insert(QStringLiteral("target_container_bytes"),
                  QString::number(config.apfsWriteTargetSizeBytes));
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(write.written_data_blocks));
    report.insert(QStringLiteral("file_name"), config.apfsWriteRootFileName);
    report.insert(QStringLiteral("plan_operation"), write.plan.operation);
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(write.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(write.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(write.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(write.blockers));
    return report;
}

QStringList appendApfsRawFileWriteValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsRawFileWriteResult& write) {
    QStringList blockers;
    if (!report || !write.ok) {
        return blockers;
    }
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = write.target_path,
         .imageSize = config.apfsWriteTargetSizeBytes,
         .reportKey = QStringLiteral("raw_written_detection"),
         .missingMessage = QStringLiteral("Raw written APFS target was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Raw written target did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }
    appendApfsListingValidation({.report = report,
                                 .imagePath = write.target_path,
                                 .reportKey = QStringLiteral("raw_written_apfs_listing"),
                                 .failureLabel = QStringLiteral("Raw written APFS listing")},
                                &blockers);
    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsWriteRootFileName.trimmed());
    appendApfsReadValidation({.report = report,
                              .imagePath = write.target_path,
                              .path = readPath,
                              .maxBytes = static_cast<uint64_t>(
                                  config.apfsWriteRootFileText.toUtf8().size()),
                              .reportKey = QStringLiteral("raw_written_file_read"),
                              .failureLabel = QStringLiteral("Raw written APFS read"),
                              .expectedData = config.apfsWriteRootFileText.toUtf8(),
                              .mismatchMessage = QStringLiteral(
                                  "Raw written APFS file contents did not round-trip"),
                              .compareExpected = true},
                             &blockers);
    return blockers;
}

int runApfsRawRootFileWriteCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const auto write = sak::PartitionApfsWriter::writeRawRootFile(
        {.target_path = config.apfsWriteRootFileTargetPath,
         .target_container_bytes = config.apfsWriteTargetSizeBytes,
         .file_name = config.apfsWriteRootFileName,
         .file_data = config.apfsWriteRootFileText.toUtf8(),
         .target_write_confirmed = config.apfsWriteTargetConfirmed,
         .allow_raw_device_target = config.apfsWriteAllowRawTarget,
         .options = apfsRawMutationOptions(
             config,
             QStringLiteral("certifier.apfs-raw-root-file-write"))});
    QJsonObject report = apfsRawFileWriteReportObject(config, write);
    QStringList blockers = write.blockers;
    blockers.append(appendApfsRawFileWriteValidation(&report, config, write));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Wrote APFS raw target "
                        << config.apfsWriteRootFileTargetPath << Qt::endl;
    return 0;
}

QJsonObject apfsRawFilePatchReportObject(
    const Config& config,
    const sak::PartitionApfsRawFilePatchResult& patch) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  patch.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS raw root-file patch"));
    report.insert(QStringLiteral("target_path"), patch.target_path);
    report.insert(QStringLiteral("target_container_bytes"),
                  QString::number(config.apfsWriteTargetSizeBytes));
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(patch.written_data_blocks));
    report.insert(QStringLiteral("file_name"), config.apfsPatchRootFileName);
    report.insert(QStringLiteral("file_bytes"), QString::number(patch.file_bytes));
    report.insert(QStringLiteral("patch_offset_bytes"),
                  QString::number(patch.patch_offset_bytes));
    report.insert(QStringLiteral("patch_bytes"), QString::number(patch.patch_bytes));
    report.insert(QStringLiteral("patch_sha256"), patch.patch_sha256);
    report.insert(QStringLiteral("readback_sha256"), patch.readback_sha256);
    report.insert(QStringLiteral("plan_operation"), patch.plan.operation);
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(patch.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(patch.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(patch.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(patch.blockers));
    return report;
}

QStringList appendApfsRawFilePatchValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsRawFilePatchResult& patch,
    const QByteArray& expectedData) {
    QStringList blockers;
    if (!report || !patch.ok) {
        return blockers;
    }
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = patch.target_path,
         .imageSize = config.apfsWriteTargetSizeBytes,
         .reportKey = QStringLiteral("raw_patched_detection"),
         .missingMessage = QStringLiteral("Raw patched APFS target was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Raw patched target did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }
    appendApfsListingValidation({.report = report,
                                 .imagePath = patch.target_path,
                                 .reportKey = QStringLiteral("raw_patched_apfs_listing"),
                                 .failureLabel = QStringLiteral("Raw patched APFS listing")},
                                &blockers);
    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsPatchRootFileName.trimmed());
    appendApfsReadValidation({.report = report,
                              .imagePath = patch.target_path,
                              .path = readPath,
                              .maxBytes = static_cast<uint64_t>(expectedData.size()),
                              .reportKey = QStringLiteral("raw_patched_file_read"),
                              .failureLabel = QStringLiteral("Raw patched APFS read"),
                              .expectedData = expectedData,
                              .mismatchMessage = QStringLiteral(
                                  "Raw patched APFS file contents did not match expected bytes"),
                              .compareExpected = true},
                             &blockers);
    return blockers;
}

std::optional<QByteArray> expectedApfsRawPatchData(const Config& config,
                                                   QStringList* blockers) {
    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsPatchRootFileName.trimmed());
    const auto original = sak::PartitionApfsFileSystemReader::readFileFromImage(
        config.apfsPatchRootFileTargetPath,
        readPath,
        kDefaultApfsExportMaxFileBytes);
    if (!original.ok) {
        blockers->append(QStringLiteral("APFS raw patch source read failed: %1")
                             .arg(original.blockers.join(QStringLiteral("; "))));
        return std::nullopt;
    }

    QByteArray expected = original.data;
    const QByteArray patchBytes = config.apfsPatchRootFileText.toUtf8();
    const uint64_t fileBytes = static_cast<uint64_t>(expected.size());
    const uint64_t patchBytesCount = static_cast<uint64_t>(patchBytes.size());
    if (config.apfsPatchRootFileOffsetBytes > fileBytes ||
        patchBytesCount > fileBytes - config.apfsPatchRootFileOffsetBytes) {
        blockers->append(QStringLiteral(
            "APFS raw patch expected range must stay inside existing file"));
        return std::nullopt;
    }
    std::copy(patchBytes.cbegin(),
              patchBytes.cend(),
              expected.begin() +
                  static_cast<qsizetype>(config.apfsPatchRootFileOffsetBytes));
    return expected;
}

int runApfsRawRootFilePatchCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    QStringList expectedBlockers;
    const auto expected = expectedApfsRawPatchData(config, &expectedBlockers);
    if (!expected.has_value()) {
        return fail(config, expectedBlockers.join(QStringLiteral("; ")));
    }

    const auto patch = sak::PartitionApfsWriter::patchRawRootFile(
        {.target_path = config.apfsPatchRootFileTargetPath,
         .target_container_bytes = config.apfsWriteTargetSizeBytes,
         .file_name = config.apfsPatchRootFileName,
         .patch_offset_bytes = config.apfsPatchRootFileOffsetBytes,
         .patch_data = config.apfsPatchRootFileText.toUtf8(),
         .target_write_confirmed = config.apfsWriteTargetConfirmed,
         .allow_raw_device_target = config.apfsWriteAllowRawTarget,
         .options = apfsRawMutationOptions(
             config,
             QStringLiteral("certifier.apfs-raw-root-file-patch"))});
    QJsonObject report = apfsRawFilePatchReportObject(config, patch);
    QStringList blockers = patch.blockers;
    blockers.append(appendApfsRawFilePatchValidation(&report, config, patch, *expected));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Patched APFS raw target "
                        << config.apfsPatchRootFileTargetPath << Qt::endl;
    return 0;
}

QJsonObject apfsRawFileDeleteReportObject(
    const Config& config,
    const sak::PartitionApfsRawFileDeleteResult& deleted) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  deleted.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS raw root-file delete"));
    report.insert(QStringLiteral("target_path"), deleted.target_path);
    report.insert(QStringLiteral("target_container_bytes"),
                  QString::number(config.apfsWriteTargetSizeBytes));
    report.insert(QStringLiteral("deleted_file_bytes"),
                  QString::number(deleted.deleted_file_bytes));
    report.insert(QStringLiteral("deleted_file_sha256"), deleted.deleted_file_sha256);
    report.insert(QStringLiteral("freed_data_blocks"),
                  QString::number(deleted.freed_data_blocks));
    report.insert(QStringLiteral("file_name"), config.apfsDeleteRootFileName);
    report.insert(QStringLiteral("plan_operation"), deleted.plan.operation);
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(deleted.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(deleted.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(deleted.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(deleted.blockers));
    return report;
}

QStringList appendApfsRawFileDeleteValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsRawFileDeleteResult& deleted) {
    QStringList blockers;
    if (!report || !deleted.ok) {
        return blockers;
    }
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = deleted.target_path,
         .imageSize = config.apfsWriteTargetSizeBytes,
         .reportKey = QStringLiteral("raw_deleted_detection"),
         .missingMessage = QStringLiteral("Raw deleted APFS target was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Raw deleted target did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }
    appendApfsListingValidation({.report = report,
                                 .imagePath = deleted.target_path,
                                 .reportKey = QStringLiteral("raw_deleted_apfs_listing"),
                                 .failureLabel = QStringLiteral("Raw deleted APFS listing")},
                                &blockers);

    const QString readPath =
        QStringLiteral("/%1").arg(config.apfsDeleteRootFileName.trimmed());
    const auto readBack = sak::PartitionApfsFileSystemReader::readFileFromImage(
        deleted.target_path,
        readPath,
        1);
    report->insert(QStringLiteral("raw_deleted_file_negative_read"),
                   QJsonObject{{QStringLiteral("status"),
                                readBack.ok ? QStringLiteral("Failed")
                                            : QStringLiteral("Passed")},
                               {QStringLiteral("path"), readPath},
                               {QStringLiteral("blockers"), stringArray(readBack.blockers)}});
    if (readBack.ok) {
        blockers.append(QStringLiteral("Raw deleted APFS file was still readable"));
    }
    return blockers;
}

int runApfsRawRootFileDeleteCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const auto deleted = sak::PartitionApfsWriter::deleteRawRootFile(
        {.target_path = config.apfsDeleteRootFileTargetPath,
         .target_container_bytes = config.apfsWriteTargetSizeBytes,
         .file_name = config.apfsDeleteRootFileName,
         .target_write_confirmed = config.apfsWriteTargetConfirmed,
         .allow_raw_device_target = config.apfsWriteAllowRawTarget,
         .options = apfsRawMutationOptions(
             config,
             QStringLiteral("certifier.apfs-raw-root-file-delete"))});
    QJsonObject report = apfsRawFileDeleteReportObject(config, deleted);
    QStringList blockers = deleted.blockers;
    blockers.append(appendApfsRawFileDeleteValidation(&report, config, deleted));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Deleted APFS raw root file from "
                        << config.apfsDeleteRootFileTargetPath << Qt::endl;
    return 0;
}

QJsonObject apfsRepairReportObject(
    const Config& config,
    const sak::PartitionApfsImageRepairResult& repair) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  repair.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS image-only object-checksum repair"));
    report.insert(QStringLiteral("source_image_path"),
                  QFileInfo(repair.source_image_path).absoluteFilePath());
    report.insert(QStringLiteral("repaired_image_path"),
                  QFileInfo(repair.repaired_image_path).absoluteFilePath());
    report.insert(QStringLiteral("repaired_image_sha256"), repair.repaired_image_sha256);
    report.insert(QStringLiteral("scanned_blocks"),
                  QString::number(repair.scanned_blocks));
    report.insert(QStringLiteral("repaired_checksum_blocks"),
                  QString::number(repair.repaired_checksum_blocks));
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(repair.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(repair.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(repair.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(repair.blockers));
    return report;
}

QStringList appendApfsRepairValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsImageRepairResult& repair) {
    QStringList blockers;
    if (!report || !repair.ok) {
        return blockers;
    }

    const QFileInfo repairedInfo(repair.repaired_image_path);
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = repair.repaired_image_path,
         .imageSize = static_cast<uint64_t>(repairedInfo.size()),
         .reportKey = QStringLiteral("repaired_detection"),
         .missingMessage = QStringLiteral("Repaired APFS image was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Repaired image did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }

    appendApfsListingValidation({.report = report,
                                 .imagePath = repair.repaired_image_path,
                                 .reportKey = QStringLiteral("repaired_apfs_listing"),
                                 .failureLabel = QStringLiteral("Repaired APFS listing")},
                                &blockers);

    if (!config.apfsRepairReadFilePath.trimmed().isEmpty()) {
        appendApfsReadValidation({.report = report,
                                  .imagePath = repair.repaired_image_path,
                                  .path = config.apfsRepairReadFilePath,
                                  .maxBytes = config.apfsRepairReadMaxBytes,
                                  .reportKey = QStringLiteral("repaired_file_read"),
                                  .failureLabel = QStringLiteral("Repaired APFS read")},
                                 &blockers);
    }
    return blockers;
}

int runApfsRepairCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    const auto repair = sak::PartitionApfsWriter::repairImageOnlyObjectChecksums(
        {.source_image_path = config.inputPath,
         .repaired_image_path = config.apfsRepairObjectChecksumsImagePath,
         .options = apfsCertifierWriteOptions()});
    QJsonObject report = apfsRepairReportObject(config, repair);
    QStringList blockers = repair.blockers;
    blockers.append(appendApfsRepairValidation(&report, config, repair));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Repaired APFS image "
                        << QFileInfo(repair.repaired_image_path).fileName() << Qt::endl;
    return 0;
}

bool corruptApfsRawMetadataChecksumBlock(const QString& targetPath,
                                         uint64_t blockIndex,
                                         QStringList* blockers) {
    QString openError;
    auto target = sak::openFileOrRawDeviceReadWrite(targetPath, &openError);
    if (!target) {
        blockers->append(QStringLiteral("Unable to open APFS raw corruption target: %1")
                             .arg(openError));
        return false;
    }
    const uint64_t offset = blockIndex * 4096ULL;
    if (offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) ||
        !target->seek(static_cast<qint64>(offset))) {
        blockers->append(QStringLiteral("Unable to seek APFS raw corruption block %1")
                             .arg(blockIndex));
        return false;
    }
    QByteArray block(4096, '\0');
    if (target->read(block.data(), block.size()) != block.size()) {
        blockers->append(QStringLiteral("Unable to read APFS raw corruption block %1")
                             .arg(blockIndex));
        return false;
    }
    block[0] = static_cast<char>(block.at(0) ^ 0x5A);
    if (!target->seek(static_cast<qint64>(offset)) ||
        target->write(block) != block.size()) {
        blockers->append(QStringLiteral("Unable to write APFS raw corruption block %1: %2")
                             .arg(blockIndex)
                             .arg(target->errorString()));
        return false;
    }
    target->close();
    return true;
}

QJsonObject apfsRawRepairReportObject(
    const Config& config,
    const sak::PartitionApfsRawRepairResult& repair) {
    QJsonObject report = baseReport(config);
    report.insert(QStringLiteral("status"),
                  repair.ok ? QStringLiteral("Passed") : QStringLiteral("Failed"));
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS raw object-checksum repair"));
    report.insert(QStringLiteral("target_path"), repair.target_path);
    report.insert(QStringLiteral("target_container_bytes"),
                  QString::number(config.apfsRepairTargetSizeBytes));
    report.insert(QStringLiteral("scanned_blocks"), QString::number(repair.scanned_blocks));
    report.insert(QStringLiteral("repaired_checksum_blocks"),
                  QString::number(repair.repaired_checksum_blocks));
    report.insert(QStringLiteral("corrupted_metadata_block"),
                  config.apfsRepairCorruptMetadataBlockProvided
                      ? QString::number(config.apfsRepairCorruptMetadataBlock)
                      : QString());
    report.insert(QStringLiteral("plan_steps"), apfsPlanStepsArray(repair.plan.steps));
    report.insert(QStringLiteral("post_apply_verification"),
                  stringArray(repair.plan.post_apply_verification));
    report.insert(QStringLiteral("warnings"), stringArray(repair.warnings));
    report.insert(QStringLiteral("blockers"), stringArray(repair.blockers));
    return report;
}

QStringList appendApfsRawRepairValidation(
    QJsonObject* report,
    const Config& config,
    const sak::PartitionApfsRawRepairResult& repair) {
    QStringList blockers;
    if (!report || !repair.ok) {
        return blockers;
    }
    const auto detection = appendApfsDetectionValidation(
        {.report = report,
         .imagePath = repair.target_path,
         .imageSize = config.apfsRepairTargetSizeBytes,
         .reportKey = QStringLiteral("raw_repaired_detection"),
         .missingMessage = QStringLiteral("Raw repaired APFS target was not detected"),
         .wrongFileSystemMessage = QStringLiteral("Raw repaired target did not detect as APFS")},
        &blockers);
    if (!detection.has_value()) {
        return blockers;
    }
    appendApfsListingValidation({.report = report,
                                 .imagePath = repair.target_path,
                                 .reportKey = QStringLiteral("raw_repaired_apfs_listing"),
                                 .failureLabel = QStringLiteral("Raw repaired APFS listing")},
                                &blockers);
    if (!config.apfsRepairReadFilePath.trimmed().isEmpty()) {
        appendApfsReadValidation({.report = report,
                                  .imagePath = repair.target_path,
                                  .path = config.apfsRepairReadFilePath,
                                  .maxBytes = config.apfsRepairReadMaxBytes,
                                  .reportKey = QStringLiteral("raw_repaired_file_read"),
                                  .failureLabel = QStringLiteral("Raw repaired APFS read")},
                                 &blockers);
    }
    return blockers;
}

int runApfsRawRepairCertifier(const Config& config) {
    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    QStringList preRepairBlockers;
    if (config.apfsRepairCorruptMetadataBlockProvided) {
        corruptApfsRawMetadataChecksumBlock(config.apfsRepairObjectChecksumsTargetPath,
                                           config.apfsRepairCorruptMetadataBlock,
                                           &preRepairBlockers);
    }
    const auto repair = sak::PartitionApfsWriter::repairRawObjectChecksums(
        {.target_path = config.apfsRepairObjectChecksumsTargetPath,
         .target_container_bytes = config.apfsRepairTargetSizeBytes,
         .target_repair_confirmed = config.apfsRepairTargetConfirmed,
         .allow_raw_device_target = config.apfsRepairAllowRawTarget,
         .options = apfsRawMutationOptions(
             config,
             QStringLiteral("certifier.apfs-raw-object-checksum-repair"))});
    QJsonObject report = apfsRawRepairReportObject(config, repair);
    QStringList blockers = preRepairBlockers;
    blockers.append(repair.blockers);
    blockers.append(appendApfsRawRepairValidation(&report, config, repair));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Repaired APFS raw target "
                        << config.apfsRepairObjectChecksumsTargetPath << Qt::endl;
    return 0;
}

using CertifierRunner = int (*)(const Config&);

struct CertifierRoute {
    bool requested{false};
    CertifierRunner runner{nullptr};
};

int runCertifier(const Config& config) {
    const std::array<CertifierRoute, 11> routes{{
        {hfsWriterFixtureRequested(config), runHfsWriterFixtureCertifier},
        {apfsFormatBuildRequested(config), runApfsFormatBuildCertifier},
        {apfsExistingFormatRequested(config), runApfsExistingFormatCertifier},
        {apfsRootFileWriteRequested(config), runApfsRootFileWriteCertifier},
        {apfsRootFilePatchRequested(config), runApfsRootFilePatchCertifier},
        {apfsRootFileDeleteRequested(config), runApfsRootFileDeleteCertifier},
        {apfsRawRootFileWriteRequested(config), runApfsRawRootFileWriteCertifier},
        {apfsRawRootFilePatchRequested(config), runApfsRawRootFilePatchCertifier},
        {apfsRawRootFileDeleteRequested(config), runApfsRawRootFileDeleteCertifier},
        {apfsRepairRequested(config), runApfsRepairCertifier},
        {apfsRawRepairRequested(config), runApfsRawRepairCertifier},
    }};
    for (const auto& route : routes) {
        if (route.requested && route.runner) {
            return route.runner(config);
        }
    }

    const QStringList configErrors = validateConfig(config);
    if (!configErrors.isEmpty()) {
        return fail(config, configErrors.join(QStringLiteral("; ")));
    }

    QString detectionError;
    uint64_t inputSizeBytes = 0;
    const auto detection = detectInput(config, &inputSizeBytes, &detectionError);
    if (!detection.has_value()) {
        return fail(config, detectionError);
    }

    QStringList blockers = validationBlockers(config, *detection);
    QJsonObject report = detectionReport(config, *detection, inputSizeBytes, blockers);
    blockers.append(appendHfsOperationReports(&report, config, *detection));
    blockers.append(appendApfsOperationReports(&report, config, *detection));
    report.insert(QStringLiteral("status"), blockers.isEmpty() ? QStringLiteral("Passed")
                                                               : QStringLiteral("Failed"));
    report.insert(QStringLiteral("blockers"), stringArray(blockers));
    if (!writeJsonFile(config.outputPath, report)) {
        QTextStream(stderr) << "Failed to write output report" << Qt::endl;
        return 1;
    }
    if (!blockers.isEmpty()) {
        QTextStream(stderr) << blockers.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "Detected " << detection->file_system << " in "
                        << QFileInfo(config.inputPath).fileName() << Qt::endl;
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    return runCertifier(parseConfig(app.arguments()));
}
