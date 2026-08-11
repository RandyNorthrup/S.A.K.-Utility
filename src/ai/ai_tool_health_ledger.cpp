// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_health_ledger.h"

#include "sak/app_action_guards.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <utility>

namespace sak::ai {

namespace {
constexpr qsizetype kHealthErrorPreviewChars = 1000;
constexpr int kBackoffFactorMax = 1024;
constexpr int kBackoffFactorMultiplier = 2;
constexpr int kSecondsPerHour = kSecondsPerMinute * kSecondsPerMinute;

QString iso(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime parseIso(const QJsonObject& object, const QString& key) {
    return QDateTime::fromString(object.value(key).toString(), Qt::ISODateWithMs);
}

QString normalizeKey(const QString& key) {
    return key.trimmed().toLower();
}

/// Tolerance for clock skew between the process that wrote a record and the one reading
/// it. Anything dated further ahead than this is not skew, it is a forged timestamp.
constexpr qint64 kClockSkewToleranceMs = 300'000;  // 5 minutes

/// Saturating increment. A persisted counter is bounded on load, but an in-process one
/// must still never wrap: signed overflow is undefined behavior, and a negative count
/// would read as a healthy record.
void incrementSaturating(int* counter) {
    if (*counter < kAiToolHealthMaxCount) {
        ++(*counter);
    }
}

/// Fail closed on a persistence path this process cannot bind safely. A relative path
/// resolves against the process CWD (a guessed location, not the configured one), and a
/// symlink/junction at the file OR any ancestor can redirect this elevated process's read
/// or write off the app's own data directory. pathReparseUnsafe also rejects blank and
/// UNC/device paths before performing any stat.
bool ledgerPathUnsafe(const QString& path, QString* error_message) {
    if (!QFileInfo(path).isAbsolute()) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("AI tool health ledger path must be absolute: %1").arg(path);
        }
        return true;
    }
    if (sak::pathReparseUnsafe(path)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral(
                                 "Refusing AI tool health ledger path (it, or an ancestor, is a "
                                 "symlink/junction or a UNC/device path): %1")
                                 .arg(path);
        }
        return true;
    }
    return false;
}

/// True when @p key is absent (the record default applies) or actually holds a string.
/// A present but wrong-typed field is malformed: coercing it to "" would turn a corrupt
/// or tampered entry into a well-formed-looking one.
bool stringFieldOk(const QJsonObject& object, const QString& key) {
    return !object.contains(key) || object.value(key).isString();
}

/// Parse a whole, non-negative, in-range counter. False for a present-but-wrong-typed,
/// fractional, negative or out-of-range value.
bool parseCount(const QJsonObject& object, const QString& key, int* out) {
    *out = 0;
    if (!object.contains(key)) {
        return true;
    }
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return false;
    }
    const double raw = value.toDouble();
    if (raw < 0.0 || raw > static_cast<double>(kAiToolHealthMaxCount) || raw != std::floor(raw)) {
        return false;
    }
    *out = static_cast<int>(raw);
    return true;
}

/// Same, for the millisecond latency. The range is checked BEFORE the cast: converting a
/// double outside the integer's range is undefined behavior, not a clamp.
bool parseMillis(const QJsonObject& object, const QString& key, qint64* out) {
    *out = 0;
    if (!object.contains(key)) {
        return true;
    }
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return false;
    }
    const double raw = value.toDouble();
    if (raw < 0.0 || raw > static_cast<double>(kAiToolHealthMaxLatencyMs) ||
        raw != std::floor(raw)) {
        return false;
    }
    *out = static_cast<qint64>(raw);
    return true;
}

/// Parse an ISO-8601 timestamp. Absent, or the empty string toJson() writes for an unset
/// QDateTime, means "never" (an invalid QDateTime, which every reader already treats as
/// unset); a non-empty string that does not parse is malformed.
bool parseIsoStrict(const QJsonObject& object, const QString& key, QDateTime* out) {
    *out = QDateTime();
    if (!object.contains(key)) {
        return true;
    }
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        return false;
    }
    if (value.toString().isEmpty()) {
        return true;
    }
    const QDateTime parsed = parseIso(object, key);
    if (!parsed.isValid()) {
        return false;
    }
    *out = parsed.toUTC();
    return true;
}

