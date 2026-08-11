// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file sak_hfs_writer_cli.cpp
/// @brief Command-line bridge for certified image-only HFS+ writer operations.

#include "sak/partition_hfs_file_system_reader.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitOperationFailed = 1;
constexpr int kExitInvalidArguments = 2;
constexpr int kExitReportFailed = 3;

// A payload file is slurped whole into RAM by readAll, so cap it to bound peak memory; a
// hostile or mistaken multi-GB path would otherwise OOM the process. 2 GiB is far above any
// real single-file HFS+ seed.
constexpr qint64 kMaxPayloadFileBytes = 2LL * 1024 * 1024 * 1024;
// HFS+ file names cap at 255 UTF-16 units; a --name-pad beyond that just wastes memory (a huge
// value would allocate a multi-GB QString), so reject anything past the name-length limit.
constexpr int kMaxNamePad = 255;
// create-empty-files-image emits one image mutation per file; an unbounded --file-count would
// run for an arbitrarily long time. Cap it so a malformed/huge value fails closed.
constexpr int kMaxGeneratorFileCount = 100'000;

// A drive-letter whole-volume path is exactly "\\.\X:" or "\\?\X:": six characters, with the drive
// letter at index 4 and the colon at index 5.
constexpr qsizetype kDriveLetterVolumePathLength = 6;
constexpr qsizetype kDriveLetterVolumeLetterIndex = 4;
constexpr qsizetype kDriveLetterVolumeColonIndex = 5;

QJsonArray stringArray(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
}

