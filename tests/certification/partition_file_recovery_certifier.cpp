// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_recovery_engine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>

#include <io.h>
#endif

namespace {

constexpr uint64_t kDefaultMaxScanBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxCandidateBytes = 4ULL * 1024ULL * 1024ULL;
constexpr int kMaxCandidates = 128;

struct CertifierConfig {
    QString outputDir;
    QString manifestPath;
    QString suggestedEvidencePath;
    QString fileSystem;
    QString fixtureName;
    QString volumeId;
    QString drive;
    uint64_t maxScanBytes{0};
};

struct CertifierPaths {
    QString restoreDir;
    QString runPath;
    QString reportPath;
    QString rootPath;
    QString fixturePath;
    QString sourcePath;
};

struct FixtureContent {
    QByteArray bytes;
    QString sha256;
};

struct RestoreArtifacts {
    QString path;
    QString sha256;
};

struct CertificationRun {
    sak::FileRecoveryScanResult scan;
    sak::FileRecoveryCandidate match;
    sak::FileRecoveryRestoreResult restore;
    RestoreArtifacts restored;
    FixtureContent fixture;
};

struct ReportFields {
    QString manifestPath;
    QString suggestedEvidencePath;
    QString status;
    QJsonObject evidence;
    QStringList artifacts;
    QString verificationSummary;
    QString operatorNotes;
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

QString normalizedDrive(QString drive) {
    drive = drive.trimmed();
    drive.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (drive.endsWith(QLatin1Char('\\'))) {
        drive.chop(1);
    }
    if (drive.size() == 1 && drive.at(0).isLetter()) {
        drive += QLatin1Char(':');
    }
    return drive.left(2).toUpper();
}

QString rawVolumePath(const QString& drive) {
    return QStringLiteral("\\\\.\\%1").arg(drive.left(2));
}

QString driveRoot(const QString& drive) {
    return QStringLiteral("%1\\").arg(drive.left(2));
}

QByteArray fixtureBytes() {
    QByteArray data;
    data += "%PDF-1.7\n";
    data += "% SAK deleted file recovery certification fixture\n";
    data += "1 0 obj\n<< /Type /Catalog >>\nendobj\n";
    data += "2 0 obj\n<< /Length 262144 >>\nstream\n";
    for (int index = 0; index < 4096; ++index) {
        data += "SAK-FILE-RECOVERY-CERTIFICATION-FIXTURE-LINE-";
        data += QByteArray::number(index);
        data += "\n";
    }
    data += "\nendstream\nendobj\n%%EOF";
    return data;
}

QString sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())
        .toUpper();
}

QString fileSha256Hex(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return {};
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex()).toUpper();
}

bool flushFileToDisk(QFile* file) {
    if (!file->flush()) {
        return false;
    }
#ifdef Q_OS_WIN
    const intptr_t osFileHandle = _get_osfhandle(file->handle());
    if (osFileHandle == -1) {
        return false;
    }
    return FlushFileBuffers(reinterpret_cast<HANDLE>(osFileHandle)) != 0;
#else
    return true;
#endif
}

QJsonArray stringArray(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
}

QString projectRelativePath(const QString& path) {
    QString normalized = QDir::fromNativeSeparators(path);
    const int artifactIndex = normalized.indexOf(QStringLiteral("artifacts/"));
    if (artifactIndex >= 0) {
        return normalized.mid(artifactIndex);
    }
    const int scriptsIndex = normalized.indexOf(QStringLiteral("scripts/"));
    if (scriptsIndex >= 0) {
        return normalized.mid(scriptsIndex);
    }
    return normalized;
}