/// True when every free-text field of a persisted record is absent or actually a string.
bool recordTextFieldsOk(const QJsonObject& object) {
    return stringFieldOk(object, QStringLiteral("key")) &&
           stringFieldOk(object, QStringLiteral("last_failure_class")) &&
           stringFieldOk(object, QStringLiteral("last_error_message"));
}

/// Parse every timestamp of a persisted record. All-or-nothing: the first malformed one
/// ends the parse, because a record accepted with some of its timestamps silently unset
/// would read as a tool that has never failed.
bool parseRecordTimestamps(const QJsonObject& object, AiToolHealthRecord* out) {
    return parseIsoStrict(object, QStringLiteral("last_success_utc"), &out->last_success_utc) &&
           parseIsoStrict(object, QStringLiteral("last_failure_utc"), &out->last_failure_utc) &&
           parseIsoStrict(object, QStringLiteral("disabled_until_utc"), &out->disabled_until_utc);
}

/// Parse the latency and the three counters of a persisted record, on the same terms: one
/// out-of-range or wrong-typed number condemns the whole record rather than being coerced.
bool parseRecordCounters(const QJsonObject& object, AiToolHealthRecord* out) {
    return parseMillis(object, QStringLiteral("last_latency_ms"), &out->last_latency_ms) &&
           parseCount(object, QStringLiteral("success_count"), &out->success_count) &&
           parseCount(object, QStringLiteral("failure_count"), &out->failure_count) &&
           parseCount(object, QStringLiteral("consecutive_failures"), &out->consecutive_failures);
}

/// The failure class carried by the result's EXPLICIT fields, or empty when it carries no
/// explicit denial/failure marker at all.
QString explicitFailureClass(const QJsonObject& result) {
    const QString declared = result.value(QStringLiteral("failure_class")).toString().trimmed();
    if (!declared.isEmpty()) {
        return declared;
    }
    if (result.value(QStringLiteral("policy_denied")).toBool(false)) {
        return QStringLiteral("policy_denied");
    }
    if (result.value(QStringLiteral("handler_missing")).toBool(false)) {
        return QStringLiteral("handler_missing");
    }
    if (result.value(QStringLiteral("lease_denied")).toBool(false)) {
        return QStringLiteral("lease_denied");
    }
    return {};
}

bool parseHealthLedgerDocument(const QString& path,
                               QJsonDocument* document,
                               QString* error_message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Could not read AI tool health ledger: %1").arg(file.errorString());
        }
        return false;
    }
    // Bound the read before readAll(): the ledger lives in a locally writable directory,
    // and a DOM parse of an arbitrarily large file is an out-of-memory abort, not an error.
    const qint64 size = file.size();
    if (size < 0 || size > kAiToolHealthMaxLedgerBytes) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("AI tool health ledger is not a readable size (%1 bytes)").arg(size);
        }
        return false;
    }

    QJsonParseError parse_error;
    *document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error == QJsonParseError::NoError && document->isObject()) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message =
            QStringLiteral("Invalid AI tool health ledger JSON: %1").arg(parse_error.errorString());
    }
    return false;
}

/// The reason a parsed ledger document's envelope cannot be trusted, or empty when every
/// claim it makes about itself holds. @p records receives the entry array only once it is
/// known to be one.
QString ledgerEnvelopeRefusal(const QJsonObject& root, QJsonArray* records) {
    if (root.value(QStringLiteral("schema_version")).toInt(-1) != kAiToolHealthSchemaVersion) {
        return QStringLiteral("unsupported or missing schema_version");
    }
    const QJsonValue records_value = root.value(QStringLiteral("records"));
    if (!records_value.isArray()) {
        // A missing or wrong-typed array used to collapse into an empty one and REPLACE
        // every in-memory record, so a truncated or tampered file silently cleared the
        // whole circuit breaker. Refuse it and keep what is already in memory.
        return QStringLiteral("'records' is missing or is not an array");
    }
    *records = records_value.toArray();
    if (records->size() > kAiToolHealthMaxRecords) {
        return QStringLiteral("more records than the ceiling of %1").arg(kAiToolHealthMaxRecords);
    }
    if (root.value(QStringLiteral("record_count")).toInt(-1) != records->size()) {
        return QStringLiteral("record_count does not match the records array");
    }
    return {};
}

