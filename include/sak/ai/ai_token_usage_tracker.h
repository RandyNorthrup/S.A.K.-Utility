// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

namespace sak::ai {

struct TokenUsage {
    qint64 input_tokens{0};
    qint64 cached_input_tokens{0};
    qint64 output_tokens{0};
    qint64 reasoning_tokens{0};
    qint64 total_tokens{0};

    [[nodiscard]] bool isEmpty() const noexcept { return total_tokens == 0; }
};

class TokenUsageTracker {
public:
    void reset() noexcept;
    void addTurn(const TokenUsage& usage) noexcept;

    [[nodiscard]] const TokenUsage& lastTurn() const noexcept { return m_last_turn; }
    [[nodiscard]] const TokenUsage& sessionTotal() const noexcept { return m_session_total; }

    [[nodiscard]] static TokenUsage fromJson(const QJsonObject& usage_json) noexcept;
    [[nodiscard]] static QString formatTurn(const TokenUsage& usage);
    [[nodiscard]] static QString formatSession(const TokenUsage& usage);

private:
    TokenUsage m_last_turn;
    TokenUsage m_session_total;
};

}  // namespace sak::ai
