// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file sak_apfs_writer_cli.cpp
/// @brief Production command-line bridge for certified S.A.K. APFS writer operations.

#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_apfs_writer.h"
#include "sak/secure_memory.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringConverter>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace {

constexpr uint32_t kDefaultApfsBlockSizeBytes = 4096;
constexpr uint64_t kDefaultApfsMaxPayloadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr int kExitOk = 0;
constexpr int kExitOperationFailed = 1;
constexpr int kExitInvalidArguments = 2;
constexpr int kExitReportFailed = 3;

// readAll slurps a whole file into RAM, so cap both inputs to bound peak memory. A seed
// payload can be large (it becomes a file in the image) but not unbounded; a credential file
// holds a password, which is tiny.
constexpr qint64 kMaxSeedPayloadFileBytes = 2LL * 1024 * 1024 * 1024;
constexpr qint64 kMaxCredentialFileBytes = 1LL * 1024 * 1024;

QJsonArray toJsonArray(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
}

std::optional<uint64_t> parseUInt64Option(const QCommandLineParser& parser,
                                          const QCommandLineOption& option,
                                          QString* error) {
    bool ok = false;
    const uint64_t parsed = parser.value(option).trimmed().toULongLong(&ok);
    if (!ok || parsed == 0) {
        *error = QStringLiteral("Option --%1 must be a positive unsigned integer")
                     .arg(option.names().constFirst());
        return std::nullopt;
    }
    return parsed;
}

std::optional<uint64_t> parseNonNegativeUInt64Option(const QCommandLineParser& parser,
                                                     const QCommandLineOption& option,
                                                     QString* error) {
    bool ok = false;
    const uint64_t parsed = parser.value(option).trimmed().toULongLong(&ok);
    if (!ok) {
        *error = QStringLiteral("Option --%1 must be a non-negative unsigned integer")
                     .arg(option.names().constFirst());
        return std::nullopt;
    }
    return parsed;
}

uint32_t parseBlockSizeOption(const QCommandLineParser& parser,
                              const QCommandLineOption& option,
                              QString* error) {
    if (!parser.isSet(option)) {
        return kDefaultApfsBlockSizeBytes;
    }
    const auto parsed = parseUInt64Option(parser, option, error);
    if (!parsed.has_value()) {
        return 0;
    }
    if (*parsed > std::numeric_limits<uint32_t>::max()) {
        *error =
            QStringLiteral("Option --%1 is outside uint32 range").arg(option.names().constFirst());
        return 0;
    }
    return static_cast<uint32_t>(*parsed);
}

std::optional<QByteArray> readPayloadFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to read payload file: %1").arg(file.errorString());
        return std::nullopt;
    }
    const qint64 expected = file.size();
    if (expected > kMaxSeedPayloadFileBytes) {
        *error =
            QStringLiteral("Payload file exceeds the %1-byte limit").arg(kMaxSeedPayloadFileBytes);
        return std::nullopt;
    }
    // Fail closed on a read error or short read: never write a truncated payload into the image.
    const QByteArray data = file.readAll();
    if (file.error() != QFile::NoError || data.size() != expected) {
        *error = QStringLiteral("Unable to fully read payload file: %1").arg(file.errorString());
        return std::nullopt;
    }
    return data;
}

sak::PartitionApfsWriteOptions imageWriteOptions(const QString& evidenceId) {
    sak::PartitionApfsWriteOptions options;
    options.enable_experimental_writer = true;
    options.image_only = true;
    // By design for this tool tier: sak_apfs_writer_cli is the manual, elevated certifier
    // whose sole purpose is to drive certified writes, so it asserts the engine's
    // destructive/hardware evidence gate itself (see review finding 4).
    options.destructive_certification_evidence = true;
    options.max_payload_bytes = kDefaultApfsMaxPayloadBytes;
    options.evidence_id = evidenceId;
    return options;
}

sak::PartitionApfsWriteOptions rawWriteOptions(const QString& evidenceId) {
    auto options = imageWriteOptions(evidenceId);
    options.image_only = false;
    options.raw_media_hardware_certification_evidence = true;
    return options;
}

void insertPlan(QJsonObject* report, const sak::PartitionApfsImageMutationPlan& plan) {
    if (!report || plan.operation.isEmpty()) {
        return;
    }
    report->insert(QStringLiteral("plan_operation"), plan.operation);
    report->insert(QStringLiteral("volume_name"), plan.volume_name);
    report->insert(QStringLiteral("target_container_bytes"),
                   QString::number(plan.target_container_bytes));
    report->insert(QStringLiteral("block_size_bytes"), static_cast<int>(plan.block_size_bytes));
    report->insert(QStringLiteral("execution_blockers"), toJsonArray(plan.execution_blockers));
    report->insert(QStringLiteral("post_apply_verification"),
                   toJsonArray(plan.post_apply_verification));
}

