// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_conversation_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace sak::ai {

namespace {

constexpr auto kManifestFile = "manifest.json";
constexpr auto kTranscriptFile = "transcript.jsonl";
constexpr auto kCommandsFile = "commands.jsonl";
constexpr auto kContextFile = "context.jsonl";
constexpr auto kUsageFile = "usage.json";
constexpr auto kMemoryFile = "memory.md";
constexpr qint64 kMaxMemoryBytes = 256LL * 1024LL;
constexpr qint64 kTrimmedMemoryBytes = 192LL * 1024LL;

QString nowIso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QDateTime parseDate(const QJsonObject& obj, const QString& key) {
    return QDateTime::fromString(obj.value(key).toString(), Qt::ISODateWithMs);
}

QString fallbackTitle(const QString& title) {
    const QString trimmed = title.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed.left(80);
    }
    return QStringLiteral("AI Session");
}

QString oneLine(const QString& value) {
    return value.simplified().left(160);
}

QString memoryHeader() {
    return QStringLiteral(
        "# Session Working Memory\n\n"
        "## Pinned Facts\n"
        "- _none_\n\n"
        "## Current Task\n"
        "- _none_\n\n"
        "## Decisions\n"
        "- _none_\n\n"
        "## Open Questions\n"
        "- _none_\n\n"
        "## Artifacts\n"
        "- _none_\n\n"
        "## Resolved History\n\n");
}

QStringList memorySectionNames() {
    return {
        QStringLiteral("Pinned Facts"),
        QStringLiteral("Current Task"),
        QStringLiteral("Decisions"),
        QStringLiteral("Open Questions"),
        QStringLiteral("Artifacts"),
        QStringLiteral("Resolved History"),
    };
}

qsizetype memorySectionStart(const QString& text, const QString& section) {
    return text.indexOf(QStringLiteral("## %1").arg(section));
}

qsizetype nextMemorySectionStart(const QString& text, qsizetype from) {
    qsizetype next = -1;
    for (const auto& section : memorySectionNames()) {
        const qsizetype candidate = text.indexOf(QStringLiteral("## %1").arg(section), from);
        if (candidate >= 0 && (next < 0 || candidate < next)) {
            next = candidate;
        }
    }
    return next;
}

QString memorySectionBody(const QString& text, const QString& section) {
    const qsizetype section_start = memorySectionStart(text, section);
    if (section_start < 0) {
        return QStringLiteral("- _none_");
    }
    qsizetype body_start = text.indexOf(QLatin1Char('\n'), section_start);
    if (body_start < 0) {
        return QStringLiteral("- _none_");
    }
    ++body_start;
    const qsizetype body_end = nextMemorySectionStart(text, body_start);
    const QString body = (body_end < 0 ? text.mid(body_start)
                                       : text.mid(body_start, body_end - body_start))
                             .trimmed();
    return body.isEmpty() ? QStringLiteral("- _none_") : body;
}

QString boundedMemorySection(const QString& body, qsizetype max_chars) {
    if (body.size() <= max_chars) {
        return body;
    }
    return QStringLiteral("[older section content compacted by SAK]\n\n%1")
        .arg(body.right(max_chars));
}

QString compactedMemoryText(const QString& existing) {
    constexpr qsizetype kPreservedSectionChars = 8 * 1024;
    QString out = QStringLiteral("# Session Working Memory\n\n");
    for (const auto& section : memorySectionNames()) {
        out += QStringLiteral("## %1\n").arg(section);
        QString body = memorySectionBody(existing, section);
        if (section == QLatin1String("Resolved History")) {
            body = boundedMemorySection(body, static_cast<qsizetype>(kTrimmedMemoryBytes));
            if (!body.startsWith(QStringLiteral("[older section content compacted by SAK]"))) {
                body.prepend(QStringLiteral("[older resolved history compacted by SAK]\n\n"));
            }
        } else {
            body = boundedMemorySection(body, kPreservedSectionChars);
        }
        out += body.trimmed();
        out += QStringLiteral("\n\n");
    }
    return out;
}

