// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_run_state.h"

#include <QString>

namespace sak::ai {

/// @brief Persists a full AiRunState snapshot to an AI session folder.
///
/// Trace events capture per-event detail; this store captures the durable
/// snapshot of "what the run currently looks like" so cancellation, recovery,
/// or a re-opened session can render the last known phase/agent/tool counts.
class AiRunStateStore {
public:
    explicit AiRunStateStore(QString session_dir = {});

    void setSessionDirectory(const QString& session_dir);
    [[nodiscard]] QString sessionDirectory() const;
    [[nodiscard]] QString runStatePath() const;

    [[nodiscard]] bool saveSnapshot(const AiRunState& state,
                                    QString* error_message = nullptr) const;
    [[nodiscard]] AiRunState loadSnapshot(QString* error_message = nullptr) const;
    [[nodiscard]] bool clearSnapshot(QString* error_message = nullptr) const;

private:
    QString m_session_dir;
};

}  // namespace sak::ai