/// Report a failed ledger write through the optional out-parameter. Every write step fails
/// the same way, so the null check lives here once instead of guarding each of them.
bool ledgerWriteFailed(QString* error_message, const QString& reason) {
    if (error_message != nullptr) {
        *error_message = reason;
    }
    return false;
}

bool messageContainsAny(const QString& message, const QStringList& markers) {
    for (const auto& marker : markers) {
        if (message.contains(marker)) {
            return true;
        }
    }
    return false;
}

}  // namespace

QJsonObject AiToolHealthRecord::toJson() const {
    QJsonObject object;
    object[QStringLiteral("key")] = key;
    object[QStringLiteral("last_success_utc")] = iso(last_success_utc);
    object[QStringLiteral("last_failure_utc")] = iso(last_failure_utc);
    object[QStringLiteral("disabled_until_utc")] = iso(disabled_until_utc);
    object[QStringLiteral("last_failure_class")] = last_failure_class;
    object[QStringLiteral("last_error_message")] = last_error_message;
    object[QStringLiteral("last_latency_ms")] = static_cast<double>(last_latency_ms);
    object[QStringLiteral("success_count")] = success_count;
    object[QStringLiteral("failure_count")] = failure_count;
    object[QStringLiteral("consecutive_failures")] = consecutive_failures;
    return object;
}

AiToolHealthRecord AiToolHealthRecord::fromJson(const QJsonObject& object, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!recordTextFieldsOk(object)) {
        return {};
    }
    AiToolHealthRecord record;
    record.key = normalizeKey(object.value(QStringLiteral("key")).toString());
    if (record.key.isEmpty() || record.key.size() > kAiToolHealthMaxKeyChars) {
        return {};
    }
    if (!parseRecordTimestamps(object, &record)) {
        return {};
    }
    record.last_failure_class = object.value(QStringLiteral("last_failure_class"))
                                    .toString()
                                    .left(kHealthErrorPreviewChars);
    record.last_error_message = object.value(QStringLiteral("last_error_message"))
                                    .toString()
                                    .left(kHealthErrorPreviewChars);
    if (!parseRecordCounters(object, &record)) {
        return {};
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return record;
}

QJsonObject AiToolAvailability::toJson() const {
    QJsonObject object;
    object[QStringLiteral("available")] = available;
    object[QStringLiteral("key")] = key;
    object[QStringLiteral("failure_class")] = failure_class;
    object[QStringLiteral("reason")] = reason;
    object[QStringLiteral("disabled_until_utc")] = iso(disabled_until_utc);
    return object;
}

AiToolHealthLedger::AiToolHealthLedger(int suppress_after_failures,
                                       int base_backoff_ms,
                                       int max_backoff_ms)
    : m_suppress_after_failures(std::max(1, suppress_after_failures))
    , m_base_backoff_ms(std::max(1, base_backoff_ms))
    , m_max_backoff_ms(std::max(m_base_backoff_ms, max_backoff_ms)) {}

AiToolAvailability AiToolHealthLedger::check(const QString& key, QDateTime now_utc) const {
    const QMutexLocker locker(&m_mutex);
    const QString normalized = normalizeKey(key);
    AiToolAvailability result;
    result.key = normalized;
    // AiToolAvailability defaults to unavailable, so every "no suppression applies"
    // conclusion is stated explicitly here rather than inherited from a default.
    result.available = true;
    if (normalized.isEmpty()) {
        // A blank key identifies nothing, and recordSuccess/recordFailure refuse it too,
        // so no record can exist for it: there is no suppression to bypass. Denying here
        // instead would block every dispatch whose health key is legitimately unset.
        return result;
    }

    const auto it = m_records.constFind(normalized);
    if (it == m_records.constEnd()) {
        return result;
    }

    const QDateTime now = normalizedNow(now_utc);
    if (it->disabled_until_utc.isValid() && it->disabled_until_utc > now) {
        result.available = false;
        result.failure_class = QStringLiteral("health_backoff");
        result.disabled_until_utc = it->disabled_until_utc;
        result.reason = QStringLiteral(
                            "Tool/provider '%1' is temporarily disabled until %2 after %3 "
                            "consecutive failure(s): %4")
                            .arg(normalized,
                                 iso(it->disabled_until_utc),
                                 QString::number(it->consecutive_failures),
                                 it->last_error_message);
    }
    return result;
}

