// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_conversation_store.h"

#include "sak/ai/ai_credential_store.h"
#include "sak/ai/ai_paths.h"
#include "sak/ai/ai_secret_redaction.h"
#include "sak/windows_reserved_names.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QReadWriteLock>
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
constexpr int kSessionArtifactSchemaVersion = 1;
constexpr qint64 kMaxMemoryBytes = 256LL * 1024LL;
constexpr qint64 kMaxMemoryRecoveryReadBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 kTrimmedMemoryBytes = 192LL * 1024LL;
constexpr qint64 kMaxSessionJsonlLineBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 kMaxTranscriptJsonlBytes = 32LL * 1024LL * 1024LL;
constexpr qint64 kMaxCommandsJsonlBytes = 32LL * 1024LL * 1024LL;
constexpr qint64 kMaxContextJsonlBytes = 8LL * 1024LL * 1024LL;
constexpr qint64 kMaxSearchAggregateBytes = 128LL * 1024LL * 1024LL;
constexpr int kMaxSearchSessionsScanned = 250;
constexpr int kMaxSessionSearchResults = 100;
constexpr qint64 kMaxManifestBytes = 64LL * 1024LL;
constexpr int kMaxSessionDirectories = 512;
constexpr qsizetype kFallbackTitleMaxChars = 80;
constexpr qsizetype kOneLineMaxChars = 160;
constexpr qsizetype kDefaultSearchSnippetMaxChars = 220;
constexpr qsizetype kSessionUuidSuffixChars = 8;
constexpr qsizetype kMemoryAppendMaxChars = 4000;
constexpr qsizetype kSafeFilenameMaxChars = 80;

std::function<void(const QString&)> g_before_manifest_child_open_hook;
std::function<void(const QString&)> g_before_manifest_file_open_hook;
std::function<void(const QString&)> g_before_manifest_file_read_hook;
std::function<void(const QString&)> g_before_artifact_child_open_hook;

QString nowIso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QDateTime parseDate(const QJsonObject& obj, const QString& key) {
    return QDateTime::fromString(obj.value(key).toString(), Qt::ISODateWithMs);
}

QString fallbackTitle(const QString& title) {
    const QString trimmed = title.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed.left(kFallbackTitleMaxChars);
    }
    return QStringLiteral("AI Session");
}

QString oneLine(const QString& value) {
    return value.simplified().left(kOneLineMaxChars);
}

// Confine an id/suffix used to build a log filename to a single safe path segment:
// strip separators/reserved characters and reject pure dot-segments (".", "..") that
// would traverse out of the logs directory. Falls back to @p fallback when nothing
// usable remains.
QString sanitizeLogToken(const QString& value, const QString& fallback) {
    QString safe = value.trimmed();
    safe.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")),
                 QStringLiteral("_"));
    // Collapse any run of 2+ dots so no ".." traversal sequence survives inside a segment
    // (a lone extension dot is fine); a bare ".."/"..." then reduces to "_".
    safe.replace(QRegularExpression(QStringLiteral(R"(\.{2,})")), QStringLiteral("_"));
    const bool all_dots = !safe.isEmpty() && std::ranges::all_of(safe, [](QChar ch) {
        return ch == QLatin1Char('.');
    });
    if (safe.isEmpty() || all_dots) {
        return fallback;
    }
    return safe;
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

// Keep the TAIL of a section within a budget expressed in UTF-8 BYTES.
//
// The budget has to be bytes because that is what it is checked against: trimMemory compares
// QFileInfo::size() -- bytes on disk -- to kMaxMemoryBytes, then spends kTrimmedMemoryBytes here.
// Spending it as a QString character count made the two units disagree: QString counts UTF-16
// code units, so 192 KiB of non-ASCII characters can serialize to well over 500 KiB of UTF-8 and
// land ABOVE the 256 KiB ceiling the trim exists to enforce. The trim then reported success while
// the file stayed over the cap, and every later call trimmed it to the same oversized result --
// a cap that never converges.
//
// Truncation walks back from the cut to a character boundary, so it can never split a surrogate
// pair into a lone half (which encodes as U+FFFD and silently corrupts the tail).
QString boundedMemorySection(const QString& body, qint64 max_bytes) {
    // A UTF-8 continuation byte matches the bit pattern 10xxxxxx: mask off the low six payload
    // bits and what remains is exactly the marker. Every byte of a multi-byte character except
    // the first is one of these, so this is how a cut lands on a character boundary.
    constexpr unsigned char kUtf8ContinuationMask = 0xC0;
    constexpr unsigned char kUtf8ContinuationMarker = 0x80;
    QByteArray encoded = body.toUtf8();
    if (encoded.size() <= max_bytes) {
        return body;
    }
    qsizetype cut = static_cast<qsizetype>(encoded.size() - max_bytes);
    // Advance to the start of a UTF-8 sequence: continuation bytes are 10xxxxxx.
    while (cut < encoded.size() && (static_cast<unsigned char>(encoded.at(cut)) &
                                    kUtf8ContinuationMask) == kUtf8ContinuationMarker) {
        ++cut;
    }
    return QStringLiteral("[older section content compacted by SAK]\n\n%1")
        .arg(QString::fromUtf8(encoded.mid(cut)));
}

// Append one already-assembled memory record, VERIFYING that it actually reached the file.
//
// The three writes this replaces discarded every return value, and the close() alongside them, so
// a short write or a close-time flush failure (disk full, quota, a device pulled mid-write)
// truncated the entry in place while appendMemoryEntry still returned true -- the caller recorded
// "memory saved" for a memory that was not. Writing the record as a SINGLE buffer also means a
// failure cannot leave a half-written heading with no body under it.
bool appendMemoryRecord(const QString& path, const QByteArray& record, QString* error_message) {
    QFile file(path);
    if (!file.open(QIODevice::Append)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Could not append memory: %1").arg(file.errorString());
        }
        return false;
    }
    const qint64 start_size = file.size();
    const qint64 written = file.write(record);
    const bool flushed = file.flush();
    if (written == record.size() && flushed && file.error() == QFileDevice::NoError) {
        return true;
    }
    const QString write_error = file.errorString();
    const bool restored = start_size >= 0 && file.resize(start_size);
    if (error_message != nullptr) {
        *error_message = QStringLiteral("Could not append memory: wrote %1 of %2 bytes (%3)")
                             .arg(written)
                             .arg(record.size())
                             .arg(write_error);
        if (!restored) {
            error_message->append(QStringLiteral("; partial memory record could not be removed"));
        }
    }
    return false;
}

