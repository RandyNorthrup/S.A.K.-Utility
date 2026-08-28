// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_layout_metrics.cpp
/// @brief Size-kind tables and layout cycling cloned from the Files app
///        (Helpers/Layout/LayoutSizeKindHelper.cs, Actions/Display/LayoutAction.cs).

#include "sak/file_explorer_layout_metrics.h"

#include <algorithm>
#include <array>

namespace sak {

namespace {

// Files LayoutSizeKindHelper.GetDetailsViewRowHeight: Compact..ExtraLarge.
constexpr std::array<int, 5> kDetailsRowHeights{28, 36, 40, 44, 48};
// GetListViewRowHeight and GetColumnsViewRowHeight share one table.
constexpr std::array<int, 5> kListRowHeights{24, 32, 36, 40, 44};
// GetGridViewItemWidth: kinds 1..12.
constexpr std::array<int, 12> kGridItemWidths{
    80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300};
// GetIconSize for Details/List/Columns: Compact and Small are
// Constants.ShellIconSizes.Small (16), then 20, 24, and -- at ExtraLarge --
// Constants.ShellIconSizes.LARGE, which upstream is 32, NOT ShellIconSizes
// .ExtraLarge (48). This table said 48, so the biggest Details row (48px tall)
// was filled edge to edge by its own icon with no vertical breathing room,
// where Files leaves 8px above and below.
constexpr std::array<int, 5> kCompactLayoutIconSizes{16, 16, 20, 24, 32};
// GetIconSize for Cards: Small and Medium 64, Large 80, ExtraLarge 96.
constexpr std::array<int, 4> kCardsIconSizes{64, 64, 80, 96};
// Files GridLayoutPage grid cells stack the square thumbnail over a fixed
// 60px name block (GridViewBrowserTemplate ItemName Height=60).
constexpr int kGridLabelHeight = 60;
// Files CardsBrowserTemplate composes an icon box and a details box per size
// kind (GridLayoutPage.xaml.cs); the list row must fit the taller of the two.
constexpr std::array<int, 4> kCardsRowHeights{104, 144, 144, 160};

// Maximum valid size kind for each layout family: the highest 1-based index
// into that family's metric table, i.e. the table's entry count.
constexpr int kDetailsRowKindMax = static_cast<int>(kDetailsRowHeights.size());
constexpr int kListRowKindMax = static_cast<int>(kListRowHeights.size());
constexpr int kCardsRowKindMax = static_cast<int>(kCardsRowHeights.size());
constexpr int kCardsIconKindMax = static_cast<int>(kCardsIconSizes.size());
constexpr int kCompactIconKindMax = static_cast<int>(kCompactLayoutIconSizes.size());
constexpr int kGridItemKindMax = static_cast<int>(kGridItemWidths.size());

// Files LayoutSizeKindHelper.GetIconSize for GridView: kind 1 -> 96px,
// kinds 2..8 -> 128px, kinds 9..12 -> 256px.
constexpr int kGridIconSizeSmall = 96;
constexpr int kGridIconSizeMedium = 128;
constexpr int kGridIconSizeLarge = 256;
constexpr int kGridIconMediumMaxKind = 8;

// Files LayoutCycler order (LayoutAction.cs): Details, List, Cards, Grid,
// Columns, wrapping in both directions. Adaptive resolves as Grid.
constexpr std::array<FileExplorerViewMode, 5> kLayoutRing{FileExplorerViewMode::Details,
                                                          FileExplorerViewMode::List,
                                                          FileExplorerViewMode::Cards,
                                                          FileExplorerViewMode::Grid,
                                                          FileExplorerViewMode::Columns};

[[nodiscard]] FileExplorerViewMode effectiveMode(const FileExplorerViewMode mode) {
    return mode == FileExplorerViewMode::Adaptive ? FileExplorerViewMode::Grid : mode;
}

[[nodiscard]] int tableAt(const int kind,
                          const int min_kind,
                          const int max_kind,
                          const int* table) {
    return table[std::clamp(kind, min_kind, max_kind) - 1];
}

}  // namespace

int fileExplorerSizeKindMin(const FileExplorerViewMode mode) {
    Q_UNUSED(mode);
    return 1;
}

int fileExplorerSizeKindMax(const FileExplorerViewMode mode) {
    switch (effectiveMode(mode)) {
    case FileExplorerViewMode::Cards:
        return static_cast<int>(kCardsIconSizes.size());
    case FileExplorerViewMode::Grid:
        return static_cast<int>(kGridItemWidths.size());
    default:
        return static_cast<int>(kDetailsRowHeights.size());
    }
}

int fileExplorerSizeKind(const FileExplorerLayoutSizes& sizes, const FileExplorerViewMode mode) {
    switch (effectiveMode(mode)) {
    case FileExplorerViewMode::List:
        return sizes.list;
    case FileExplorerViewMode::Grid:
        return sizes.grid;
    case FileExplorerViewMode::Cards:
        return sizes.cards;
    case FileExplorerViewMode::Columns:
        return sizes.columns;
    default:
        return sizes.details;
    }
}

void setFileExplorerSizeKind(FileExplorerLayoutSizes& sizes,
                             const FileExplorerViewMode mode,
                             const int kind) {
    const int clamped =
        std::clamp(kind, fileExplorerSizeKindMin(mode), fileExplorerSizeKindMax(mode));
    switch (effectiveMode(mode)) {
    case FileExplorerViewMode::List:
        sizes.list = clamped;
        break;
    case FileExplorerViewMode::Grid:
        sizes.grid = clamped;
        break;
    case FileExplorerViewMode::Cards:
        sizes.cards = clamped;
        break;
    case FileExplorerViewMode::Columns:
        sizes.columns = clamped;
        break;
    default:
        sizes.details = clamped;
        break;
    }
}

void clampFileExplorerLayoutSizes(FileExplorerLayoutSizes& sizes) {
    for (const FileExplorerViewMode mode : kLayoutRing) {
        setFileExplorerSizeKind(sizes, mode, fileExplorerSizeKind(sizes, mode));
    }
}

int fileExplorerRowHeight(const FileExplorerViewMode mode, const int kind) {
    switch (effectiveMode(mode)) {
    case FileExplorerViewMode::Details:
        return tableAt(kind, 1, kDetailsRowKindMax, kDetailsRowHeights.data());
    case FileExplorerViewMode::Cards:
        return tableAt(kind, 1, kCardsRowKindMax, kCardsRowHeights.data());
    case FileExplorerViewMode::Grid:
        return fileExplorerGridItemWidth(kind) + kGridLabelHeight;
    default:
        return tableAt(kind, 1, kListRowKindMax, kListRowHeights.data());
    }
}

int fileExplorerIconSize(const FileExplorerViewMode mode, const int kind) {
    switch (effectiveMode(mode)) {
    case FileExplorerViewMode::Cards:
        return tableAt(kind, 1, kCardsIconKindMax, kCardsIconSizes.data());
    case FileExplorerViewMode::Grid:
        // GetIconSize: GridView kind 1 -> 96, kinds 2..8 -> 128, 9..12 -> 256.
        if (kind <= 1) {
            return kGridIconSizeSmall;
        }
        return kind <= kGridIconMediumMaxKind ? kGridIconSizeMedium : kGridIconSizeLarge;
    default:
        return tableAt(kind, 1, kCompactIconKindMax, kCompactLayoutIconSizes.data());
    }
}

int fileExplorerGridItemWidth(const int kind) {
    return tableAt(kind, 1, kGridItemKindMax, kGridItemWidths.data());
}

FileExplorerViewMode fileExplorerAdjacentLayout(const FileExplorerViewMode mode,
                                                const bool forward) {
    const FileExplorerViewMode current = effectiveMode(mode);
    const auto it = std::ranges::find(kLayoutRing, current);
    const auto position = static_cast<int>(it - kLayoutRing.begin());
    const auto count = static_cast<int>(kLayoutRing.size());
    const int next = (((position + (forward ? 1 : -1)) % count) + count) % count;
    return kLayoutRing.at(static_cast<size_t>(next));
}

}  // namespace sak