std::optional<QByteArray> readPayloadFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to read payload file: %1").arg(file.errorString());
        return std::nullopt;
    }
    const qint64 expected = file.size();
    if (expected > kMaxPayloadFileBytes) {
        *error = QStringLiteral("Payload file exceeds the %1-byte limit").arg(kMaxPayloadFileBytes);
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

bool writeReport(const QJsonObject& report, const QString& outputPath, QString* error) {
    const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    QTextStream(stdout) << bytes << Qt::endl;
    if (outputPath.trimmed().isEmpty()) {
        return true;
    }

    const QFileInfo outputInfo(outputPath);
    if (!outputInfo.absoluteDir().exists() && !QDir().mkpath(outputInfo.absolutePath())) {
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
// symlink/hardlink aliases of one file share this token. Returns nullopt when
// the path does not exist / cannot be opened (a not-yet-created report file or
// a raw device that requires elevation), so the caller falls back to the
// string/canonical comparison rather than treating "unknown" as "distinct".
std::optional<QString> fileIdentityToken(const QString& path) {
#ifdef Q_OS_WIN
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
// target image, clobbering the filesystem result with JSON. Detects three kinds
// of aliasing: identical strings (raw device paths), symlink aliases (via
// canonicalFilePath), and hardlink aliases (via OS volume+file-id identity).
bool outputJsonAliasesTarget(const QString& outputJson, const QString& targetImagePath) {
    const QString out = outputJson.trimmed();
    const QString target = targetImagePath.trimmed();
    if (out.isEmpty() || target.isEmpty()) {
        return false;
    }
    if (out.compare(target, Qt::CaseInsensitive) == 0) {
        return true;
    }
    const auto canon = [](const QString& p) {
        const QString resolved = QFileInfo(p).canonicalFilePath();
        return resolved.isEmpty() ? QDir::cleanPath(QFileInfo(p).absoluteFilePath()) : resolved;
    };
    if (canon(out).compare(canon(target), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const std::optional<QString> outId = fileIdentityToken(out);
    const std::optional<QString> targetId = fileIdentityToken(target);
    return outId.has_value() && outId == targetId;
}

QString evidenceIdForCommand(const QCommandLineParser& parser,
                             const QCommandLineOption& evidenceOption,
                             const QString& command) {
    const QString value = parser.value(evidenceOption).trimmed();
    return value.isEmpty() ? QStringLiteral("sak-ui.hfs.%1").arg(command) : value;
}

struct CliInvocation {
    QString command;
    QString target_image_path;
    QString hfs_path;
    QString destination_hfs_path;
    uint32_t file_id{0};
    QString attribute_name;
    QByteArray payload;
    QString evidence_id;
    int file_count{0};
    int name_pad{0};
    bool confirm_target{false};
    bool allow_journaled_volume{false};
    bool allow_wrapped_volume{false};
    bool allow_compressed_file_mutation{false};
    bool secure_wipe_released_blocks{false};
    bool allow_raw_target{false};
};

struct AttributeArguments {
    uint32_t file_id{0};
    QString attribute_name;
};

struct RequiredPathArguments {
    QString target_path;
    QString hfs_path;
    QString destination_hfs_path;
    QString payload_path;
    bool attribute_command{false};
    bool rename_move_command{false};
    bool truncate_command{false};
    bool journal_command{false};
};

struct CliOptions {
    QCommandLineOption target;
    QCommandLineOption hfs_path;
    QCommandLineOption destination_hfs_path;
    QCommandLineOption file_id;
    QCommandLineOption attribute_name;
    QCommandLineOption payload;
    QCommandLineOption output_json;
    QCommandLineOption evidence;
    QCommandLineOption file_count;
    QCommandLineOption name_pad;
    QCommandLineOption allow_raw;
};

using FileCommandRunner = std::function<sak::PartitionHfsFileWriteResult(const CliInvocation&)>;

sak::PartitionHfsFileWriteOptions writeOptions(const CliInvocation& invocation) {
    sak::PartitionHfsFileWriteOptions options;
    // By design for this tool tier: sak_hfs_writer_cli is the manual, elevated certifier that
    // synthesizes its own evidence id and enables the writer to drive certified mutations
    // (see review finding 4).
    options.enable_writer = true;
    options.target_write_confirmed = invocation.confirm_target;
    options.image_only = !invocation.allow_raw_target;
    options.allow_journaled_volume = invocation.allow_journaled_volume;
    options.allow_wrapped_volume = invocation.allow_wrapped_volume;
    options.allow_compressed_file_mutation = invocation.allow_compressed_file_mutation;
    options.secure_wipe_deleted_blocks = invocation.secure_wipe_released_blocks;
    options.evidence_id = invocation.evidence_id;
    return options;
}

using FileCommandRunnerTable = QHash<QString, FileCommandRunner>;

// Folder + simple-file create/delete commands taking only --hfs-path.
void appendFolderAndSimpleFileRunners(FileCommandRunnerTable& runners) {
    runners.insert(
        QStringLiteral("delete-empty-folder-image"), [](const CliInvocation& invocation) {
            return sak::PartitionHfsFileSystemWriter::deleteEmptyFolderFromImage(
                invocation.target_image_path, invocation.hfs_path, writeOptions(invocation));
        });
    runners.insert(QStringLiteral("delete-folder-tree-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::
            deleteFolderTreeAndReleaseAllocatedBlocksFromImage(invocation.target_image_path,
                                                               invocation.hfs_path,
                                                               writeOptions(invocation));
    });
    runners.insert(QStringLiteral("rename-catalog-entry-image"),
                   [](const CliInvocation& invocation) {
                       return sak::PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
                           invocation.target_image_path,
                           invocation.hfs_path,
                           invocation.destination_hfs_path,
                           writeOptions(invocation));
                   });
    runners.insert(QStringLiteral("delete-file-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
            invocation.target_image_path, invocation.hfs_path, writeOptions(invocation));
    });
    runners.insert(
        QStringLiteral("create-empty-folder-image"), [](const CliInvocation& invocation) {
            return sak::PartitionHfsFileSystemWriter::createEmptyFolderFromImage(
                invocation.target_image_path, invocation.hfs_path, writeOptions(invocation));
        });
    runners.insert(QStringLiteral("delete-empty-file-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
            invocation.target_image_path, invocation.hfs_path, writeOptions(invocation));
    });
    runners.insert(QStringLiteral("create-empty-file-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::createEmptyFileFromImage(
            invocation.target_image_path, invocation.hfs_path, writeOptions(invocation));
    });
}

// Symlink/hardlink + journal-replay + batch create-empty-files commands.
void appendLinkJournalAndBatchRunners(FileCommandRunnerTable& runners) {
    runners.insert(QStringLiteral("create-symlink-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::createSymlinkFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.destination_hfs_path,
            writeOptions(invocation));
    });
    runners.insert(QStringLiteral("create-hardlink-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::createHardlinkFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.destination_hfs_path,
            writeOptions(invocation));
    });
    runners.insert(QStringLiteral("delete-hardlink-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::deleteHardlinkFromImage(
            invocation.target_image_path, invocation.hfs_path, writeOptions(invocation));
    });
    runners.insert(QStringLiteral("replay-journal-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::replayJournalFromImage(
            invocation.target_image_path, writeOptions(invocation));
    });
    runners.insert(QStringLiteral("create-empty-files-image"), [](const CliInvocation& invocation) {
        sak::PartitionHfsFileWriteResult result;
        const QString pad(std::clamp(invocation.name_pad, 0, kMaxNamePad), QLatin1Char('x'));
        constexpr int kIndexFieldWidth = 4;
        constexpr int kDecimalBase = 10;
        for (int index = 0; index < invocation.file_count; ++index) {
            const QString path = QStringLiteral("%1-%2-%3.txt")
                                     .arg(invocation.hfs_path)
                                     .arg(index, kIndexFieldWidth, kDecimalBase, QLatin1Char('0'))
                                     .arg(pad);
            result = sak::PartitionHfsFileSystemWriter::createEmptyFileFromImage(
                invocation.target_image_path, path, writeOptions(invocation));
            if (!result.ok) {
                return result;
            }
        }
        if (invocation.file_count <= 0) {
            result.blockers.append(
                QStringLiteral("create-empty-files-image requires --file-count > 0"));
            result.ok = false;
        }
        return result;
    });
}

