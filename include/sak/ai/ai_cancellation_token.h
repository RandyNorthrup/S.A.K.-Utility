// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <memory>

namespace sak::ai {

/// @brief Shared hierarchical cancellation token for AI runs, phases, agents, and tools.
class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] static CancellationToken createRoot(const QString& id);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] QString id() const;
    [[nodiscard]] CancellationToken createChild(const QString& id) const;
    [[nodiscard]] int childCount() const;

    void cancel(const QString& reason = QStringLiteral("cancelled")) const;

    [[nodiscard]] bool isCancellationRequested() const noexcept;
    [[nodiscard]] QString cancelReason() const;
    [[nodiscard]] QDateTime cancelledAtUtc() const;
    [[nodiscard]] QJsonObject toJson() const;

private:
    struct State {
        QString id;
        std::weak_ptr<State> parent;
        QVector<std::weak_ptr<State>> children;
        bool cancelled{false};
        QString reason;
        QDateTime cancelled_at_utc;
    };

    explicit CancellationToken(std::shared_ptr<State> state);
    static void cancelState(const std::shared_ptr<State>& state,
                            const QString& reason,
                            const QDateTime& when_utc);

    std::shared_ptr<State> m_state;
};

}  // namespace sak::ai
