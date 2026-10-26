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

// Hard ceilings on what a caller (ultimately a model- or tool-server-supplied key) can put
// into this ledger. A key outside them is not tracked; counters and latency saturate.
inline constexpr int kAiToolHealthMaxRecords = 4096;         // the real tool/provider count is ~100
inline constexpr int kAiToolHealthMaxKeyChars = 256;
inline constexpr int kAiToolHealthMaxCount = 1'000'000'000;  // counters stay far from INT_MAX
inline constexpr qint64 kAiToolHealthMaxLatencyMs = 86'400'000LL;  // 24h; no tool call is longer

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

/// Per-session tool/provider circuit breaker. It lives in memory only: the owner ruled
/// (2026-10-24) that a tool which failed in one chat session must not stay suppressed in
/// the next one or after a restart, so nothing here is ever written to disk. The panel
/// calls reset() when the technician starts or opens a different chat session.
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
    /// Forget every record: a new chat session starts with every tool available.
    void reset();

    [[nodiscard]] AiToolHealthRecord record(const QString& key) const;
    [[nodiscard]] QJsonObject snapshot() const;
    [[nodiscard]] int size() const;

    [[nodiscard]] static QString classifyResult(const QJsonObject& result);

private:
    [[nodiscard]] static QDateTime normalizedNow(QDateTime now_utc);
    [[nodiscard]] int backoffMs(int consecutive_failures) const;
    /// The record to update for @p normalized_key, or nullptr when the key is unusable
    /// (blank or over-long) or the ledger is already at its record ceiling. Bounds the
    /// growth of m_records (and therefore of the snapshot) under model- or
    /// response-supplied keys. Callers must already hold m_mutex.
    [[nodiscard]] AiToolHealthRecord* recordForUpdateUnlocked(const QString& normalized_key);

    // Guards every access to m_records. The ledger is read on the GUI thread (run-details
    // snapshot) while tool dispatch may record success/failure from a worker thread, so
    // all access is serialized.
    mutable QMutex m_mutex;
    QHash<QString, AiToolHealthRecord> m_records;
    int m_suppress_after_failures{kAiToolHealthDefaultSuppressAfterFailures};
    int m_base_backoff_ms{kAiToolHealthDefaultBaseBackoffMs};
    int m_max_backoff_ms{kAiToolHealthDefaultMaxBackoffMs};
};

}  // namespace sak::ai
