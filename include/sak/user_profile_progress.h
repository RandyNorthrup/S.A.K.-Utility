// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/format_utils.h"
#include "sak/layout_constants.h"

#include <QCoreApplication>
#include <QString>

#include <algorithm>

namespace sak {

/// Presentation-ready state for backup/restore aggregate byte progress.
struct UserProfileProgressState {
    int minimum{0};
    int maximum{0};
    int value{0};
    qint64 processed_bytes{0};
    qint64 total_bytes{0};
    int completed_users{0};
    int total_users{0};
    QString format;
    QString accessible_description;
};

/// Calculate a bounded percentage without overflowing a 64-bit byte count.
[[nodiscard]] inline int boundedProgressPercent(qint64 current, qint64 total) {
    if (total <= 0) {
        return 0;
    }
    const qint64 bounded = std::clamp(current, qint64{0}, total);
    const qint64 whole_percent = total / kPercentMax;
    const qint64 remainder = total % kPercentMax;
    int percent = 0;
    while (percent < kPercentMax) {
        const int next_percent = percent + 1;
        const qint64 threshold = whole_percent * next_percent +
                                 ((remainder * next_percent + (kPercentMax - 1)) / kPercentMax);
        if (bounded < threshold) {
            break;
        }
        percent = next_percent;
    }
    return percent;
}

/// Build one shared progress state for both profile backup and restore UIs.
///
/// Byte progress is authoritative when its total is known. A zero/unknown byte total falls back
/// to completed-user progress; when neither total is known the bar remains indeterminate while its
/// accessible text still reports bytes processed.
[[nodiscard]] inline UserProfileProgressState userProfileProgressState(int completed_users,
                                                                       int total_users,
                                                                       qint64 processed_bytes,
                                                                       qint64 total_bytes) {
    UserProfileProgressState state;
    state.processed_bytes = std::max(processed_bytes, qint64{0});
    state.total_bytes = std::max(total_bytes, qint64{0});
    state.total_users = std::max(total_users, 0);
    state.completed_users = std::clamp(completed_users, 0, state.total_users);

    if (state.total_bytes > 0) {
        state.maximum = kPercentMax;
        state.value = boundedProgressPercent(state.processed_bytes, state.total_bytes);
        state.format = QCoreApplication::translate("UserProfileProgress", "%1% - %2 / %3")
                           .arg(state.value)
                           .arg(formatBytes(state.processed_bytes), formatBytes(state.total_bytes));
    } else if (state.total_users > 0) {
        state.maximum = kPercentMax;
        state.value = boundedProgressPercent(state.completed_users, state.total_users);
        state.format = QCoreApplication::translate("UserProfileProgress",
                                                   "%1% - %2 / %3 users - %4 processed")
                           .arg(state.value)
                           .arg(state.completed_users)
                           .arg(state.total_users)
                           .arg(formatBytes(state.processed_bytes));
    } else {
        state.format = QCoreApplication::translate("UserProfileProgress",
                                                   "%1 processed - total size unknown")
                           .arg(formatBytes(state.processed_bytes));
    }

    state.accessible_description = QCoreApplication::translate("UserProfileProgress",
                                                               "%1. %2 of %3 users complete.")
                                       .arg(state.format)
                                       .arg(state.completed_users)
                                       .arg(state.total_users);
    return state;
}

}  // namespace sak
