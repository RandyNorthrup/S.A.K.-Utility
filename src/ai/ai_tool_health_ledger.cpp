// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_health_ledger.h"

#include "sak/logger.h"

#include <QJsonArray>
#include <QStringList>

#include <algorithm>

namespace sak::ai {

namespace {
constexpr qsizetype kHealthErrorPreviewChars = 1000;
constexpr int kBackoffFactorMax = 1024;
constexpr int kBackoffFactorMultiplier = 2;

QString iso(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QString normalizeKey(const QString& key) {
    return key.trimmed().toLower();
}

/// Saturating increment: signed overflow is undefined behavior, and a negative count
/// would read as a healthy record.
void incrementSaturating(int* counter) {
    if (*counter < kAiToolHealthMaxCount) {
        ++(*counter);
    }
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

bool messageContainsAny(const QString& message, const QStringList& markers) {
    return std::ranges::any_of(markers,
                               [&message](const auto& marker) { return message.contains(marker); });
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
        // keys expand m_records and every snapshot without bound.
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
}

void AiToolHealthLedger::reset() {
    const QMutexLocker locker(&m_mutex);
    m_records.clear();
}

AiToolHealthRecord AiToolHealthLedger::record(const QString& key) const {
    const QMutexLocker locker(&m_mutex);
    return m_records.value(normalizeKey(key));
}

QJsonObject AiToolHealthLedger::snapshot() const {
    const QMutexLocker locker(&m_mutex);
    QJsonArray records;
    QStringList keys = m_records.keys();
    keys.sort(Qt::CaseSensitive);
    for (const QString& key : keys) {
        records.append(m_records.value(key).toJson());
    }
    QJsonObject object;
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

}  // namespace sak::ai
