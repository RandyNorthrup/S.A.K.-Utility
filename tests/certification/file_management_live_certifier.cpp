// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_live_certifier.cpp
/// @brief Live raw APFS/HFS File Management certification helper.

#include "sak/advanced_search_worker.h"
#include "sak/duplicate_finder_worker.h"
#include "sak/file_management_file_system.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace {

constexpr int kDefaultMaxDepth = 8;
constexpr int kDefaultMaxDirectories = 512;
constexpr int kDefaultMaxEntriesPerDirectory = 1024;
constexpr int kDefaultWorkerTimeoutMs = 180000;
constexpr uint64_t kDefaultReadMaxBytes = 1024ULL * 1024ULL;
constexpr uint64_t kMinimumGeneratedApfsBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kGeneratedApfsSingleChunkMaxBytes = 128ULL * 1024ULL * 1024ULL;

struct TargetSpec {
    QString file_system;
    QString path;
    uint64_t size_bytes{0};
};

struct Config {
    QString output_path;
    QVector<TargetSpec> targets;
    bool destructive{false};
    int max_depth{kDefaultMaxDepth};
    int max_directories{kDefaultMaxDirectories};
    int max_entries_per_directory{kDefaultMaxEntriesPerDirectory};
    int worker_timeout_ms{kDefaultWorkerTimeoutMs};
    uint64_t read_max_bytes{kDefaultReadMaxBytes};
};

struct Sample {
    sak::FileManagementEntry entry;
    QByteArray data;
};

struct TraversalState {
    int directories_visited{0};
    int files_seen{0};
    QStringList warnings;
    QStringList blockers;
};

struct DuplicateRun {
    bool ok{false};
    bool timed_out{false};
    QString summary;
    QString error;
    int duplicate_count{0};
    qint64 wasted_space{0};
    int progress_events{0};
};

struct SearchRun {
    bool ok{false};
    bool timed_out{false};
    QString error;
    int result_count{0};
    int searched_files{0};
    QStringList matched_paths;
};

