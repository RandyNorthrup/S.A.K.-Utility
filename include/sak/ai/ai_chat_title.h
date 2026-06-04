// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>

namespace sak::ai {

inline constexpr qsizetype kGeneratedChatTitleMaxChars = 48;

[[nodiscard]] QString chatTitleFromFirstPrompt(const QString& prompt,
                                               const QString& workflow_title = {});
[[nodiscard]] bool isDefaultChatTitle(const QString& title);

}  // namespace sak::ai