QString uniqueDestinationPath(const QString& path) {
    QFileInfo info(path);
    if (!info.exists()) {
        return path;
    }

    const QString base = info.completeBaseName().isEmpty() ? info.fileName()
                                                           : info.completeBaseName();
    const QString suffix = info.suffix();
    const QString dir = info.absolutePath();
    for (int i = 1; i < 10'000; ++i) {
        const QString name = suffix.isEmpty()
                                 ? QStringLiteral("%1_%2").arg(base).arg(i)
                                 : QStringLiteral("%1_%2.%3").arg(base).arg(i).arg(suffix);
        const QString candidate = QDir(dir).filePath(name);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return path;
}

QString portableDataRoot() {
    QString app_dir = QCoreApplication::applicationDirPath();
    if (app_dir.trimmed().isEmpty()) {
        app_dir = QDir::currentPath();
    }
    return QDir(app_dir).filePath(QStringLiteral("data"));
}

void setError(QString* error_message, const QString& message) {
    if (error_message) {
        *error_message = message;
    }
}

bool ensureDirectory(const QString& path, QString* error_message, const QString& failure_prefix) {
    if (QDir().mkpath(path)) {
        return true;
    }
    setError(error_message, QStringLiteral("%1: %2").arg(failure_prefix, path));
    return false;
}

bool moveFileWithCopyFallback(const QString& source,
                              const QString& destination,
                              QString* error_message) {
    const QString final_destination = uniqueDestinationPath(destination);
    if (QFile::rename(source, final_destination)) {
        return true;
    }
    if (QFile::copy(source, final_destination) && QFile::remove(source)) {
        return true;
    }
    setError(error_message,
             QStringLiteral("Could not move artifact '%1' to '%2'").arg(source, final_destination));
    return false;
}

bool mergeDirectoryEntry(const QDir& source,
                         const QString& current,
                         const QString& destination_path,
                         QString* error_message) {
    const QFileInfo info(current);
    const QString relative = source.relativeFilePath(current);
    const QString destination = QDir(destination_path).filePath(relative);
    if (info.isDir()) {
        return ensureDirectory(destination,
                               error_message,
                               QStringLiteral("Could not create artifact subdirectory"));
    }
    const QString parent = QFileInfo(destination).absolutePath();
    if (!ensureDirectory(
            parent, error_message, QStringLiteral("Could not create artifact subdirectory"))) {
        return false;
    }
    return moveFileWithCopyFallback(current, destination, error_message);
}

bool mergeDirectoryContents(const QString& source_path,
                            const QString& destination_path,
                            QString* error_message) {
    QDir source(source_path);
    if (!source.exists()) {
        return true;
    }
    if (!ensureDirectory(destination_path,
                         error_message,
                         QStringLiteral("Could not create artifact destination"))) {
        return false;
    }

    QDirIterator iter(source_path,
                      QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
    while (iter.hasNext()) {
        if (!mergeDirectoryEntry(source, iter.next(), destination_path, error_message)) {
            return false;
        }
    }

    QDir(source_path).removeRecursively();
    return true;
}

bool writeMemoryFile(const QString& path,
                     const QByteArray& content,
                     const QString& action,
                     QString* error_message) {
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(error_message,
                 QStringLiteral("Could not %1 memory: %2").arg(action, out.errorString()));
        return false;
    }
    out.write(content);
    if (!out.commit()) {
        setError(error_message,
                 QStringLiteral("Could not commit %1 memory: %2").arg(action, out.errorString()));
        return false;
    }
    return true;
}

bool createInitialMemoryFile(const QString& path, const QString& header, QString* error_message) {
    const QFileInfo info(path);
    if (!ensureDirectory(info.absolutePath(),
                         error_message,
                         QStringLiteral("Could not create memory directory"))) {
        return false;
    }
    return writeMemoryFile(path, header.toUtf8(), QStringLiteral("initialize"), error_message);
}

std::optional<QString> readMemoryFile(const QString& path, QString* error_message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error_message,
                 QStringLiteral("Could not read memory: %1").arg(file.errorString()));
        return std::nullopt;
    }
    return QString::fromUtf8(file.readAll());
}

bool memoryHasRequiredSections(const QString& existing) {
    return existing.contains(QStringLiteral("## Pinned Facts")) &&
           existing.contains(QStringLiteral("## Resolved History"));
}

QByteArray normalizedMemoryContent(const QString& header, const QString& existing) {
    QByteArray content = header.toUtf8();
    const QString trimmed = existing.trimmed();
    if (!trimmed.isEmpty()) {
        content += "[previous memory content]\n\n";
        content += trimmed.toUtf8();
        content += "\n\n";
    }
    return content;
}

