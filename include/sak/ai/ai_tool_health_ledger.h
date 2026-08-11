// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QString>

namespace sak::ai {

inline constexpr int kAiToolHealthDefaultSuppressAfterFailures = 3;
inline constexpr int kAiToolHealthDefaultBaseBackoffMs = 5000;
inline constexpr int kAiToolHealthDefaultMaxBackoffMs = 300'000;
inline constexpr int kAiToolHealthDefaultTtlHours = 24;

// Hard ceilings on anything a persisted (locally attacker-writable) ledger file or a
// caller can put into this ledger. Each is a fail-closed bound: a value outside it is
// refused, never coerced into range and then trusted as if it had been valid.
inline constexpr int kAiToolHealthSchemaVersion = 1;
inline constexpr int kAiToolHealthMaxTtlHours = 24 * 365;    // 1 year; keeps ttl*3600 in int
inline constexpr int kAiToolHealthMaxRecords = 4096;         // the real tool/provider count is ~100
inline constexpr int kAiToolHealthMaxKeyChars = 256;
inline constexpr int kAiToolHealthMaxCount = 1'000'000'000;  // counters stay far from INT_MAX
inline constexpr qint64 kAiToolHealthMaxLatencyMs = 86'400'000LL;  // 24h; no tool call is longer
inline constexpr qint64 kAiToolHealthMaxLedgerBytes = 8LL << 20;   // 8 MiB read cap

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
    /// Parse ONE persisted record. Strict: a present-but-wrong-typed field, an
    /// unparseable timestamp, a blank/over-long key, or a negative, fractional or
    /// out-of-range number makes the object malformed. Sets @p ok (when given) to false
    /// and returns a default record in that case -- a malformed entry must never be
    /// coerced into a healthy-looking one.
    [[nodiscard]] static AiToolHealthRecord fromJson(const QJsonObject& object, bool* ok = nullptr);
};

struct AiToolAvailability {
    /// Defaults to UNAVAILABLE: an availability that was never evaluated must not read
    /// as healthy. check() sets it true explicitly on every path that concludes no
    /// suppression applies.
    bool available{false};
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
    [[nodiscard]] static QDateTime normalizedNow(QDateTime now_utc);
    [[nodiscard]] int backoffMs(int consecutive_failures) const;
    [[nodiscard]] bool recordIsFresh(const AiToolHealthRecord& record, QDateTime now_utc) const;
    /// True when a PERSISTED record's timestamps are consistent with this ledger's own
    /// configuration: nothing dated in the future (a forged future stamp would keep the
    /// record "fresh" forever) and no disable window longer than the configured maximum
    /// backoff (this ledger never writes one, so a longer one is a tampered or corrupt
    /// file that would suppress a tool far past its own ceiling).
    [[nodiscard]] bool persistedRecordIsSane(const AiToolHealthRecord& record,
                                             QDateTime now_utc) const;
    // Unlocked cores: callers must already hold m_mutex. They exist so the
    // public methods can lock once and the record*/prune paths can persist
    // without re-locking the non-recursive mutex (which would self-deadlock).
    /// Validate and install the "records" array of a parsed ledger document. Fails
    /// closed -- m_records is left exactly as it was -- on any malformed, duplicate or
    /// out-of-bounds entry, instead of replacing the live ledger with whatever survived.
    [[nodiscard]] bool loadRecordsUnlocked(const QJsonObject& root, QString* error_message);
    /// The record to update for @p normalized_key, or nullptr when the key is unusable
    /// (blank or over-long) or the ledger is already at its record ceiling. Bounds the
    /// growth of m_records (and therefore of the snapshot and the persisted file) under
    /// model- or response-supplied keys.
    [[nodiscard]] AiToolHealthRecord* recordForUpdateUnlocked(const QString& normalized_key);
    [[nodiscard]] bool saveUnlocked(QString* error_message) const;
    [[nodiscard]] QJsonObject snapshotUnlocked() const;
    void persistIfConfigured() const;

    // Guards every access to m_records and the config fields. The ledger is
    // read on the GUI thread (run-details snapshot) while tool dispatch may
    // record success/failure from a worker thread, so all access is serialized.
    mutable QMutex m_mutex;
    QHash<QString, AiToolHealthRecord> m_records;
    int m_suppress_after_failures{kAiToolHealthDefaultSuppressAfterFailures};
    int m_base_backoff_ms{kAiToolHealthDefaultBaseBackoffMs};
    int m_max_backoff_ms{kAiToolHealthDefaultMaxBackoffMs};
    QString m_persistence_path;
    int m_ttl_hours{kAiToolHealthDefaultTtlHours};
};

}  // namespace sak::ai