QString compactedMemoryText(const QString& existing) {
    // BYTES, like every other budget here -- see boundedMemorySection. The five non-history
    // sections take 8 KiB each and Resolved History takes kTrimmedMemoryBytes, so the compacted
    // whole is bounded by 192 + 5*8 = 232 KiB plus headers, safely under the 256 KiB cap that
    // trimMemory checks the file against.
    constexpr qint64 kPreservedSectionBytes = 8 * 1024;
    QString out = QStringLiteral("# Session Working Memory\n\n");
    for (const auto& section : memorySectionNames()) {
        out += QStringLiteral("## %1\n").arg(section);
        QString body = memorySectionBody(existing, section);
        if (section == QLatin1String("Resolved History")) {
            body = boundedMemorySection(body, kTrimmedMemoryBytes);
            if (!body.startsWith(QStringLiteral("[older section content compacted by SAK]"))) {
                body.prepend(QStringLiteral("[older resolved history compacted by SAK]\n\n"));
            }
        } else {
            body = boundedMemorySection(body, kPreservedSectionBytes);
        }
        out += body.trimmed();
        out += QStringLiteral("\n\n");
    }
    return out;
}

void setError(QString* error_message, const QString& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

int boundedSearchResultCount(int requested) {
    return requested > 0 ? std::clamp(requested, 1, kMaxSessionSearchResults)
                         : kMaxSessionSearchResults;
}

std::optional<QStringList> artifactSubdirComponents(const QString& subdir, QString* error_message) {
    const QString normalized = QDir::fromNativeSeparators(subdir.trimmed());
    const QString clean = QDir::cleanPath(normalized);
    const QString refusal =
        QStringLiteral("Artifact subdir escapes the artifact directory: %1").arg(subdir);
    if (QDir::isAbsolutePath(normalized) || clean == QLatin1String("..") ||
        clean.startsWith(QStringLiteral("../"))) {
        setError(error_message, refusal);
        return std::nullopt;
    }
    const QStringList nested = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (nested.isEmpty() || std::ranges::any_of(nested, [](const QString& component) {
            return component == QLatin1String(".") || component == QLatin1String("..") ||
                   component.contains(QLatin1Char(':'));
        })) {
        setError(error_message, refusal);
        return std::nullopt;
    }
    return nested;
}

detail::StoreChildFileLoadPolicy manifestLoadPolicy() {
    return {.store_name = QStringLiteral("AI session"),
            .child_type_name = QStringLiteral("session"),
            .file_name = QString::fromLatin1(kManifestFile),
            .max_file_bytes = kMaxManifestBytes,
            .max_child_directories = kMaxSessionDirectories};
}

detail::StoreFileLoadHooks manifestLoadHooks() {
    return {.before_file_open = g_before_manifest_file_open_hook,
            .before_file_read = g_before_manifest_file_read_hook,
            .before_child_open = g_before_manifest_child_open_hook};
}

std::optional<AiSessionInfo> parseManifestBytes(const QString& directory_name,
                                                const QString& session_path,
                                                const QByteArray& bytes,
                                                QStringList* errors) {
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        errors->append(
            QStringLiteral("AI session manifest is not a valid JSON object: %1").arg(session_path));
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QJsonValue id_value = object.value(QStringLiteral("id"));
    const QJsonValue title_value = object.value(QStringLiteral("title"));
    const QString id = id_value.toString();
    const QString title = title_value.toString().trimmed();
    if (!id_value.isString() || id != directory_name) {
        errors->append(QStringLiteral("AI session manifest id does not match directory '%1'")
                           .arg(directory_name));
        return std::nullopt;
    }
    if (!title_value.isString() || title.isEmpty()) {
        errors->append(
            QStringLiteral("AI session manifest title is missing: %1").arg(session_path));
        return std::nullopt;
    }
    const QDateTime created = parseDate(object, QStringLiteral("created_at"));
    const QDateTime updated = parseDate(object, QStringLiteral("updated_at"));
    if (!created.isValid() || !updated.isValid()) {
        errors->append(
            QStringLiteral("AI session manifest timestamps are invalid: %1").arg(session_path));
        return std::nullopt;
    }
    return AiSessionInfo{.id = id,
                         .title = title,
                         .path = session_path,
                         .created_at = created,
                         .updated_at = updated};
}

bool ensureDirectory(const QString& path, QString* error_message, const QString& failure_prefix) {
    if (QDir().mkpath(path)) {
        return true;
    }
    setError(error_message, QStringLiteral("%1: %2").arg(failure_prefix, path));
    return false;
}

bool writeMemoryFile(const QString& path,
                     const QByteArray& content,
                     const QString& action,
                     QString* error_message) {
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        setError(error_message,
                 QStringLiteral("Could not %1 memory: %2").arg(action, out.errorString()));
        return false;
    }
    if (out.write(content) != content.size()) {
        setError(
            error_message,
            QStringLiteral("Could not %1 memory: short write (%2)").arg(action, out.errorString()));
        out.cancelWriting();
        return false;
    }
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
    const QFileInfo info(path);
    const qint64 size_before = info.size();
    if (size_before < 0 || size_before > kMaxMemoryRecoveryReadBytes) {
        setError(error_message,
                 QStringLiteral("Memory file exceeds the %1-byte recovery read cap: %2")
                     .arg(kMaxMemoryRecoveryReadBytes)
                     .arg(path));
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error_message,
                 QStringLiteral("Could not read memory: %1").arg(file.errorString()));
        return std::nullopt;
    }
    const QByteArray bytes = file.read(kMaxMemoryRecoveryReadBytes + 1);
    const qint64 size_after = file.size();
    if (file.error() != QFileDevice::NoError || size_before != size_after ||
        bytes.size() != size_after) {
        setError(error_message,
                 QStringLiteral("Memory file changed or was incompletely read: %1").arg(path));
        return std::nullopt;
    }
    return QString::fromUtf8(bytes);
}