AiToolHealthRecord* AiToolHealthLedger::recordForUpdateUnlocked(const QString& normalized_key) {
    if (normalized_key.isEmpty() || normalized_key.size() > kAiToolHealthMaxKeyChars) {
        return nullptr;
    }
    const auto existing = m_records.find(normalized_key);
    if (existing != m_records.end()) {
        return &existing.value();
    }
    if (m_records.size() >= kAiToolHealthMaxRecords) {
        // Refuse to grow past the ceiling rather than let model- or response-supplied
        // keys expand m_records, every snapshot and the persisted file without bound.
        sak::logWarning(
            "AI tool health ledger is at its record ceiling; a new tool key is not tracked");
        return nullptr;
    }
    auto& new_record = m_records[normalized_key];
    new_record.key = normalized_key;
    return &new_record;
}

void AiToolHealthLedger::recordSuccess(const QString& key, qint64 latency_ms, QDateTime now_utc) {
    const QMutexLocker locker(&m_mutex);
    AiToolHealthRecord* rec = recordForUpdateUnlocked(normalizeKey(key));
    if (rec == nullptr) {
        return;
    }
    rec->last_success_utc = normalizedNow(now_utc);
    rec->last_latency_ms = std::clamp<qint64>(latency_ms, 0, kAiToolHealthMaxLatencyMs);
    incrementSaturating(&rec->success_count);
    rec->consecutive_failures = 0;
    rec->disabled_until_utc = {};
    persistIfConfigured();
}

void AiToolHealthLedger::recordFailure(const QString& key,
                                       const QString& failure_class,
                                       const QString& error_message,
                                       qint64 latency_ms,
                                       QDateTime now_utc) {
    const QMutexLocker locker(&m_mutex);
    AiToolHealthRecord* rec = recordForUpdateUnlocked(normalizeKey(key));
    if (rec == nullptr) {
        return;
    }
    const QDateTime now = normalizedNow(now_utc);
    rec->last_failure_utc = now;
    rec->last_latency_ms = std::clamp<qint64>(latency_ms, 0, kAiToolHealthMaxLatencyMs);
    rec->last_failure_class = failure_class.trimmed().isEmpty()
                                  ? QStringLiteral("tool_failed")
                                  : failure_class.trimmed().left(kHealthErrorPreviewChars);
    rec->last_error_message = error_message.trimmed().left(kHealthErrorPreviewChars);
    incrementSaturating(&rec->failure_count);
    incrementSaturating(&rec->consecutive_failures);
    if (rec->consecutive_failures >= m_suppress_after_failures) {
        rec->disabled_until_utc = now.addMSecs(backoffMs(rec->consecutive_failures));
    }
    persistIfConfigured();
}

void AiToolHealthLedger::setPersistencePath(const QString& path, int ttl_hours) {
    const QMutexLocker locker(&m_mutex);
    m_persistence_path = path.trimmed();
    // Bound the TTL. The freshness cutoff multiplies it by the seconds in an hour, which
    // overflows a plain int past ~596k hours (undefined behavior, and a corrupt cutoff),
    // and no caller has business asking to retain health state for longer than a year.
    m_ttl_hours = std::clamp(ttl_hours, 1, kAiToolHealthMaxTtlHours);
}

QString AiToolHealthLedger::persistencePath() const {
    const QMutexLocker locker(&m_mutex);
    return m_persistence_path;
}

bool AiToolHealthLedger::load(QString* error_message) {
    const QMutexLocker locker(&m_mutex);
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (m_persistence_path.isEmpty()) {
        return true;  // Persistence not configured: nothing to load.
    }
    if (ledgerPathUnsafe(m_persistence_path, error_message)) {
        return false;
    }
    if (!QFileInfo::exists(m_persistence_path)) {
        return true;  // First run: no ledger has been written yet.
    }

    QJsonDocument doc;
    if (!parseHealthLedgerDocument(m_persistence_path, &doc, error_message)) {
        return false;
    }
    return loadRecordsUnlocked(doc.object(), error_message);
}