// Truncate + resource-fork mutation commands.
void appendTruncateAndResourceForkRunners(FileCommandRunnerTable& runners) {
    runners.insert(QStringLiteral("create-file-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::createFileWithDataFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.payload,
            writeOptions(invocation));
    });
    runners.insert(
        QStringLiteral("truncate-resource-fork-image"), [](const CliInvocation& invocation) {
            return sak::PartitionHfsFileSystemWriter::
                truncateResourceForkWithinAllocatedBlocksFromImage(invocation.target_image_path,
                                                                   invocation.hfs_path,
                                                                   writeOptions(invocation));
        });
    runners.insert(QStringLiteral("truncate-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::truncateFileWithinAllocatedBlocksFromImage(
            invocation.target_image_path, invocation.hfs_path, writeOptions(invocation));
    });
    runners.insert(
        QStringLiteral("replace-resource-fork-image"), [](const CliInvocation& invocation) {
            return sak::PartitionHfsFileSystemWriter::
                replaceResourceForkWithinAllocatedBlocksFromImage(invocation.target_image_path,
                                                                  invocation.hfs_path,
                                                                  invocation.payload,
                                                                  writeOptions(invocation));
        });
    runners.insert(QStringLiteral("grow-resource-fork-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::replaceResourceForkWithAllocationGrowthFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.payload,
            writeOptions(invocation));
    });
}

// Data-fork replace/grow/overwrite commands.
void appendDataForkReplaceRunners(FileCommandRunnerTable& runners) {
    runners.insert(QStringLiteral("grow-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.payload,
            writeOptions(invocation));
    });
    runners.insert(QStringLiteral("replace-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::replaceFileWithinAllocatedBlocksFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.payload,
            writeOptions(invocation));
    });
    runners.insert(QStringLiteral("replace-compressed-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::replaceCompressedFileContentFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.payload,
            writeOptions(invocation));
    });
    runners.insert(QStringLiteral("overwrite-image"), [](const CliInvocation& invocation) {
        return sak::PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
            invocation.target_image_path,
            invocation.hfs_path,
            invocation.payload,
            writeOptions(invocation));
    });
}

FileCommandRunnerTable buildFileCommandRunners() {
    FileCommandRunnerTable runners;
    appendFolderAndSimpleFileRunners(runners);
    appendLinkJournalAndBatchRunners(runners);
    appendTruncateAndResourceForkRunners(runners);
    appendDataForkReplaceRunners(runners);
    return runners;
}

const QHash<QString, FileCommandRunner>& fileCommandRunners() {
    static const QHash<QString, FileCommandRunner> kRunners = buildFileCommandRunners();
    return kRunners;
}

sak::PartitionHfsFileWriteResult runFileWriteCommand(const CliInvocation& invocation) {
    // QHash::value returns a default-constructed (empty) std::function for an unknown key, and
    // invoking that throws std::bad_function_call. Fail closed on parser/registry drift rather
    // than crash: surface a blocker for any command with no registered runner.
    const FileCommandRunner runner = fileCommandRunners().value(invocation.command);
    if (!runner) {
        sak::PartitionHfsFileWriteResult result;
        result.ok = false;
        result.blockers.append(
            QStringLiteral("No registered writer for command: %1").arg(invocation.command));
        return result;
    }
    return runner(invocation);
}