bool writeJsonFile(const QString& path, const QJsonObject& object) {
    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

QJsonObject reportBase(const ReportFields& fields) {
    return QJsonObject{
        {QStringLiteral("tool"), QStringLiteral("partition-manager-external-evidence-report")},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("created_utc"), utcNow()},
        {QStringLiteral("gate_id"), QStringLiteral("external.file-level-data-recovery")},
        {QStringLiteral("gate_name"),
         QStringLiteral("File-level Data Recovery scan and restore proof")},
        {QStringLiteral("status"), fields.status},
        {QStringLiteral("manifest"), fields.manifestPath},
        {QStringLiteral("certification_matrix"),
         QStringLiteral("docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json")},
        {QStringLiteral("suggested_evidence_path"), fields.suggestedEvidencePath},
        {QStringLiteral("safety_contract"),
         stringArray({QStringLiteral("disposable_recovery_volume"),
                      QStringLiteral("read_only_source_scan"),
                      QStringLiteral("restore_to_separate_destination"),
                      QStringLiteral("recovered_hash_verified")})},
        {QStringLiteral("required_evidence_keys"),
         stringArray({QStringLiteral("source_volume_id"),
                      QStringLiteral("file_system"),
                      QStringLiteral("deleted_fixture_name"),
                      QStringLiteral("scan_result"),
                      QStringLiteral("restore_destination"),
                      QStringLiteral("recovered_file_hash"),
                      QStringLiteral("source_not_mutated")})},
        {QStringLiteral("required_evidence_values"), QJsonValue::Null},
        {QStringLiteral("evidence"), fields.evidence},
        {QStringLiteral("artifacts"), stringArray(fields.artifacts)},
        {QStringLiteral("verification_summary"), fields.verificationSummary},
        {QStringLiteral("operator_notes"), fields.operatorNotes}};
}

int failWithReport(const QString& outputDir,
                   const QString& message,
                   const QString& manifestPath,
                   const QString& suggestedEvidencePath,
                   const QJsonObject& evidence = {}) {
    const QString failedPath = QDir(outputDir).filePath(QStringLiteral("report.failed.json"));
    const auto report =
        reportBase(ReportFields{manifestPath,
                                suggestedEvidencePath,
                                QStringLiteral("Failed"),
                                evidence,
                                {projectRelativePath(failedPath)},
                                QStringLiteral("File recovery certification failed."),
                                message});
    writeJsonFile(failedPath, report);
    QTextStream(stderr) << message << Qt::endl;
    return 2;
}

CertifierConfig configFromArgs(const QStringList& args) {
    const QString outputDir =
        argValue(args,
                 QStringLiteral("--output-dir"),
                 QStringLiteral("artifacts/partition-manager-certification/vm-lab/"
                                "external-evidence/external.file-level-data-recovery"));
    const QString manifestPath =
        argValue(args,
                 QStringLiteral("--manifest"),
                 QStringLiteral("artifacts\\partition-manager-certification\\vm-lab\\"
                                "external-evidence.json"));
    const QString suggestedEvidencePath =
        argValue(args,
                 QStringLiteral("--suggested-evidence-path"),
                 QStringLiteral("artifacts/partition-manager-certification/vm-lab/"
                                "external-evidence/external.file-level-data-recovery/"
                                "report.json"));
    const QString fileSystem =
        argValue(args, QStringLiteral("--file-system"), QStringLiteral("NTFS"));
    const QString fixtureName = argValue(args,
                                         QStringLiteral("--fixture-name"),
                                         QStringLiteral("sak-deleted-recovery-fixture.pdf"));
    const QString volumeId = argValue(args, QStringLiteral("--volume-id"));
    const QString drive = normalizedDrive(argValue(args, QStringLiteral("--drive")));
    const uint64_t maxScanBytes = std::max<uint64_t>(
        1,
        argValue(args, QStringLiteral("--max-scan-bytes"), QString::number(kDefaultMaxScanBytes))
            .toULongLong());

    return CertifierConfig{outputDir,
                           manifestPath,
                           suggestedEvidencePath,
                           fileSystem,
                           fixtureName,
                           volumeId,
                           drive,
                           maxScanBytes};
}

CertifierPaths pathsFromConfig(const CertifierConfig& config) {
    return CertifierPaths{
        QDir(config.outputDir).filePath(QStringLiteral("restored")),
        QDir(config.outputDir).filePath(QStringLiteral("file-recovery-certifier-run.json")),
        QDir(config.outputDir).filePath(QStringLiteral("report.json")),
        driveRoot(config.drive),
        QDir(driveRoot(config.drive)).filePath(config.fixtureName),
        rawVolumePath(config.drive)};
}

