// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_style.h
/// @brief Files-community-inspired style sheet for the File Explorer shell.

#pragma once

#include <QString>

namespace sak::ui {

/// Returns the explorer-scoped style sheet (subtle toolbar buttons, breadcrumb
/// address row, top tab strip, sectioned sidebar, borderless item views, and
/// bottom status row). Colors resolve through palette() references, so one
/// sheet serves both the light and dark application themes.
[[nodiscard]] QString fileExplorerShellStyleSheet();

}  // namespace sak::ui