QJsonObject fileWriteReport(const CliInvocation& invocation) {
    const sak::PartitionHfsFileWriteResult result = runFileWriteCommand(invocation);

    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_hfs_writer_cli"));
    report.insert(QStringLiteral("command"), invocation.command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("image_path"), invocation.target_image_path);
    report.insert(QStringLiteral("hfs_path"), result.path);
    if (!invocation.destination_hfs_path.isEmpty()) {
        report.insert(QStringLiteral("destination_hfs_path"), invocation.destination_hfs_path);
    }
    report.insert(QStringLiteral("file_system"), result.file_system);
    report.insert(QStringLiteral("catalog_id"), QString::number(result.catalog_id));
    report.insert(QStringLiteral("bytes_written"), QString::number(result.bytes_written));
    report.insert(QStringLiteral("chunks_written"), result.chunks_written);
    report.insert(QStringLiteral("before_sha256"), result.before_sha256);
    report.insert(QStringLiteral("after_sha256"), result.after_sha256);
    report.insert(QStringLiteral("evidence_id"), result.evidence_id);
    report.insert(QStringLiteral("blockers"), stringArray(result.blockers));
    report.insert(QStringLiteral("warnings"), stringArray(result.warnings));
    return report;
}

QJsonObject attributeWriteReport(const CliInvocation& invocation) {
    sak::PartitionHfsAttributeWriteResult result;
    if (invocation.command == QStringLiteral("grow-fork-attribute-image")) {
        result = sak::PartitionHfsFileSystemWriter::
            replaceForkAttributeValueWithAllocationGrowthFromImage(invocation.target_image_path,
                                                                   invocation.file_id,
                                                                   invocation.attribute_name,
                                                                   invocation.payload,
                                                                   writeOptions(invocation));
    } else if (invocation.command == QStringLiteral("replace-fork-attribute-image")) {
        result = sak::PartitionHfsFileSystemWriter::
            replaceForkAttributeValueWithinAllocatedBlocksFromImage(invocation.target_image_path,
                                                                    invocation.file_id,
                                                                    invocation.attribute_name,
                                                                    invocation.payload,
                                                                    writeOptions(invocation));
    } else if (invocation.command == QStringLiteral("create-inline-attribute-image")) {
        result = sak::PartitionHfsFileSystemWriter::createInlineAttributeValueFromImage(
            invocation.target_image_path,
            invocation.file_id,
            invocation.attribute_name,
            invocation.payload,
            writeOptions(invocation));
    } else if (invocation.command == QStringLiteral("create-fork-attribute-image")) {
        result = sak::PartitionHfsFileSystemWriter::createForkAttributeValueFromImage(
            invocation.target_image_path,
            invocation.file_id,
            invocation.attribute_name,
            invocation.payload,
            writeOptions(invocation));
    } else if (invocation.command == QStringLiteral("delete-attribute-image")) {
        result = sak::PartitionHfsFileSystemWriter::deleteAttributeValueFromImage(
            invocation.target_image_path,
            invocation.file_id,
            invocation.attribute_name,
            writeOptions(invocation));
    } else {
        result = sak::PartitionHfsFileSystemWriter::replaceInlineAttributeValueFromImage(
            invocation.target_image_path,
            invocation.file_id,
            invocation.attribute_name,
            invocation.payload,
            writeOptions(invocation));
    }

    QJsonObject report;
    report.insert(QStringLiteral("tool"), QStringLiteral("sak_hfs_writer_cli"));
    report.insert(QStringLiteral("command"), invocation.command);
    report.insert(QStringLiteral("ok"), result.ok);
    report.insert(QStringLiteral("image_path"), invocation.target_image_path);
    report.insert(QStringLiteral("file_id"), QString::number(result.file_id));
    report.insert(QStringLiteral("attribute_name"), result.attribute_name);
    report.insert(QStringLiteral("file_system"), result.file_system);
    report.insert(QStringLiteral("bytes_written"), QString::number(result.bytes_written));
    report.insert(QStringLiteral("chunks_written"), result.chunks_written);
    report.insert(QStringLiteral("before_sha256"), result.before_sha256);
    report.insert(QStringLiteral("after_sha256"), result.after_sha256);
    report.insert(QStringLiteral("evidence_id"), result.evidence_id);
    report.insert(QStringLiteral("blockers"), stringArray(result.blockers));
    report.insert(QStringLiteral("warnings"), stringArray(result.warnings));
    return report;
}

bool isAttributeCommand(const QString& command) {
    return command == QStringLiteral("replace-inline-attribute-image") ||
           command == QStringLiteral("create-inline-attribute-image") ||
           command == QStringLiteral("create-fork-attribute-image") ||
           command == QStringLiteral("delete-attribute-image") ||
           command == QStringLiteral("replace-fork-attribute-image") ||
           command == QStringLiteral("grow-fork-attribute-image");
}