bool AiToolHealthLedger::loadRecordsUnlocked(const QJsonObject& root, QString* error_message) {
    const auto refuse = [error_message](const QString& reason) {
        if (error_message) {
            *error_message = QStringLiteral("Refusing AI tool health ledger: %1").arg(reason);
        }
        return false;
    };
    QJsonArray records;
    const QString envelope_refusal = ledgerEnvelopeRefusal(root, &records);
    if (!envelope_refusal.isEmpty()) {
        return refuse(envelope_refusal);
    }

    const QDateTime now = normalizedNow({});
    QHash<QString, AiToolHealthRecord> loaded;
    for (const auto& value : records) {
        bool record_ok = false;
        const AiToolHealthRecord parsed_record = AiToolHealthRecord::fromJson(value.toObject(),
                                                                              &record_ok);
        if (!value.isObject() || !record_ok || !persistedRecordIsSane(parsed_record, now)) {
            return refuse(QStringLiteral("a record is malformed or inconsistent"));
        }
        if (loaded.contains(parsed_record.key)) {
            return refuse(QStringLiteral("duplicate record key '%1'").arg(parsed_record.key));
        }
        if (recordIsFresh(parsed_record, now)) {
            loaded.insert(parsed_record.key, parsed_record);
        }
    }
    m_records = std::move(loaded);
    return true;
}

bool AiToolHealthLedger::save(QString* error_message) const {
    const QMutexLocker locker(&m_mutex);
    return saveUnlocked(error_message);
}

bool AiToolHealthLedger::saveUnlocked(QString* error_message) const {
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (m_persistence_path.isEmpty()) {
        return true;  // Persistence not configured: there is nothing to write.
    }
    if (ledgerPathUnsafe(m_persistence_path, error_message)) {
        return false;
    }

    const QString directory = QFileInfo(m_persistence_path).absolutePath();
    if (!QDir().mkpath(directory)) {
        return ledgerWriteFailed(
            error_message,
            QStringLiteral("Could not create AI tool health ledger directory: %1").arg(directory));
    }

    QSaveFile file(m_persistence_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return ledgerWriteFailed(
            error_message,
            QStringLiteral("Could not write AI tool health ledger: %1").arg(file.errorString()));
    }
    const QByteArray payload = QJsonDocument(snapshotUnlocked()).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        // A short write would otherwise be committed as a truncated (and therefore
        // unloadable) ledger. Returning without commit() discards the partial file.
        return ledgerWriteFailed(
            error_message,
            QStringLiteral("Short write to AI tool health ledger: %1").arg(file.errorString()));
    }
    if (!file.commit()) {
        return ledgerWriteFailed(
            error_message,
            QStringLiteral("Could not commit AI tool health ledger: %1").arg(file.errorString()));
    }
    return true;
}

void AiToolHealthLedger::pruneExpired(QDateTime now_utc) {
    const QMutexLocker locker(&m_mutex);
    const QDateTime now = normalizedNow(now_utc);
    for (auto it = m_records.begin(); it != m_records.end();) {
        if (recordIsFresh(it.value(), now)) {
            ++it;
        } else {
            it = m_records.erase(it);
        }
    }
    persistIfConfigured();
}

AiToolHealthRecord AiToolHealthLedger::record(const QString& key) const {
    const QMutexLocker locker(&m_mutex);
    return m_records.value(normalizeKey(key));
}

QJsonObject AiToolHealthLedger::snapshot() const {
    const QMutexLocker locker(&m_mutex);
    return snapshotUnlocked();
}

