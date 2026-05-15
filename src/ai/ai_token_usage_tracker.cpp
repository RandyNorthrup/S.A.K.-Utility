// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_token_usage_tracker.h"

#include <QLocale>

namespace sak::ai {

namespace {

[[nodiscard]] qint64 jsonInt64(const QJsonObject& obj, const QString& key) noexcept {
    const auto value = obj.value(key);
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    return 0;
}

[[nodiscard]] QString n(qint64 value) {
    return QLocale().toString(value);
}

}  // namespace

void TokenUsageTracker::reset() noexcept {
    m_last_turn = {};
    m_session_total = {};
}

void TokenUsageTracker::addTurn(const TokenUsage& usage) noexcept {
    m_last_turn = usage;
    m_session_total.input_tokens += usage.input_tokens;
    m_session_total.cached_input_tokens += usage.cached_input_tokens;
    m_session_total.output_tokens += usage.output_tokens;
    m_session_total.reasoning_tokens += usage.reasoning_tokens;
    m_session_total.total_tokens += usage.total_tokens;
}

TokenUsage TokenUsageTracker::fromJson(const QJsonObject& usage_json) noexcept {
    TokenUsage usage;
    usage.input_tokens = jsonInt64(usage_json, QStringLiteral("input_tokens"));
    usage.output_tokens = jsonInt64(usage_json, QStringLiteral("output_tokens"));
    usage.total_tokens = jsonInt64(usage_json, QStringLiteral("total_tokens"));

    const auto input_details = usage_json.value(QStringLiteral("input_tokens_details")).toObject();
    usage.cached_input_tokens = jsonInt64(input_details, QStringLiteral("cached_tokens"));

    const auto output_details =
        usage_json.value(QStringLiteral("output_tokens_details")).toObject();
    usage.reasoning_tokens = jsonInt64(output_details, QStringLiteral("reasoning_tokens"));

    return usage;
}

QString TokenUsageTracker::formatTurn(const TokenUsage& usage) {
    return QStringLiteral("%1 in / %2 cached / %3 out / %4 reasoning / %5 total")
        .arg(n(usage.input_tokens),
             n(usage.cached_input_tokens),
             n(usage.output_tokens),
             n(usage.reasoning_tokens),
             n(usage.total_tokens));
}

QString TokenUsageTracker::formatSession(const TokenUsage& usage) {
    return QStringLiteral("%1 total (%2 in, %3 out)")
        .arg(n(usage.total_tokens), n(usage.input_tokens), n(usage.output_tokens));
}

}  // namespace sak::ai