bool isTruncateCommand(const QString& command) {
    return command == QStringLiteral("truncate-image") ||
           command == QStringLiteral("truncate-resource-fork-image");
}

bool isRenameMoveCommand(const QString& command) {
    return command == QStringLiteral("rename-catalog-entry-image");
}

// H5: symlink/hardlink creates take --hfs-path plus --destination-hfs-path (the
// target string / new link) and no payload file, like a rename/move.
bool isLinkCommand(const QString& command) {
    return command == QStringLiteral("create-symlink-image") ||
           command == QStringLiteral("create-hardlink-image");
}

bool isJournalCommand(const QString& command) {
    return command == QStringLiteral("replay-journal-image");
}

bool isNoPayloadFileCommand(const QString& command) {
    static const QSet<QString> kNoPayloadCommands{
        QStringLiteral("delete-attribute-image"),
        QStringLiteral("create-empty-file-image"),
        QStringLiteral("create-empty-files-image"),
        QStringLiteral("delete-empty-file-image"),
        QStringLiteral("delete-file-image"),
        QStringLiteral("create-empty-folder-image"),
        QStringLiteral("delete-empty-folder-image"),
        QStringLiteral("delete-folder-tree-image"),
        QStringLiteral("delete-hardlink-image"),
    };
    return isTruncateCommand(command) || kNoPayloadCommands.contains(command) ||
           isJournalCommand(command) || isRenameMoveCommand(command) || isLinkCommand(command);
}

bool isSupportedCommand(const QString& command) {
    return command == QStringLiteral("overwrite-image") ||
           command == QStringLiteral("create-file-image") ||
           command == QStringLiteral("replace-image") ||
           command == QStringLiteral("replace-resource-fork-image") ||
           command == QStringLiteral("replace-compressed-image") ||
           command == QStringLiteral("grow-image") ||
           command == QStringLiteral("grow-resource-fork-image") ||
           isNoPayloadFileCommand(command) || isAttributeCommand(command);
}

std::optional<QString> parseCommand(const QCommandLineParser& parser, QString* error) {
    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 1) {
        *error = QStringLiteral("Exactly one command is required.");
        return std::nullopt;
    }
    const QString command = positional.first().trimmed().toLower();
    if (!isSupportedCommand(command)) {
        *error = QStringLiteral("Unsupported command: %1").arg(command);
        return std::nullopt;
    }
    return command;
}

QString missingPathArgumentsError(const RequiredPathArguments& arguments) {
    if (arguments.rename_move_command) {
        return QStringLiteral("--target, --hfs-path, and --destination-hfs-path are required.");
    }
    return arguments.truncate_command
               ? QStringLiteral("--target and --hfs-path are required.")
               : QStringLiteral("--target, --hfs-path, and --payload-file are required.");
}

bool validatePathArguments(const RequiredPathArguments& arguments, QString* error) {
    if (arguments.journal_command) {
        if (arguments.target_path.isEmpty()) {
            *error = QStringLiteral("--target is required.");
            return false;
        }
        return true;
    }
    const bool missing = arguments.target_path.isEmpty() ||
                         (!arguments.attribute_command && arguments.hfs_path.isEmpty()) ||
                         (arguments.rename_move_command &&
                          arguments.destination_hfs_path.isEmpty()) ||
                         (!arguments.truncate_command && arguments.payload_path.isEmpty());
    if (missing) {
        *error = missingPathArgumentsError(arguments);
        return false;
    }
    return true;
}

std::optional<AttributeArguments> parseAttributeArguments(const QCommandLineParser& parser,
                                                          const CliOptions& options,
                                                          bool attributeCommand,
                                                          QString* error) {
    AttributeArguments arguments;
    if (!attributeCommand) {
        return arguments;
    }

    bool fileIdOk = false;
    arguments.file_id = parser.value(options.file_id).toUInt(&fileIdOk);
    arguments.attribute_name = parser.value(options.attribute_name).trimmed();
    if (!fileIdOk || arguments.file_id == 0 || arguments.attribute_name.isEmpty()) {
        *error =
            QStringLiteral("--file-id and --attribute-name are required for attribute commands.");
        return std::nullopt;
    }
    return arguments;
}

QJsonObject invocationReport(const CliInvocation& invocation) {
    if (isAttributeCommand(invocation.command)) {
        return attributeWriteReport(invocation);
    }
    return fileWriteReport(invocation);
}