std::optional<int> validateConfig(const CertifierConfig& config) {
    if (config.drive.isEmpty()) {
        return failWithReport(config.outputDir,
                              QStringLiteral("Missing required --drive argument."),
                              config.manifestPath,
                              config.suggestedEvidencePath);
    }
    if (config.volumeId.trimmed().isEmpty()) {
        return failWithReport(config.outputDir,
                              QStringLiteral("Missing required --volume-id argument."),
                              config.manifestPath,
                              config.suggestedEvidencePath);
    }
    return std::nullopt;
}

void ensureOutputDirectories(const CertifierConfig& config, const CertifierPaths& paths) {
    QDir().mkpath(config.outputDir);
    QDir().mkpath(paths.restoreDir);
}

bool writeDeletedFixture(const QString& fixturePath,
                         const QByteArray& fixture,
                         QString* errorMessage) {
    QFile::remove(fixturePath);
    QFile fixtureFile(fixturePath);
    if (!fixtureFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorMessage = QStringLiteral("Could not create deleted fixture: %1").arg(fixturePath);
        return false;
    }
    if (fixtureFile.write(fixture) != fixture.size()) {
        *errorMessage =
            QStringLiteral("Could not fully write deleted fixture: %1").arg(fixturePath);
        fixtureFile.close();
        return false;
    }
    if (!flushFileToDisk(&fixtureFile)) {
        fixtureFile.close();
        *errorMessage =
            QStringLiteral("Could not flush deleted fixture to disk: %1").arg(fixturePath);
        return false;
    }
    fixtureFile.close();
    if (!QFile::remove(fixturePath)) {
        *errorMessage = QStringLiteral("Could not delete fixture: %1").arg(fixturePath);
        return false;
    }
    return true;
}

std::optional<int> prepareFixture(const CertifierConfig& config,
                                  const CertifierPaths& paths,
                                  FixtureContent* fixture) {
    ensureOutputDirectories(config, paths);
    if (!QDir(paths.rootPath).exists()) {
        return failWithReport(config.outputDir,
                              QStringLiteral("Drive root does not exist: %1").arg(paths.rootPath),
                              config.manifestPath,
                              config.suggestedEvidencePath);
    }

    fixture->bytes = fixtureBytes();
    fixture->sha256 = sha256Hex(fixture->bytes);
    QString errorMessage;
    if (!writeDeletedFixture(paths.fixturePath, fixture->bytes, &errorMessage)) {
        return failWithReport(
            config.outputDir, errorMessage, config.manifestPath, config.suggestedEvidencePath);
    }
    return std::nullopt;
}

sak::FileRecoveryScanResult scanRawSource(const CertifierConfig& config,
                                          const CertifierPaths& paths) {
    sak::FileRecoveryScanOptions scanOptions;
    scanOptions.image_path = paths.sourcePath;
    scanOptions.max_scan_bytes = config.maxScanBytes;
    scanOptions.max_candidate_bytes = kMaxCandidateBytes;
    scanOptions.max_candidates = kMaxCandidates;
    scanOptions.capture_candidate_bytes = true;
    return sak::FileRecoveryEngine::scanOfflineImage(scanOptions);
}

std::optional<sak::FileRecoveryCandidate> matchingCandidate(const sak::FileRecoveryScanResult& scan,
                                                            const QString& fixtureHash) {
    std::optional<sak::FileRecoveryCandidate> match;
    for (const auto& candidate : scan.candidates) {
        if (QString::fromLatin1(candidate.sha256.toHex()).toUpper() == fixtureHash) {
            match = candidate;
            break;
        }
    }
    return match;
}

