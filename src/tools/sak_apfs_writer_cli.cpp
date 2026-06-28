// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file sak_apfs_writer_cli.cpp
/// @brief Production command-line bridge for certified S.A.K. APFS writer operations.

#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_apfs_writer.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <limits>
#include <optional>

namespace {

constexpr uint32_t kDefaultApfsBlockSizeBytes = 4096;
constexpr uint64_t kDefaultApfsMaxPayloadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr int kExitOk = 0;
constexpr int kExitOperationFailed = 1;
constexpr int kExitInvalidArguments = 2;
constexpr int kExitReportFailed = 3;

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
    return file.readAll();
}

sak::PartitionApfsWriteOptions imageWriteOptions(const QString& evidenceId) {
    sak::PartitionApfsWriteOptions options;
    options.enable_experimental_writer = true;
    options.image_only = true;
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

QJsonObject fileWriteImageReport(const QString& command,
                                 const sak::PartitionApfsImageFileWriteResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("source_image_path"), result.source_image_path);
    report.insert(QStringLiteral("written_image_path"), result.written_image_path);
    report.insert(QStringLiteral("written_image_sha256"), result.written_image_sha256);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
    report.insert(QStringLiteral("file_name"), result.file_name);
    report.insert(QStringLiteral("file_bytes"), QString::number(result.file_bytes));
    report.insert(QStringLiteral("payload_sha256"), result.payload_sha256);
    report.insert(QStringLiteral("readback_sha256"), result.readback_sha256);
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(result.written_data_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject filePatchImageReport(const QString& command,
                                 const sak::PartitionApfsImageFilePatchResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("source_image_path"), result.source_image_path);
    report.insert(QStringLiteral("written_image_path"), result.written_image_path);
    report.insert(QStringLiteral("written_image_sha256"), result.written_image_sha256);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
    report.insert(QStringLiteral("file_name"), result.file_name);
    report.insert(QStringLiteral("file_bytes"), QString::number(result.file_bytes));
    report.insert(QStringLiteral("patch_offset_bytes"), QString::number(result.patch_offset_bytes));
    report.insert(QStringLiteral("patch_bytes"), QString::number(result.patch_bytes));
    report.insert(QStringLiteral("patch_sha256"), result.patch_sha256);
    report.insert(QStringLiteral("readback_sha256"), result.readback_sha256);
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(result.written_data_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject fileDeleteImageReport(const QString& command,
                                  const sak::PartitionApfsImageFileDeleteResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("source_image_path"), result.source_image_path);
    report.insert(QStringLiteral("written_image_path"), result.written_image_path);
    report.insert(QStringLiteral("written_image_sha256"), result.written_image_sha256);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
    report.insert(QStringLiteral("file_name"), result.file_name);
    report.insert(QStringLiteral("deleted_file_bytes"), QString::number(result.deleted_file_bytes));
    report.insert(QStringLiteral("deleted_file_sha256"), result.deleted_file_sha256);
    report.insert(QStringLiteral("freed_data_blocks"), QString::number(result.freed_data_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject directoryImageReport(const QString& command,
                                 const sak::PartitionApfsImageDirectoryMutationResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("source_image_path"), result.source_image_path);
    report.insert(QStringLiteral("written_image_path"), result.written_image_path);
    report.insert(QStringLiteral("written_image_sha256"), result.written_image_sha256);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject fileWriteRawReport(const QString& command,
                               const sak::PartitionApfsRawFileWriteResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("target_path"), result.target_path);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
    report.insert(QStringLiteral("file_name"), result.file_name);
    report.insert(QStringLiteral("file_bytes"), QString::number(result.file_bytes));
    report.insert(QStringLiteral("payload_sha256"), result.payload_sha256);
    report.insert(QStringLiteral("readback_sha256"), result.readback_sha256);
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(result.written_data_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject fileDeleteRawReport(const QString& command,
                                const sak::PartitionApfsRawFileDeleteResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("target_path"), result.target_path);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
    report.insert(QStringLiteral("file_name"), result.file_name);
    report.insert(QStringLiteral("deleted_file_bytes"), QString::number(result.deleted_file_bytes));
    report.insert(QStringLiteral("deleted_file_sha256"), result.deleted_file_sha256);
    report.insert(QStringLiteral("freed_data_blocks"), QString::number(result.freed_data_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject filePatchRawReport(const QString& command,
                               const sak::PartitionApfsRawFilePatchResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("target_path"), result.target_path);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
    report.insert(QStringLiteral("file_name"), result.file_name);
    report.insert(QStringLiteral("file_bytes"), QString::number(result.file_bytes));
    report.insert(QStringLiteral("patch_offset_bytes"), QString::number(result.patch_offset_bytes));
    report.insert(QStringLiteral("patch_bytes"), QString::number(result.patch_bytes));
    report.insert(QStringLiteral("patch_sha256"), result.patch_sha256);
    report.insert(QStringLiteral("readback_sha256"), result.readback_sha256);
    report.insert(QStringLiteral("written_data_blocks"),
                  QString::number(result.written_data_blocks));
    report.insert(QStringLiteral("blockers"), toJsonArray(result.blockers));
    report.insert(QStringLiteral("warnings"), toJsonArray(result.warnings));
    insertPlan(&report, result.plan);
    return report;
}

QJsonObject directoryRawReport(const QString& command,
                               const sak::PartitionApfsRawDirectoryMutationResult& result) {
    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_apfs_writer_cli"));
    report.insert(QStringLiteral("command"), command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("target_path"), result.target_path);
    report.insert(QStringLiteral("directory_name"), result.directory_name);
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

    QFile file(outputPath);
    if (!QFileInfo(outputPath).absoluteDir().exists()) {
        QDir().mkpath(QFileInfo(outputPath).absolutePath());
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *error = file.errorString();
        return false;
    }
    return true;
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
    const QCommandLineOption* new_file_name{nullptr};
    const QCommandLineOption* destination_directory_name{nullptr};
    const QCommandLineOption* payload{nullptr};
    const QCommandLineOption* patch_offset{nullptr};
    const QCommandLineOption* snapshot_name{nullptr};
    const QCommandLineOption* evidence{nullptr};
    const QCommandLineOption* confirm{nullptr};
    const QCommandLineOption* allow_raw{nullptr};
    const QCommandLineOption* compress_zlib{nullptr};
    const QCommandLineOption* volume_password{nullptr};
    const QCommandLineOption* recovery_key{nullptr};
    const QCommandLineOption* sparse_size{nullptr};
    const QCommandLineOption* xattr{nullptr};
};

// Parse repeatable --xattr name=hexvalue options into (name, value) pairs.
[[nodiscard]] QVector<QPair<QByteArray, QByteArray>> parseXattrOptions(const QStringList& values) {
    QVector<QPair<QByteArray, QByteArray>> out;
    for (const QString& spec : values) {
        const int eq = spec.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        out.append({spec.left(eq).toUtf8(), QByteArray::fromHex(spec.mid(eq + 1).toUtf8())});
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
    return CliInvocation{.command = *command,
                         .target_path = *target,
                         .target_size_bytes = numeric->size,
                         .block_size_bytes = numeric->block_size,
                         .volume_name = parser.value(*options.volume_name),
                         .additional_volume_names = parser.values(*options.additional_volume_name),
                         .output_image_path = parser.value(*options.output_image).trimmed(),
                         .file_name = mutation->file_name,
                         .directory_name = mutation->directory_name,
                         .new_file_name = parser.value(*options.new_file_name).trimmed(),
                         .destination_directory_name =
                             parser.value(*options.destination_directory_name).trimmed(),
                         .payload = mutation->payload,
                         .patch_offset_bytes = mutation->patch_offset,
                         .patch_offset_error = mutation->patch_offset_error,
                         .snapshot_name = parser.value(*options.snapshot_name).trimmed(),
                         .evidence_id = evidenceIdForCommand(parser, *options.evidence, *command),
                         .volume_password = parser.value(*options.volume_password),
                         .recovery_key = parser.value(*options.recovery_key),
                         .confirm_target = parser.isSet(*options.confirm),
                         .allow_raw_target = parser.isSet(*options.allow_raw),
                         .compress_zlib = parser.isSet(*options.compress_zlib),
                         .sparse_logical_size = parser.value(*options.sparse_size).toULongLong(),
                         .xattrs = parseXattrOptions(parser.values(*options.xattr))};
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
                             .target_wipe_confirmed = invocation.confirm_target,
                             .allow_raw_device_target = invocation.allow_raw_target,
                             .options = rawWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildWriteImageReport(const CliInvocation& invocation, QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for write-image-root-file.");
        return std::nullopt;
    }
    return fileWriteImageReport(invocation.command,
                                sak::PartitionApfsWriter::writeImageOnlyRootFile(
                                    {.source_image_path = invocation.target_path,
                                     .written_image_path = invocation.output_image_path,
                                     .file_name = invocation.file_name,
                                     .file_data = invocation.payload,
                                     .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildWriteDirectoryFileImageReport(const CliInvocation& invocation,
                                                              QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for write-image-root-directory-file.");
        return std::nullopt;
    }
    return fileWriteImageReport(invocation.command,
                                sak::PartitionApfsWriter::writeImageOnlyRootDirectoryFile(
                                    {.source_image_path = invocation.target_path,
                                     .written_image_path = invocation.output_image_path,
                                     .directory_name = invocation.directory_name,
                                     .file_name = invocation.file_name,
                                     .file_data = invocation.payload,
                                     .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildPatchImageReport(const CliInvocation& invocation, QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for patch-image-root-file.");
        return std::nullopt;
    }
    if (!invocation.patch_offset_error.isEmpty()) {
        *error = invocation.patch_offset_error;
        return std::nullopt;
    }
    return filePatchImageReport(invocation.command,
                                sak::PartitionApfsWriter::patchImageOnlyRootFile(
                                    {.source_image_path = invocation.target_path,
                                     .written_image_path = invocation.output_image_path,
                                     .file_name = invocation.file_name,
                                     .patch_offset_bytes = invocation.patch_offset_bytes,
                                     .patch_data = invocation.payload,
                                     .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildPatchDirectoryFileImageReport(const CliInvocation& invocation,
                                                              QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for patch-image-root-directory-file.");
        return std::nullopt;
    }
    if (!invocation.patch_offset_error.isEmpty()) {
        *error = invocation.patch_offset_error;
        return std::nullopt;
    }
    return filePatchImageReport(invocation.command,
                                sak::PartitionApfsWriter::patchImageOnlyRootDirectoryFile(
                                    {.source_image_path = invocation.target_path,
                                     .written_image_path = invocation.output_image_path,
                                     .directory_name = invocation.directory_name,
                                     .file_name = invocation.file_name,
                                     .patch_offset_bytes = invocation.patch_offset_bytes,
                                     .patch_data = invocation.payload,
                                     .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildDeleteImageReport(const CliInvocation& invocation, QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for delete-image-root-file.");
        return std::nullopt;
    }
    return fileDeleteImageReport(invocation.command,
                                 sak::PartitionApfsWriter::deleteImageOnlyRootFile(
                                     {.source_image_path = invocation.target_path,
                                      .written_image_path = invocation.output_image_path,
                                      .file_name = invocation.file_name,
                                      .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildDeleteDirectoryFileImageReport(const CliInvocation& invocation,
                                                               QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for delete-image-root-directory-file.");
        return std::nullopt;
    }
    return fileDeleteImageReport(invocation.command,
                                 sak::PartitionApfsWriter::deleteImageOnlyRootDirectoryFile(
                                     {.source_image_path = invocation.target_path,
                                      .written_image_path = invocation.output_image_path,
                                      .directory_name = invocation.directory_name,
                                      .file_name = invocation.file_name,
                                      .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildCreateDirectoryImageReport(const CliInvocation& invocation,
                                                           QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for create-image-root-directory.");
        return std::nullopt;
    }
    return directoryImageReport(invocation.command,
                                sak::PartitionApfsWriter::createImageOnlyRootDirectory(
                                    {.source_image_path = invocation.target_path,
                                     .written_image_path = invocation.output_image_path,
                                     .directory_name = invocation.directory_name,
                                     .options = imageWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildDeleteDirectoryImageReport(const CliInvocation& invocation,
                                                           QString* error) {
    if (invocation.output_image_path.isEmpty()) {
        *error = QStringLiteral("--output-image is required for delete-image-root-directory.");
        return std::nullopt;
    }
    return directoryImageReport(invocation.command,
                                sak::PartitionApfsWriter::deleteImageOnlyRootDirectory(
                                    {.source_image_path = invocation.target_path,
                                     .written_image_path = invocation.output_image_path,
                                     .directory_name = invocation.directory_name,
                                     .options = imageWriteOptions(invocation.evidence_id)}));
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

QJsonObject buildWriteRawReport(const CliInvocation& invocation) {
    return fileWriteRawReport(invocation.command,
                              sak::PartitionApfsWriter::writeRawRootFile(
                                  {.target_path = invocation.target_path,
                                   .target_container_bytes = invocation.target_size_bytes,
                                   .file_name = invocation.file_name,
                                   .file_data = invocation.payload,
                                   .target_write_confirmed = invocation.confirm_target,
                                   .allow_raw_device_target = invocation.allow_raw_target,
                                   .options = rawWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildWriteDirectoryFileRawReport(const CliInvocation& invocation) {
    return fileWriteRawReport(invocation.command,
                              sak::PartitionApfsWriter::writeRawRootDirectoryFile(
                                  {.target_path = invocation.target_path,
                                   .target_container_bytes = invocation.target_size_bytes,
                                   .directory_name = invocation.directory_name,
                                   .file_name = invocation.file_name,
                                   .file_data = invocation.payload,
                                   .target_write_confirmed = invocation.confirm_target,
                                   .allow_raw_device_target = invocation.allow_raw_target,
                                   .options = rawWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildDeleteRawReport(const CliInvocation& invocation) {
    return fileDeleteRawReport(invocation.command,
                               sak::PartitionApfsWriter::deleteRawRootFile(
                                   {.target_path = invocation.target_path,
                                    .target_container_bytes = invocation.target_size_bytes,
                                    .file_name = invocation.file_name,
                                    .target_write_confirmed = invocation.confirm_target,
                                    .allow_raw_device_target = invocation.allow_raw_target,
                                    .options = rawWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildDeleteDirectoryFileRawReport(const CliInvocation& invocation) {
    return fileDeleteRawReport(invocation.command,
                               sak::PartitionApfsWriter::deleteRawRootDirectoryFile(
                                   {.target_path = invocation.target_path,
                                    .target_container_bytes = invocation.target_size_bytes,
                                    .directory_name = invocation.directory_name,
                                    .file_name = invocation.file_name,
                                    .target_write_confirmed = invocation.confirm_target,
                                    .allow_raw_device_target = invocation.allow_raw_target,
                                    .options = rawWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildPatchDirectoryFileRawReport(const CliInvocation& invocation,
                                                            QString* error) {
    if (!invocation.patch_offset_error.isEmpty()) {
        *error = invocation.patch_offset_error;
        return std::nullopt;
    }
    return filePatchRawReport(invocation.command,
                              sak::PartitionApfsWriter::patchRawRootDirectoryFile(
                                  {.target_path = invocation.target_path,
                                   .target_container_bytes = invocation.target_size_bytes,
                                   .directory_name = invocation.directory_name,
                                   .file_name = invocation.file_name,
                                   .patch_offset_bytes = invocation.patch_offset_bytes,
                                   .patch_data = invocation.payload,
                                   .target_write_confirmed = invocation.confirm_target,
                                   .allow_raw_device_target = invocation.allow_raw_target,
                                   .options = rawWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildCreateDirectoryRawReport(const CliInvocation& invocation) {
    return directoryRawReport(invocation.command,
                              sak::PartitionApfsWriter::createRawRootDirectory(
                                  {.target_path = invocation.target_path,
                                   .target_container_bytes = invocation.target_size_bytes,
                                   .directory_name = invocation.directory_name,
                                   .target_write_confirmed = invocation.confirm_target,
                                   .allow_raw_device_target = invocation.allow_raw_target,
                                   .options = rawWriteOptions(invocation.evidence_id)}));
}

QJsonObject buildDeleteDirectoryRawReport(const CliInvocation& invocation) {
    return directoryRawReport(invocation.command,
                              sak::PartitionApfsWriter::deleteRawRootDirectory(
                                  {.target_path = invocation.target_path,
                                   .target_container_bytes = invocation.target_size_bytes,
                                   .directory_name = invocation.directory_name,
                                   .target_write_confirmed = invocation.confirm_target,
                                   .allow_raw_device_target = invocation.allow_raw_target,
                                   .options = rawWriteOptions(invocation.evidence_id)}));
}

std::optional<QJsonObject> buildPatchRawReport(const CliInvocation& invocation, QString* error) {
    if (!invocation.patch_offset_error.isEmpty()) {
        *error = invocation.patch_offset_error;
        return std::nullopt;
    }
    return filePatchRawReport(invocation.command,
                              sak::PartitionApfsWriter::patchRawRootFile(
                                  {.target_path = invocation.target_path,
                                   .target_container_bytes = invocation.target_size_bytes,
                                   .file_name = invocation.file_name,
                                   .patch_offset_bytes = invocation.patch_offset_bytes,
                                   .patch_data = invocation.payload,
                                   .target_write_confirmed = invocation.confirm_target,
                                   .allow_raw_device_target = invocation.allow_raw_target,
                                   .options = rawWriteOptions(invocation.evidence_id)}));
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
        QStringLiteral("write-image-root-file"),
        QStringLiteral("write-image-root-directory-file"),
        QStringLiteral("patch-image-root-file"),
        QStringLiteral("patch-image-root-directory-file"),
        QStringLiteral("delete-image-root-file"),
        QStringLiteral("delete-image-root-directory-file"),
        QStringLiteral("create-image-root-directory"),
        QStringLiteral("delete-image-root-directory"),
        QStringLiteral("change-image-volume-label"),
        QStringLiteral("list-image"),
        QStringLiteral("import-image"),
    };
    return kImageCommands.contains(command);
}

bool isRawCommand(const QString& command) {
    static const QStringList kRawCommands = {
        QStringLiteral("format-raw"),
        QStringLiteral("write-raw-root-file"),
        QStringLiteral("write-raw-root-directory-file"),
        QStringLiteral("patch-raw-root-file"),
        QStringLiteral("patch-raw-root-directory-file"),
        QStringLiteral("delete-raw-root-file"),
        QStringLiteral("delete-raw-root-directory-file"),
        QStringLiteral("create-raw-root-directory"),
        QStringLiteral("delete-raw-root-directory"),
        QStringLiteral("change-raw-volume-label"),
        QStringLiteral("repair-raw"),
    };
    return kRawCommands.contains(command);
}

bool isImageFileMutationCommand(const QString& command) {
    return command == QStringLiteral("write-image-root-file") ||
           command == QStringLiteral("write-image-root-directory-file") ||
           command == QStringLiteral("patch-image-root-file") ||
           command == QStringLiteral("patch-image-root-directory-file") ||
           command == QStringLiteral("delete-image-root-file") ||
           command == QStringLiteral("delete-image-root-directory-file");
}

bool isImageDirectoryCommand(const QString& command) {
    return command == QStringLiteral("create-image-root-directory") ||
           command == QStringLiteral("delete-image-root-directory");
}

bool isRawFileMutationCommand(const QString& command) {
    return command == QStringLiteral("write-raw-root-file") ||
           command == QStringLiteral("write-raw-root-directory-file") ||
           command == QStringLiteral("patch-raw-root-file") ||
           command == QStringLiteral("patch-raw-root-directory-file") ||
           command == QStringLiteral("delete-raw-root-file") ||
           command == QStringLiteral("delete-raw-root-directory-file");
}

bool isRawDirectoryCommand(const QString& command) {
    return command == QStringLiteral("create-raw-root-directory") ||
           command == QStringLiteral("delete-raw-root-directory");
}

std::optional<QJsonObject> buildImageFileMutationReport(const CliInvocation& invocation,
                                                        QString* error) {
    if (invocation.command == QStringLiteral("write-image-root-file")) {
        return buildWriteImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("write-image-root-directory-file")) {
        return buildWriteDirectoryFileImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("patch-image-root-file")) {
        return buildPatchImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("patch-image-root-directory-file")) {
        return buildPatchDirectoryFileImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("delete-image-root-file")) {
        return buildDeleteImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("delete-image-root-directory-file")) {
        return buildDeleteDirectoryFileImageReport(invocation, error);
    }
    *error = QStringLiteral("Unsupported image file command: %1").arg(invocation.command);
    return std::nullopt;
}

std::optional<QJsonObject> buildImageDirectoryReport(const CliInvocation& invocation,
                                                     QString* error) {
    if (invocation.command == QStringLiteral("create-image-root-directory")) {
        return buildCreateDirectoryImageReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("delete-image-root-directory")) {
        return buildDeleteDirectoryImageReport(invocation, error);
    }
    *error = QStringLiteral("Unsupported image directory command: %1").arg(invocation.command);
    return std::nullopt;
}

std::optional<QJsonObject> buildRawFileMutationReport(const CliInvocation& invocation,
                                                      QString* error) {
    if (invocation.command == QStringLiteral("write-raw-root-file")) {
        return buildWriteRawReport(invocation);
    }
    if (invocation.command == QStringLiteral("write-raw-root-directory-file")) {
        return buildWriteDirectoryFileRawReport(invocation);
    }
    if (invocation.command == QStringLiteral("patch-raw-root-file")) {
        return buildPatchRawReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("patch-raw-root-directory-file")) {
        return buildPatchDirectoryFileRawReport(invocation, error);
    }
    if (invocation.command == QStringLiteral("delete-raw-root-file")) {
        return buildDeleteRawReport(invocation);
    }
    if (invocation.command == QStringLiteral("delete-raw-root-directory-file")) {
        return buildDeleteDirectoryFileRawReport(invocation);
    }
    *error = QStringLiteral("Unsupported raw file command: %1").arg(invocation.command);
    return std::nullopt;
}

std::optional<QJsonObject> buildRawDirectoryReport(const CliInvocation& invocation,
                                                   QString* error) {
    if (invocation.command == QStringLiteral("create-raw-root-directory")) {
        return buildCreateDirectoryRawReport(invocation);
    }
    if (invocation.command == QStringLiteral("delete-raw-root-directory")) {
        return buildDeleteDirectoryRawReport(invocation);
    }
    *error = QStringLiteral("Unsupported raw directory command: %1").arg(invocation.command);
    return std::nullopt;
}

std::optional<QJsonObject> buildImageCommandReport(const CliInvocation& invocation,
                                                   QString* error) {
    if (invocation.command == QStringLiteral("format-image")) {
        return buildFormatImageReport(invocation);
    }
    if (invocation.command == QStringLiteral("repair-image")) {
        return buildRepairImageReport(invocation, error);
    }
    if (isImageFileMutationCommand(invocation.command)) {
        return buildImageFileMutationReport(invocation, error);
    }
    if (isImageDirectoryCommand(invocation.command)) {
        return buildImageDirectoryReport(invocation, error);
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
    if (isRawFileMutationCommand(invocation.command)) {
        return buildRawFileMutationReport(invocation, error);
    }
    if (isRawDirectoryCommand(invocation.command)) {
        return buildRawDirectoryReport(invocation, error);
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
    const auto listing = sak::PartitionApfsFileSystemReader::listDirectoryFromImage(
        sourcePath, QStringLiteral("/"), sak::kPartitionApfsDefaultBrowseEntryLimit);
    if (!listing.ok) {
        *error = QStringLiteral("Unable to read source container: %1")
                     .arg(listing.blockers.join(QStringLiteral("; ")));
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
    const uint32_t blockSize = invocation.block_size_bytes;

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        *error = QStringLiteral("Unable to create scratch directory for arbitrary import");
        return std::nullopt;
    }
    const auto options = imageWriteOptions(invocation.evidence_id);
    const QString freshPath = scratch.filePath(QStringLiteral("import-fresh.apfs"));
    const auto formatResult = sak::PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = freshPath,
         .target_container_bytes = invocation.target_size_bytes,
         .block_size_bytes = blockSize,
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
            sak::PartitionApfsWriter::writeImageOnlyRootFile({.source_image_path = currentPath,
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

    if (!QFile::exists(currentPath)) {
        *error = QStringLiteral("Arbitrary import produced no output");
        return std::nullopt;
    }
    QFile::remove(outputPath);
    if (!QFile::copy(currentPath, outputPath)) {
        *error = QStringLiteral("Unable to write imported container to %1").arg(outputPath);
        return std::nullopt;
    }

    QJsonObject report;
    report.insert(QStringLiteral("ok"), true);
    report.insert(QStringLiteral("operation"), QStringLiteral("Arbitrary APFS import"));
    report.insert(QStringLiteral("source_image"), sourcePath);
    report.insert(QStringLiteral("output_image"), outputPath);
    report.insert(QStringLiteral("volume_name"), volumeName);
    report.insert(QStringLiteral("imported_file_count"), static_cast<int>(files.size()));
    return report;
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
         .compress_zlib = invocation.compress_zlib,
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
         .options = imageWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS in-place snapshot delete commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawSnapshotDeleteReport(const CliInvocation& invocation,
                                                              QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawSnapshotDelete(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
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
         .options = imageWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS in-place snapshot revert commit"), invocation, error);
}

std::optional<QJsonObject> buildCommitRawSnapshotRevertReport(const CliInvocation& invocation,
                                                              QString* error) {
    const auto commit = sak::PartitionApfsWriter::commitRawSnapshotRevert(
        {.target_path = invocation.target_path,
         .target_container_bytes = invocation.target_size_bytes,
         .target_mutation_confirmed = invocation.confirm_target,
         .allow_raw_device_target = invocation.allow_raw_target,
         .options = rawWriteOptions(invocation.evidence_id)});
    return commitResultReport(
        commit, QStringLiteral("APFS raw in-place snapshot revert commit"), invocation, error);
}

// Dispatch the in-place commit family (image + raw). Sets *handled when the
// command is a commit command, keeping buildCommandReport's branch count low.
std::optional<QJsonObject> buildCommitCommandReport(const CliInvocation& invocation,
                                                    QString* error,
                                                    bool* handled) {
    using CommitBuilder = std::optional<QJsonObject> (*)(const CliInvocation&, QString*);
    static const std::array<std::pair<QLatin1StringView, CommitBuilder>, 30> kCommitBuilders = {{
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

bool isFileWriteCommand(const QString& command) {
    return command == QStringLiteral("write-image-root-file") ||
           command == QStringLiteral("write-image-root-directory-file") ||
           command == QStringLiteral("patch-image-root-file") ||
           command == QStringLiteral("patch-image-root-directory-file") ||
           command == QStringLiteral("patch-raw-root-file") ||
           command == QStringLiteral("patch-raw-root-directory-file") ||
           command == QStringLiteral("write-raw-root-directory-file") ||
           command == QStringLiteral("write-raw-root-file");
}

bool isFileNameCommand(const QString& command) {
    static const QStringList kFileNameCommands = {
        QStringLiteral("delete-image-root-file"),
        QStringLiteral("delete-image-root-directory-file"),
        QStringLiteral("delete-raw-root-directory-file"),
        QStringLiteral("delete-raw-root-file"),
        QStringLiteral("commit-image-file-write"),
        QStringLiteral("commit-image-file-insert"),
        QStringLiteral("commit-image-file-clone"),
        QStringLiteral("commit-image-file-hardlink"),
        QStringLiteral("commit-image-file-delete"),
        QStringLiteral("commit-image-file-rename"),
        QStringLiteral("commit-raw-file-write"),
        QStringLiteral("commit-raw-file-insert"),
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
    return isFileWriteCommand(command) || kFileNameCommands.contains(command);
}

bool isDirectoryNameCommand(const QString& command) {
    static const QStringList kDirectoryNameCommands = {
        QStringLiteral("create-image-root-directory"),
        QStringLiteral("write-image-root-directory-file"),
        QStringLiteral("patch-image-root-directory-file"),
        QStringLiteral("delete-image-root-directory-file"),
        QStringLiteral("delete-image-root-directory"),
        QStringLiteral("create-raw-root-directory"),
        QStringLiteral("write-raw-root-directory-file"),
        QStringLiteral("patch-raw-root-directory-file"),
        QStringLiteral("delete-raw-root-directory-file"),
        QStringLiteral("delete-raw-root-directory"),
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

bool isFilePatchCommand(const QString& command) {
    return command == QStringLiteral("patch-image-root-file") ||
           command == QStringLiteral("patch-image-root-directory-file") ||
           command == QStringLiteral("patch-raw-root-directory-file") ||
           command == QStringLiteral("patch-raw-root-file");
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
// without an added file, or a zero-byte file). Required-payload commands fall through
// to the isFileWriteCommand path below.
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
    if (!isFileWriteCommand(command)) {
        return QByteArray();
    }
    const QString payloadPath = parser.value(option).trimmed();
    if (payloadPath.isEmpty()) {
        *error = QStringLiteral("--payload-file is required for APFS root-file writes.");
        return std::nullopt;
    }
    return readPayloadFile(payloadPath, error);
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
    if (!isFilePatchCommand(command) && !cowPatch) {
        return 0ULL;
    }
    if (!parser.isSet(option)) {
        *error = QStringLiteral("--patch-offset-bytes is required for APFS root-file patch.");
        return std::nullopt;
    }
    return parseNonNegativeUInt64Option(parser, option, error);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sak_apfs_writer_cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.9.1.9"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "S.A.K. APFS generated-container writer. Supports APFS image build/repair/file/directory "
        "and volume-label mutation plus confirmed raw-partition format/repair and certifier raw "
        "file/directory/volume-label mutation."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption targetOption({QStringLiteral("target")},
                                          QStringLiteral("Target raw partition or image path."),
                                          QStringLiteral("path"));
    const QCommandLineOption sizeOption({QStringLiteral("size-bytes")},
                                        QStringLiteral("Target APFS container size in bytes."),
                                        QStringLiteral("bytes"));
    const QCommandLineOption blockSizeOption({QStringLiteral("block-size-bytes")},
                                             QStringLiteral("APFS block size, default 4096."),
                                             QStringLiteral("bytes"));
    const QCommandLineOption volumeNameOption({QStringLiteral("volume-name")},
                                              QStringLiteral("APFS volume name."),
                                              QStringLiteral("name"),
                                              QStringLiteral("SAK APFS"));
    const QCommandLineOption additionalVolumeNameOption(
        {QStringLiteral("additional-volume-name")},
        QStringLiteral("Name of an additional APFS volume (repeatable; multi-volume format)."),
        QStringLiteral("name"));
    const QCommandLineOption outputImageOption({QStringLiteral("output-image")},
                                               QStringLiteral("Image repair output path."),
                                               QStringLiteral("path"));
    const QCommandLineOption fileNameOption(
        {QStringLiteral("file-name")},
        QStringLiteral("Root or child file name for generated APFS writes."),
        QStringLiteral("name"));
    const QCommandLineOption directoryNameOption(
        {QStringLiteral("directory-name")},
        QStringLiteral("Root directory name for generated APFS directory or child-file mutations."),
        QStringLiteral("name"));
    const QCommandLineOption newFileNameOption(
        {QStringLiteral("new-file-name")},
        QStringLiteral("Destination file name for a generated APFS file move."),
        QStringLiteral("name"));
    const QCommandLineOption destinationDirectoryNameOption(
        {QStringLiteral("destination-directory-name")},
        QStringLiteral("Destination directory (empty = root) for a generated APFS file move."),
        QStringLiteral("name"));
    const QCommandLineOption payloadOption(
        {QStringLiteral("payload-file")},
        QStringLiteral("Payload file for generated APFS writes or patches."),
        QStringLiteral("path"));
    const QCommandLineOption patchOffsetOption(
        {QStringLiteral("patch-offset-bytes")},
        QStringLiteral("Byte offset for generated APFS partial root or child-file patch."),
        QStringLiteral("bytes"));
    const QCommandLineOption snapshotNameOption(
        {QStringLiteral("snapshot-name")},
        QStringLiteral("Snapshot name for a generated APFS snapshot create."),
        QStringLiteral("name"));
    const QCommandLineOption outputJsonOption({QStringLiteral("output-json")},
                                              QStringLiteral("Optional report JSON path."),
                                              QStringLiteral("path"));
    const QCommandLineOption evidenceOption({QStringLiteral("evidence-id")},
                                            QStringLiteral("Certification/evidence ID."),
                                            QStringLiteral("id"));
    const QCommandLineOption confirmOption({QStringLiteral("confirm-target")},
                                           QStringLiteral("Confirm destructive target mutation."));
    const QCommandLineOption allowRawOption({QStringLiteral("allow-raw-target")},
                                            QStringLiteral("Permit Windows raw-device mutation."));
    const QCommandLineOption compressZlibOption(
        {QStringLiteral("compress-zlib")},
        QStringLiteral("Store the inserted file transparently compressed (inline zlib "
                       "com.apple.decmpfs); for commit-image/raw-file-insert."));
    const QCommandLineOption volumePasswordOption(
        {QStringLiteral("volume-password")},
        QStringLiteral("Format a software-encrypted (FileVault) volume unlockable by this "
                       "password; for format-image. Credential-in, never stored."),
        QStringLiteral("password"));
    const QCommandLineOption recoveryKeyOption(
        {QStringLiteral("recovery-key")},
        QStringLiteral("Add a personal-recovery-key unlock record (used with --volume-password); "
                       "the volume then unlocks by either the password or this recovery key; for "
                       "format-image. Credential-in, never stored."),
        QStringLiteral("recovery-key"));
    const QCommandLineOption sparseSizeOption(
        {QStringLiteral("sparse-size")},
        QStringLiteral("Insert the file sparse with a trailing hole: its extents cover --payload "
                       "and the inode's logical size is this many bytes (the gap reads as zeros); "
                       "for commit-image-file-insert."),
        QStringLiteral("bytes"));
    const QCommandLineOption xattrOption(
        {QStringLiteral("xattr")},
        QStringLiteral("Attach a named extended attribute name=hexvalue (repeatable); ACL names "
                       "com.apple.system.Security / com.apple.FinderInfo set the matching inode "
                       "flag; for commit-image-file-insert."),
        QStringLiteral("name=hex"));
    parser.addOptions({targetOption,
                       sizeOption,
                       blockSizeOption,
                       volumeNameOption,
                       additionalVolumeNameOption,
                       outputImageOption,
                       fileNameOption,
                       directoryNameOption,
                       newFileNameOption,
                       destinationDirectoryNameOption,
                       payloadOption,
                       patchOffsetOption,
                       snapshotNameOption,
                       outputJsonOption,
                       evidenceOption,
                       confirmOption,
                       allowRawOption,
                       compressZlibOption,
                       volumePasswordOption,
                       recoveryKeyOption,
                       sparseSizeOption,
                       xattrOption});
    parser.addPositionalArgument(
        QStringLiteral("command"),
        QStringLiteral(
            "format-image, repair-image, write-image-root-file, write-image-root-directory-file, "
            "patch-image-root-file, patch-image-root-directory-file, delete-image-root-file, "
            "delete-image-root-directory-file, create-image-root-directory, "
            "delete-image-root-directory, change-image-volume-label, format-raw, "
            "write-raw-root-file, write-raw-root-directory-file, patch-raw-root-file, "
            "patch-raw-root-directory-file, delete-raw-root-file, delete-raw-root-directory-file, "
            "create-raw-root-directory, delete-raw-root-directory, change-raw-volume-label, "
            "repair-raw, commit-raw-file-insert, commit-raw-file-delete, or "
            "commit-raw-file-rename."));
    parser.process(app);

    QString parseError;
    const auto invocation =
        invocationFromParser(parser,
                             {.target = &targetOption,
                              .size = &sizeOption,
                              .block_size = &blockSizeOption,
                              .volume_name = &volumeNameOption,
                              .additional_volume_name = &additionalVolumeNameOption,
                              .output_image = &outputImageOption,
                              .file_name = &fileNameOption,
                              .directory_name = &directoryNameOption,
                              .new_file_name = &newFileNameOption,
                              .destination_directory_name = &destinationDirectoryNameOption,
                              .payload = &payloadOption,
                              .patch_offset = &patchOffsetOption,
                              .snapshot_name = &snapshotNameOption,
                              .evidence = &evidenceOption,
                              .confirm = &confirmOption,
                              .allow_raw = &allowRawOption,
                              .compress_zlib = &compressZlibOption,
                              .volume_password = &volumePasswordOption,
                              .recovery_key = &recoveryKeyOption,
                              .sparse_size = &sparseSizeOption,
                              .xattr = &xattrOption},
                             &parseError);
    if (!invocation.has_value()) {
        QTextStream(stderr) << parseError << Qt::endl;
        return kExitInvalidArguments;
    }

    QString commandError;
    const auto report = buildCommandReport(*invocation, &commandError);
    if (!report.has_value()) {
        QTextStream(stderr) << commandError << Qt::endl;
        return kExitInvalidArguments;
    }

    QString reportError;
    if (!writeReport(*report, parser.value(outputJsonOption).trimmed(), &reportError)) {
        QTextStream(stderr) << "Failed to write report: " << reportError << Qt::endl;
        return kExitReportFailed;
    }
    return report->value(QStringLiteral("ok")).toBool(false) ? kExitOk : kExitOperationFailed;
}