bool memoryHasRequiredSections(const QString& existing) {
    // Derived from memorySectionNames(), never restated. This used to check two of the six names
    // as string literals, so a memory file carrying only "Pinned Facts" and "Resolved History"
    // was judged fully structured and skipped normalization -- and adding a section to
    // memorySectionNames() would not have been reflected here at all. Same duplicated-knowledge
    // shape as the other drift defects in this campaign: one fact, two copies, one updated.
    return std::ranges::all_of(memorySectionNames(), [&existing](const QString& section) {
        return existing.contains(QStringLiteral("## %1").arg(section));
    });
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
    const QFileInfo info(path);
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
    const QFileInfo info(path);
    if (!info.exists() || info.size() <= kMaxMemoryBytes) {
        return true;
    }

    const auto existing = readMemoryFile(path, error_message);
    if (!existing.has_value()) {
        return false;
    }
    const QString compacted = compactedMemoryText(*existing);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Could not trim memory: %1").arg(out.errorString());
        }
        return false;
    }
    out.write(compacted.toUtf8());
    if (!out.commit()) {
        if (error_message != nullptr) {
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

std::optional<QString> transcriptDisplayLine(const QJsonObject& obj) {
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

bool transcriptLineIsUserPrompt(const QJsonObject& obj) {
    const QString role = obj.value(QStringLiteral("role")).toString().trimmed().toLower();
    const QString text = obj.value(QStringLiteral("text")).toString().trimmed();
    return !text.isEmpty() && (role == QLatin1String("you") || role == QLatin1String("user"));
}

QDateTime objectTimestamp(const QJsonObject& object) {
    return QDateTime::fromString(object.value(QStringLiteral("timestamp")).toString(),
                                 Qt::ISODateWithMs);
}

struct JsonlVisitRequest {
    QString path;
    QString label;
    qint64 file_cap{0};
    QString* error_message{nullptr};
    qint64* aggregate_bytes{nullptr};
};

enum class JsonlVisitOpenResult {
    Missing,
    Ready,
    Failed
};

JsonlVisitOpenResult openJsonlForVisit(const JsonlVisitRequest& request,
                                       QFile* file,
                                       qint64* initial_size) {
    const QFileInfo info(request.path);
    if (!info.exists()) {
        if (request.error_message != nullptr) {
            request.error_message->clear();
        }
        return JsonlVisitOpenResult::Missing;
    }
    *initial_size = info.size();
    if (*initial_size < 0 || *initial_size > request.file_cap) {
        setError(request.error_message,
                 QStringLiteral("%1 exceeds the %2-byte file cap")
                     .arg(request.label)
                     .arg(request.file_cap));
        return JsonlVisitOpenResult::Failed;
    }
    if (request.aggregate_bytes != nullptr &&
        *request.aggregate_bytes > kMaxSearchAggregateBytes - *initial_size) {
        setError(request.error_message,
                 QStringLiteral("Session search input exceeds the %1-byte aggregate cap")
                     .arg(kMaxSearchAggregateBytes));
        return JsonlVisitOpenResult::Failed;
    }
    file->setFileName(request.path);
    if (!file->open(QIODevice::ReadOnly)) {
        setError(request.error_message,
                 QStringLiteral("Could not read %1: %2")
                     .arg(request.label.toLower(), file->errorString()));
        return JsonlVisitOpenResult::Failed;
    }
    return JsonlVisitOpenResult::Ready;
}

QString validateJsonlLine(const QByteArray& raw_line,
                          QFile* file,
                          const QString& label,
                          qint64 line_number) {
    if (file->error() != QFileDevice::NoError) {
        return QStringLiteral("Could not read %1 line %2: %3")
            .arg(label.toLower())
            .arg(line_number)
            .arg(file->errorString());
    }
    if (raw_line.size() > kMaxSessionJsonlLineBytes ||
        (!raw_line.endsWith('\n') && !file->atEnd())) {
        return QStringLiteral("%1 line %2 exceeds the %3-byte line cap")
            .arg(label)
            .arg(line_number)
            .arg(kMaxSessionJsonlLineBytes);
    }
    if (!raw_line.endsWith('\n')) {
        return QStringLiteral("%1 line %2 is truncated (missing newline)")
            .arg(label)
            .arg(line_number);
    }
    return {};
}

std::optional<QJsonObject> parseJsonlObject(const QByteArray& raw_line,
                                            const QString& label,
                                            qint64 line_number,
                                            QString* error_message) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(raw_line.trimmed(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        setError(
            error_message,
            QStringLiteral("%1 line %2 is not a valid JSON object").arg(label).arg(line_number));
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QJsonValue schema = object.value(QStringLiteral("schema_version"));
    if (!schema.isDouble() ||
        schema.toDouble() != static_cast<double>(kSessionArtifactSchemaVersion)) {
        setError(error_message,
                 QStringLiteral("%1 line %2 has unsupported schema_version")
                     .arg(label)
                     .arg(line_number));
        return std::nullopt;
    }
    return object;
}

bool finishJsonlVisit(QFile* file,
                      const JsonlVisitRequest& request,
                      qint64 initial_size,
                      qint64 loaded_bytes) {
    if (file->size() != initial_size || loaded_bytes != initial_size) {
        setError(request.error_message,
                 QStringLiteral("%1 changed while it was being read").arg(request.label));
        return false;
    }
    if (request.aggregate_bytes != nullptr) {
        *request.aggregate_bytes += initial_size;
    }
    if (request.error_message != nullptr) {
        request.error_message->clear();
    }
    return true;
}

template <typename Consumer>
bool visitJsonLineObjects(const JsonlVisitRequest& request, Consumer&& consume) {
    QFile file;
    qint64 initial_size = 0;
    const JsonlVisitOpenResult opened = openJsonlForVisit(request, &file, &initial_size);
    if (opened != JsonlVisitOpenResult::Ready) {
        return opened == JsonlVisitOpenResult::Missing;
    }
    qint64 loaded_bytes = 0;
    qint64 line_number = 0;
    while (!file.atEnd()) {
        ++line_number;
        const QByteArray raw_line = file.readLine(kMaxSessionJsonlLineBytes + 1);
        loaded_bytes += raw_line.size();
        const QString line_error = validateJsonlLine(raw_line, &file, request.label, line_number);
        if (!line_error.isEmpty()) {
            setError(request.error_message, line_error);
            return false;
        }
        const auto object =
            parseJsonlObject(raw_line, request.label, line_number, request.error_message);
        if (!object.has_value()) {
            return false;
        }
        consume(*object);
    }
    return finishJsonlVisit(&file, request, initial_size, loaded_bytes);
}

bool sessionHasUserPrompt(const QString& session_path) {
    bool found = false;
    QString error;
    const QString path = QDir(session_path).filePath(QString::fromLatin1(kTranscriptFile));
    const bool loaded = visitJsonLineObjects({.path = path,
                                              .label = QStringLiteral("Transcript"),
                                              .file_cap = kMaxTranscriptJsonlBytes,
                                              .error_message = &error,
                                              .aggregate_bytes = nullptr},
                                             [&found](const QJsonObject& object) {
                                                 found = found ||
                                                         transcriptLineIsUserPrompt(object);
                                             });
    return loaded && found;
}

QString snippetAround(const QString& text,
                      const QString& query,
                      qsizetype max_chars = kDefaultSearchSnippetMaxChars) {
    const QString simplified = text.simplified();
    const qsizetype pos = simplified.indexOf(query, 0, Qt::CaseInsensitive);
    if (pos < 0 || simplified.size() <= max_chars) {
        return simplified.left(max_chars);
    }
    const qsizetype start = std::max<qsizetype>(0, pos - (max_chars / 3));
    QString snippet = simplified.mid(start, max_chars);
    if (start > 0) {
        snippet.prepend(QStringLiteral("..."));
    }
    if (start + max_chars < simplified.size()) {
        snippet.append(QStringLiteral("..."));
    }
    return snippet;
}

int matchScore(const QString& haystack, const QString& query) {
    if (query.isEmpty()) {
        return 0;
    }
    int score = 0;
    qsizetype offset = 0;
    while ((offset = haystack.indexOf(query, offset, Qt::CaseInsensitive)) >= 0) {
        ++score;
        offset += query.size();
    }
    return score;
}

struct SearchHitCandidate {
    QString m_source;
    QString m_text;
    QString m_query;
    QDateTime m_timestamp;
};

void appendSearchHit(QVector<AiSessionSearchResult>* results,
                     const AiSessionInfo& session,
                     const SearchHitCandidate& candidate) {
    const int score = matchScore(candidate.m_text, candidate.m_query);
    if (score <= 0) {
        return;
    }
    AiSessionSearchResult hit;
    hit.session = session;
    hit.source = candidate.m_source;
    hit.score = score;
    hit.snippet = snippetAround(candidate.m_text, candidate.m_query);
    hit.timestamp_utc = candidate.m_timestamp;
    results->append(hit);
}

bool searchSessionFiles(const AiSessionInfo& session,
                        const QString& query,
                        QVector<AiSessionSearchResult>* results,
                        qint64* aggregate_bytes,
                        QString* error_message) {
    const QString transcript_path =
        QDir(session.path).filePath(QString::fromLatin1(kTranscriptFile));
    if (!visitJsonLineObjects({.path = transcript_path,
                               .label = QStringLiteral("Transcript"),
                               .file_cap = kMaxTranscriptJsonlBytes,
                               .error_message = error_message,
                               .aggregate_bytes = aggregate_bytes},
                              [&](const QJsonObject& object) {
                                  const QString text = QStringLiteral("%1 %2").arg(
                                      object.value(QStringLiteral("role")).toString(),
                                      object.value(QStringLiteral("text")).toString());
                                  appendSearchHit(results,
                                                  session,
                                                  {.m_source = QStringLiteral("transcript"),
                                                   .m_text = text,
                                                   .m_query = query,
                                                   .m_timestamp = objectTimestamp(object)});
                              })) {
        return false;
    }

    const QString commands_path = QDir(session.path).filePath(QString::fromLatin1(kCommandsFile));
    return visitJsonLineObjects(
        {.path = commands_path,
         .label = QStringLiteral("Command log"),
         .file_cap = kMaxCommandsJsonlBytes,
         .error_message = error_message,
         .aggregate_bytes = aggregate_bytes},
        [&](const QJsonObject& object) {
            const QString result_text =
                QString::fromUtf8(QJsonDocument(object.value(QStringLiteral("result")).toObject())
                                      .toJson(QJsonDocument::Compact));
            const QString text = QStringLiteral("%1 %2").arg(
                object.value(QStringLiteral("command")).toString(), result_text);
            appendSearchHit(results,
                            session,
                            {.m_source = QStringLiteral("command"),
                             .m_text = text,
                             .m_query = query,
                             .m_timestamp = objectTimestamp(object)});
        });
}

}  // namespace

ConversationStore::ConversationStore(QString root_dir)
    : m_root_dir(root_dir.trimmed().isEmpty() ? defaultRootDirectory() : std::move(root_dir)) {}

void ConversationStore::setBeforeManifestChildOpenHookForTesting(
    std::function<void(const QString&)> hook) {
    g_before_manifest_child_open_hook = std::move(hook);
}

void ConversationStore::setBeforeManifestFileOpenHookForTesting(
    std::function<void(const QString&)> hook) {
    g_before_manifest_file_open_hook = std::move(hook);
}

void ConversationStore::setBeforeManifestFileReadHookForTesting(
    std::function<void(const QString&)> hook) {
    g_before_manifest_file_read_hook = std::move(hook);
}

void ConversationStore::setBeforeArtifactChildOpenHookForTesting(
    std::function<void(const QString&)> hook) {
    g_before_artifact_child_open_hook = std::move(hook);
}

QString ConversationStore::rootDirectory() const {
    return m_root_dir;
}

QString ConversationStore::currentSessionId() const {
    const QReadLocker locker(&m_lock);
    return m_current_session.id;
}

AiSessionInfo ConversationStore::currentSessionInfo() const {
    const QReadLocker locker(&m_lock);
    return m_current_session;
}

QString ConversationStore::defaultRootDirectory() {
    return sessionRootDirectory();
}

bool ConversationStore::ensureRoot(QString* error_message) const {
    const QDir dir(m_root_dir);
    if (dir.exists()) {
        return true;
    }
    if (dir.mkpath(QStringLiteral("."))) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message = QStringLiteral("Could not create AI session root: %1").arg(m_root_dir);
    }
    return false;
}

QVector<AiSessionInfo> ConversationStore::listSessions(QString* error_message) const {
    QVector<AiSessionInfo> sessions;
    QStringList errors;
    const auto consume = [&sessions, &errors](const QString& directory_name,
                                              const QString& session_path,
                                              const QByteArray& bytes) {
        const auto info = parseManifestBytes(directory_name, session_path, bytes, &errors);
        if (!info) {
            return false;
        }
        sessions.append(*info);
        return true;
    };
    const detail::StoreChildFileLoadOptions options{.policy = manifestLoadPolicy(),
                                                    .hooks = manifestLoadHooks(),
                                                    .consume = consume};
    if (!detail::loadStoreChildDirectoryFiles(m_root_dir, options, &errors)) {
        sessions.clear();
        setError(error_message, errors.join(QStringLiteral("; ")));
        return sessions;
    }
    std::ranges::sort(sessions, [](const auto& lhs, const auto& rhs) {
        return lhs.updated_at > rhs.updated_at;
    });
    if (error_message != nullptr) {
        error_message->clear();
    }
    return sessions;
}

QVector<AiSessionInfo> ConversationStore::listPromptedSessions(QString* error_message) const {
    QVector<AiSessionInfo> sessions;
    const QVector<AiSessionInfo> listed = listSessions(error_message);
    if (error_message != nullptr && !error_message->isEmpty()) {
        return sessions;
    }
    for (const auto& session : listed) {
        if (sessionHasUserPrompt(session.path)) {
            sessions.append(session);
        }
    }
    return sessions;
}

QStringList ConversationStore::loadTranscriptLines(const QString& session_id,
                                                   QString* error_message) const {
    QStringList lines;
    const QString session_path = sessionPath(session_id);
    if (session_path.isEmpty()) {
        setError(error_message,
                 QStringLiteral("Invalid AI session id for transcript load: %1").arg(session_id));
        return lines;
    }
    const QString path = QDir(session_path).filePath(QString::fromLatin1(kTranscriptFile));
    if (!visitJsonLineObjects({.path = path,
                               .label = QStringLiteral("Transcript"),
                               .file_cap = kMaxTranscriptJsonlBytes,
                               .error_message = error_message,
                               .aggregate_bytes = nullptr},
                              [&lines](const QJsonObject& object) {
                                  const auto line = transcriptDisplayLine(object);
                                  if (line.has_value()) {
                                      lines.append(*line);
                                  }
                              })) {
        lines.clear();
    }
    return lines;
}

QString ConversationStore::latestAssistantResponseId(const QString& session_id,
                                                     QString* error_message) const {
    QString latest;
    const QString session_path = sessionPath(session_id);
    if (session_path.isEmpty()) {
        setError(
            error_message,
            QStringLiteral("Invalid AI session id for response-id lookup: %1").arg(session_id));
        return {};
    }
    const QString path = QDir(session_path).filePath(QString::fromLatin1(kTranscriptFile));
    if (!visitJsonLineObjects(
            {.path = path,
             .label = QStringLiteral("Transcript"),
             .file_cap = kMaxTranscriptJsonlBytes,
             .error_message = error_message,
             .aggregate_bytes = nullptr},
            [&latest](const QJsonObject& object) {
                const QString role =
                    object.value(QStringLiteral("role")).toString().trimmed().toLower();
                if (role != QLatin1String("assistant")) {
                    return;
                }
                const QString response_id = object.value(QStringLiteral("metadata"))
                                                .toObject()
                                                .value(QStringLiteral("response_id"))
                                                .toString()
                                                .trimmed();
                if (!response_id.isEmpty()) {
                    latest = response_id;
                }
            })) {
        return {};
    }
    return latest;
}

QVector<AiSessionSearchResult> ConversationStore::searchSessions(const QString& query,
                                                                 int max_results,
                                                                 QString* error_message) const {
    QVector<AiSessionSearchResult> results;
    const QString term = query.trimmed();
    if (term.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Search query is empty");
        }
        return results;
    }
    const QVector<AiSessionInfo> sessions = listSessions(error_message);
    if (error_message != nullptr && !error_message->isEmpty()) {
        return results;
    }
    const int session_count = std::min(static_cast<int>(sessions.size()),
                                       kMaxSearchSessionsScanned);
    const int bounded_max_results = boundedSearchResultCount(max_results);
    qint64 aggregate_bytes = 0;
    for (int i = 0; i < session_count; ++i) {
        const auto& session = sessions.at(i);
        appendSearchHit(&results,
                        session,
                        {.m_source = QStringLiteral("manifest"),
                         .m_text = session.title,
                         .m_query = term,
                         .m_timestamp = session.updated_at});
        if (!searchSessionFiles(session, term, &results, &aggregate_bytes, error_message)) {
            results.clear();
            return results;
        }
    }
    std::ranges::sort(results, [](const auto& lhs, const auto& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.timestamp_utc > rhs.timestamp_utc;
    });
    if (results.size() > bounded_max_results) {
        results.resize(bounded_max_results);
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return results;
}

bool ConversationStore::startSession(const QString& title, QString* error_message) {
    const QWriteLocker locker(&m_lock);
    if (!ensureRoot(error_message)) {
        return false;
    }

    const QString id =
        QStringLiteral("ai_%1_%2")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss")),
                 QUuid::createUuid().toString(QUuid::WithoutBraces).left(kSessionUuidSuffixChars));
    const QString path = sessionPath(id);
    if (!QDir().mkpath(path)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Could not create AI session: %1").arg(path);
        }
        return false;
    }

    // The manifest is what makes a session EXIST on disk -- listSessions and openSession both
    // read it. So the in-memory record is not allowed to outlive a failed manifest write: the
    // caller would be handed a currentSessionId() for a session no reader can ever find, and
    // (the live case) the panel logs the failure and returns WITHOUT clearing it, so every
    // subsequent append targets a directory that is not a session.
    const AiSessionInfo previous = m_current_session;
    m_current_session.id = id;
    m_current_session.title = fallbackTitle(title);
    m_current_session.path = path;
    m_current_session.created_at = QDateTime::currentDateTimeUtc();
    m_current_session.updated_at = m_current_session.created_at;
    if (!writeManifest(error_message)) {
        m_current_session = previous;  // never present a session that is not on disk
        return false;
    }
    clearArtifactDirectoryPins();
    return true;
}

