// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file shield_icon.h
/// @brief Windows UAC shield icon extraction for elevated-action buttons
///
/// Phase 4 UX Polish: adds the standard Windows shield overlay to any
/// button or action that will trigger a UAC prompt.

#include <QIcon>
#include <QImage>
#include <QPixmap>

#include <windows.h>

#include <shellapi.h>

namespace sak {

/// @brief Extract the Windows UAC shield icon as a QIcon
///
/// Uses SHGetStockIconInfo to retrieve the system's shield icon at the
/// small-icon size (typically 16x16). Returns an empty QIcon on failure.
///
/// The result is cached internally — safe to call multiple times.
[[nodiscard]] inline QIcon getShieldIcon() {
    static QIcon cached;
    if (!cached.isNull()) {
        return cached;
    }

    SHSTOCKICONINFO sii{};
    sii.cbSize = sizeof(sii);

    HRESULT hr = SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &sii);
    if (FAILED(hr)) {
        return {};
    }

    // Convert HICON → QPixmap via DIB extraction
    ICONINFO icon_info{};
    if (!GetIconInfo(sii.hIcon, &icon_info)) {
        DestroyIcon(sii.hIcon);
        return {};
    }

    BITMAP bm{};
    GetObject(icon_info.hbmColor, sizeof(bm), &bm);

    const int width = bm.bmWidth;
    const int height = bm.bmHeight;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = width;
    bih.biHeight = -height;  // top-down DIB
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    HDC hdc = GetDC(nullptr);
    GetDIBits(hdc,
              icon_info.hbmColor,
              0,
              static_cast<UINT>(height),
              image.bits(),
              reinterpret_cast<BITMAPINFO*>(&bih),  // NOLINT
              DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    DeleteObject(icon_info.hbmColor);
    DeleteObject(icon_info.hbmMask);
    DestroyIcon(sii.hIcon);

    cached = QIcon(QPixmap::fromImage(std::move(image)));
    return cached;
}

}  // namespace sak