bool ensureMemoryFileInitialized(const QString& path, QString* error_message) {
    QFileInfo info(path);
    const QString header = memoryHeader();
    if (!info.exists() || info.size() == 0) {
        return createInitialMemoryFile(path, header, error_message);
    }

    const auto existing = readMemoryFile(path, error_message);
    if (!existing.has_value()) {
        return false;
    }
    if (memoryHasRequiredSections(*existing)) {
        return true;
    }

    return writeMemoryFile(path,
                           normalizedMemoryContent(header, *existing),
                           QStringLiteral("normalize"),
                           error_message);
}

bool trimMemoryFile(const QString& path, QString* error_message) {
    QFileInfo info(path);
    if (!info.exists() || info.size() <= kMaxMemoryBytes) {
        return true;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not trim memory: %1").arg(file.errorString());
        }
        return false;
    }
    const QString existing = QString::fromUtf8(file.readAll());
    file.close();
    const QString compacted = compactedMemoryText(existing);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not trim memory: %1").arg(out.errorString());
        }
        return false;
    }
    out.write(compacted.toUtf8());
    if (!out.commit()) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not commit trimmed memory: %1").arg(out.errorString());
        }
        return false;
    }
    return true;
}

QString transcriptHeading(const QString& role) {
    const QString normalized = role.trimmed().toLower();
    if (normalized == QLatin1String("you") || normalized == QLatin1String("user")) {
        return QStringLiteral("USER REQUEST");
    }
    if (normalized == QLatin1String("assistant")) {
        return QStringLiteral("ASSISTANT RESULT");
    }
    const std::vector<std::pair<const char*, const char*>> rules{
        {"workflow", "WORKFLOW RESULT"},
        {"tool", "TOOL RESULT"},
        {"error", "ERROR"},
        {"system", "SYSTEM"},
        {"queued", "QUEUED REQUEST"},
        {"steering", "RUN STEERING"},
    };
    for (const auto& [needle, heading] : rules) {
        if (normalized.contains(QString::fromLatin1(needle))) {
            return QString::fromLatin1(heading);
        }
    }
    return role.trimmed().toUpper();
}

QString transcriptSourcesText(const QJsonArray& citations) {
    if (citations.isEmpty()) {
        return {};
    }
    QStringList source_lines;
    source_lines << QStringLiteral("Sources:");
    for (int i = 0; i < citations.size(); ++i) {
        const QJsonObject citation = citations.at(i).toObject();
        const QString url = citation.value(QStringLiteral("url")).toString();
        const QString title = citation.value(QStringLiteral("title")).toString(url);
        if (!url.isEmpty()) {
            source_lines << QStringLiteral("  [%1] %2 - %3").arg(i + 1).arg(title, url);
        }
    }
    return source_lines.size() > 1 ? source_lines.join(QLatin1Char('\n')) : QString{};
}

std::optional<QString> transcriptDisplayLine(const QByteArray& raw_line) {
    QJsonParseError parse_error;
    const auto doc = QJsonDocument::fromJson(raw_line.trimmed(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    const auto obj = doc.object();
    const QString role = obj.value(QStringLiteral("role")).toString();
    QString text = obj.value(QStringLiteral("text")).toString().trimmed();
    const QString sources = transcriptSourcesText(obj.value(QStringLiteral("metadata"))
                                                      .toObject()
                                                      .value(QStringLiteral("citations"))
                                                      .toArray());
    if (!sources.isEmpty()) {
        text += QStringLiteral("\n\n") + sources;
    }
    if (role.isEmpty() || text.isEmpty()) {
        return std::nullopt;
    }
    return QStringLiteral("\n[%1]\n%2").arg(transcriptHeading(role), text);
}

}  // namespace

ConversationStore::ConversationStore(QString root_dir)
    : m_root_dir(root_dir.trimmed().isEmpty() ? defaultRootDirectory() : std::move(root_dir)) {}

QString ConversationStore::rootDirectory() const {
    return m_root_dir;
}

QString ConversationStore::currentSessionId() const {
    return m_current_session.id;
}

AiSessionInfo ConversationStore::currentSessionInfo() const {
    return m_current_session;
}

QString ConversationStore::defaultRootDirectory() {
    return QDir(portableDataRoot()).filePath(QStringLiteral("ai_sessions"));
}

bool ConversationStore::ensureRoot(QString* error_message) const {
    QDir dir(m_root_dir);
    if (dir.exists()) {
        return true;
    }
    if (dir.mkpath(QStringLiteral("."))) {
        return true;
    }
    if (error_message) {
        *error_message = QStringLiteral("Could not create AI session root: %1").arg(m_root_dir);
    }
    return false;
}

QVector<AiSessionInfo> ConversationStore::listSessions() const {
    QVector<AiSessionInfo> sessions;
    const QDir root(m_root_dir);
    const auto entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                            QDir::Time | QDir::Reversed);
    sessions.reserve(entries.size());
    for (const auto& entry : entries) {
        auto info = readManifest(entry.absoluteFilePath());
        if (!info.id.isEmpty()) {
            sessions.append(info);
        }
    }
    std::sort(sessions.begin(), sessions.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.updated_at > rhs.updated_at;
    });
    return sessions;
}

