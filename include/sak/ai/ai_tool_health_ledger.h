// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>

namespace sak::ai {

inline constexpr int kAiToolHealthDefaultSuppressAfterFailures = 3;
inline constexpr int kAiToolHealthDefaultBaseBackoffMs = 5000;
inline constexpr int kAiToolHealthDefaultMaxBackoffMs = 300'000;
inline constexpr int kAiToolHealthDefaultTtlHours = 24;

struct AiToolHealthRecord {
    QString key;
    QDateTime last_success_utc;
    QDateTime last_failure_utc;
    QDateTime disabled_until_utc;
    QString last_failure_class;
    QString last_error_message;
    qint64 last_latency_ms{0};
    int success_count{0};
    int failure_count{0};
    int consecutive_failures{0};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiToolHealthRecord fromJson(const QJsonObject& object);
};

struct AiToolAvailability {
    bool available{true};
    QString key;
    QString failure_class;
    QString reason;
    QDateTime disabled_until_utc;

    [[nodiscard]] QJsonObject toJson() const;
};

class AiToolHealthLedger {
public:
    explicit AiToolHealthLedger(
        int suppress_after_failures = kAiToolHealthDefaultSuppressAfterFailures,
        int base_backoff_ms = kAiToolHealthDefaultBaseBackoffMs,
        int max_backoff_ms = kAiToolHealthDefaultMaxBackoffMs);

    [[nodiscard]] AiToolAvailability check(const QString& key, QDateTime now_utc = {}) const;
    void recordSuccess(const QString& key, qint64 latency_ms, QDateTime now_utc = {});
    void recordFailure(const QString& key,
                       const QString& failure_class,
                       const QString& error_message,
                       qint64 latency_ms,
                       QDateTime now_utc = {});
    void setPersistencePath(const QString& path, int ttl_hours = kAiToolHealthDefaultTtlHours);
    [[nodiscard]] QString persistencePath() const;
    [[nodiscard]] bool load(QString* error_message = nullptr);
    [[nodiscard]] bool save(QString* error_message = nullptr) const;
    void pruneExpired(QDateTime now_utc = {});

    [[nodiscard]] AiToolHealthRecord record(const QString& key) const;
    [[nodiscard]] QJsonObject snapshot() const;
    [[nodiscard]] int size() const;

    [[nodiscard]] static QString classifyResult(const QJsonObject& result);

private:
    [[nodiscard]] QDateTime normalizedNow(QDateTime now_utc) const;
    [[nodiscard]] int backoffMs(int consecutive_failures) const;
    [[nodiscard]] bool recordIsFresh(const AiToolHealthRecord& record, QDateTime now_utc) const;
    void persistIfConfigured() const;

    QHash<QString, AiToolHealthRecord> m_records;
    int m_suppress_after_failures{kAiToolHealthDefaultSuppressAfterFailures};
    int m_base_backoff_ms{kAiToolHealthDefaultBaseBackoffMs};
    int m_max_backoff_ms{kAiToolHealthDefaultMaxBackoffMs};
    QString m_persistence_path;
    int m_ttl_hours{kAiToolHealthDefaultTtlHours};
};

}  // namespace sak::ai
