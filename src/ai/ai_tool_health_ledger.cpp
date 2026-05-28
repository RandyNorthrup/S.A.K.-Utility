// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_health_ledger.h"

#include "sak/layout_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <algorithm>
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

bool parseHealthLedgerDocument(const QString& path,
                               QJsonDocument* document,
                               QString* error_message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not read AI tool health ledger: %1").arg(file.errorString());
        }
        return false;
    }

    QJsonParseError parse_error;
    *document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error == QJsonParseError::NoError && document->isObject()) {
        return true;
    }
    if (error_message) {
        *error_message =
            QStringLiteral("Invalid AI tool health ledger JSON: %1").arg(parse_error.errorString());
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

AiToolHealthRecord AiToolHealthRecord::fromJson(const QJsonObject& object) {
    AiToolHealthRecord record;
    record.key = normalizeKey(object.value(QStringLiteral("key")).toString());
    record.last_success_utc = parseIso(object, QStringLiteral("last_success_utc"));
    record.last_failure_utc = parseIso(object, QStringLiteral("last_failure_utc"));
    record.disabled_until_utc = parseIso(object, QStringLiteral("disabled_until_utc"));
    record.last_failure_class = object.value(QStringLiteral("last_failure_class")).toString();
    record.last_error_message = object.value(QStringLiteral("last_error_message")).toString();
    record.last_latency_ms =
        static_cast<qint64>(object.value(QStringLiteral("last_latency_ms")).toDouble());
    record.success_count = object.value(QStringLiteral("success_count")).toInt();
    record.failure_count = object.value(QStringLiteral("failure_count")).toInt();
    record.consecutive_failures = object.value(QStringLiteral("consecutive_failures")).toInt();
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
    const QString normalized = normalizeKey(key);
    AiToolAvailability result;
    result.key = normalized;
    if (normalized.isEmpty()) {
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

void AiToolHealthLedger::recordSuccess(const QString& key, qint64 latency_ms, QDateTime now_utc) {
    const QString normalized = normalizeKey(key);
    if (normalized.isEmpty()) {
        return;
    }
    auto& record = m_records[normalized];
    record.key = normalized;
    record.last_success_utc = normalizedNow(now_utc);
    record.last_latency_ms = std::max<qint64>(0, latency_ms);
    ++record.success_count;
    record.consecutive_failures = 0;
    record.disabled_until_utc = {};
    persistIfConfigured();
}

void AiToolHealthLedger::recordFailure(const QString& key,
                                       const QString& failure_class,
                                       const QString& error_message,
                                       qint64 latency_ms,
                                       QDateTime now_utc) {
    const QString normalized = normalizeKey(key);
    if (normalized.isEmpty()) {
        return;
    }
    const QDateTime now = normalizedNow(now_utc);
    auto& record = m_records[normalized];
    record.key = normalized;
    record.last_failure_utc = now;
    record.last_latency_ms = std::max<qint64>(0, latency_ms);
    record.last_failure_class = failure_class.trimmed().isEmpty() ? QStringLiteral("tool_failed")
                                                                  : failure_class.trimmed();
    record.last_error_message = error_message.trimmed().left(kHealthErrorPreviewChars);
    ++record.failure_count;
    ++record.consecutive_failures;
    if (record.consecutive_failures >= m_suppress_after_failures) {
        record.disabled_until_utc = now.addMSecs(backoffMs(record.consecutive_failures));
    }
    persistIfConfigured();
}

void AiToolHealthLedger::setPersistencePath(const QString& path, int ttl_hours) {
    m_persistence_path = path.trimmed();
    m_ttl_hours = std::max(1, ttl_hours);
}

QString AiToolHealthLedger::persistencePath() const {
    return m_persistence_path;
}

bool AiToolHealthLedger::load(QString* error_message) {
    if (error_message) {
        error_message->clear();
    }
    if (m_persistence_path.isEmpty() || !QFileInfo::exists(m_persistence_path)) {
        return true;
    }

    QJsonDocument doc;
    if (!parseHealthLedgerDocument(m_persistence_path, &doc, error_message)) {
        return false;
    }

    const QDateTime now = normalizedNow({});
    QHash<QString, AiToolHealthRecord> loaded;
    const QJsonArray records = doc.object().value(QStringLiteral("records")).toArray();
    for (const auto& value : records) {
        const AiToolHealthRecord record = AiToolHealthRecord::fromJson(value.toObject());
        if (!record.key.isEmpty() && recordIsFresh(record, now)) {
            loaded.insert(record.key, record);
        }
    }
    m_records = std::move(loaded);
    return true;
}

bool AiToolHealthLedger::save(QString* error_message) const {
    if (error_message) {
        error_message->clear();
    }
    if (m_persistence_path.isEmpty()) {
        return true;
    }

    const QFileInfo info(m_persistence_path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error_message) {
            *error_message = QStringLiteral("Could not create AI tool health ledger directory: %1")
                                 .arg(info.absolutePath());
        }
        return false;
    }

    QSaveFile file(m_persistence_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not write AI tool health ledger: %1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(snapshot()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error_message) {
            *error_message = QStringLiteral("Could not commit AI tool health ledger: %1")
                                 .arg(file.errorString());
        }
        return false;
    }
    return true;
}

void AiToolHealthLedger::pruneExpired(QDateTime now_utc) {
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
    return m_records.value(normalizeKey(key));
}

QJsonObject AiToolHealthLedger::snapshot() const {
    QJsonArray records;
    for (const auto& record : m_records) {
        records.append(record.toJson());
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
    return m_records.size();
}

QString AiToolHealthLedger::classifyResult(const QJsonObject& result) {
    if (result.value(QStringLiteral("success")).toBool(false)) {
        return {};
    }
    const QString explicit_class =
        result.value(QStringLiteral("failure_class")).toString().trimmed();
    if (!explicit_class.isEmpty()) {
        return explicit_class;
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

QDateTime AiToolHealthLedger::normalizedNow(QDateTime now_utc) const {
    return now_utc.isValid() ? now_utc.toUTC() : QDateTime::currentDateTimeUtc();
}

int AiToolHealthLedger::backoffMs(int consecutive_failures) const {
    int factor = 1;
    const int exponent = std::max(0, consecutive_failures - m_suppress_after_failures);
    for (int i = 0; i < exponent && factor < kBackoffFactorMax; ++i) {
        factor *= kBackoffFactorMultiplier;
    }
    return std::min(m_max_backoff_ms, m_base_backoff_ms * factor);
}

bool AiToolHealthLedger::recordIsFresh(const AiToolHealthRecord& record, QDateTime now_utc) const {
    const QDateTime now = normalizedNow(now_utc);
    const QDateTime cutoff = now.addSecs(-m_ttl_hours * kSecondsPerHour);
    if (record.disabled_until_utc.isValid() && record.disabled_until_utc > now) {
        return true;
    }
    if (record.last_success_utc.isValid() && record.last_success_utc >= cutoff) {
        return true;
    }
    return record.last_failure_utc.isValid() && record.last_failure_utc >= cutoff;
}

void AiToolHealthLedger::persistIfConfigured() const {
    if (!m_persistence_path.isEmpty()) {
        (void)save(nullptr);
    }
}

}  // namespace sak::ai