QStringList ConversationStore::loadTranscriptLines(const QString& session_id,
                                                   QString* error_message) const {
    QStringList lines;
    QFile file(QDir(sessionPath(session_id)).filePath(QString::fromLatin1(kTranscriptFile)));
    if (!file.exists()) {
        return lines;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not read transcript: %1").arg(file.errorString());
        }
        return lines;
    }

    while (!file.atEnd()) {
        const auto line = transcriptDisplayLine(file.readLine());
        if (line.has_value()) {
            lines.append(*line);
        }
    }
    return lines;
}

bool ConversationStore::startSession(const QString& title, QString* error_message) {
    if (!ensureRoot(error_message)) {
        return false;
    }

    const QString id =
        QStringLiteral("ai_%1_%2")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss")),
                 QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    const QString path = sessionPath(id);
    if (!QDir().mkpath(path)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not create AI session: %1").arg(path);
        }
        return false;
    }

    m_current_session.id = id;
    m_current_session.title = fallbackTitle(title);
    m_current_session.path = path;
    m_current_session.created_at = QDateTime::currentDateTimeUtc();
    m_current_session.updated_at = m_current_session.created_at;
    return writeManifest(error_message);
}

bool ConversationStore::openSession(const QString& session_id, QString* error_message) {
    const QString path = sessionPath(session_id);
    if (!QFileInfo::exists(QDir(path).filePath(QString::fromLatin1(kManifestFile)))) {
        if (error_message) {
            *error_message = QStringLiteral("AI session not found: %1").arg(session_id);
        }
        return false;
    }

    auto info = readManifest(path);
    if (info.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("AI session manifest invalid: %1").arg(session_id);
        }
        return false;
    }
    m_current_session = info;
    return true;
}

bool ConversationStore::renameCurrentSession(const QString& title, QString* error_message) {
    const QString trimmed = fallbackTitle(title);
    if (m_current_session.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session");
        }
        return false;
    }
    const QString old_artifact_name = safeArtifactDirectoryName(m_current_session.title,
                                                                m_current_session.id);
    const QString new_artifact_name = safeArtifactDirectoryName(trimmed, m_current_session.id);
    const QString artifacts_root = QDir(currentSessionPath()).filePath(QStringLiteral("artifacts"));
    if (old_artifact_name != new_artifact_name && QDir(artifacts_root).exists()) {
        QDir root_dir(artifacts_root);
        const QString old_path = root_dir.filePath(old_artifact_name);
        const QString new_path = root_dir.filePath(new_artifact_name);
        if (QDir(old_path).exists()) {
            if (!QDir(new_path).exists()) {
                if (!root_dir.rename(old_artifact_name, new_artifact_name)) {
                    if (!mergeDirectoryContents(old_path, new_path, error_message)) {
                        return false;
                    }
                }
            } else if (!mergeDirectoryContents(old_path, new_path, error_message)) {
                return false;
            }
        }
    }
    m_current_session.title = trimmed;
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