QJsonObject scanRunObject(const CertifierConfig& config,
                          const CertifierPaths& paths,
                          const FixtureContent& fixture,
                          const sak::FileRecoveryScanResult& scan) {
    return QJsonObject{
        {QStringLiteral("tool"), QStringLiteral("partition-file-recovery-certifier")},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("created_utc"), utcNow()},
        {QStringLiteral("drive"), config.drive},
        {QStringLiteral("source_path"), paths.sourcePath},
        {QStringLiteral("volume_id"), config.volumeId},
        {QStringLiteral("file_system"), config.fileSystem},
        {QStringLiteral("deleted_fixture_name"), config.fixtureName},
        {QStringLiteral("deleted_fixture_absent"), !QFileInfo::exists(paths.fixturePath)},
        {QStringLiteral("fixture_sha256"), fixture.sha256},
        {QStringLiteral("max_scan_bytes"), QString::number(config.maxScanBytes)},
        {QStringLiteral("source_opened_read_only"), scan.source_opened_read_only},
        {QStringLiteral("bytes_read"), QString::number(scan.bytes_read)},
        {QStringLiteral("scan_candidate_count"), scan.candidates.size()},
        {QStringLiteral("scan_warnings"), stringArray(scan.warnings)}};
}

QJsonObject evidenceObject(const CertifierConfig& config,
                           const CertifierPaths& paths,
                           const QString& scanResult,
                           const QString& recoveredHash,
                           const QString& sourceNotMutated) {
    return QJsonObject{{QStringLiteral("source_volume_id"), config.volumeId},
                       {QStringLiteral("file_system"), config.fileSystem},
                       {QStringLiteral("deleted_fixture_name"), config.fixtureName},
                       {QStringLiteral("scan_result"), scanResult},
                       {QStringLiteral("restore_destination"), paths.restoreDir},
                       {QStringLiteral("recovered_file_hash"), recoveredHash},
                       {QStringLiteral("source_not_mutated"), sourceNotMutated}};
}

int failNoMatchingFixture(const CertifierConfig& config,
                          const CertifierPaths& paths,
                          const QJsonObject& run,
                          int candidateCount) {
    writeJsonFile(paths.runPath, run);
    const QJsonObject evidence =
        evidenceObject(config,
                       paths,
                       QStringLiteral("No matching deleted fixture found among %1 candidate(s).")
                           .arg(candidateCount),
                       QStringLiteral("not-recovered"),
                       QStringLiteral("not-verified"));
    return failWithReport(config.outputDir,
                          QStringLiteral("No matching deleted fixture was recovered."),
                          config.manifestPath,
                          config.suggestedEvidencePath,
                          evidence);
}

sak::FileRecoveryRestoreResult restoreMatch(const CertifierConfig& config,
                                            const CertifierPaths& paths,
                                            const sak::FileRecoveryCandidate& match) {
    sak::FileRecoveryRestoreOptions restoreOptions;
    restoreOptions.image_path = paths.sourcePath;
    restoreOptions.destination_directory = paths.restoreDir;
    restoreOptions.candidates = {match};
    restoreOptions.source_hash_bytes = config.maxScanBytes;
    restoreOptions.overwrite_existing = true;
    return sak::FileRecoveryEngine::restoreCandidates(restoreOptions);
}

RestoreArtifacts restoredArtifacts(const sak::FileRecoveryRestoreResult& restore) {
    RestoreArtifacts artifacts;
    if (!restore.restored_paths.isEmpty()) {
        artifacts.path = restore.restored_paths.first();
        artifacts.sha256 = fileSha256Hex(artifacts.path);
    }
    return artifacts;
}

CertificationRun certificationRun(const CertifierConfig& config,
                                  const CertifierPaths& paths,
                                  const FixtureContent& fixture,
                                  const sak::FileRecoveryScanResult& scan,
                                  const sak::FileRecoveryCandidate& match) {
    CertificationRun run;
    run.scan = scan;
    run.match = match;
    run.fixture = fixture;
    run.restore = restoreMatch(config, paths, match);
    run.restored = restoredArtifacts(run.restore);
    return run;
}

void addRestoreRunFields(QJsonObject* run,
                         const CertifierPaths& paths,
                         const CertificationRun& certification) {
    run->insert(QStringLiteral("matched_candidate_id"), certification.match.id);
    run->insert(QStringLiteral("matched_offset_bytes"),
                QString::number(certification.match.offset_bytes));
    run->insert(QStringLiteral("matched_size_bytes"),
                QString::number(certification.match.size_bytes));
    run->insert(QStringLiteral("restore_destination"), paths.restoreDir);
    run->insert(QStringLiteral("restored_path"), certification.restored.path);
    run->insert(QStringLiteral("restored_sha256"), certification.restored.sha256);
    run->insert(QStringLiteral("restore_source_opened_read_only"),
                certification.restore.source_opened_read_only);
    run->insert(QStringLiteral("source_not_mutated"), certification.restore.source_not_mutated);
    run->insert(QStringLiteral("restore_warnings"), stringArray(certification.restore.warnings));
}

