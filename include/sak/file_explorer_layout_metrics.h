// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_layout_metrics.h
/// @brief Files-style layout size-kind tables and layout-cycling for the
///        File Management Explorer (clone of LayoutSizeKindHelper and
///        LayoutCycler from the Files app source).

#pragma once

#include "sak/file_explorer_types.h"

namespace sak {

/// Smallest size kind for a layout (always 1, matching the Files enums).
[[nodiscard]] int fileExplorerSizeKindMin(FileExplorerViewMode mode);

/// Largest size kind for a layout: Details/List/Columns 5, Cards 4, Grid 12.
[[nodiscard]] int fileExplorerSizeKindMax(FileExplorerViewMode mode);

/// Reads the size kind for a layout. Adaptive shares the Grid size.
[[nodiscard]] int fileExplorerSizeKind(const FileExplorerLayoutSizes& sizes,
                                       FileExplorerViewMode mode);

/// Writes the size kind for a layout, clamped to the layout's bounds.
void setFileExplorerSizeKind(FileExplorerLayoutSizes& sizes, FileExplorerViewMode mode, int kind);

/// Clamps every per-layout size kind to its bounds.
void clampFileExplorerLayoutSizes(FileExplorerLayoutSizes& sizes);

/// Row height (Details/List/Columns/Cards) or full cell height (Grid) for a
/// size kind, from the Files LayoutSizeKindHelper tables.
[[nodiscard]] int fileExplorerRowHeight(FileExplorerViewMode mode, int kind);

/// Icon edge length for a layout and size kind (LayoutSizeKindHelper.GetIconSize).
[[nodiscard]] int fileExplorerIconSize(FileExplorerViewMode mode, int kind);

/// Grid cell width for a size kind (LayoutSizeKindHelper.GetGridViewItemWidth).
[[nodiscard]] int fileExplorerGridItemWidth(int kind);

/// Next or previous layout in the Files LayoutCycler ring
/// Details -> List -> Cards -> Grid -> Columns (wrapping both ways).
[[nodiscard]] FileExplorerViewMode fileExplorerAdjacentLayout(FileExplorerViewMode mode,
                                                              bool forward);

}  // namespace sak