struct VerifyRun {
    bool ok{false};
    QString path;
    QString sha256;
    QString expected_sha256;
    uint64_t bytes_read{0};
    QStringList blockers;
    QStringList warnings;
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

QVector<QString> repeatedArgValues(const QStringList& args, const QString& name) {
    QVector<QString> values;
    for (int i = 0; i + 1 < args.size(); ++i) {
        if (args.at(i) == name) {
            values.append(args.at(i + 1));
            ++i;
        }
    }
    return values;
}

bool hasArg(const QStringList& args, const QString& name) {
    return args.contains(name);
}

std::optional<uint64_t> parseUInt64(const QString& value) {
    bool ok = false;
    const uint64_t parsed = value.toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return parsed;
}

int parseIntArg(const QStringList& args, const QString& name, int fallback) {
    const QString value = argValue(args, name);
    if (value.trimmed().isEmpty()) {
        return fallback;
    }
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? parsed : fallback;
}

uint64_t parseUInt64Arg(const QStringList& args, const QString& name, uint64_t fallback) {
    const QString value = argValue(args, name);
    if (value.trimmed().isEmpty()) {
        return fallback;
    }
    return parseUInt64(value).value_or(fallback);
}

QString targetSizeKey(const QString& fileSystem) {
    const QString key = sak::FileManagementFileSystemBridge::normalizedFileSystem(fileSystem);
    return key.isEmpty() ? fileSystem.trimmed().toLower() : key;
}

QStringList parseErrors(const Config& config) {
    QStringList errors;
    if (config.output_path.trimmed().isEmpty()) {
        errors.append(QStringLiteral("--output is required"));
    }
    if (config.targets.isEmpty()) {
        errors.append(QStringLiteral("At least one --target FS=path argument is required"));
    }
    for (const auto& target : config.targets) {
        if (target.file_system.trimmed().isEmpty() || target.path.trimmed().isEmpty()) {
            errors.append(QStringLiteral("--target values must use FS=path"));
        }
        if (config.destructive &&
            sak::FileManagementFileSystemBridge::normalizedFileSystem(target.file_system) ==
                QStringLiteral("apfs") &&
            target.size_bytes == 0) {
            errors.append(
                QStringLiteral("--target-size APFS=bytes is required for destructive APFS raw writes"));
        }
        if (config.destructive &&
            sak::FileManagementFileSystemBridge::normalizedFileSystem(target.file_system) ==
                QStringLiteral("apfs") &&
            target.size_bytes != 0 &&
            (target.size_bytes < kMinimumGeneratedApfsBytes ||
             target.size_bytes > kGeneratedApfsSingleChunkMaxBytes)) {
            errors.append(QStringLiteral(
                "Destructive APFS live certification currently supports one-spaceman-chunk "
                "generated targets only (64 MiB through 128 MiB)"));
        }
    }
    return errors;
}

Config parseConfig(const QStringList& args) {
    Config config;
    config.output_path = argValue(args, QStringLiteral("--output"));
    config.max_depth = parseIntArg(args, QStringLiteral("--max-depth"), config.max_depth);
    config.max_directories =
        parseIntArg(args, QStringLiteral("--max-directories"), config.max_directories);
    config.max_entries_per_directory =
        parseIntArg(args, QStringLiteral("--max-entries"), config.max_entries_per_directory);
    config.worker_timeout_ms =
        parseIntArg(args, QStringLiteral("--worker-timeout-ms"), config.worker_timeout_ms);
    config.read_max_bytes =
        parseUInt64Arg(args, QStringLiteral("--read-max-bytes"), config.read_max_bytes);
    config.destructive = hasArg(args, QStringLiteral("--destructive"));

    QHash<QString, uint64_t> targetSizes;
    const auto targetSizeValues = repeatedArgValues(args, QStringLiteral("--target-size"));
    for (const auto& value : targetSizeValues) {
        const int split = value.indexOf(QLatin1Char('='));
        if (split <= 0) {
            continue;
        }
        const auto parsed = parseUInt64(value.mid(split + 1).trimmed());
        if (parsed.has_value()) {
            targetSizes.insert(targetSizeKey(value.left(split).trimmed()), *parsed);
        }
    }

    const auto targetValues = repeatedArgValues(args, QStringLiteral("--target"));
    for (const auto& value : targetValues) {
        const int split = value.indexOf(QLatin1Char('='));
        if (split <= 0) {
            config.targets.append({{}, value});
            continue;
        }
        TargetSpec target{value.left(split).trimmed(), value.mid(split + 1).trimmed(), 0};
        target.size_bytes = targetSizes.value(targetSizeKey(target.file_system), 0);
        config.targets.append(target);
    }
    return config;
}

bool writeJsonFile(const QString& path, const QJsonObject& object, QString* error) {
    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        if (error) {
            *error = QStringLiteral("Unable to create output directory: %1")
                         .arg(info.absolutePath());
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Unable to open output report: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

QJsonArray stringsToJson(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
}

QString sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString safeStamp() {
    QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    stamp.replace(QLatin1Char(':'), QLatin1Char('-'));
    return stamp;
}

QString parentPath(QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.contains(QStringLiteral("//"))) {
        path.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    if (path == QStringLiteral("/") || path.trimmed().isEmpty()) {
        return QStringLiteral("/");
    }
    if (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const int split = path.lastIndexOf(QLatin1Char('/'));
    if (split <= 0) {
        return QStringLiteral("/");
    }
    return path.left(split);
}

QString joinTargetPath(QString directory, const QString& name) {
    directory.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (directory.contains(QStringLiteral("//"))) {
        directory.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    if (directory.trimmed().isEmpty()) {
        directory = QStringLiteral("/");
    }
    if (!directory.startsWith(QLatin1Char('/'))) {
        directory.prepend(QLatin1Char('/'));
    }
    if (!directory.endsWith(QLatin1Char('/'))) {
        directory.append(QLatin1Char('/'));
    }
    return directory + name;
}

QString previewToken(const QByteArray& data) {
    const QString text = QString::fromUtf8(data);
    static const QRegularExpression tokenPattern(QStringLiteral("[A-Za-z0-9][A-Za-z0-9 _.-]{2,31}"));
    const auto match = tokenPattern.match(text);
    return match.hasMatch() ? match.captured(0).trimmed() : QString();
}

std::optional<Sample> findReadableSample(const sak::FileManagementTarget& target,
                                         const QString& directory,
                                         int depth,
                                         const Config& config,
                                         TraversalState* state) {
    if (!state || state->directories_visited >= config.max_directories ||
        depth > config.max_depth) {
        return std::nullopt;
    }
    ++state->directories_visited;

    const auto listing = sak::FileManagementFileSystemBridge::listDirectory(
        target, directory, config.max_entries_per_directory);
    if (!listing.ok) {
        state->warnings.append(QStringLiteral("List failed at %1: %2")
                                   .arg(directory, listing.blockers.join(QStringLiteral("; "))));
        return std::nullopt;
    }
    state->warnings.append(listing.warnings);

    for (const auto& entry : listing.entries) {
        if (!entry.regular_file || entry.size_bytes == 0 ||
            entry.size_bytes > config.read_max_bytes) {
            continue;
        }
        ++state->files_seen;
        const auto read =
            sak::FileManagementFileSystemBridge::readFile(target, entry.path, config.read_max_bytes);
        if (read.ok && !read.data.isEmpty()) {
            return Sample{entry, read.data};
        }
        state->warnings.append(QStringLiteral("Read skipped at %1: %2")
                                   .arg(entry.path, read.blockers.join(QStringLiteral("; "))));
    }

    for (const auto& entry : listing.entries) {
        if (entry.directory && !entry.symlink) {
            auto sample = findReadableSample(target, entry.path, depth + 1, config, state);
            if (sample.has_value()) {
                return sample;
            }
        }
    }
    return std::nullopt;
}

DuplicateRun runDuplicateWorker(const sak::FileManagementTarget& target,
                                const QString& directory,
                                int timeoutMs) {
    DuplicateRun run;
    DuplicateFinderWorker::Config config;
    config.file_system_target = target;
    config.use_file_system_target = true;
    config.virtual_directories = {directory};
    config.recursive_scan = false;
    config.parallel_hashing = false;
    config.minimum_file_size = 0;

    DuplicateFinderWorker worker(config);
    QEventLoop loop;
    QTimer timeout;
    QTimer hardTimeout;
    timeout.setSingleShot(true);
    hardTimeout.setSingleShot(true);

    QObject::connect(&worker,
                     &DuplicateFinderWorker::resultsReady,
                     &loop,
                     [&](const QString& summary, int duplicateCount, qint64 wastedSpace) {
                         run.summary = summary;
                         run.duplicate_count = duplicateCount;
                         run.wasted_space = wastedSpace;
                     });
    QObject::connect(&worker,
                     &DuplicateFinderWorker::scanProgress,
                     &loop,
                     [&](int, int, const QString&) { ++run.progress_events; });
    QObject::connect(&worker, &DuplicateFinderWorker::finished, &loop, [&]() {
        run.ok = true;
        loop.quit();
    });
    QObject::connect(&worker, &DuplicateFinderWorker::failed, &loop, [&](int, const QString& msg) {
        run.ok = false;
        run.error = msg;
        loop.quit();
    });
    QObject::connect(&worker, &DuplicateFinderWorker::cancelled, &loop, [&]() {
        run.ok = false;
        run.error = QStringLiteral("cancelled");
        loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        run.timed_out = true;
        worker.requestStop();
    });
    QObject::connect(&hardTimeout, &QTimer::timeout, &loop, [&]() {
        run.timed_out = true;
        run.error = QStringLiteral("hard timeout");
        loop.quit();
    });

    worker.start();
    timeout.start(timeoutMs);
    hardTimeout.start(timeoutMs + 15000);
    loop.exec();
    if (worker.isRunning()) {
        worker.requestStop();
        worker.wait(15000);
    }
    return run;
}

SearchRun runAdvancedSearchWorker(const sak::FileManagementTarget& target,
                                  const Sample& sample,
                                  int timeoutMs,
                                  uint64_t readMaxBytes) {
    SearchRun run;
    sak::SearchConfig config;
    config.file_system_target = target;
    config.use_file_system_target = true;
    config.root_path = parentPath(sample.entry.path);
    config.pattern = sample.entry.name.isEmpty() ? previewToken(sample.data) : sample.entry.name;
    config.search_file_metadata = true;
    config.exclude_patterns = {};
    config.max_results = 1;
    config.max_file_size = static_cast<qint64>(std::min<uint64_t>(
        readMaxBytes, static_cast<uint64_t>(std::numeric_limits<qint64>::max())));
    if (config.pattern.trimmed().isEmpty()) {
        config.pattern = sample.entry.path;
    }

    sak::AdvancedSearchWorker worker(config);
    QEventLoop loop;
    QTimer timeout;
    QTimer hardTimeout;
    timeout.setSingleShot(true);
    hardTimeout.setSingleShot(true);

    QObject::connect(&worker,
                     &sak::AdvancedSearchWorker::resultsReady,
                     &loop,
                     [&](const QVector<sak::SearchMatch>& matches) {
                         run.result_count += matches.size();
                         for (const auto& match : matches) {
                             run.matched_paths.append(match.file_path);
                         }
                     });
    QObject::connect(&worker,
                     &sak::AdvancedSearchWorker::fileSearched,
                     &loop,
                     [&](const QString&, int) { ++run.searched_files; });
    QObject::connect(&worker, &sak::AdvancedSearchWorker::finished, &loop, [&]() {
        run.ok = run.result_count > 0;
        if (!run.ok) {
            run.error = QStringLiteral("no search match returned");
        }
        loop.quit();
    });
    QObject::connect(&worker, &sak::AdvancedSearchWorker::failed, &loop, [&](int, const QString& msg) {
        run.ok = false;
        run.error = msg;
        loop.quit();
    });
    QObject::connect(&worker, &sak::AdvancedSearchWorker::cancelled, &loop, [&]() {
        run.ok = false;
        run.error = QStringLiteral("cancelled");
        loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        run.timed_out = true;
        worker.requestStop();
    });
    QObject::connect(&hardTimeout, &QTimer::timeout, &loop, [&]() {
        run.timed_out = true;
        run.error = QStringLiteral("hard timeout");
        loop.quit();
    });

    worker.start();
    timeout.start(timeoutMs);
    hardTimeout.start(timeoutMs + 15000);
    loop.exec();
    if (worker.isRunning()) {
        worker.requestStop();
        worker.wait(15000);
    }
    return run;
}

QJsonObject entryToJson(const sak::FileManagementEntry& entry) {
    return QJsonObject{{QStringLiteral("name"), entry.name},
                       {QStringLiteral("path"), entry.path},
                       {QStringLiteral("type"), entry.type},
                       {QStringLiteral("size_bytes"), QString::number(entry.size_bytes)},
                       {QStringLiteral("identifier"), entry.identifier},
                       {QStringLiteral("directory"), entry.directory},
                       {QStringLiteral("regular_file"), entry.regular_file},
                       {QStringLiteral("symlink"), entry.symlink}};
}

QJsonObject mutationToJson(const QString& action,
                           const sak::FileManagementMutationResult& mutation) {
    return QJsonObject{{QStringLiteral("action"), action},
                       {QStringLiteral("ok"), mutation.ok},
                       {QStringLiteral("file_system"), mutation.file_system},
                       {QStringLiteral("path"), mutation.path},
                       {QStringLiteral("bytes_written"), QString::number(mutation.bytes_written)},
                       {QStringLiteral("before_sha256"), mutation.before_sha256},
                       {QStringLiteral("after_sha256"), mutation.after_sha256},
                       {QStringLiteral("blockers"), stringsToJson(mutation.blockers)},
                       {QStringLiteral("warnings"), stringsToJson(mutation.warnings)}};
}

VerifyRun verifyReadBack(const sak::FileManagementTarget& target,
                         const QString& path,
                         const QByteArray& expected,
                         uint64_t readMaxBytes) {
    VerifyRun run;
    run.path = path;
    run.expected_sha256 = sha256Hex(expected);
    const auto read = sak::FileManagementFileSystemBridge::readFile(target, path, readMaxBytes);
    run.blockers = read.blockers;
    run.warnings = read.warnings;
    run.bytes_read = static_cast<uint64_t>(std::max<qsizetype>(0, read.data.size()));
    run.sha256 = sha256Hex(read.data);
    run.ok = read.ok && read.data == expected;
    if (read.ok && !run.ok) {
        run.blockers.append(QStringLiteral("Readback payload mismatch"));
    }
    return run;
}

QJsonObject verifyToJson(const QString& action, const VerifyRun& run) {
    return QJsonObject{{QStringLiteral("action"), action},
                       {QStringLiteral("ok"), run.ok},
                       {QStringLiteral("path"), run.path},
                       {QStringLiteral("bytes_read"), QString::number(run.bytes_read)},
                       {QStringLiteral("sha256"), run.sha256},
                       {QStringLiteral("expected_sha256"), run.expected_sha256},
                       {QStringLiteral("blockers"), stringsToJson(run.blockers)},
                       {QStringLiteral("warnings"), stringsToJson(run.warnings)}};
}

QJsonObject runMutationCertification(const sak::FileManagementTarget& target,
                                     const Config& config) {
    QJsonArray steps;
    bool ok = target.can_write_files;
    const QString fs = sak::FileManagementFileSystemBridge::normalizedFileSystem(target.file_system);
    const QString stamp = safeStamp();
    const QString rootDirectory = QStringLiteral("/sak-fm-live-%1").arg(stamp);
    const QString filePath = joinTargetPath(rootDirectory, QStringLiteral("payload.txt"));
    const QString renamedPath = joinTargetPath(rootDirectory, QStringLiteral("payload-renamed.txt"));
    const QByteArray payload =
        QStringLiteral("S.A.K. File Management live mutation proof\nfs=%1\nstamp=%2\n")
            .arg(target.file_system, stamp)
            .toUtf8();

    if (!ok) {
        steps.append(QJsonObject{{QStringLiteral("action"), QStringLiteral("capability-check")},
                                 {QStringLiteral("ok"), false},
                                 {QStringLiteral("blockers"), stringsToJson(target.blockers)}});
    }

    const auto createDirectory =
        sak::FileManagementFileSystemBridge::createDirectory(target, rootDirectory);
    steps.append(mutationToJson(QStringLiteral("create-directory"), createDirectory));
    ok = ok && createDirectory.ok;

    const auto writeFile = sak::FileManagementFileSystemBridge::writeFile(target, filePath, payload);
    steps.append(mutationToJson(QStringLiteral("write-file"), writeFile));
    ok = ok && writeFile.ok;

    const auto verifyWrite = verifyReadBack(target, filePath, payload, config.read_max_bytes);
    steps.append(verifyToJson(QStringLiteral("read-after-write"), verifyWrite));
    ok = ok && verifyWrite.ok;
    if (verifyWrite.ok) {
        Sample mutationSample;
        mutationSample.entry.name = QStringLiteral("payload.txt");
        mutationSample.entry.path = filePath;
        mutationSample.entry.type = QStringLiteral("File");
        mutationSample.entry.size_bytes = static_cast<uint64_t>(payload.size());
        mutationSample.entry.regular_file = true;
        mutationSample.data = payload;

        const auto duplicate = runDuplicateWorker(target, rootDirectory, config.worker_timeout_ms);
        steps.append(QJsonObject{{QStringLiteral("action"), QStringLiteral("duplicate-finder-created-file")},
                                 {QStringLiteral("ok"), duplicate.ok},
                                 {QStringLiteral("timed_out"), duplicate.timed_out},
                                 {QStringLiteral("scan_directory"), rootDirectory},
                                 {QStringLiteral("summary"), duplicate.summary},
                                 {QStringLiteral("duplicate_count"), duplicate.duplicate_count},
                                 {QStringLiteral("wasted_space"), QString::number(duplicate.wasted_space)},
                                 {QStringLiteral("progress_events"), duplicate.progress_events},
                                 {QStringLiteral("error"), duplicate.error}});
        ok = ok && duplicate.ok;

        const auto search =
            runAdvancedSearchWorker(target, mutationSample, config.worker_timeout_ms, config.read_max_bytes);
        steps.append(QJsonObject{{QStringLiteral("action"), QStringLiteral("advanced-search-created-file")},
                                 {QStringLiteral("ok"), search.ok},
                                 {QStringLiteral("timed_out"), search.timed_out},
                                 {QStringLiteral("result_count"), search.result_count},
                                 {QStringLiteral("searched_files"), search.searched_files},
                                 {QStringLiteral("matched_paths"), stringsToJson(search.matched_paths)},
                                 {QStringLiteral("error"), search.error}});
        ok = ok && search.ok;
    }

    QString deletePath = filePath;
    if (fs == QStringLiteral("hfsplus") || fs == QStringLiteral("hfsx")) {
        const auto rename =
            sak::FileManagementFileSystemBridge::renameEntry(target, filePath, renamedPath);
        steps.append(mutationToJson(QStringLiteral("rename-entry"), rename));
        ok = ok && rename.ok;
        if (rename.ok) {
            deletePath = renamedPath;
            const auto verifyRename =
                verifyReadBack(target, renamedPath, payload, config.read_max_bytes);
            steps.append(verifyToJson(QStringLiteral("read-after-rename"), verifyRename));
            ok = ok && verifyRename.ok;
        }
    }

    const auto deleteFile = sak::FileManagementFileSystemBridge::deleteFile(target, deletePath);
    steps.append(mutationToJson(QStringLiteral("delete-file"), deleteFile));
    ok = ok && deleteFile.ok;

    const auto deleteVerify =
        sak::FileManagementFileSystemBridge::readFile(target, deletePath, config.read_max_bytes);
    const bool deleteVerified = !deleteVerify.ok;
    steps.append(QJsonObject{{QStringLiteral("action"), QStringLiteral("read-after-delete")},
                             {QStringLiteral("ok"), deleteVerified},
                             {QStringLiteral("path"), deletePath},
                             {QStringLiteral("blockers"), stringsToJson(deleteVerify.blockers)},
                             {QStringLiteral("warnings"), stringsToJson(deleteVerify.warnings)}});
    ok = ok && deleteVerified;

    const auto deleteDirectory =
        sak::FileManagementFileSystemBridge::deleteDirectory(target, rootDirectory);
    steps.append(mutationToJson(QStringLiteral("delete-directory"), deleteDirectory));
    ok = ok && deleteDirectory.ok;

    return QJsonObject{{QStringLiteral("ok"), ok},
                       {QStringLiteral("root_directory"), rootDirectory},
                       {QStringLiteral("steps"), steps}};
}

QJsonObject certifyTarget(const TargetSpec& spec, const Config& config) {
    auto target =
        sak::FileManagementFileSystemBridge::manualTarget(spec.path, spec.file_system, spec.size_bytes);
    target.label = QStringLiteral("%1 live target").arg(target.file_system);

    QJsonObject result;
    result.insert(QStringLiteral("file_system"), target.file_system);
    result.insert(QStringLiteral("target_path"), target.root_path);
    result.insert(QStringLiteral("target_size_bytes"), QString::number(target.size_bytes));
    result.insert(QStringLiteral("capability_summary"),
                  sak::FileManagementFileSystemBridge::capabilitySummary(target));
    result.insert(QStringLiteral("organizer_can_organize"), target.can_organize);
    result.insert(QStringLiteral("explorer_can_write"), target.can_write_files);
    result.insert(QStringLiteral("organizer_blockers"), stringsToJson(target.blockers));

    const auto rootListing = sak::FileManagementFileSystemBridge::listDirectory(
        target, QStringLiteral("/"), config.max_entries_per_directory);
    QJsonObject root;
    root.insert(QStringLiteral("ok"), rootListing.ok);
    root.insert(QStringLiteral("file_system"), rootListing.file_system);
    root.insert(QStringLiteral("volume_name"), rootListing.volume_name);
    root.insert(QStringLiteral("entry_count"), rootListing.entries.size());
    root.insert(QStringLiteral("blockers"), stringsToJson(rootListing.blockers));
    root.insert(QStringLiteral("warnings"), stringsToJson(rootListing.warnings));
    result.insert(QStringLiteral("file_explorer_root_listing"), root);

    TraversalState state;
    auto sample = findReadableSample(target, QStringLiteral("/"), 0, config, &state);
    QJsonObject traversal;
    traversal.insert(QStringLiteral("directories_visited"), state.directories_visited);
    traversal.insert(QStringLiteral("files_seen"), state.files_seen);
    traversal.insert(QStringLiteral("warnings"), stringsToJson(state.warnings));
    traversal.insert(QStringLiteral("blockers"), stringsToJson(state.blockers));
    result.insert(QStringLiteral("sample_traversal"), traversal);

    bool targetPassed = rootListing.ok && !target.can_organize;
    if (sample.has_value()) {
        QJsonObject sampleJson = entryToJson(sample->entry);
        sampleJson.insert(QStringLiteral("read_bytes"), sample->data.size());
        sampleJson.insert(QStringLiteral("sha256"), sha256Hex(sample->data));
        sampleJson.insert(QStringLiteral("preview_token"), previewToken(sample->data));
        result.insert(QStringLiteral("file_explorer_sample_read"), sampleJson);

        const QString scanDirectory = parentPath(sample->entry.path);
        const auto duplicate = runDuplicateWorker(target, scanDirectory, config.worker_timeout_ms);
        result.insert(QStringLiteral("duplicate_finder"),
                      QJsonObject{{QStringLiteral("ok"), duplicate.ok},
                                  {QStringLiteral("timed_out"), duplicate.timed_out},
                                  {QStringLiteral("scan_directory"), scanDirectory},
                                  {QStringLiteral("summary"), duplicate.summary},
                                  {QStringLiteral("duplicate_count"), duplicate.duplicate_count},
                                  {QStringLiteral("wasted_space"), QString::number(duplicate.wasted_space)},
                                  {QStringLiteral("progress_events"), duplicate.progress_events},
                                  {QStringLiteral("error"), duplicate.error}});

        const auto search =
            runAdvancedSearchWorker(target, *sample, config.worker_timeout_ms, config.read_max_bytes);
        result.insert(QStringLiteral("advanced_search"),
                      QJsonObject{{QStringLiteral("ok"), search.ok},
                                  {QStringLiteral("timed_out"), search.timed_out},
                                  {QStringLiteral("result_count"), search.result_count},
                                  {QStringLiteral("searched_files"), search.searched_files},
                                  {QStringLiteral("matched_paths"), stringsToJson(search.matched_paths)},
                                  {QStringLiteral("error"), search.error}});
        targetPassed = targetPassed && duplicate.ok && search.ok;
    } else {
        result.insert(QStringLiteral("file_explorer_sample_read"),
                      QJsonObject{{QStringLiteral("ok"), false},
                                  {QStringLiteral("error"), QStringLiteral("no readable sample file found")}});
        result.insert(QStringLiteral("duplicate_finder"),
                      QJsonObject{{QStringLiteral("ok"), false},
                                  {QStringLiteral("error"), QStringLiteral("not run without sample")}});
        result.insert(QStringLiteral("advanced_search"),
                      QJsonObject{{QStringLiteral("ok"), false},
                                  {QStringLiteral("error"), QStringLiteral("not run without sample")}});
        if (!config.destructive) {
            targetPassed = false;
        }
    }

    if (config.destructive) {
        const auto mutation = runMutationCertification(target, config);
        result.insert(QStringLiteral("file_explorer_mutations"), mutation);
        targetPassed = targetPassed && mutation.value(QStringLiteral("ok")).toBool(false);
    } else {
        result.insert(QStringLiteral("file_explorer_mutations"),
                      QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("skipped"), true},
                                  {QStringLiteral("reason"), QStringLiteral("--destructive not set")}});
    }

    result.insert(QStringLiteral("status"), targetPassed ? QStringLiteral("Passed")
                                                         : QStringLiteral("Failed"));
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<QVector<sak::SearchMatch>>("QVector<sak::SearchMatch>");

    const QStringList args = QCoreApplication::arguments();
    const Config config = parseConfig(args);
    const QStringList errors = parseErrors(config);

    QJsonObject report{{QStringLiteral("schema_version"), 1},
                       {QStringLiteral("tool"), QStringLiteral("file-management-live-certifier")},
                       {QStringLiteral("created_utc"), utcNow()},
                       {QStringLiteral("destructive"), config.destructive},
                       {QStringLiteral("writes_target_media"), config.destructive},
                       {QStringLiteral("status"), QStringLiteral("Failed")}};

    if (!errors.isEmpty()) {
        report.insert(QStringLiteral("errors"), stringsToJson(errors));
        QString writeError;
        if (!config.output_path.trimmed().isEmpty()) {
            writeJsonFile(config.output_path, report, &writeError);
        }
        return 2;
    }

    QJsonArray targetReports;
    bool passed = true;
    for (const auto& target : config.targets) {
        QJsonObject targetReport = certifyTarget(target, config);
        passed = passed && targetReport.value(QStringLiteral("status")).toString() ==
                               QStringLiteral("Passed");
        targetReports.append(targetReport);
    }

    report.insert(QStringLiteral("targets"), targetReports);
    report.insert(QStringLiteral("status"), passed ? QStringLiteral("Passed")
                                                   : QStringLiteral("Failed"));
    report.insert(QStringLiteral("finished_utc"), utcNow());

    QString writeError;
    if (!writeJsonFile(config.output_path, report, &writeError)) {
        qCritical("%s", qPrintable(writeError));
        return 3;
    }
    return passed ? 0 : 1;
}