struct GeneratorCounts {
    int file_count{0};
    int name_pad{0};
};

// Validate the numeric generator options (used only by create-empty-files-image). A malformed
// --name-pad previously coerced to 0 and a huge --file-count was unbounded; both now fail closed.
std::optional<GeneratorCounts> parseGeneratorCounts(const QCommandLineParser& parser,
                                                    const CliOptions& options,
                                                    QString* error) {
    GeneratorCounts counts;
    if (parser.isSet(options.file_count)) {
        bool ok = false;
        counts.file_count = parser.value(options.file_count).toInt(&ok);
        if (!ok || counts.file_count < 0 || counts.file_count > kMaxGeneratorFileCount) {
            *error = QStringLiteral("--file-count must be an integer in [0, %1].")
                         .arg(kMaxGeneratorFileCount);
            return std::nullopt;
        }
    }
    if (parser.isSet(options.name_pad)) {
        bool ok = false;
        counts.name_pad = parser.value(options.name_pad).toInt(&ok);
        if (!ok || counts.name_pad < 0 || counts.name_pad > kMaxNamePad) {
            *error = QStringLiteral("--name-pad must be an integer in [0, %1].").arg(kMaxNamePad);
            return std::nullopt;
        }
    }
    return counts;
}

std::optional<CliInvocation> parseInvocation(const QCommandLineParser& parser,
                                             const CliOptions& options,
                                             QString* error) {
    const auto command = parseCommand(parser, error);
    if (!command.has_value()) {
        return std::nullopt;
    }

    const QString targetPath = parser.value(options.target).trimmed();
    const QString hfsPath = parser.value(options.hfs_path).trimmed();
    const QString destinationHfsPath = parser.value(options.destination_hfs_path).trimmed();
    const QString payloadPath = parser.value(options.payload).trimmed();
    const bool attributeCommand = isAttributeCommand(*command);
    const bool noPayloadFileCommand = isNoPayloadFileCommand(*command);
    const bool renameMoveCommand = isRenameMoveCommand(*command) || isLinkCommand(*command);
    if (!validatePathArguments({.target_path = targetPath,
                                .hfs_path = hfsPath,
                                .destination_hfs_path = destinationHfsPath,
                                .payload_path = payloadPath,
                                .attribute_command = attributeCommand,
                                .rename_move_command = renameMoveCommand,
                                .truncate_command = noPayloadFileCommand,
                                .journal_command = isJournalCommand(*command)},
                               error)) {
        return std::nullopt;
    }
    const auto attributeArguments =
        parseAttributeArguments(parser, options, attributeCommand, error);
    if (!attributeArguments.has_value()) {
        return std::nullopt;
    }
    const auto counts = parseGeneratorCounts(parser, options, error);
    if (!counts.has_value()) {
        return std::nullopt;
    }

    QByteArray payload;
    if (!noPayloadFileCommand) {
        const auto payloadBytes = readPayloadFile(payloadPath, error);
        if (!payloadBytes.has_value()) {
            return std::nullopt;
        }
        payload = *payloadBytes;
    }
    return CliInvocation{
        .command = *command,
        .target_image_path = targetPath,
        .hfs_path = hfsPath,
        .destination_hfs_path = destinationHfsPath,
        .file_id = attributeArguments->file_id,
        .attribute_name = attributeArguments->attribute_name,
        .payload = payload,
        .evidence_id = evidenceIdForCommand(parser, options.evidence, *command),
        .file_count = counts->file_count,
        .name_pad = counts->name_pad,
        .confirm_target = parser.isSet(QStringLiteral("confirm-target")),
        .allow_journaled_volume = parser.isSet(QStringLiteral("allow-journaled-volume")),
        .allow_wrapped_volume = parser.isSet(QStringLiteral("allow-wrapped-volume")),
        .allow_compressed_file_mutation =
            parser.isSet(QStringLiteral("allow-compressed-file-mutation")),
        .secure_wipe_released_blocks = parser.isSet(QStringLiteral("secure-wipe-released-blocks")),
        .allow_raw_target = parser.isSet(options.allow_raw)};
}