bool ConversationStore::openSession(const QString& session_id, QString* error_message) {
    const QWriteLocker locker(&m_lock);
    const QString path = sessionPath(session_id);
    if (path.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Invalid AI session id: %1").arg(session_id);
        }
        return false;
    }
    QStringList errors;
    std::optional<AiSessionInfo> info;
    const auto consume = [&info, &errors](const QString& directory_name,
                                          const QString& session_path,
                                          const QByteArray& bytes) {
        info = parseManifestBytes(directory_name, session_path, bytes, &errors);
        return info.has_value();
    };
    const detail::StoreChildFileLoadOptions options{.policy = manifestLoadPolicy(),
                                                    .hooks = manifestLoadHooks(),
                                                    .consume = consume};
    const detail::StoreChildFileLoadResult loaded =
        detail::loadStoreChildDirectoryFile(m_root_dir, session_id, options, &errors);
    if (loaded == detail::StoreChildFileLoadResult::Missing) {
        setError(error_message, QStringLiteral("AI session not found: %1").arg(session_id));
        return false;
    }
    if (loaded != detail::StoreChildFileLoadResult::Loaded || !info) {
        setError(error_message,
                 errors.isEmpty()
                     ? QStringLiteral("AI session manifest invalid: %1").arg(session_id)
                     : errors.join(QStringLiteral("; ")));
        return false;
    }
    clearArtifactDirectoryPins();
    m_current_session = *info;
    return true;
}

