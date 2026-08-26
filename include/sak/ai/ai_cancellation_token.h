// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>
#include <mutex>

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
        QString id;  // set once at construction, never mutated afterwards
        // STRONG, deliberately: a child keeps its whole ancestor chain alive. The links are
        // asymmetric -- parent-to-child is weak, child-to-parent is strong -- so there is no
        // cycle, and a state dies once no descendant token refers to it.
        //
        // It was weak, and that silently broke cancellation. Root -> child -> grandchild, with
        // the caller holding only the grandchild token: the intermediate state was destroyed the
        // moment its token went out of scope, the root's weak link to it expired, and cancel() on
        // the root then walked past the expired entry and never reached the LIVE grandchild. The
        // work the user cancelled kept running, and nothing reported it.
        std::shared_ptr<State> parent;
        // The token is shared across the UI thread (cancel) and worker threads
        // (createChild/isCancellationRequested), so mutable state is synchronized:
        // cancelled is atomic for lock-free polling; mutex guards the rest.
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        QVector<std::weak_ptr<State>> children;
        // Monotonic, never reused, and NOT derived from children.size(): expired children are
        // pruned from that vector, so sizing an id off it would hand two live children the same
        // generated name once anything had been collected.
        int next_child_index{0};
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