CliOptions buildCliOptions() {
    return CliOptions{
        .target = QCommandLineOption({QStringLiteral("target")},
                                     QStringLiteral("Target HFS+/HFSX image path."),
                                     QStringLiteral("path")),
        .hfs_path = QCommandLineOption({QStringLiteral("hfs-path")},
                                       QStringLiteral("HFS path for file mutation."),
                                       QStringLiteral("path")),
        .destination_hfs_path =
            QCommandLineOption({QStringLiteral("destination-hfs-path")},
                               QStringLiteral("Destination HFS path for catalog rename/move."),
                               QStringLiteral("path")),
        .file_id = QCommandLineOption({QStringLiteral("file-id")},
                                      QStringLiteral("HFS catalog file ID for attribute writes."),
                                      QStringLiteral("id")),
        .attribute_name = QCommandLineOption({QStringLiteral("attribute-name")},
                                             QStringLiteral("HFS attribute name to replace."),
                                             QStringLiteral("name")),
        .payload = QCommandLineOption({QStringLiteral("payload-file")},
                                      QStringLiteral("Replacement payload file."),
                                      QStringLiteral("path")),
        .output_json = QCommandLineOption({QStringLiteral("output-json")},
                                          QStringLiteral("Optional report JSON path."),
                                          QStringLiteral("path")),
        .evidence = QCommandLineOption({QStringLiteral("evidence-id")},
                                       QStringLiteral("Certification/evidence ID."),
                                       QStringLiteral("id")),
        .file_count =
            QCommandLineOption({QStringLiteral("file-count")},
                               QStringLiteral("Number of files for create-empty-files-image."),
                               QStringLiteral("count")),
        .name_pad = QCommandLineOption({QStringLiteral("name-pad")},
                                       QStringLiteral(
                                           "Filename padding length for create-empty-files-image."),
                                       QStringLiteral("length")),
        .allow_raw = QCommandLineOption({QStringLiteral("allow-raw-target")},
                                        QStringLiteral("Permit Windows raw-device mutation."))};
}

void registerCliOptions(QCommandLineParser& parser, const CliOptions& options) {
    parser.addOptions(
        {options.target,
         options.hfs_path,
         options.destination_hfs_path,
         options.file_id,
         options.attribute_name,
         options.payload,
         options.output_json,
         options.evidence,
         options.file_count,
         options.name_pad,
         options.allow_raw,
         QCommandLineOption({QStringLiteral("confirm-target")},
                            QStringLiteral("Confirm target image mutation.")),
         QCommandLineOption({QStringLiteral("allow-journaled-volume")},
                            QStringLiteral("Permit journaled HFS+ image overwrite.")),
         QCommandLineOption({QStringLiteral("allow-wrapped-volume")},
                            QStringLiteral("Permit classic-wrapped HFS+ image overwrite.")),
         QCommandLineOption({QStringLiteral("allow-compressed-file-mutation")},
                            QStringLiteral("Permit compressed-file mutation.")),
         QCommandLineOption({QStringLiteral("secure-wipe-released-blocks")},
                            QStringLiteral(
                                "Zero released file blocks before HFS delete operations."))});
}

void registerPositionalCommand(QCommandLineParser& parser) {
    parser.addPositionalArgument(
        QStringLiteral("command"),
        QStringLiteral(
            "overwrite-image, replace-image, replace-resource-fork-image, grow-image, "
            "grow-resource-fork-image, truncate-image, truncate-resource-fork-image, "
            "create-file-image, create-empty-file-image, delete-empty-file-image, "
            "delete-file-image, "
            "create-empty-folder-image, delete-empty-folder-image, delete-folder-tree-image, "
            "rename-catalog-entry-image, "
            "replace-inline-attribute-image, replace-fork-attribute-image, or "
            "grow-fork-attribute-image."));
}

#ifdef Q_OS_WIN
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
    if ((ok == 0) || extents->NumberOfDiskExtents == 0) {
        return -1;
    }
    return static_cast<int>(extents->Extents[0].DiskNumber);
}

// Physical drive numbers backing a volume/device alias (\\.\C:, \\?\Volume{GUID},
// \\.\GLOBALROOT\Device\HarddiskVolumeN). Empty when it cannot be opened/queried.
std::vector<int> volumeBackingPhysicalDrives(const QString& path) {
    std::vector<int> drives;
    const std::wstring wide = path.toStdWString();
    HANDLE handle = CreateFileW(
        wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return drives;
    }
    constexpr DWORD kMaxExtents = 32;
    const DWORD bufSize = sizeof(VOLUME_DISK_EXTENTS) + ((kMaxExtents - 1) * sizeof(DISK_EXTENT));
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
    if (ok == 0) {
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
    if (path.size() != kDriveLetterVolumePathLength ||
        path[kDriveLetterVolumeColonIndex] != QLatin1Char(':') ||
        !path[kDriveLetterVolumeLetterIndex].isLetter()) {
        return false;
    }
    return path.startsWith(QStringLiteral("\\\\.\\")) || path.startsWith(QStringLiteral("\\\\?\\"));
}

// Raw volume/device aliases that resolve to a whole disk but are NOT the
// \\.\PhysicalDriveN form the numeric guard covers: drive-letter volumes,
// \Volume{GUID} handles, and GLOBALROOT device paths.
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
            "Refusing raw HFS write: could not parse the PhysicalDrive number of the target");
    }
    if (drive == 0) {
        return QStringLiteral("Refusing raw HFS write to PhysicalDrive0 (the first physical disk)");
    }