void ConversationStore::clearCurrentSession() {
    const QWriteLocker locker(&m_lock);
    clearArtifactDirectoryPins();
    m_current_session = {};
}

bool ConversationStore::renameCurrentSession(const QString& title, QString* error_message) {
    const QWriteLocker locker(&m_lock);
    const QString trimmed = fallbackTitle(title);
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("No active AI session");
        }
        return false;
    }
    const QString old_artifact_name = safeArtifactDirectoryName(m_current_session.title,
                                                                m_current_session.id);
    const QString new_artifact_name = safeArtifactDirectoryName(trimmed, m_current_session.id);
    const QString artifacts_root = QDir(currentSessionPath()).filePath(QStringLiteral("artifacts"));
    // Retained directory handles deliberately deny rename/delete. Release this session's pins
    // before the explicit artifact rename; later path requests repin the resulting directory.
    clearArtifactDirectoryPins();
    const ArtifactRenameOutcome outcome = renameArtifactDirectory(
        old_artifact_name, new_artifact_name, artifacts_root, error_message);
    if (outcome == ArtifactRenameOutcome::Failed) {
        return false;
    }

    // THE DIRECTORY HAS ALREADY MOVED AT THIS POINT. Readers derive the artifact directory from
    // the CURRENT title, so if the manifest write now fails and nothing is undone, the manifest
    // still names the old title while the artifacts sit under the new one -- and every artifact
    // of that session becomes unreachable. Roll the move back when it was reversible.
    const QString previous_title = m_current_session.title;
    const QDateTime previous_updated = m_current_session.updated_at;
    m_current_session.title = trimmed;
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    if (writeManifest(error_message)) {
        return true;
    }

    m_current_session.title = previous_title;
    m_current_session.updated_at = previous_updated;
    if (outcome == ArtifactRenameOutcome::Renamed) {
        QString rollback_error;
        if (renameArtifactDirectory(
                new_artifact_name, old_artifact_name, artifacts_root, &rollback_error) ==
                ArtifactRenameOutcome::Failed &&
            error_message != nullptr) {
            // Both the rename and its rollback failed. Say so: the artifacts are under the new
            // name while the manifest names the old one, and only a human can reconcile that.
            *error_message += QStringLiteral(
                                  "; artifact directory could not be restored to '%1' "
                                  "and now sits under '%2': %3")
                                  .arg(old_artifact_name, new_artifact_name, rollback_error);
        }
    }
    return false;
}