QJsonObject AiToolHealthLedger::snapshotUnlocked() const {
    QJsonArray records;
    for (const auto& rec : m_records) {
        records.append(rec.toJson());
    }
    QJsonObject object;
    object[QStringLiteral("schema_version")] = 1;
    object[QStringLiteral("saved_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    object[QStringLiteral("ttl_hours")] = m_ttl_hours;
    object[QStringLiteral("records")] = records;
    object[QStringLiteral("record_count")] = records.size();
    return object;
}

int AiToolHealthLedger::size() const {
    const QMutexLocker locker(&m_mutex);
    return static_cast<int>(m_records.size());
}

QString AiToolHealthLedger::classifyResult(const QJsonObject& result) {
    const QString explicit_class = explicitFailureClass(result);
    if (result.value(QStringLiteral("success")).toBool(false)) {
        // A result cannot be both a success and a denial/failure. Believing the success
        // flag over the field that contradicts it would walk a malformed (or hostile
        // tool-server) result straight past the health circuit breaker, so a contradictory
        // result is classified as the failure it also claims to be. With no such field
        // this is the empty "succeeded" classification, exactly as before.
        return explicit_class;
    }
    if (!explicit_class.isEmpty()) {
        return explicit_class;
    }
    const QString message = result.value(QStringLiteral("error_message")).toString().toLower();
    if (messageContainsAny(message, {QStringLiteral("checksum")})) {
        return QStringLiteral("checksum_mismatch");
    }
    if (messageContainsAny(message, {QStringLiteral("timed out"), QStringLiteral("timeout")})) {
        return QStringLiteral("timeout");
    }
    if (messageContainsAny(message, {QStringLiteral("cancelled"), QStringLiteral("canceled")})) {
        return QStringLiteral("cancelled");
    }
    if (messageContainsAny(message, {QStringLiteral("unavailable"), QStringLiteral("missing")})) {
        return QStringLiteral("unavailable");
    }
    return QStringLiteral("tool_failed");
}

QDateTime AiToolHealthLedger::normalizedNow(QDateTime now_utc) {
    return now_utc.isValid() ? now_utc.toUTC() : QDateTime::currentDateTimeUtc();
}

int AiToolHealthLedger::backoffMs(int consecutive_failures) const {
    int factor = 1;
    const int exponent = std::max(0, consecutive_failures - m_suppress_after_failures);
    for (int i = 0; i < exponent && factor < kBackoffFactorMax; ++i) {
        factor *= kBackoffFactorMultiplier;
    }
    // Compute in qint64 so a large configured base times the capped factor cannot overflow
    // int (UB) before the max_backoff clamp is applied.
    const qint64 scaled = static_cast<qint64>(m_base_backoff_ms) * factor;
    return static_cast<int>(std::min<qint64>(m_max_backoff_ms, scaled));
}

bool AiToolHealthLedger::persistedRecordIsSane(const AiToolHealthRecord& record,
                                               QDateTime now_utc) const {
    const QDateTime now = normalizedNow(now_utc);
    const QDateTime skew_limit = now.addMSecs(kClockSkewToleranceMs);
    if (record.last_success_utc.isValid() && record.last_success_utc > skew_limit) {
        return false;
    }
    if (record.last_failure_utc.isValid() && record.last_failure_utc > skew_limit) {
        return false;
    }
    // This ledger only ever writes disabled_until = failure time + backoffMs(), which is
    // capped at m_max_backoff_ms. A file claiming more would suppress the tool past its
    // own ceiling AND stay permanently fresh (recordIsFresh honours a live disable).
    if (record.disabled_until_utc.isValid() &&
        record.disabled_until_utc > now.addMSecs(m_max_backoff_ms + kClockSkewToleranceMs)) {
        return false;
    }
    return record.consecutive_failures <= record.failure_count;
}

bool AiToolHealthLedger::recordIsFresh(const AiToolHealthRecord& record, QDateTime now_utc) const {
    const QDateTime now = normalizedNow(now_utc);
    // Widen before multiplying: m_ttl_hours is bounded, but the product is computed in
    // qint64 so the seconds arithmetic can never overflow int (undefined behavior).
    const QDateTime cutoff = now.addSecs(-static_cast<qint64>(m_ttl_hours) * kSecondsPerHour);
    if (record.disabled_until_utc.isValid() && record.disabled_until_utc > now) {
        return true;
    }
    if (record.last_success_utc.isValid() && record.last_success_utc >= cutoff) {
        return true;
    }
    return record.last_failure_utc.isValid() && record.last_failure_utc >= cutoff;
}

void AiToolHealthLedger::persistIfConfigured() const {
    // Callers already hold m_mutex; use the unlocked save to avoid re-locking.
    if (m_persistence_path.isEmpty()) {
        return;
    }
    QString error;
    if (!saveUnlocked(&error)) {
        // The in-memory ledger is still authoritative for this process, but a failed
        // write means the suppression state will NOT survive a restart. The mutators are
        // void by contract, so surface the real error here instead of discarding it.
        sak::logWarning(
            QStringLiteral("Could not persist AI tool health ledger: %1").arg(error).toStdString());
    }
}

}  // namespace sak::ai