#ifdef Q_OS_WIN
    const int osDrive = osSystemPhysicalDrive();
    if (osDrive < 0) {
        return QStringLiteral(
            "Refusing raw HFS write: could not establish the OS system-disk identity");
    }
    if (osDrive == drive) {
        return QStringLiteral(
            "Refusing raw HFS write: target PhysicalDrive backs the OS system volume");
    }
#endif
    return QString();
}

// Refuse a volume/device alias (\\.\C:, \Volume{GUID}, GLOBALROOT) whose backing
// disk is PhysicalDrive0 or the OS system disk. Fails closed when the OS identity
// or the alias's backing drive cannot be resolved.
QString volumeAliasTargetRefusal(const QString& path) {
#ifdef Q_OS_WIN
    const int osDrive = osSystemPhysicalDrive();
    if (osDrive < 0) {
        return QStringLiteral(
            "Refusing raw HFS write: could not establish the OS system-disk identity");
    }
    const std::vector<int> drives = volumeBackingPhysicalDrives(path);
    if (drives.empty()) {
        return QStringLiteral(
            "Refusing raw HFS write: could not resolve the backing physical drive of the "
            "volume/device target");
    }
    for (const int drive : drives) {
        if (drive == 0 || drive == osDrive) {
            return QStringLiteral(
                "Refusing raw HFS write: volume/device target resolves to the OS system disk "
                "or PhysicalDrive0");
        }
    }
#else
    Q_UNUSED(path);
#endif
    return QString();
}

// Defense-in-depth OS-disk guard for --allow-raw-target HFS commands, mirroring the
// APFS CLI. image_only = !allow_raw_target, so --allow-raw-target enables raw device
// writes and a path typo could otherwise hit PhysicalDrive0 or the OS disk. Covers the
// \\.\PhysicalDriveN form and volume/device aliases that resolve to the OS volume.
QString rawTargetProtectedDiskRefusal(const CliInvocation& invocation) {
    if (!invocation.allow_raw_target) {
        return QString();
    }
    QString path = invocation.target_image_path.trimmed();
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

}  // namespace

int main(int argc, char* argv[]) {
    const QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sak_hfs_writer_cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.9.2.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "S.A.K. HFS+ image-only writer. Supports certified data/resource-fork replacement "
        "and bounded allocation-growth file/fork mutations in an HFS+/HFSX image."));
    parser.addHelpOption();
    parser.addVersionOption();

    const CliOptions options = buildCliOptions();
    registerCliOptions(parser, options);
    registerPositionalCommand(parser);
    parser.process(app);

    QString error;
    const auto invocation = parseInvocation(parser, options, &error);
    if (!invocation.has_value()) {
        QTextStream(stderr) << error << Qt::endl;
        return kExitInvalidArguments;
    }

    const QString outputJsonPath = parser.value(options.output_json).trimmed();
    if (outputJsonAliasesTarget(outputJsonPath, invocation->target_image_path)) {
        QTextStream(stderr) << "--output-json path must not alias the target image" << Qt::endl;
        return kExitInvalidArguments;
    }

    // Defense-in-depth: never let an --allow-raw-target command hit the OS disk or
    // PhysicalDrive0 (there is no GUI validator in this CLI path).
    const QString rawRefusal = rawTargetProtectedDiskRefusal(*invocation);
    if (!rawRefusal.isEmpty()) {
        QTextStream(stderr) << rawRefusal << Qt::endl;
        return kExitInvalidArguments;
    }

    QString reportError;
    const QJsonObject report = invocationReport(*invocation);
    if (!writeReport(report, outputJsonPath, &reportError)) {
        QTextStream(stderr) << "Failed to write report: " << reportError << Qt::endl;
        return kExitReportFailed;
    }
    return report.value(QStringLiteral("ok")).toBool(false) ? kExitOk : kExitOperationFailed;
}