ConversationStore::ArtifactRenameOutcome ConversationStore::renameArtifactDirectory(
    const QString& old_name,
    const QString& new_name,
    const QString& artifacts_root,
    QString* error_message) {
    if (old_name == new_name || !QDir(artifacts_root).exists()) {
        return ArtifactRenameOutcome::NoChange;
    }
    QDir root_dir(artifacts_root);
    const QString old_path = root_dir.filePath(old_name);
    const QString new_path = root_dir.filePath(new_name);
    // A case-only rename ("Foo" -> "foo") produces different names but the same
    // physical directory on case-insensitive filesystems. Merging then
    // removeRecursively-ing the "source" would delete every artifact, so treat a
    // canonically-identical target as an in-place rename (manifest update only).
    const QString old_canonical = QFileInfo(old_path).canonicalFilePath();
    const QString new_canonical = QFileInfo(new_path).canonicalFilePath();
    // Case-insensitive: on Windows canonicalFilePath preserves the requested case,
    // so "Foo" and "foo" (the same NTFS directory) yield case-different strings.
    const bool same_physical_dir =
        !old_canonical.isEmpty() &&
        QString::compare(old_canonical, new_canonical, Qt::CaseInsensitive) == 0;
    if (!QDir(old_path).exists() || same_physical_dir) {
        return ArtifactRenameOutcome::NoChange;
    }
    if (QDir(new_path).exists()) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Artifact destination already exists; refusing merge: %1")
                    .arg(new_path);
        }
        return ArtifactRenameOutcome::Failed;
    }
    if (root_dir.rename(old_name, new_name)) {
        return ArtifactRenameOutcome::Renamed;  // reversible: the caller can rename back
    }
    if (error_message != nullptr) {
        *error_message = QStringLiteral("Could not rename artifact directory '%1' to '%2'")
                             .arg(old_path, new_path);
    }
    return ArtifactRenameOutcome::Failed;
}