bool restorePassed(const CertificationRun& certification) {
    return certification.restore.source_opened_read_only &&
           certification.restore.source_not_mutated && !certification.restored.path.isEmpty() &&
           certification.restored.sha256 == certification.fixture.sha256;
}

QString passedScanSummary(const CertifierPaths& paths, const CertificationRun& certification) {
    return QStringLiteral(
               "Recovered matching deleted PDF fixture %1 from raw source %2 at offset %3 "
               "after %4 candidate(s).")
        .arg(certification.match.id,
             paths.sourcePath,
             QString::number(certification.match.offset_bytes),
             QString::number(certification.scan.candidates.size()));
}

QJsonObject passedEvidence(const CertifierConfig& config,
                           const CertifierPaths& paths,
                           const CertificationRun& certification) {
    return evidenceObject(config,
                          paths,
                          passedScanSummary(paths, certification),
                          certification.restored.sha256,
                          certification.restore.source_not_mutated ? QStringLiteral("true")
                                                                   : QStringLiteral("false"));
}

int failRestoreVerification(const CertifierConfig& config,
                            const CertifierPaths& paths,
                            const CertificationRun& certification) {
    return failWithReport(config.outputDir,
                          QStringLiteral("Restore/hash/source mutation verification failed."),
                          config.manifestPath,
                          config.suggestedEvidencePath,
                          passedEvidence(config, paths, certification));
}

QString passedSummary(const CertifierConfig& config, const CertificationRun& certification) {
    return QStringLiteral(
               "Disposable %1 volume %2 created and deleted fixture %3, "
               "scanned raw source read-only with FileRecoveryEngine, restored "
               "matching candidate to a separate destination, verified SHA256 "
               "%4, and verified the scanned source range was unchanged.")
        .arg(config.fileSystem, config.volumeId, config.fixtureName, certification.restored.sha256);
}

int writePassedReport(const CertifierConfig& config,
                      const CertifierPaths& paths,
                      const CertificationRun& certification) {
    const QStringList artifacts{projectRelativePath(paths.runPath),
                                projectRelativePath(certification.restored.path)};
    const auto report = reportBase(
        ReportFields{config.manifestPath,
                     config.suggestedEvidencePath,
                     QStringLiteral("Passed"),
                     passedEvidence(config, paths, certification),
                     artifacts,
                     passedSummary(config, certification),
                     QStringLiteral("Run from VM/lab admin context against disposable data volume; "
                                    "restore destination stayed off the source volume.")});
    if (!writeJsonFile(paths.reportPath, report)) {
        QTextStream(stderr) << "Could not write report: " << paths.reportPath << Qt::endl;
        return 3;
    }

    QTextStream(stdout) << "File recovery certification passed: " << paths.reportPath << Qt::endl;
    return 0;
}

int runCertification(const CertifierConfig& config) {
    if (const auto failure = validateConfig(config)) {
        return *failure;
    }

    const CertifierPaths paths = pathsFromConfig(config);
    FixtureContent fixture;
    if (const auto failure = prepareFixture(config, paths, &fixture)) {
        return *failure;
    }

    const auto scan = scanRawSource(config, paths);
    const auto match = matchingCandidate(scan, fixture.sha256);
    QJsonObject run = scanRunObject(config, paths, fixture, scan);
    if (!scan.source_opened_read_only || !match) {
        return failNoMatchingFixture(config, paths, run, scan.candidates.size());
    }

    const auto certification = certificationRun(config, paths, fixture, scan, *match);
    addRestoreRunFields(&run, paths, certification);
    writeJsonFile(paths.runPath, run);
    if (!restorePassed(certification)) {
        return failRestoreVerification(config, paths, certification);
    }
    return writePassedReport(config, paths, certification);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    return runCertification(configFromArgs(app.arguments()));
}
