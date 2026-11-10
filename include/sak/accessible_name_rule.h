// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file accessible_name_rule.h
/// @brief The rule the start-up accessibility audit holds every button to.

#include <QString>
#include <QStringView>

namespace sak {

/// @brief Whether a button's accessible name holds the words on the button, ignoring case, its
///        access-key marker, and any symbols around them.
///
/// Someone using voice control says what they can see. A name that drops those words, however
/// descriptive, leaves them nothing to say. It may add to them: "Stop HTTP speed test" for a button
/// reading Stop is fine, and tells two Stop buttons apart. A label of symbols alone has no words
/// to hold. Pure.
[[nodiscard]] inline bool accessibleNameHoldsLabel(const QString& accessible_name,
                                                   const QString& label) {
    QString visible = label;
    visible.remove(QLatin1Char('&'));
    qsizetype begin = 0;
    qsizetype end = visible.size();
    while (begin < end && !visible.at(begin).isLetterOrNumber()) {
        ++begin;
    }
    while (end > begin && !visible.at(end - 1).isLetterOrNumber()) {
        --end;
    }
    return begin == end || accessible_name.contains(QStringView(visible).mid(begin, end - begin),
                                                    Qt::CaseInsensitive);
}

}  // namespace sak