QJsonObject formatReport(const QString& command, const sak::PartitionApfsImageBuildResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("image_path"), result.image_path);
    report.insert(QStringLiteral("image_sha256"), result.image_sha256);
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject repairRawReport(const QString& command,
                            const sak::PartitionApfsRawRepairResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("target_path"), result.target_path);
    report.insert(QStringLiteral("scanned_blocks"), QString::number(result.scanned_blocks));
    report.insert(QStringLiteral("repaired_checksum_blocks"),
                  QString::number(result.repaired_checksum_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject repairImageReport(const QString& command,
                              const sak::PartitionApfsImageRepairResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("source_image_path"), result.source_image_path);
    report.insert(QStringLiteral("repaired_image_path"), result.repaired_image_path);
    report.insert(QStringLiteral("repaired_image_sha256"), result.repaired_image_sha256);
    report.insert(QStringLiteral("scanned_blocks"), QString::number(result.scanned_blocks));
    report.insert(QStringLiteral("repaired_checksum_blocks"),
                  QString::number(result.repaired_checksum_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject volumeLabelImageReport(const QString& command,
                                   const sak::PartitionApfsImageVolumeLabelResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("source_image_path"), result.source_image_path);
    report.insert(QStringLiteral("written_image_path"), result.written_image_path);
    report.insert(QStringLiteral("written_image_sha256"), result.written_image_sha256);
    report.insert(QStringLiteral("old_volume_name"), result.old_volume_name);
    report.insert(QStringLiteral("new_volume_name"), result.new_volume_name);
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject volumeLabelRawReport(const QString& command,
                                 const sak::PartitionApfsRawVolumeLabelResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("target_path"), result.target_path);
    report.insert(QStringLiteral("old_volume_name"), result.old_volume_name);
    report.insert(QStringLiteral("new_volume_name"), result.new_volume_name);
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

bool writeReport(const QJsonObject& report, const QString& outputPath, QString* error) {
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    QTextStream(stdout) << bytes << Qt::endl;
    if (outputPath.trimmed().isEmpty()) {
        return true;
    }

    if (!QFileInfo(outputPath).absoluteDir().exists() &&
        !QDir().mkpath(QFileInfo(outputPath).absolutePath())) {
        *error = QStringLiteral("Unable to create report directory for %1").arg(outputPath);
        return false;
    }
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = file.errorString();
        return false;
    }
    // Fail closed on a short write or a flush/close error so a truncated report never
    // accompanies a success exit status.
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        *error = file.errorString();
        return false;
    }
    file.close();
    if (file.error() != QFile::NoError) {
        *error = file.errorString();
        return false;
    }
    return true;
}

// OS-level identity of an existing filesystem object: two paths that are
// symlink/hardlink aliases of one file share this token. nullopt when the path
// does not exist / cannot be opened (a not-yet-created output file or a raw
// device needing elevation), so the caller falls back to string/canonical
// comparison rather than treating "unknown" as "distinct".
std::optional<QString> fileIdentityToken(const QString& path) {
#ifdef _WIN32
    const std::wstring wide = path.toStdWString();
    HANDLE handle = ::CreateFileW(wide.c_str(),
                                  0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = ::GetFileInformationByHandle(handle, &info) != 0;
    ::CloseHandle(handle);
    if (!ok) {
        return std::nullopt;
    }
    return QStringLiteral("win:%1:%2:%3")
        .arg(info.dwVolumeSerialNumber)
        .arg(info.nFileIndexHigh)
        .arg(info.nFileIndexLow);
#else
    struct stat st{};
    if (::stat(path.toLocal8Bit().constData(), &st) != 0) {
        return std::nullopt;
    }
    return QStringLiteral("posix:%1:%2")
        .arg(static_cast<qulonglong>(st.st_dev))
        .arg(static_cast<qulonglong>(st.st_ino));
#endif
}

// True when the --output-json path would truncate-write onto the operation's
// target device/image or its --output-image, which would clobber the filesystem
// result with JSON. Detects identical strings (raw device paths like
// \\.\PhysicalDriveN), symlink aliases (canonicalFilePath follows symlinks), and
// hardlink aliases (OS volume+file-id identity, which path comparison cannot see).
bool outputJsonAliasesTarget(const QString& outputJson,
                             const QString& targetPath,
                             const QString& outputImagePath) {
    if (outputJson.trimmed().isEmpty()) {
        return false;
    }
    const auto canon = [](const QString& p) {
        const QFileInfo fi(p);
        const QString real = fi.canonicalFilePath();  // resolves symlinks; empty if absent
        return real.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : real;
    };
    const QString jsonCanon = canon(outputJson);
    const std::optional<QString> jsonId = fileIdentityToken(outputJson);
    const auto aliases = [&](const QString& other) {
        if (other.trimmed().isEmpty()) {
            return false;
        }
        if (outputJson.trimmed().compare(other.trimmed(), Qt::CaseInsensitive) == 0 ||
            jsonCanon.compare(canon(other), Qt::CaseInsensitive) == 0) {
            return true;
        }
        return jsonId.has_value() && jsonId == fileIdentityToken(other);
    };
    return aliases(targetPath) || aliases(outputImagePath);
}

QString evidenceIdForCommand(const QCommandLineParser& parser,
                             const QCommandLineOption& evidenceOption,
                             const QString& command) {
    const QString value = parser.value(evidenceOption).trimmed();
    return value.isEmpty() ? QStringLiteral("sak-ui.%1").arg(command) : value;
}

struct CliInvocation {
    QString command;
    QString target_path;
    uint64_t target_size_bytes{0};
    uint32_t block_size_bytes{kDefaultApfsBlockSizeBytes};
    QString volume_name;
    QStringList additional_volume_names;
    QString output_image_path;
    QString file_name;
    QString directory_name;
    QString parent_directory_path;
    QString new_file_name;
    QString destination_directory_name;
    QByteArray payload;
    uint64_t patch_offset_bytes{0};
    QString patch_offset_error;
    QString snapshot_name;
    QString evidence_id;
    QString volume_password;
    QString recovery_key;
    bool confirm_target{false};
    bool allow_raw_target{false};
    bool compress_zlib{false};
    bool compress_lzfse{false};
    bool compress_lzvn{false};
    bool compress_lzbitmap{false};
    uint64_t sparse_logical_size{0};
    QVector<QPair<QByteArray, QByteArray>> xattrs;
};

std::optional<QString> fileNameForCommand(const QCommandLineParser& parser,
                                          const QCommandLineOption& option,
                                          const QString& command,
                                          QString* error);
std::optional<QString> directoryNameForCommand(const QCommandLineParser& parser,
                                               const QCommandLineOption& directoryOption,
                                               const QCommandLineOption& fileOption,
                                               const QString& command,
                                               QString* error);
std::optional<QByteArray> payloadForCommand(const QCommandLineParser& parser,
                                            const QCommandLineOption& option,
                                            const QString& command,
                                            QString* error);
std::optional<uint64_t> patchOffsetForCommand(const QCommandLineParser& parser,
                                              const QCommandLineOption& option,
                                              const QString& command,
                                              QString* error);

struct CliParserOptions {
    const QCommandLineOption* target{nullptr};
    const QCommandLineOption* size{nullptr};
    const QCommandLineOption* block_size{nullptr};
    const QCommandLineOption* volume_name{nullptr};
    const QCommandLineOption* additional_volume_name{nullptr};
    const QCommandLineOption* output_image{nullptr};
    const QCommandLineOption* file_name{nullptr};
    const QCommandLineOption* directory_name{nullptr};
    const QCommandLineOption* parent_directory_path{nullptr};
    const QCommandLineOption* new_file_name{nullptr};
    const QCommandLineOption* destination_directory_name{nullptr};
    const QCommandLineOption* payload{nullptr};
    const QCommandLineOption* patch_offset{nullptr};
    const QCommandLineOption* snapshot_name{nullptr};
    const QCommandLineOption* evidence{nullptr};
    const QCommandLineOption* confirm{nullptr};
    const QCommandLineOption* allow_raw{nullptr};
    const QCommandLineOption* compress_zlib{nullptr};
    const QCommandLineOption* compress_lzfse{nullptr};
    const QCommandLineOption* compress_lzvn{nullptr};
    const QCommandLineOption* compress_lzbitmap{nullptr};
    const QCommandLineOption* volume_password{nullptr};
    const QCommandLineOption* recovery_key{nullptr};
    const QCommandLineOption* volume_password_file{nullptr};
    const QCommandLineOption* recovery_key_file{nullptr};
    const QCommandLineOption* sparse_size{nullptr};
    const QCommandLineOption* xattr{nullptr};
};

// Parse repeatable --xattr name=hexvalue options into (name, value) pairs. Fails closed on a
// malformed spec (no name, or a value that is not clean even-length hex) instead of silently
// dropping the entry or letting QByteArray::fromHex mangle it by skipping non-hex bytes.
[[nodiscard]] std::optional<QVector<QPair<QByteArray, QByteArray>>> parseXattrOptions(
    const QStringList& values, QString* error) {
    QVector<QPair<QByteArray, QByteArray>> out;
    for (const QString& spec : values) {
        const int eq = spec.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            *error = QStringLiteral("--xattr must be name=hexvalue: %1").arg(spec);
            return std::nullopt;
        }
        const QString hex = spec.mid(eq + 1);
        const auto isHexDigit = [](QChar ch) {
            return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
                   (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')) ||
                   (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
        };
        const bool cleanHex = (hex.size() % 2) == 0 &&
                              std::all_of(hex.cbegin(), hex.cend(), isHexDigit);
        if (!cleanHex) {
            *error = QStringLiteral("--xattr value must be even-length hex: %1").arg(spec);
            return std::nullopt;
        }
        out.append({spec.left(eq).toUtf8(), QByteArray::fromHex(hex.toUtf8())});
    }
    return out;
}

struct CliNumericInputs {
    uint64_t size{0};
    uint32_t block_size{kDefaultApfsBlockSizeBytes};
};

struct CliMutationInputs {
    QString file_name;
    QString directory_name;
    QByteArray payload;
    uint64_t patch_offset{0};
    QString patch_offset_error;
};

std::optional<QString> commandFromParser(const QCommandLineParser& parser, QString* error) {
    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 1) {
        *error = QStringLiteral("Exactly one command is required.");
        return std::nullopt;
    }
    return positional.first().trimmed().toLower();
}

std::optional<QString> targetFromParser(const QCommandLineParser& parser,
                                        const CliParserOptions& options,
                                        QString* error) {
    const QString targetPath = parser.value(*options.target).trimmed();
    if (targetPath.isEmpty()) {
        *error = QStringLiteral("--target is required.");
        return std::nullopt;
    }
    return targetPath;
}

std::optional<CliNumericInputs> numericInputsFromParser(const QCommandLineParser& parser,
                                                        const CliParserOptions& options,
                                                        QString* error) {
    const auto size = parseUInt64Option(parser, *options.size, error);
    if (!size.has_value()) {
        return std::nullopt;
    }
    const uint32_t blockSize = parseBlockSizeOption(parser, *options.block_size, error);
    if (blockSize == 0) {
        return std::nullopt;
    }
    return CliNumericInputs{*size, blockSize};
}

std::optional<CliMutationInputs> mutationInputsFromParser(const QCommandLineParser& parser,
                                                          const CliParserOptions& options,
                                                          const QString& command,
                                                          QString* error) {
    const auto fileName = fileNameForCommand(parser, *options.file_name, command, error);
    if (!fileName.has_value()) {
        return std::nullopt;
    }
    const auto directoryName = directoryNameForCommand(
        parser, *options.directory_name, *options.file_name, command, error);
    if (!directoryName.has_value()) {
        return std::nullopt;
    }
    const auto payload = payloadForCommand(parser, *options.payload, command, error);
    if (!payload.has_value()) {
        return std::nullopt;
    }
    QString patchOffsetError;
    const auto patchOffset =
        patchOffsetForCommand(parser, *options.patch_offset, command, &patchOffsetError);
    return CliMutationInputs{
        *fileName, *directoryName, *payload, patchOffset.value_or(0), patchOffsetError};
}

// Read a credential file's exact bytes and STRICT-UTF-8 decode them. Fails closed on an
// unreadable/oversized file, a read error / short read, or invalid UTF-8 -- never
// replacement-decodes a malformed credential into a silently-wrong password.
QString readCredentialFile(const QString& filePath, QString* error) {
    QFile credentialFile(filePath);
    if (!credentialFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to read credential file: %1").arg(filePath);
        return QString();
    }
    const qint64 expected = credentialFile.size();
    if (expected > kMaxCredentialFileBytes) {
        *error = QStringLiteral("Credential file exceeds the %1-byte limit")
                     .arg(kMaxCredentialFileBytes);
        return QString();
    }
    QByteArray raw = credentialFile.readAll();
    if (credentialFile.error() != QFile::NoError || raw.size() != expected) {
        sak::secure_wiper::wipe(raw.data(), raw.size());
        *error = QStringLiteral("Unable to fully read credential file: %1").arg(filePath);
        return QString();
    }
    // Default UTF-8 decoder; decoder.hasError() below is set on any invalid sequence, so we
    // fail closed on non-UTF-8 credential bytes rather than accept a replacement-char decode.
    QStringDecoder decoder(QStringConverter::Utf8);
    QString credential = decoder.decode(raw);
    // Wipe the raw byte copy of the secret as soon as it is converted; the returned QString
    // still holds the credential (unavoidable while it is in use) but this removes one
    // lingering plaintext copy from the process heap.
    sak::secure_wiper::wipe(raw.data(), raw.size());
    if (decoder.hasError()) {
        *error = QStringLiteral("Credential file is not valid UTF-8: %1").arg(filePath);
        return QString();
    }
    return credential;
}

// Resolve a credential value: when the *-file option is set, return that file's exact UTF-8
// bytes (so the secret never lands on the command line or in any generated script text);
// otherwise fall back to the inline --volume-password / --recovery-key value. Specifying both
// the inline and the file form is a conflicting-precedence error -- fail closed rather than
// silently letting the file win.
QString credentialFromParser(const QCommandLineParser& parser,
                             const QCommandLineOption& directOption,
                             const QCommandLineOption& fileOption,
                             QString* error) {
    const QString filePath = parser.value(fileOption).trimmed();
    if (filePath.isEmpty()) {
        return parser.value(directOption);
    }
    if (parser.isSet(directOption)) {
        *error = QStringLiteral("Specify only one of --%1 or --%2, not both")
                     .arg(directOption.names().constFirst(), fileOption.names().constFirst());
        return QString();
    }
    return readCredentialFile(filePath, error);
}

struct CliSparseXattrInputs {
    uint64_t sparse_logical_size{0};
    QVector<QPair<QByteArray, QByteArray>> xattrs;
};

// Validate --sparse-size (a bad value would silently become 0, i.e. non-sparse) and the
// --xattr specs (bad hex would be silently mangled) up front, failing closed on either.
std::optional<CliSparseXattrInputs> sparseXattrFromParser(const QCommandLineParser& parser,
                                                          const CliParserOptions& options,
                                                          QString* error) {
    const QString sparseRaw = parser.value(*options.sparse_size).trimmed();
    bool sparseOk = true;
    const uint64_t sparseSize = sparseRaw.isEmpty() ? 0 : sparseRaw.toULongLong(&sparseOk);
    if (!sparseOk) {
        *error = QStringLiteral("--sparse-size must be a non-negative integer: %1").arg(sparseRaw);
        return std::nullopt;
    }
    auto xattrs = parseXattrOptions(parser.values(*options.xattr), error);
    if (!xattrs.has_value()) {
        return std::nullopt;
    }
    return CliSparseXattrInputs{.sparse_logical_size = sparseSize, .xattrs = std::move(*xattrs)};
}

std::optional<CliInvocation> invocationFromParser(const QCommandLineParser& parser,
                                                  const CliParserOptions& options,
                                                  QString* error) {
    const auto command = commandFromParser(parser, error);
    if (!command.has_value()) {
        return std::nullopt;
    }
    const auto target = targetFromParser(parser, options, error);
    if (!target.has_value()) {
        return std::nullopt;
    }
    const auto numeric = numericInputsFromParser(parser, options, error);
    if (!numeric.has_value()) {
        return std::nullopt;
    }
    const auto mutation = mutationInputsFromParser(parser, options, *command, error);
    if (!mutation.has_value()) {
        return std::nullopt;
    }
    const QString volumePassword = credentialFromParser(
        parser, *options.volume_password, *options.volume_password_file, error);
    if (!error->isEmpty()) {
        return std::nullopt;
    }
    const QString recoveryKey =
        credentialFromParser(parser, *options.recovery_key, *options.recovery_key_file, error);
    if (!error->isEmpty()) {
        return std::nullopt;
    }
    const auto sparseXattr = sparseXattrFromParser(parser, options, error);
    if (!sparseXattr.has_value()) {
        return std::nullopt;
    }
    return CliInvocation{
        .command = *command,
        .target_path = *target,
        .target_size_bytes = numeric->size,
        .block_size_bytes = numeric->block_size,
        .volume_name = parser.value(*options.volume_name),
        .additional_volume_names = parser.values(*options.additional_volume_name),
        .output_image_path = parser.value(*options.output_image).trimmed(),
        .file_name = mutation->file_name,
        .directory_name = mutation->directory_name,
        .parent_directory_path = parser.value(*options.parent_directory_path).trimmed(),
        .new_file_name = parser.value(*options.new_file_name).trimmed(),
        .destination_directory_name = parser.value(*options.destination_directory_name).trimmed(),
        .payload = mutation->payload,
        .patch_offset_bytes = mutation->patch_offset,
        .patch_offset_error = mutation->patch_offset_error,
        .snapshot_name = parser.value(*options.snapshot_name).trimmed(),
        .evidence_id = evidenceIdForCommand(parser, *options.evidence, *command),
        .volume_password = volumePassword,
        .recovery_key = recoveryKey,
        .confirm_target = parser.isSet(*options.confirm),
        .allow_raw_target = parser.isSet(*options.allow_raw),
        .compress_zlib = parser.isSet(*options.compress_zlib),
        .compress_lzfse = parser.isSet(*options.compress_lzfse),
        .compress_lzvn = parser.isSet(*options.compress_lzvn),
        .compress_lzbitmap = parser.isSet(*options.compress_lzbitmap),
        .sparse_logical_size = sparseXattr->sparse_logical_size,
        .xattrs = sparseXattr->xattrs};
}

QJsonObject buildFormatImageReport(const CliInvocation& invocation) {
    return formatReport(invocation.command,
                        sak::PartitionApfsWriter::buildImageOnlyFormatImage(
                            {.image_path = invocation.target_path,
                             .target_container_bytes = invocation.target_size_bytes,
                             .block_size_bytes = invocation.block_size_bytes,
                             .volume_name = invocation.volume_name,
                             .additional_volume_names = invocation.additional_volume_names,
                             .volume_password = invocation.volume_password,
                             .recovery_key = invocation.recovery_key,
                             .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildRepairImageReport(const CliInvocation& invocation, QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for repair-image.");
        return std::nullopt;
    }
    return repairImageReport(invocation.command,
                             sak::PartitionApfsWriter::repairImageOnlyObjectChecksums(
                                 {.source_image_path = invocation.target_path,
                                  .repaired_image_path = invocation.output_image_path,
                                  .options = imageWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildFormatRawReport(const CliInvocation& invocation) {
    return formatReport(invocation.command,
                        sak::PartitionApfsWriter::formatExistingContainerTarget(
                            {.image_path = invocation.target_path,
                             .target_container_bytes = invocation.target_size_bytes,
                             .block_size_bytes = invocation.block_size_bytes,
                             .volume_name = invocation.volume_name,
                             .additional_volume_names = invocation.additional_volume_names,
                             .volume_password = invocation.volume_password,
                             .recovery_key = invocation.recovery_key,
                             .target_wipe_confirmed = invocation.confirm_target,
                             .allow_raw_device_target = invocation.allow_raw_target,
                             .options = rawWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildChangeVolumeLabelImageReport(const CliInvocation& invocation,
                                                             QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for change-image-volume-label.");
        return std::nullopt;
    }
    return volumeLabelImageReport(invocation.command,
                                  sak::PartitionApfsWriter::changeImageOnlyVolumeLabel(
                                      {.source_image_path = invocation.target_path,
                                       .written_image_path = invocation.output_image_path,
                                       .volume_name = invocation.volume_name,
                                       .options = imageWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildChangeVolumeLabelRawReport(const CliInvocation& invocation) {
    return volumeLabelRawReport(invocation.command,
                                sak::PartitionApfsWriter::changeRawVolumeLabel(
                                    {.target_path = invocation.target_path,
                                     .target_container_bytes = invocation.target_size_bytes,
                                     .volume_name = invocation.volume_name,
                                     .target_write_confirmed = invocation.confirm_target,
                                     .allow_raw_device_target = invocation.allow_raw_target,
                                     .options = rawWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildRepairRawReport(const CliInvocation& invocation) {
    return repairRawReport(invocation.command,
                           sak::PartitionApfsWriter::repairRawObjectChecksums(
                               {.target_path = invocation.target_path,
                                .target_container_bytes = invocation.target_size_bytes,
                                .target_repair_confirmed = invocation.confirm_target,
                                .allow_raw_device_target = invocation.allow_raw_target,
                                .options = rawWriteOptions(invocation.evidence_id)}));
}

bool isImageCommand(const QString& command) {
    static const QStringList kImageCommands = {
        QStringLiteral("format-image"),
        QStringLiteral("repair-image"),
        QStringLiteral("change-image-volume-label"),
        QStringLiteral("list-image"),
        QStringLiteral("import-image"),
    };
    return kImageCommands.contains(command);
}

bool isRawCommand(const QString& command) {
    static const QStringList kRawCommands = {
        QStringLiteral("format-raw"),
        QStringLiteral("change-raw-volume-label"),
        QStringLiteral("repair-raw"),
    };
    return kRawCommands.contains(command);
}

std::optional<QJsonObject> buildImageCommandReport(const CliInvocation& invocation,
                                                   QString* error) {
    if (invocation.command == QStringLiteral("format-image")) {
        return buildFormatImageReport(invocation);
    }
    if (invocation.command == QStringLiteral("repair-image")) {
        return buildRepairImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("change-image-volume-label")) {
        return buildChangeVolumeLabelImageReport(invocation, error);
    }
    *error = QStringLiteral("Unsupported image command: %1").arg(invocation.command);
    return std::nullopt;
}

std::optional<QJsonObject> buildRawCommandReport(const CliInvocation& invocation, QString* error) {
    if (invocation.command == QStringLiteral("format-raw")) {
        return buildFormatRawReport(invocation);
    }
    if (invocation.command == QStringLiteral("change-raw-volume-label")) {
        return buildChangeVolumeLabelRawReport(invocation);
    }
    if (invocation.command == QStringLiteral("repair-raw")) {
        return buildRepairRawReport(invocation);
    }
    *error = QStringLiteral("Unsupported raw command: %1").arg(invocation.command);
    return std::nullopt;
}

struct ImportedFile {
    QString name;
    QByteArray data;
};

// Read every flat root file from a foreign source container into `files`.
bool collectImportSourceFiles(const QString& sourcePath,
                              QVector<ImportedFile>* files,
                              QString* volumeName,
                              QString* error) {
    // Request one past the cap so an over-cap listing is detectable: if the reader returns
    // more than the cap, the root has more files than we can enumerate and re-emitting would
    // silently drop the overflow. Fail closed rather than build a truncated image.
    constexpr int kRootBrowseCap = sak::kPartitionApfsDefaultBrowseEntryLimit;
    const auto listing = sak::PartitionApfsFileSystemReader::listDirectoryFromImage(
        sourcePath, QStringLiteral("/"), kRootBrowseCap + 1);
    if (!listing.ok) {
        *error = QStringLiteral("Unable to read source container: %1")
                     .arg(listing.blockers.join(QStringLiteral("; ")));
        return false;
    }
    if (listing.entries.size() > kRootBrowseCap) {
        *error = QStringLiteral(
                     "Source container root holds more than %1 files; refusing to import a "
                     "truncated set")
                     .arg(kRootBrowseCap);
        return false;
    }
    *volumeName = listing.volume_name;
    for (const auto& entry : listing.entries) {
        if (entry.directory) {
            *error = QStringLiteral(
                         "Arbitrary import is limited to flat root files; source contains "
                         "directory '%1'")
                         .arg(entry.name);
            return false;
        }
        const auto read = sak::PartitionApfsFileSystemReader::readFileFromImage(
            sourcePath, QStringLiteral("/%1").arg(entry.name), entry.size_bytes);
        if (!read.ok) {
            *error = QStringLiteral("Unable to read source file '%1': %2")
                         .arg(entry.name, read.blockers.join(QStringLiteral("; ")));
            return false;
        }
        files->append({entry.name, read.data});
    }
    return true;
}

// Format a fresh certified container in `scratch`, then re-emit every imported
// file into successive scratch images; returns the final container path.
std::optional<QString> reEmitImportedFiles(const CliInvocation& invocation,
                                           QTemporaryDir& scratch,
                                           const QString& volumeName,
                                           const QVector<ImportedFile>& files,
                                           QString* error) {
    const auto options = imageWriteOptions(invocation.evidence_id);
    const QString freshPath = scratch.filePath(QStringLiteral("import-fresh.apfs"));
    const auto formatResult = sak::PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = freshPath,
         .target_container_bytes = invocation.target_size_bytes,
         .block_size_bytes = invocation.block_size_bytes,
         .volume_name = volumeName,
         .options = options});
    if (!formatResult.ok) {
        *error = formatResult.blockers.join(QStringLiteral("; "));
        return std::nullopt;
    }

    QString currentPath = freshPath;
    int stage = 0;
    for (const auto& file : files) {
        const QString nextPath = scratch.filePath(QStringLiteral("import-%1.apfs").arg(stage++));
        const auto write =
            sak::PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = currentPath,
                                                                .written_image_path = nextPath,
                                                                .file_name = file.name,
                                                                .file_data = file.data,
                                                                .options = options});
        if (!write.ok) {
            *error = QStringLiteral("Unable to re-emit file '%1': %2")
                         .arg(file.name, write.blockers.join(QStringLiteral("; ")));
            return std::nullopt;
        }
        currentPath = nextPath;
    }
    return currentPath;
}

QJsonObject buildImportReportObject(const QString& sourcePath,
                                    const QString& outputPath,
                                    const QString& volumeName,
                                    int importedFileCount) {
    QJsonObject report;
    report.insert(QStringLiteral("ok"), true);
    report.insert(QStringLiteral("operation"), QStringLiteral("Arbitrary APFS import"));
    report.insert(QStringLiteral("source_image"), sourcePath);
    report.insert(QStringLiteral("output_image"), outputPath);
    report.insert(QStringLiteral("volume_name"), volumeName);
    report.insert(QStringLiteral("imported_file_count"), importedFileCount);
    return report;
}

// Atomically publish the staged container to outputPath. Uses QSaveFile so a
// failure (disk full, permission, read error) never destroys an existing prior
// output: the temp is committed with an atomic rename only after a full, verified
// copy, and discarded otherwise. Replaces a remove-then-copy that deleted the old
// output before writing, leaving nothing if the copy failed.
bool publishImportedContainer(const QString& stagedPath,
                              const QString& outputPath,
                              QString* error) {
    QFile source(stagedPath);
    if (!source.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to read staged container %1").arg(stagedPath);
        return false;
    }
    QSaveFile sink(outputPath);
    if (!sink.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Unable to write imported container to %1").arg(outputPath);
        return false;
    }
    constexpr qint64 kImportCopyChunkBytes = 1 << 20;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kImportCopyChunkBytes);
        if (chunk.isEmpty() || sink.write(chunk) != chunk.size()) {
            *error = QStringLiteral("Unable to write imported container to %1").arg(outputPath);
            return false;
        }
    }
    if (!sink.commit()) {
        *error = QStringLiteral("Unable to write imported container to %1").arg(outputPath);
        return false;
    }
    return true;
}

std::optional<QJsonObject> buildImportImageReport(const CliInvocation& invocation, QString* error) {
    // Arbitrary Apple-media APFS read-modify-write: read every root file from a
    // foreign source container (any block layout the generic reader can walk),
    // then re-emit a freshly certified S.A.K. container carrying those files
    // plus an optional added file. This delivers mutation of arbitrary
    // unencrypted single-volume Apple containers without fragile in-place
    // checkpoint-ring surgery.
    const QString sourcePath = invocation.target_path;
    const QString outputPath = invocation.output_image_path;
    QVector<ImportedFile> files;
    QString volumeName;
    if (!collectImportSourceFiles(sourcePath, &files, &volumeName, error)) {
        return std::nullopt;
    }
    if (!invocation.file_name.trimmed().isEmpty()) {
        files.append({invocation.file_name, invocation.payload});
    }

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        *error = QStringLiteral("Unable to create scratch directory for arbitrary import");
        return std::nullopt;
    }
    const auto currentPath = reEmitImportedFiles(invocation, scratch, volumeName, files, error);
    if (!currentPath.has_value()) {
        return std::nullopt;
    }

    if (!QFile::exists(*currentPath)) {
        *error = QStringLiteral("Arbitrary import produced no output");
        return std::nullopt;
    }
    if (!publishImportedContainer(*currentPath, outputPath, error)) {
        return std::nullopt;
    }
    return buildImportReportObject(
        sourcePath, outputPath, volumeName, static_cast<int>(files.size()));
}

std::optional<QJsonObject> buildListImageReport(const CliInvocation& invocation, QString* error) {
    const auto listing = sak::PartitionApfsFileSystemReader::listDirectoryFromImage(
        invocation.target_path, QStringLiteral("/"), 4096);
    QJsonObject report;
    report.insert(QStringLiteral("ok"), listing.ok);
    report.insert(QStringLiteral("volume_name"), listing.volume_name);
    QJsonArray entries;
    for (const auto& entry : listing.entries) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), entry.name);
        item.insert(QStringLiteral("size"), static_cast<qint64>(entry.size_bytes));
        item.insert(QStringLiteral("is_directory"), entry.directory);
        entries.append(item);
    }
    report.insert(QStringLiteral("entries"), entries);
    QJsonArray blockers;
    for (const auto& blocker : listing.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!listing.ok) {
        *error = listing.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildProbeLayoutReport(const CliInvocation& invocation, QString* error) {
    const auto probe = sak::PartitionApfsWriter::probeLiveLayout(invocation.target_path);
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), QStringLiteral("probe-layout"));
    report.insert(QStringLiteral("ok"), probe.ok);
    report.insert(QStringLiteral("block_size"), static_cast<qint64>(probe.block_size));
    report.insert(QStringLiteral("block_count"), static_cast<qint64>(probe.block_count));
    report.insert(QStringLiteral("latest_xid"), static_cast<qint64>(probe.latest_xid));
    report.insert(QStringLiteral("nx_omap_oid"), static_cast<qint64>(probe.nx_omap_oid));
    report.insert(QStringLiteral("spaceman_oid"), static_cast<qint64>(probe.spaceman_oid));
    report.insert(QStringLiteral("volume_oid"), static_cast<qint64>(probe.volume_oid));
    report.insert(QStringLiteral("volume_sb_block"), static_cast<qint64>(probe.volume_sb_block));
    report.insert(QStringLiteral("vol_root_tree_oid"),
                  static_cast<qint64>(probe.vol_root_tree_oid));
    report.insert(QStringLiteral("vol_omap_oid"), static_cast<qint64>(probe.vol_omap_oid));
    report.insert(QStringLiteral("vol_extentref_oid"),
                  static_cast<qint64>(probe.vol_extentref_oid));
    report.insert(QStringLiteral("vol_snap_meta_oid"),
                  static_cast<qint64>(probe.vol_snap_meta_oid));
    report.insert(QStringLiteral("sm_main_block_count"),
                  static_cast<qint64>(probe.sm_main_block_count));
    report.insert(QStringLiteral("sm_main_chunk_count"),
                  static_cast<qint64>(probe.sm_main_chunk_count));
    report.insert(QStringLiteral("sm_main_cib_count"),
                  static_cast<qint64>(probe.sm_main_cib_count));
    report.insert(QStringLiteral("sm_main_cab_count"),
                  static_cast<qint64>(probe.sm_main_cab_count));
    report.insert(QStringLiteral("sm_main_free_count"),
                  static_cast<qint64>(probe.sm_main_free_count));
    report.insert(QStringLiteral("ip_base"), static_cast<qint64>(probe.ip_base));
    report.insert(QStringLiteral("ip_block_count"), static_cast<qint64>(probe.ip_block_count));
    report.insert(QStringLiteral("ip_bm_size_blocks"),
                  static_cast<qint64>(probe.ip_bm_size_blocks));
    report.insert(QStringLiteral("ip_bm_block_count"),
                  static_cast<qint64>(probe.ip_bm_block_count));
    report.insert(QStringLiteral("ip_bm_base"), static_cast<qint64>(probe.ip_bm_base));
    report.insert(QStringLiteral("sm_addr_offset"), static_cast<qint64>(probe.sm_addr_offset));
    report.insert(QStringLiteral("ip_bm_free_head"), static_cast<qint64>(probe.ip_bm_free_head));
    report.insert(QStringLiteral("ip_bm_free_tail"), static_cast<qint64>(probe.ip_bm_free_tail));
    report.insert(QStringLiteral("ip_bm_xid_array_off"),
                  static_cast<qint64>(probe.ip_bm_xid_array_off));
    report.insert(QStringLiteral("ip_bm_addr_array_off"),
                  static_cast<qint64>(probe.ip_bm_addr_array_off));
    report.insert(QStringLiteral("ip_bm_free_next_off"),
                  static_cast<qint64>(probe.ip_bm_free_next_off));
    QJsonArray liveSlots;
    for (const uint64_t slot : probe.live_bm_slots) {
        liveSlots.append(static_cast<qint64>(slot));
    }
    report.insert(QStringLiteral("live_bm_slots"), liveSlots);
    QJsonArray livePop;
    for (const uint64_t pop : probe.live_bm_pop) {
        livePop.append(static_cast<qint64>(pop));
    }
    report.insert(QStringLiteral("live_bm_pop"), livePop);
    QJsonArray cibs;
    for (const uint64_t addr : probe.cib_addrs) {
        cibs.append(static_cast<qint64>(addr));
    }
    report.insert(QStringLiteral("cib_addrs"), cibs);
    report.insert(QStringLiteral("blockers"), toJsonArray(probe.blockers));
    if (!probe.ok) {
        *error = probe.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitCheckpointReport(const CliInvocation& invocation,
                                                       QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-checkpoint.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyCheckpoint(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS in-place checkpoint commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    report.insert(QStringLiteral("checkpoint_map_block"),
                  static_cast<qint64>(commit.checkpoint_map_block));
    report.insert(QStringLiteral("superblock_block"), static_cast<qint64>(commit.superblock_block));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitFileRenameReport(const CliInvocation& invocation,
                                                       QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-rename.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFileRename(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .file_name = invocation.file_name,
         .new_file_name = invocation.directory_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS in-place file rename commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("old_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("new_name"), invocation.directory_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitDirectoryCreateReport(const CliInvocation& invocation,
                                                            QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-directory-create.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyDirectoryCreate(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .directory_name = invocation.directory_name,
         .parent_directory_path = invocation.parent_directory_path,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS in-place directory create commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("directory_name"), invocation.directory_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    QJsonArray dirBlockers;
    for (const auto& blocker : commit.blockers) {
        dirBlockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), dirBlockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitDirectoryDeleteReport(const CliInvocation& invocation,
                                                            QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-directory-delete.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyDirectoryDelete(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .directory_name = invocation.directory_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS in-place directory delete commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("directory_name"), invocation.directory_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    QJsonArray dirBlockers;
    for (const auto& blocker : commit.blockers) {
        dirBlockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), dirBlockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitDirectoryChildWriteReport(const CliInvocation& invocation,
                                                                QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error =
            QStringLiteral("--output-image is required for commit-image-directory-child-write.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyDirectoryChildWrite(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .file_data = invocation.payload,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS in-place directory-child write (create-or-replace) commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("directory_name"), invocation.directory_name.trimmed());
    report.insert(QStringLiteral("file_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitDirectoryChildDeleteReport(const CliInvocation& invocation,
                                                                 QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error =
            QStringLiteral("--output-image is required for commit-image-directory-child-delete.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyDirectoryChildDelete(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS in-place directory-child delete commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("directory_name"), invocation.directory_name.trimmed());
    report.insert(QStringLiteral("file_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitFileWriteReport(const CliInvocation& invocation,
                                                      QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-write.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFileWrite(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .file_name = invocation.file_name,
         .file_data = invocation.payload,
         .compress_zlib = invocation.compress_zlib,
         .compress_lzfse = invocation.compress_lzfse,
         .compress_lzvn = invocation.compress_lzvn,
         .compress_lzbitmap = invocation.compress_lzbitmap,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS in-place file write (create-or-replace) commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("file_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    report.insert(QStringLiteral("superblock_block"), static_cast<qint64>(commit.superblock_block));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitFileInsertReport(const CliInvocation& invocation,
                                                       QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-insert.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFileInsert(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .file_name = invocation.file_name,
         .file_data = invocation.payload,
         .compress_zlib = invocation.compress_zlib,
         .compress_lzfse = invocation.compress_lzfse,
         .compress_lzvn = invocation.compress_lzvn,
         .compress_lzbitmap = invocation.compress_lzbitmap,
         .xattrs = invocation.xattrs,
         .sparse_logical_size = invocation.sparse_logical_size,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS in-place file insert commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("file_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    report.insert(QStringLiteral("superblock_block"), static_cast<qint64>(commit.superblock_block));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitFileCloneReport(const CliInvocation& invocation,
                                                      QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-clone.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFileClone(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .source_file_name = invocation.file_name,
         .clone_file_name = invocation.directory_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS in-place file clone commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("source_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("clone_name"), invocation.directory_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    report.insert(QStringLiteral("superblock_block"), static_cast<qint64>(commit.superblock_block));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitResizeReport(const CliInvocation& invocation,
                                                   QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-resize.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyResize(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .new_size_bytes = invocation.target_size_bytes,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS in-place container resize commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("new_size_bytes"),
                  static_cast<qint64>(invocation.target_size_bytes));
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    report.insert(QStringLiteral("superblock_block"), static_cast<qint64>(commit.superblock_block));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitFileHardlinkReport(const CliInvocation& invocation,
                                                         QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-hardlink.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFileHardlink(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .source_file_name = invocation.file_name,
         .link_file_name = invocation.directory_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"),
                  QStringLiteral("APFS in-place file hard link commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("source_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("link_name"), invocation.directory_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    report.insert(QStringLiteral("superblock_block"), static_cast<qint64>(commit.superblock_block));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitFileDeleteReport(const CliInvocation& invocation,
                                                       QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-delete.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFileDelete(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .file_name = invocation.file_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"), QStringLiteral("APFS in-place file delete commit"));
    report.insert(QStringLiteral("source_image"), commit.source_image_path);
    report.insert(QStringLiteral("output_image"), commit.written_image_path);
    report.insert(QStringLiteral("file_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

QJsonObject commitResultReport(const sak::PartitionApfsImageCheckpointCommitResult& commit,
                               const QString& operation,
                               const CliInvocation& invocation,
                               QString* error) {
    QJsonObject report;
    report.insert(QStringLiteral("ok"), commit.ok);
    report.insert(QStringLiteral("operation"), operation);
    report.insert(QStringLiteral("target"), commit.written_image_path);
    report.insert(QStringLiteral("file_name"), invocation.file_name.trimmed());
    report.insert(QStringLiteral("previous_xid"), static_cast<qint64>(commit.previous_xid));
    report.insert(QStringLiteral("new_xid"), static_cast<qint64>(commit.new_xid));
    QJsonArray blockers;
    for (const auto& blocker : commit.blockers) {
        blockers.append(blocker);
    }
    report.insert(QStringLiteral("blockers"), blockers);
    if (!commit.ok) {
        *error = commit.blockers.join(QStringLiteral("; "));
    }
    return report;
}

std::optional<QJsonObject> buildCommitRawFileWriteReport(const CliInvocation& invocation,
                                                         QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFileWrite(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .file_name = invocation.file_name,
         .file_data = invocation.payload,
         .parent_directory_path = invocation.parent_directory_path,
         .compress_zlib = invocation.compress_zlib,
         .compress_lzfse = invocation.compress_lzfse,
         .compress_lzvn = invocation.compress_lzvn,
         .compress_lzbitmap = invocation.compress_lzbitmap,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(commit,
                              QStringLiteral(
                                  "APFS raw in-place file write (create-or-replace) commit"),
                              invocation,
                              error);
}

std::optional<QJsonObject> buildCommitRawFileInsertReport(const CliInvocation& invocation,
                                                          QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFileInsert(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .file_name = invocation.file_name,
         .file_data = invocation.payload,
         .parent_directory_path = invocation.parent_directory_path,
         .compress_zlib = invocation.compress_zlib,
         .compress_lzfse = invocation.compress_lzfse,
         .compress_lzvn = invocation.compress_lzvn,
         .compress_lzbitmap = invocation.compress_lzbitmap,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place file insert commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawFileDeleteReport(const CliInvocation& invocation,
                                                          QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFileDelete(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .file_name = invocation.file_name,
         .parent_directory_path = invocation.parent_directory_path,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place file delete commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawFileRenameReport(const CliInvocation& invocation,
                                                          QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFileRename(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .file_name = invocation.file_name,
         .new_file_name = invocation.directory_name,
         .parent_directory_path = invocation.parent_directory_path,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place file rename commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawDirectoryCreateReport(const CliInvocation& invocation,
                                                               QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawDirectoryCreate(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .directory_name = invocation.directory_name,
         .parent_directory_path = invocation.parent_directory_path,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place directory create commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawDirectoryDeleteReport(const CliInvocation& invocation,
                                                               QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawDirectoryDelete(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .directory_name = invocation.directory_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place directory delete commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawDirectoryChildWriteReport(const CliInvocation& invocation,
                                                                   QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawDirectoryChildWrite(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .file_data = invocation.payload,
         .compress_zlib = invocation.compress_zlib,
         .compress_lzfse = invocation.compress_lzfse,
         .compress_lzvn = invocation.compress_lzvn,
         .compress_lzbitmap = invocation.compress_lzbitmap,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit,
        QStringLiteral("APFS raw in-place directory-child write (create-or-replace) commit"),
        invocation,
        error);
}

std::optional<QJsonObject> buildCommitRawDirectoryChildDeleteReport(const CliInvocation& invocation,
                                                                    QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawDirectoryChildDelete(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(commit,
                              QStringLiteral("APFS raw in-place directory-child delete commit"),
                              invocation,
                              error);
}

std::optional<QJsonObject> buildCommitFileMoveReport(const CliInvocation& invocation,
                                                     QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-move.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFileMove(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .source_directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .destination_directory_name = invocation.destination_directory_name,
         .new_file_name = invocation.new_file_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS in-place file move commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawFileMoveReport(const CliInvocation& invocation,
                                                        QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFileMove(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .source_directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .destination_directory_name = invocation.destination_directory_name,
         .new_file_name = invocation.new_file_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place file move commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitFilePatchReport(const CliInvocation& invocation,
                                                      QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-file-patch.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlyFilePatch(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .patch_offset_bytes = invocation.patch_offset_bytes,
         .patch_data = invocation.payload,
         .options = imageWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS in-place file patch commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawFilePatchReport(const CliInvocation& invocation,
                                                         QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFilePatch(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .directory_name = invocation.directory_name,
         .file_name = invocation.file_name,
         .patch_offset_bytes = invocation.patch_offset_bytes,
         .patch_data = invocation.payload,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place file patch commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitSnapshotCreateReport(const CliInvocation& invocation,
                                                           QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-snapshot-create.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlySnapshotCreate(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .snapshot_name = invocation.snapshot_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS in-place snapshot create commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawSnapshotCreateReport(const CliInvocation& invocation,
                                                              QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawSnapshotCreate(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .snapshot_name = invocation.snapshot_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place snapshot create commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitSnapshotDeleteReport(const CliInvocation& invocation,
                                                           QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-snapshot-delete.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlySnapshotDelete(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .snapshot_name = invocation.snapshot_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS in-place snapshot delete commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawSnapshotDeleteReport(const CliInvocation& invocation,
                                                              QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawSnapshotDelete(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .snapshot_name = invocation.snapshot_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place snapshot delete commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitSnapshotRevertReport(const CliInvocation& invocation,
                                                           QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for commit-image-snapshot-revert.");
        return std::nullopt;
    }
    const auto commit = sak::PartitionApfsWriter::commitImageOnlySnapshotRevert(
        {.source_image_path = invocation.target_path,
         .written_image_path = invocation.output_image_path,
         .snapshot_name = invocation.snapshot_name,
         .options = imageWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS in-place snapshot revert commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawSnapshotRevertReport(const CliInvocation& invocation,
                                                              QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawSnapshotRevert(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .snapshot_name = invocation.snapshot_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place snapshot revert commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawFileCloneReport(const CliInvocation& invocation,
                                                         QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFileClone(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .source_file_name = invocation.file_name,
         .clone_file_name = invocation.new_file_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place file clone commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawFileHardlinkReport(const CliInvocation& invocation,
                                                            QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawFileHardlink(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .source_file_name = invocation.file_name,
         .link_file_name = invocation.new_file_name,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place file hard link commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawResizeReport(const CliInvocation& invocation,
                                                      QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawResize(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .new_size_bytes = invocation.target_size_bytes,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place container resize commit"), invocation, error);
}

// Dispatch the in-place commit family (image + raw). Sets *handled when the
// command is a commit command, keeping buildCommandReport's branch count low.
std::optional<QJsonObject> buildCommitCommandReport(const CliInvocation& invocation,
                                                    QString* error,
                                                    bool* handled) {
    using CommitBuilder = std::optional<QJsonObject> (*)(const CliInvocation&, QString*);
    static const std::array<std::pair<QLatin1StringView, CommitBuilder>, 33> kCommitBuilders = {{
        {QLatin1StringView("commit-image-checkpoint"), buildCommitCheckpointReport},
        {QLatin1StringView("commit-image-file-move"), buildCommitFileMoveReport},
        {QLatin1StringView("commit-raw-file-move"), buildCommitRawFileMoveReport},
        {QLatin1StringView("commit-image-file-patch"), buildCommitFilePatchReport},
        {QLatin1StringView("commit-raw-file-patch"), buildCommitRawFilePatchReport},
        {QLatin1StringView("commit-image-file-write"), buildCommitFileWriteReport},
        {QLatin1StringView("commit-image-directory-create"), buildCommitDirectoryCreateReport},
        {QLatin1StringView("commit-image-directory-delete"), buildCommitDirectoryDeleteReport},
        {QLatin1StringView("commit-image-directory-child-write"),
         buildCommitDirectoryChildWriteReport},
        {QLatin1StringView("commit-image-directory-child-delete"),
         buildCommitDirectoryChildDeleteReport},
        {QLatin1StringView("commit-image-file-insert"), buildCommitFileInsertReport},
        {QLatin1StringView("commit-image-file-clone"), buildCommitFileCloneReport},
        {QLatin1StringView("commit-image-file-hardlink"), buildCommitFileHardlinkReport},
        {QLatin1StringView("commit-image-resize"), buildCommitResizeReport},
        {QLatin1StringView("commit-image-file-delete"), buildCommitFileDeleteReport},
        {QLatin1StringView("commit-image-file-rename"), buildCommitFileRenameReport},
        {QLatin1StringView("commit-raw-file-insert"), buildCommitRawFileInsertReport},
        {QLatin1StringView("commit-raw-file-write"), buildCommitRawFileWriteReport},
        {QLatin1StringView("commit-raw-file-clone"), buildCommitRawFileCloneReport},
        {QLatin1StringView("commit-raw-file-hardlink"), buildCommitRawFileHardlinkReport},
        {QLatin1StringView("commit-raw-resize"), buildCommitRawResizeReport},
        {QLatin1StringView("commit-raw-file-delete"), buildCommitRawFileDeleteReport},
        {QLatin1StringView("commit-raw-file-rename"), buildCommitRawFileRenameReport},
        {QLatin1StringView("commit-raw-directory-create"), buildCommitRawDirectoryCreateReport},
        {QLatin1StringView("commit-raw-directory-delete"), buildCommitRawDirectoryDeleteReport},
        {QLatin1StringView("commit-raw-directory-child-write"),
         buildCommitRawDirectoryChildWriteReport},
        {QLatin1StringView("commit-raw-directory-child-delete"),
         buildCommitRawDirectoryChildDeleteReport},
        {QLatin1StringView("commit-image-snapshot-create"), buildCommitSnapshotCreateReport},
        {QLatin1StringView("commit-raw-snapshot-create"), buildCommitRawSnapshotCreateReport},
        {QLatin1StringView("commit-image-snapshot-delete"), buildCommitSnapshotDeleteReport},
        {QLatin1StringView("commit-raw-snapshot-delete"), buildCommitRawSnapshotDeleteReport},
        {QLatin1StringView("commit-image-snapshot-revert"), buildCommitSnapshotRevertReport},
        {QLatin1StringView("commit-raw-snapshot-revert"), buildCommitRawSnapshotRevertReport},
    }};
    for (const auto& [command, builder] : kCommitBuilders) {
        if (invocation.command == command) {
            *handled = true;
            return builder(invocation, error);
        }
    }
    *handled = false;
    return std::nullopt;
}

std::optional<QJsonObject> buildCommandReport(const CliInvocation& invocation, QString* error) {
    if (invocation.command == QStringLiteral("list-image")) {
        return buildListImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("probe-layout")) {
        return buildProbeLayoutReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("import-image")) {
        return buildImportImageReport(invocation, error);
    }
    bool commitHandled = false;
    auto commitReport = buildCommitCommandReport(invocation, error, &commitHandled);
    if (commitHandled) {
        return commitReport;
    }
    if (isImageCommand(invocation.command)) {
        return buildImageCommandReport(invocation, error);
    }
    if (isRawCommand(invocation.command)) {
        return buildRawCommandReport(invocation, error);
    }
    *error = QStringLiteral("Unsupported command: %1").arg(invocation.command);
    return std::nullopt;
}

bool isFileNameCommand(const QString& command) {
    static const QStringList kFileNameCommands = {
        QStringLiteral("commit-image-file-write"),
        QStringLiteral("commit-image-file-insert"),
        QStringLiteral("commit-image-file-clone"),
        QStringLiteral("commit-image-file-hardlink"),
        QStringLiteral("commit-image-file-delete"),
        QStringLiteral("commit-image-file-rename"),
        QStringLiteral("commit-raw-file-write"),
        QStringLiteral("commit-raw-file-insert"),
        QStringLiteral("commit-raw-file-clone"),
        QStringLiteral("commit-raw-file-hardlink"),
        QStringLiteral("commit-raw-file-delete"),
        QStringLiteral("commit-raw-file-rename"),
        QStringLiteral("commit-image-directory-child-write"),
        QStringLiteral("commit-image-directory-child-delete"),
        QStringLiteral("commit-raw-directory-child-write"),
        QStringLiteral("commit-raw-directory-child-delete"),
        QStringLiteral("commit-image-file-move"),
        QStringLiteral("commit-raw-file-move"),
        QStringLiteral("commit-image-file-patch"),
        QStringLiteral("commit-raw-file-patch")};
    return kFileNameCommands.contains(command);
}

bool isDirectoryNameCommand(const QString& command) {
    static const QStringList kDirectoryNameCommands = {
        QStringLiteral("commit-image-file-rename"),
        QStringLiteral("commit-raw-file-rename"),
        QStringLiteral("commit-image-file-clone"),
        QStringLiteral("commit-image-file-hardlink"),
        QStringLiteral("commit-image-directory-create"),
        QStringLiteral("commit-image-directory-delete"),
        QStringLiteral("commit-image-directory-child-write"),
        QStringLiteral("commit-image-directory-child-delete"),
        QStringLiteral("commit-raw-directory-create"),
        QStringLiteral("commit-raw-directory-delete"),
        QStringLiteral("commit-raw-directory-child-write"),
        QStringLiteral("commit-raw-directory-child-delete"),
    };
    return kDirectoryNameCommands.contains(command);
}

std::optional<QString> fileNameForCommand(const QCommandLineParser& parser,
                                          const QCommandLineOption& option,
                                          const QString& command,
                                          QString* error) {
    if (command == QStringLiteral("import-image")) {
        // Optional: an added/overwritten file to apply during arbitrary import.
        return parser.value(option).trimmed();
    }
    if (!isFileNameCommand(command)) {
        return QString();
    }
    const QString fileName = parser.value(option).trimmed();
    if (fileName.isEmpty()) {
        *error = QStringLiteral("--file-name is required for APFS root-file mutations.");
        return std::nullopt;
    }
    return fileName;
}

std::optional<QString> directoryNameForCommand(const QCommandLineParser& parser,
                                               const QCommandLineOption& directoryOption,
                                               const QCommandLineOption& fileOption,
                                               const QString& command,
                                               QString* error) {
    // File-move and file-patch commands take an OPTIONAL directory (empty = the root).
    if (command == QStringLiteral("commit-image-file-move") ||
        command == QStringLiteral("commit-raw-file-move") ||
        command == QStringLiteral("commit-image-file-patch") ||
        command == QStringLiteral("commit-raw-file-patch")) {
        return parser.value(directoryOption).trimmed();
    }
    if (!isDirectoryNameCommand(command)) {
        return QString();
    }
    QString directoryName = parser.value(directoryOption).trimmed();
    if (directoryName.isEmpty()) {
        directoryName = parser.value(fileOption).trimmed();
    }
    if (directoryName.isEmpty()) {
        *error = QStringLiteral("--directory-name is required for APFS root-directory mutations.");
        return std::nullopt;
    }
    return directoryName;
}

// Commands that accept an OPTIONAL payload (an empty payload is valid - e.g. import
// without an added file, or a zero-byte file).
bool commandTakesOptionalPayload(const QString& command) {
    static const QStringList kOptionalPayloadCommands = {
        QStringLiteral("import-image"),
        QStringLiteral("commit-image-file-write"),
        QStringLiteral("commit-image-file-insert"),
        QStringLiteral("commit-image-directory-child-write"),
        QStringLiteral("commit-raw-directory-child-write"),
        QStringLiteral("commit-raw-file-write"),
        QStringLiteral("commit-raw-file-insert"),
        QStringLiteral("commit-image-file-patch"),
        QStringLiteral("commit-raw-file-patch")};
    return kOptionalPayloadCommands.contains(command);
}

std::optional<QByteArray> payloadForCommand(const QCommandLineParser& parser,
                                            const QCommandLineOption& option,
                                            const QString& command,
                                            QString* error) {
    if (commandTakesOptionalPayload(command)) {
        const QString payloadPath = parser.value(option).trimmed();
        if (payloadPath.isEmpty()) {
            return QByteArray();
        }
        return readPayloadFile(payloadPath, error);
    }
    return QByteArray();
}

std::optional<uint64_t> patchOffsetForCommand(const QCommandLineParser& parser,
                                              const QCommandLineOption& option,
                                              const QString& command,
                                              QString* error) {
    // The COW patch commands take an optional offset (default 0 = patch from the start).
    const bool cowPatch = command == QStringLiteral("commit-image-file-patch") ||
                          command == QStringLiteral("commit-raw-file-patch");
    if (cowPatch && !parser.isSet(option)) {
        return 0ULL;
    }
    if (!cowPatch) {
        return 0ULL;
    }
    if (!parser.isSet(option)) {
        *error = QStringLiteral("--patch-offset-bytes is required for APFS root-file patch.");
        return std::nullopt;
    }
    return parseNonNegativeUInt64Option(parser, option, error);
}

// Owns every QCommandLineOption used by the CLI so the objects outlive parsing.
struct CliOptions {
    QCommandLineOption target{{QStringLiteral("target")},
                              QStringLiteral("Target raw partition or image path."),
                              QStringLiteral("path")};
    QCommandLineOption size{{QStringLiteral("size-bytes")},
                            QStringLiteral("Target APFS container size in bytes."),
                            QStringLiteral("bytes")};
    QCommandLineOption blockSize{{QStringLiteral("block-size-bytes")},
                                 QStringLiteral("APFS block size, default 4096."),
                                 QStringLiteral("bytes")};
    QCommandLineOption volumeName{{QStringLiteral("volume-name")},
                                  QStringLiteral("APFS volume name."),
                                  QStringLiteral("name"),
                                  QStringLiteral("SAK APFS")};
    QCommandLineOption additionalVolumeName{
        {QStringLiteral("additional-volume-name")},
        QStringLiteral("Name of an additional APFS volume (repeatable; multi-volume format)."),
        QStringLiteral("name")};
    QCommandLineOption outputImage{{QStringLiteral("output-image")},
                                   QStringLiteral("Image repair output path."),
                                   QStringLiteral("path")};
    QCommandLineOption fileName{{QStringLiteral("file-name")},
                                QStringLiteral(
                                    "Root or child file name for generated APFS writes."),
                                QStringLiteral("name")};
    QCommandLineOption directoryName{
        {QStringLiteral("directory-name")},
        QStringLiteral("Root directory name for generated APFS directory or child-file mutations."),
        QStringLiteral("name")};
    QCommandLineOption parentDirectoryPath{
        {QStringLiteral("parent-directory-path")},
        QStringLiteral("Parent directory the new directory nests under (empty = container root, "
                       "\"/docs\" or \"/docs/sub\"); for create-image/raw-root-directory."),
        QStringLiteral("path")};
    QCommandLineOption newFileName{{QStringLiteral("new-file-name")},
                                   QStringLiteral(
                                       "Destination file name for a generated APFS file move."),
                                   QStringLiteral("name")};
    QCommandLineOption destinationDirectoryName{
        {QStringLiteral("destination-directory-name")},
        QStringLiteral("Destination directory (empty = root) for a generated APFS file move."),
        QStringLiteral("name")};
    QCommandLineOption payload{{QStringLiteral("payload-file")},
                               QStringLiteral("Payload file for generated APFS writes or patches."),
                               QStringLiteral("path")};
    QCommandLineOption patchOffset{
        {QStringLiteral("patch-offset-bytes")},
        QStringLiteral("Byte offset for generated APFS partial root or child-file patch."),
        QStringLiteral("bytes")};
    QCommandLineOption snapshotName{{QStringLiteral("snapshot-name")},
                                    QStringLiteral(
                                        "Snapshot name for a generated APFS snapshot create."),
                                    QStringLiteral("name")};
    QCommandLineOption outputJson{{QStringLiteral("output-json")},
                                  QStringLiteral("Optional report JSON path."),
                                  QStringLiteral("path")};
    QCommandLineOption evidence{{QStringLiteral("evidence-id")},
                                QStringLiteral("Certification/evidence ID."),
                                QStringLiteral("id")};
    QCommandLineOption confirm{{QStringLiteral("confirm-target")},
                               QStringLiteral("Confirm destructive target mutation.")};
    QCommandLineOption allowRaw{{QStringLiteral("allow-raw-target")},
                                QStringLiteral("Permit Windows raw-device mutation.")};
    QCommandLineOption compressZlib{
        {QStringLiteral("compress-zlib")},
        QStringLiteral("Store the inserted/written file transparently compressed (inline zlib "
                       "com.apple.decmpfs); for commit-image/raw-file-insert, "
                       "commit-raw-file-write, and commit-raw-directory-child-write.")};
    QCommandLineOption compressLzfse{
        {QStringLiteral("compress-lzfse")},
        QStringLiteral("Store the inserted file transparently compressed (inline LZFSE, "
                       "com.apple.decmpfs algo 11); takes precedence over --compress-zlib.")};
    QCommandLineOption compressLzvn{
        {QStringLiteral("compress-lzvn")},
        QStringLiteral("Store the inserted file transparently compressed (inline LZVN, "
                       "com.apple.decmpfs algo 7); takes precedence over --compress-lzfse.")};
    QCommandLineOption compressLzbitmap{
        {QStringLiteral("compress-lzbitmap")},
        QStringLiteral("Store the inserted file transparently compressed (LZBITMAP, "
                       "com.apple.decmpfs algo 14, resource fork); works for any file size.")};
    QCommandLineOption volumePassword{
        {QStringLiteral("volume-password")},
        QStringLiteral("Format a software-encrypted (FileVault) volume unlockable by this "
                       "password; for format-image. Credential-in, never stored."),
        QStringLiteral("password")};
    QCommandLineOption recoveryKey{
        {QStringLiteral("recovery-key")},
        QStringLiteral("Add a personal-recovery-key unlock record (used with --volume-password); "
                       "the volume then unlocks by either the password or this recovery key; for "
                       "format-image. Credential-in, never stored."),
        QStringLiteral("recovery-key")};
    QCommandLineOption volumePasswordFile{
        {QStringLiteral("volume-password-file")},
        QStringLiteral("Read the FileVault volume password from this file's exact UTF-8 bytes "
                       "instead of --volume-password (keeps the credential off the command line "
                       "and out of any script text); for format-image/format-raw."),
        QStringLiteral("path")};
    QCommandLineOption recoveryKeyFile{
        {QStringLiteral("recovery-key-file")},
        QStringLiteral("Read the personal-recovery-key from this file's exact UTF-8 bytes instead "
                       "of --recovery-key (same off-command-line handling); for "
                       "format-image/format-raw."),
        QStringLiteral("path")};
    QCommandLineOption sparseSize{
        {QStringLiteral("sparse-size")},
        QStringLiteral("Insert the file sparse with a trailing hole: its extents cover --payload "
                       "and the inode's logical size is this many bytes (the gap reads as zeros); "
                       "for commit-image-file-insert."),
        QStringLiteral("bytes")};
    QCommandLineOption xattr{
        {QStringLiteral("xattr")},
        QStringLiteral("Attach a named extended attribute name=hexvalue (repeatable); ACL names "
                       "com.apple.system.Security / com.apple.FinderInfo set the matching inode "
                       "flag; for commit-image-file-insert."),
        QStringLiteral("name=hex")};
};

// Register every CLI option plus the positional command argument on `parser`.
void registerCliOptions(QCommandLineParser& parser, CliOptions& options) {
    parser.addOptions({options.target,
                       options.size,
                       options.blockSize,
                       options.volumeName,
                       options.additionalVolumeName,
                       options.outputImage,
                       options.fileName,
                       options.directoryName,
                       options.parentDirectoryPath,
                       options.newFileName,
                       options.destinationDirectoryName,
                       options.payload,
                       options.patchOffset,
                       options.snapshotName,
                       options.outputJson,
                       options.evidence,
                       options.confirm,
                       options.allowRaw,
                       options.compressZlib,
                       options.compressLzfse,
                       options.compressLzvn,
                       options.compressLzbitmap,
                       options.volumePassword,
                       options.recoveryKey,
                       options.volumePasswordFile,
                       options.recoveryKeyFile,
                       options.sparseSize,
                       options.xattr});
    parser.addPositionalArgument(
        QStringLiteral("command"),
        QStringLiteral(
            "format-image, repair-image, change-image-volume-label, format-raw, "
            "change-raw-volume-label, repair-raw, commit-raw-file-insert, commit-raw-file-delete, "
            "or commit-raw-file-rename."));
}

// Bind the parsed options into a CliInvocation via invocationFromParser.
std::optional<CliInvocation> parseCliInvocation(const QCommandLineParser& parser,
                                                const CliOptions& options,
                                                QString* error) {
    return invocationFromParser(parser,
                                {.target = &options.target,
                                 .size = &options.size,
                                 .block_size = &options.blockSize,
                                 .volume_name = &options.volumeName,
                                 .additional_volume_name = &options.additionalVolumeName,
                                 .output_image = &options.outputImage,
                                 .file_name = &options.fileName,
                                 .directory_name = &options.directoryName,
                                 .parent_directory_path = &options.parentDirectoryPath,
                                 .new_file_name = &options.newFileName,
                                 .destination_directory_name = &options.destinationDirectoryName,
                                 .payload = &options.payload,
                                 .patch_offset = &options.patchOffset,
                                 .snapshot_name = &options.snapshotName,
                                 .evidence = &options.evidence,
                                 .confirm = &options.confirm,
                                 .allow_raw = &options.allowRaw,
                                 .compress_zlib = &options.compressZlib,
                                 .compress_lzfse = &options.compressLzfse,
                                 .compress_lzvn = &options.compressLzvn,
                                 .compress_lzbitmap = &options.compressLzbitmap,
                                 .volume_password = &options.volumePassword,
                                 .recovery_key = &options.recoveryKey,
                                 .volume_password_file = &options.volumePasswordFile,
                                 .recovery_key_file = &options.recoveryKeyFile,
                                 .sparse_size = &options.sparseSize,
                                 .xattr = &options.xattr},
                                error);
}

}  // namespace

#ifdef _WIN32
// Physical drive number backing the OS system volume, or -1 if it cannot be
// determined. Native (no elevation needed).
int osSystemPhysicalDrive() {
    wchar_t winDir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH || winDir[1] != L':') {
        return -1;
    }
    const wchar_t volumePath[] = {L'\\', L'\\', L'.', L'\\', winDir[0], L':', L'\0'};
    HANDLE hVol = CreateFileW(
        volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        return -1;
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
    if (!ok || extents->NumberOfDiskExtents == 0) {
        return -1;
    }
    return static_cast<int>(extents->Extents[0].DiskNumber);
}

// Physical drive numbers backing a volume/device alias (\\.\C:, \\?\Volume{GUID},
// \\.\GLOBALROOT\Device\HarddiskVolumeN). Empty when the path cannot be opened or
// its disk extents cannot be queried. Native (no elevation needed).
std::vector<int> volumeBackingPhysicalDrives(const QString& path) {
    std::vector<int> drives;
    const std::wstring wide = path.toStdWString();
    HANDLE handle = CreateFileW(
        wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return drives;
    }
    constexpr DWORD kMaxExtents = 32;
    const DWORD bufSize = sizeof(VOLUME_DISK_EXTENTS) + (kMaxExtents - 1) * sizeof(DISK_EXTENT);
    std::vector<unsigned char> buffer(bufSize, 0);
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buffer.data());
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(handle,
                                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                    nullptr,
                                    0,
                                    extents,
                                    bufSize,
                                    &bytesReturned,
                                    nullptr);
    CloseHandle(handle);
    if (!ok) {
        return drives;
    }
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        drives.push_back(static_cast<int>(extents->Extents[i].DiskNumber));
    }
    return drives;
}
#endif

// A \\.\X: or \\?\X: whole-volume handle addressed by drive letter.
bool isDriveLetterVolumePath(const QString& path) {
    if (path.size() != 6 || path[5] != QLatin1Char(':') || !path[4].isLetter()) {
        return false;
    }
    return path.startsWith(QStringLiteral("\\\\.\\")) || path.startsWith(QStringLiteral("\\\\?\\"));
}

// Raw volume/device aliases that resolve to a whole disk but are NOT the
// \\.\PhysicalDriveN form the numeric guard covers: drive-letter volumes,
// \Volume{GUID} handles, and GLOBALROOT device paths. A \\.\C: alias resolves to
// the OS volume yet slips past a PhysicalDrive-prefix-only check.
bool isWindowsRawVolumeAliasPath(const QString& path) {
    if (isDriveLetterVolumePath(path)) {
        return true;
    }
    if (!path.startsWith(QStringLiteral("\\\\"))) {
        return false;
    }
    return path.contains(QStringLiteral("Volume{"), Qt::CaseInsensitive) ||
           path.contains(QStringLiteral("GLOBALROOT"), Qt::CaseInsensitive);
}

// Refuse a \\.\PhysicalDriveN target that is PhysicalDrive0 or the OS system disk.
// Fails closed when the number cannot be parsed or the OS identity is unknown.
QString physicalDriveTargetRefusal(const QString& path, const QString& prefix) {
    bool ok = false;
    const int drive = path.mid(prefix.size()).toInt(&ok);
    if (!ok || drive < 0) {
        return QStringLiteral(
            "Refusing raw APFS write: could not parse the PhysicalDrive number of the target");
    }
    if (drive == 0) {
        return QStringLiteral(
            "Refusing raw APFS write to PhysicalDrive0 (the first physical disk)");
    }
#ifdef _WIN32
    const int osDrive = osSystemPhysicalDrive();
    if (osDrive < 0) {
        return QStringLiteral(
            "Refusing raw APFS write: could not establish the OS system-disk identity");
    }
    if (osDrive == drive) {
        return QStringLiteral(
            "Refusing raw APFS write: target PhysicalDrive backs the OS system volume");
    }
#endif
    return QString();
}

// Refuse a volume/device alias (\\.\C:, \Volume{GUID}, GLOBALROOT) whose backing
// disk is PhysicalDrive0 or the OS system disk. Fails closed when the OS identity
// or the alias's backing drive cannot be resolved.
QString volumeAliasTargetRefusal(const QString& path) {
#ifdef _WIN32
    const int osDrive = osSystemPhysicalDrive();
    if (osDrive < 0) {
        return QStringLiteral(
            "Refusing raw APFS write: could not establish the OS system-disk identity");
    }
    const std::vector<int> drives = volumeBackingPhysicalDrives(path);
    if (drives.empty()) {
        return QStringLiteral(
            "Refusing raw APFS write: could not resolve the backing physical drive of the "
            "volume/device target");
    }
    for (const int drive : drives) {
        if (drive == 0 || drive == osDrive) {
            return QStringLiteral(
                "Refusing raw APFS write: volume/device target resolves to the OS system disk "
                "or PhysicalDrive0");
        }
    }
#else
    Q_UNUSED(path);
#endif
    return QString();
}

// Defense-in-depth OS-disk guard for confirmed raw-device APFS commands. The GUI
// path is guarded by PartitionSafetyValidator::isUnsafeRawWriteTargetDisk, but
// this CLI bypasses it, so a confirmed --allow-raw path typo could otherwise
// format PhysicalDrive0 or the OS disk. Covers both the \\.\PhysicalDriveN form
// and volume/device aliases (\\.\C:, \Volume{GUID}, GLOBALROOT) that resolve to
// the OS volume yet bypass a PhysicalDrive-prefix-only check. Returns a refusal
// reason, or an empty string when the target is provably a different disk / not a
// raw whole-disk form this guard covers.
QString rawTargetProtectedDiskRefusal(const CliInvocation& invocation) {
    if (!invocation.allow_raw_target) {
        return QString();
    }
    QString path = invocation.target_path.trimmed();
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const QString prefix = QStringLiteral("\\\\.\\PhysicalDrive");
    if (path.startsWith(prefix, Qt::CaseInsensitive)) {
        return physicalDriveTargetRefusal(path, prefix);
    }
    if (isWindowsRawVolumeAliasPath(path)) {
        return volumeAliasTargetRefusal(path);
    }
    return QString();
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sak_apfs_writer_cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.9.2.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "S.A.K. APFS generated-container writer. Supports APFS image build/repair/file/directory "
        "and volume-label mutation plus confirmed raw-partition format/repair and certifier raw "
        "file/directory/volume-label mutation."));
    parser.addHelpOption();
    parser.addVersionOption();

    CliOptions options;
    registerCliOptions(parser, options);
    parser.process(app);

    QString parseError;
    const auto invocation = parseCliInvocation(parser, options, &parseError);
    if (!invocation.has_value()) {
        QTextStream(stderr) << parseError << Qt::endl;
        return kExitInvalidArguments;
    }

    const QString outputJsonPath = parser.value(options.outputJson).trimmed();
    if (outputJsonAliasesTarget(
            outputJsonPath, invocation->target_path, invocation->output_image_path)) {
        QTextStream(stderr) << "--output-json path must not alias the target or output image"
                            << Qt::endl;
        return kExitInvalidArguments;
    }

    // Defense-in-depth: never let a confirmed --allow-raw command hit the OS disk
    // or PhysicalDrive0 (the GUI validator does not run for this CLI).
    const QString rawRefusal = rawTargetProtectedDiskRefusal(*invocation);
    if (!rawRefusal.isEmpty()) {
        QTextStream(stderr) << rawRefusal << Qt::endl;
        return kExitInvalidArguments;
    }

    QString commandError;
    const auto report = buildCommandReport(*invocation, &commandError);
    if (!report.has_value()) {
        QTextStream(stderr) << commandError << Qt::endl;
        return kExitInvalidArguments;
    }

    QString reportError;
    if (!writeReport(*report, outputJsonPath, &reportError)) {
        QTextStream(stderr) << "Failed to write report: " << reportError << Qt::endl;
        return kExitReportFailed;
    }
    return report->value(QStringLiteral("ok")).toBool(false) ? kExitOk : kExitOperationFailed;
}