bool ConversationStore::appendTranscript(const QString& role,
                                         const QString& text,
                                         const QJsonObject& metadata,
                                         QString* error_message) {
    const QWriteLocker locker(&m_lock);
    const QString redacted_role = redactSecrets(role);
    const QString redacted_text = redactSecrets(text);
    const QJsonObject redacted_metadata = redactSecretsInJson(metadata);
    QJsonObject obj;
    obj[QStringLiteral("role")] = redacted_role;
    obj[QStringLiteral("text")] = redacted_text;
    obj[QStringLiteral("metadata")] = redacted_metadata;
    if (!appendJsonLine(QString::fromLatin1(kTranscriptFile), obj, error_message)) {
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

bool ConversationStore::appendCommand(const QString& command,
                                      const QJsonObject& result,
                                      QString* error_message) {
    const QWriteLocker locker(&m_lock);
    // Redact at the persistence boundary: some callers (tool-result recorder,
    // workflow PowerShell runner) pass the raw command/preview, which can contain
    // passwords or API keys. This is the single point every command record and its
    // durable command record passes through.
    //
    // THE RESULT IS REDACTED TOO, and that is not belt-and-braces. Only the command string used
    // to be cleaned, while `result` -- the tool result, i.e. the CAPTURED STDOUT/STDERR of the
    // command (ai_tool_result_recorder passes request.result_json; the panel passes the workflow
    // result) -- used to be written verbatim into the command log. A token echoed by a command,
    // or a config dump, landed unredacted in durable session data.
    const QString redacted_command = redactSecrets(command);
    const QJsonObject redacted_result = redactSecretsInJson(result);
    QJsonObject obj;
    obj[QStringLiteral("command")] = redacted_command;
    obj[QStringLiteral("result")] = redacted_result;
    if (!appendJsonLine(QString::fromLatin1(kCommandsFile), obj, error_message)) {
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

QString ConversationStore::commandLogPath(const QString& command_id,
                                          const QString& suffix,
                                          QString* error_message) const {
    const QReadLocker locker(&m_lock);
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("No active AI session");
        }
        return {};
    }
    const QString safe_id = sanitizeLogToken(command_id, QStringLiteral("cmd"));
    const QString safe_suffix = sanitizeLogToken(suffix, QStringLiteral("output"));
    const QString logs_dir = pinnedArtifactDirectory(QStringLiteral("logs"), error_message);
    if (logs_dir.isEmpty()) {
        return {};
    }
    return QDir(logs_dir).filePath(QStringLiteral("%1_%2.txt").arg(safe_id, safe_suffix));
}

QString ConversationStore::artifactSubdir(const QString& subdir, QString* error_message) const {
    const QReadLocker locker(&m_lock);
    const QString trimmed_subdir = subdir.trimmed();
    if (trimmed_subdir.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Artifact subdir is empty");
        }
        return {};
    }
    return pinnedArtifactDirectory(trimmed_subdir, error_message);
}

QString ConversationStore::artifactRootDirectory(QString* error_message) const {
    const QReadLocker locker(&m_lock);
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("No active AI session");
        }
        return {};
    }
    return pinnedArtifactDirectory({}, error_message);
}

QString ConversationStore::pinnedArtifactDirectory(const QString& subdir,
                                                   QString* error_message) const {
    QStringList components{m_current_session.id,
                           QStringLiteral("artifacts"),
                           safeArtifactDirectoryName(m_current_session.title,
                                                     m_current_session.id)};
    if (!subdir.isEmpty()) {
        const auto nested = artifactSubdirComponents(subdir, error_message);
        if (!nested) {
            return {};
        }
        components.append(*nested);
    }
    const QString key = components.join(QLatin1Char('/'));
    const QMutexLocker pin_locker(&m_artifact_pin_mutex);
    const auto existing = m_artifact_directory_pins.constFind(key);
    if (existing != m_artifact_directory_pins.constEnd() && existing->valid()) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return existing->path();
    }
    detail::StoreFileLoadHooks hooks;
    hooks.before_child_open = g_before_artifact_child_open_hook;
    detail::StoreDirectoryPin pin =
        detail::pinOrCreateStoreDirectory(m_root_dir, components, 1, hooks, error_message);
    if (!pin.valid()) {
        return {};
    }
    const QString path = pin.path();
    m_artifact_directory_pins.insert(key, std::move(pin));
    return path;
}

void ConversationStore::clearArtifactDirectoryPins() const {
    const QMutexLocker pin_locker(&m_artifact_pin_mutex);
    m_artifact_directory_pins.clear();
}

QString ConversationStore::artifactPath(const QString& subdir,
                                        const QString& filename,
                                        QString* error_message) const {
    const QReadLocker locker(&m_lock);
    const QString dir = artifactSubdir(subdir, error_message);
    if (dir.isEmpty()) {
        return {};
    }
    // Confine the model-supplied filename to the artifact subdir: QDir::filePath
    // returns an absolute path verbatim and does not resolve "..", so without this
    // an artifact write could overwrite an arbitrary file.
    const QString base = QDir::cleanPath(dir);
    const QString resolved = QDir::cleanPath(QDir(dir).filePath(filename));
    // An empty or dot-only filename resolves to the DIRECTORY itself, which the containment
    // check below then accepts (resolved == base) and returns as a successful FILE path. The
    // caller opens it, and a write to a directory fails at a point where the error no longer
    // names the cause. Reject it here: this function promises a path to a file.
    if (resolved == base) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Artifact filename does not name a file: '%1'").arg(filename);
        }
        return {};
    }
    if (!resolved.startsWith(base + QLatin1Char('/'))) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Artifact filename escapes the artifact directory: %1")
                                 .arg(filename);
        }
        return {};
    }
    return resolved;
}

bool ConversationStore::appendContext(const QString& kind,
                                      const QString& label,
                                      const QString& path,
                                      qint64 size_bytes,
                                      QString* error_message) {
    const QWriteLocker locker(&m_lock);
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
    const QReadLocker locker(&m_lock);
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("No active AI session");
        }
        return {};
    }
    const QString memory_path =
        QDir(currentSessionPath()).filePath(QString::fromLatin1(kMemoryFile));
    if (!QFileInfo::exists(memory_path)) {
        return {};
    }
    const auto memory = readMemoryFile(memory_path, error_message);
    if (!memory.has_value()) {
        return {};
    }
    QString text = *memory;
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
    const QWriteLocker locker(&m_lock);
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
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
    const QString heading =
        QStringLiteral("## %1 - %2 - %3\n").arg(nowIso(), oneLine(kind), oneLine(title));
    QByteArray record = heading.toUtf8();
    record += body.left(kMemoryAppendMaxChars).toUtf8();
    record += "\n\n";
    if (!appendMemoryRecord(memory_path, record, error_message)) {
        return false;
    }
    if (!trimMemoryFile(memory_path, error_message)) {
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

bool ConversationStore::writeUsage(const TokenUsage& turn,
                                   const TokenUsage& session_total,
                                   QString* error_message) {
    const QWriteLocker locker(&m_lock);
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("No active AI session");
        }
        return false;
    }

    QJsonObject root;
    root[QStringLiteral("updated_at")] = nowIso();
    root[QStringLiteral("last_turn")] = usageToJson(turn);
    root[QStringLiteral("session_total")] = usageToJson(session_total);

    QSaveFile file(QDir(currentSessionPath()).filePath(QString::fromLatin1(kUsageFile)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Could not write usage: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Could not commit usage: %1").arg(file.errorString());
        }
        return false;
    }
    m_current_session.updated_at = QDateTime::currentDateTimeUtc();
    return writeManifest(error_message);
}