bool ConversationStore::appendTranscript(const QString& role,
                                         const QString& text,
                                         const QJsonObject& metadata,
                                         QString* error_message) {
    QJsonObject obj;
    obj[QStringLiteral("role")] = role;
    obj[QStringLiteral("text")] = text;
    obj[QStringLiteral("metadata")] = metadata;
    if (!appendJsonLine(QString::fromLatin1(kTranscriptFile), obj, error_message)) {
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

bool ConversationStore::appendCommand(const QString& command,
                                      const QJsonObject& result,
                                      QString* error_message) {
    QJsonObject obj;
    obj[QStringLiteral("command")] = command;
    obj[QStringLiteral("result")] = result;
    if (!appendJsonLine(QString::fromLatin1(kCommandsFile), obj, error_message)) {
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

QString ConversationStore::commandLogPath(const QString& command_id,
                                          const QString& suffix,
                                          QString* error_message) const {
    if (m_current_session.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session");
        }
        return {};
    }
    const QString safe_id = command_id.trimmed().isEmpty() ? QStringLiteral("cmd")
                                                           : command_id.trimmed();
    const QString safe_suffix = suffix.trimmed().isEmpty() ? QStringLiteral("output")
                                                           : suffix.trimmed();
    const QString artifact_root = artifactRootDirectory(error_message);
    if (artifact_root.isEmpty()) {
        return {};
    }
    const QString logs_dir = QDir(artifact_root).filePath(QStringLiteral("logs"));
    QDir dir(logs_dir);
    if (!dir.exists() && !QDir().mkpath(logs_dir)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not create artifacts directory: %1").arg(logs_dir);
        }
        return {};
    }
    return dir.filePath(QStringLiteral("%1_%2.txt").arg(safe_id, safe_suffix));
}

QString ConversationStore::artifactSubdir(const QString& subdir, QString* error_message) const {
    const QString trimmed_subdir = subdir.trimmed();
    if (trimmed_subdir.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Artifact subdir is empty");
        }
        return {};
    }
    const QString artifact_root = artifactRootDirectory(error_message);
    if (artifact_root.isEmpty()) {
        return {};
    }
    const QString path = QDir(artifact_root).filePath(trimmed_subdir);
    if (!QDir().mkpath(path)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not create artifact directory: %1").arg(path);
        }
        return {};
    }
    return path;
}

QString ConversationStore::artifactRootDirectory(QString* error_message) const {
    if (m_current_session.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session");
        }
        return {};
    }
    const QString name = safeArtifactDirectoryName(m_current_session.title, m_current_session.id);
    const QString artifacts_root = QDir(currentSessionPath()).filePath(QStringLiteral("artifacts"));
    const QString path = QDir(artifacts_root).filePath(name);
    if (!QDir().mkpath(path)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not create artifact directory: %1").arg(path);
        }
        return {};
    }
    return path;
}

QString ConversationStore::artifactPath(const QString& subdir,
                                        const QString& filename,
                                        QString* error_message) const {
    const QString dir = artifactSubdir(subdir, error_message);
    if (dir.isEmpty()) {
        return {};
    }
    return QDir(dir).filePath(filename);
}

bool ConversationStore::appendContext(const QString& kind,
                                      const QString& label,
                                      const QString& path,
                                      qint64 size_bytes,
                                      QString* error_message) {
    QJsonObject obj;
    obj[QStringLiteral("kind")] = kind;
    obj[QStringLiteral("label")] = label;
    obj[QStringLiteral("path")] = path;
    obj[QStringLiteral("size_bytes")] = static_cast<double>(size_bytes);
    if (!appendJsonLine(QString::fromLatin1(kContextFile), obj, error_message)) {
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

QString ConversationStore::memoryText(int max_chars, QString* error_message) const {
    if (m_current_session.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session");
        }
        return {};
    }
    QFile file(QDir(currentSessionPath()).filePath(QString::fromLatin1(kMemoryFile)));
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not read memory: %1").arg(file.errorString());
        }
        return {};
    }
    QString text = QString::fromUtf8(file.readAll());
    if (max_chars > 0 && text.size() > max_chars) {
        text = text.right(max_chars);
        text.prepend(QStringLiteral("[older working memory omitted]\n\n"));
    }
    return text.trimmed();
}