QString ConversationStore::sessionPath(const QString& session_id) const {
    // Confine session_id to a direct child of the session root: an id containing a path
    // separator, an absolute path, or a ".." segment must not resolve to (or above) the
    // root. Fail closed with an empty path so downstream file ops simply find nothing
    // rather than reading/writing outside the session tree.
    const QString base = QDir::cleanPath(m_root_dir);
    const QString resolved = QDir::cleanPath(QDir(m_root_dir).filePath(session_id));
    if (resolved == base || !resolved.startsWith(base + QLatin1Char('/'))) {
        return {};
    }
    return resolved;
}

QString ConversationStore::currentSessionPath() const {
    return sessionPath(m_current_session.id);
}

bool ConversationStore::writeManifest(QString* error_message) const {
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
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
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Could not write manifest: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Could not commit manifest: %1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

namespace {

qint64 sessionJsonlFileCap(const QString& filename) {
    if (filename == QLatin1String(kTranscriptFile)) {
        return kMaxTranscriptJsonlBytes;
    }
    if (filename == QLatin1String(kCommandsFile)) {
        return kMaxCommandsJsonlBytes;
    }
    if (filename == QLatin1String(kContextFile)) {
        return kMaxContextJsonlBytes;
    }
    return 0;
}

struct JsonlAppendPlan {
    QString path;
    QByteArray payload;
    qint64 current_size{0};
};

std::optional<JsonlAppendPlan> planJsonlAppend(const QString& session_path,
                                               const QString& filename,
                                               const QJsonObject& object,
                                               QString* error_message) {
    JsonlAppendPlan plan;
    plan.path = QDir(session_path).filePath(filename);
    plan.payload = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (plan.payload.size() > kMaxSessionJsonlLineBytes) {
        setError(error_message,
                 QStringLiteral("%1 exceeds the %2-byte line cap")
                     .arg(filename)
                     .arg(kMaxSessionJsonlLineBytes));
        return std::nullopt;
    }
    const qint64 file_cap = sessionJsonlFileCap(filename);
    if (file_cap == 0) {
        setError(error_message,
                 QStringLiteral("Unsupported AI session JSONL file: %1").arg(filename));
        return std::nullopt;
    }
    const QFileInfo info(plan.path);
    plan.current_size = info.exists() ? info.size() : 0;
    if (plan.current_size < 0 || plan.current_size > file_cap - plan.payload.size()) {
        setError(error_message,
                 QStringLiteral("%1 exceeds the %2-byte file cap").arg(filename).arg(file_cap));
        return std::nullopt;
    }
    return plan;
}

bool executeJsonlAppend(const JsonlAppendPlan& plan,
                        const QString& filename,
                        QString* error_message) {
    QFile file(plan.path);
    if (!file.open(QIODevice::Append)) {
        setError(error_message,
                 QStringLiteral("Could not append %1: %2").arg(filename, file.errorString()));
        return false;
    }
    const qint64 start_size = file.size();
    if (start_size != plan.current_size) {
        setError(error_message, QStringLiteral("%1 changed before append").arg(filename));
        return false;
    }
    if (file.write(plan.payload) == plan.payload.size() && file.flush()) {
        return true;
    }
    const QString write_error = file.errorString();
    const bool restored = file.resize(start_size);
    setError(error_message,
             QStringLiteral("Short write appending %1: %2").arg(filename, write_error));
    if (!restored && error_message != nullptr) {
        error_message->append(QStringLiteral("; partial JSONL record could not be removed"));
    }
    return false;
}

}  // namespace

bool ConversationStore::appendJsonLine(const QString& filename,
                                       QJsonObject object,
                                       QString* error_message) const {
    if (m_current_session.id.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("No active AI session");
        }
        return false;
    }

    object[QStringLiteral("timestamp")] = nowIso();
    object[QStringLiteral("schema_version")] = kSessionArtifactSchemaVersion;
    const auto plan = planJsonlAppend(currentSessionPath(), filename, object, error_message);
    return plan.has_value() && executeJsonlAppend(*plan, filename, error_message);
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
    safe = safe.trimmed().left(kSafeFilenameMaxChars);
    // Windows SILENTLY STRIPS trailing dots and spaces from a path component, so a title ending
    // in '.' does not name the directory it appears to: "Report." and "Report" resolve to the
    // SAME directory, and two sessions with those titles would share one artifact folder and
    // overwrite each other's files. Strip them here so the name we compute is the name the
    // filesystem uses.
    while (!safe.isEmpty() &&
           (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' ')))) {
        safe.chop(1);
    }
    // A name that is only dots ("." / ".." / "...") would traverse or alias the parent
    // directory, so collapse it to the safe fallback rather than let it name the dir.
    const bool all_dots = !safe.isEmpty() && std::ranges::all_of(safe, [](QChar ch) {
        return ch == QLatin1Char('.');
    });
    // The DOS device names are reserved on Windows for every extension and in every case, so
    // mkpath on one of them FAILS -- and artifactRootDirectory then returns empty, which makes
    // every artifact write in that session fail for a reason no message would explain. A chat
    // titled "CON" or "aux" is perfectly ordinary, so fall back rather than break the session.
    if (safe.isEmpty() || all_dots || isWindowsReservedName(safe)) {
        safe = QStringLiteral("AI Session");
    }
    return safe;
}

}  // namespace sak::ai