bool ConversationStore::appendMemoryEntry(const QString& kind,
                                          const QString& title,
                                          const QString& text,
                                          QString* error_message) {
    if (m_current_session.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session");
        }
        return false;
    }
    const QString body = text.trimmed();
    if (body.isEmpty()) {
        return true;
    }
    const QString memory_path =
        QDir(currentSessionPath()).filePath(QString::fromLatin1(kMemoryFile));
    if (!ensureMemoryFileInitialized(memory_path, error_message)) {
        return false;
    }
    QFile file(memory_path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not append memory: %1").arg(file.errorString());
        }
        return false;
    }
    const QString heading =
        QStringLiteral("## %1 - %2 - %3\n").arg(nowIso(), oneLine(kind), oneLine(title));
    file.write(heading.toUtf8());
    file.write(body.left(4000).toUtf8());
    file.write("\n\n");
    file.close();
    if (!trimMemoryFile(file.fileName(), error_message)) {
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

bool ConversationStore::writeUsage(const TokenUsage& turn,
                                   const TokenUsage& session_total,
                                   QString* error_message) {
    if (m_current_session.id.isEmpty() && !startSession({}, error_message)) {
        return false;
    }

    QJsonObject root;
    root[QStringLiteral("updated_at")] = nowIso();
    root[QStringLiteral("last_turn")] = usageToJson(turn);
    root[QStringLiteral("session_total")] = usageToJson(session_total);

    QSaveFile file(QDir(currentSessionPath()).filePath(QString::fromLatin1(kUsageFile)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not write usage: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error_message) {
            *error_message = QStringLiteral("Could not commit usage: %1").arg(file.errorString());
        }
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

QString ConversationStore::sessionPath(const QString& session_id) const {
    return QDir(m_root_dir).filePath(session_id);
}

QString ConversationStore::currentSessionPath() const {
    return sessionPath(m_current_session.id);
}

bool ConversationStore::writeManifest(QString* error_message) const {
    if (m_current_session.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session");
        }
        return false;
    }

    QJsonObject root;
    root[QStringLiteral("id")] = m_current_session.id;
    root[QStringLiteral("title")] = m_current_session.title;
    root[QStringLiteral("created_at")] = m_current_session.created_at.toString(Qt::ISODateWithMs);
    root[QStringLiteral("updated_at")] = m_current_session.updated_at.toString(Qt::ISODateWithMs);

    QSaveFile file(QDir(currentSessionPath()).filePath(QString::fromLatin1(kManifestFile)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not write manifest: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not commit manifest: %1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

bool ConversationStore::appendJsonLine(const QString& filename,
                                       QJsonObject object,
                                       QString* error_message) const {
    if (m_current_session.id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session");
        }
        return false;
    }

    object[QStringLiteral("timestamp")] = nowIso();
    QFile file(QDir(currentSessionPath()).filePath(filename));
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not append %1: %2").arg(filename, file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.write("\n");
    return true;
}

AiSessionInfo ConversationStore::readManifest(const QString& session_path) {
    AiSessionInfo info;
    QFile file(QDir(session_path).filePath(QString::fromLatin1(kManifestFile)));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return info;
    }

    QJsonParseError parse_error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return info;
    }

    const auto obj = doc.object();
    info.id = obj.value(QStringLiteral("id")).toString();
    info.title = obj.value(QStringLiteral("title")).toString();
    info.path = session_path;
    info.created_at = parseDate(obj, QStringLiteral("created_at"));
    info.updated_at = parseDate(obj, QStringLiteral("updated_at"));
    return info;
}

QJsonObject ConversationStore::usageToJson(const TokenUsage& usage) {
    QJsonObject obj;
    obj[QStringLiteral("input_tokens")] = static_cast<double>(usage.input_tokens);
    obj[QStringLiteral("cached_input_tokens")] = static_cast<double>(usage.cached_input_tokens);
    obj[QStringLiteral("output_tokens")] = static_cast<double>(usage.output_tokens);
    obj[QStringLiteral("reasoning_tokens")] = static_cast<double>(usage.reasoning_tokens);
    obj[QStringLiteral("total_tokens")] = static_cast<double>(usage.total_tokens);
    return obj;
}

QString ConversationStore::safeArtifactDirectoryName(const QString& title,
                                                     const QString& fallback_id) {
    QString safe = title.simplified();
    if (safe.isEmpty()) {
        safe = fallback_id.trimmed();
    }
    safe.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")),
                 QStringLiteral("_"));
    safe.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    safe = safe.trimmed().left(80);
    if (safe.isEmpty()) {
        safe = QStringLiteral("AI Session");
    }
    return safe;
}

}  // namespace sak::ai
