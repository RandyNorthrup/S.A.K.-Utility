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

/// @brief Copy an icon's 32-bit color bitmap into a QImage, or fail closed
///
/// Every Win32 step below is checked and a QImage is only produced when the DIB was fully
/// copied. A failed GetObject/GetDC/GetDIBits, non-positive dimensions, or a partial scan-line
/// copy all fail closed (null QImage) rather than feeding uninitialized/partial pixels to
/// QPixmap. A negative height would also become a huge UINT in the GetDIBits row count, so
/// dimensions are validated as strictly positive first.
[[nodiscard]] inline QImage iconColorBitmapToImage(HBITMAP color_bitmap) {
    BITMAP bm{};
    const bool got_object = GetObject(color_bitmap, sizeof(bm), &bm) ==
                            static_cast<int>(sizeof(bm));
    const int width = bm.bmWidth;
    const int height = bm.bmHeight;
    if (!got_object || width <= 0 || height <= 0) {
        return {};
    }

    BITMAPINFOHEADER bih{};
    constexpr WORD kShieldIconBitmapBitCount = 32;
    bih.biSize = sizeof(bih);
    bih.biWidth = width;
    bih.biHeight = -height;  // top-down DIB
    bih.biPlanes = 1;
    bih.biBitCount = kShieldIconBitmapBitCount;
    bih.biCompression = BI_RGB;

    QImage buffer(width, height, QImage::Format_ARGB32_Premultiplied);
    buffer.fill(Qt::transparent);  // never leave scan lines uninitialized on a partial copy
    HDC hdc = GetDC(nullptr);
    if (hdc == nullptr) {
        return {};
    }
    const int copied = GetDIBits(hdc,
                                 color_bitmap,
                                 0,
                                 static_cast<UINT>(height),
                                 buffer.bits(),
                                 reinterpret_cast<BITMAPINFO*>(&bih),  // NOLINT
                                 DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (copied != height) {
        return {};
    }
    return buffer;
}

/// @brief Extract the Windows UAC shield icon as a QIcon
///
/// Uses SHGetStockIconInfo to retrieve the system's shield icon at the
/// small-icon size (typically 16x16). Returns an empty QIcon on failure.
///
/// The result is cached internally -- safe to call multiple times.
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

    // Convert HICON -> QPixmap via DIB extraction
    ICONINFO icon_info{};
    if (!GetIconInfo(sii.hIcon, &icon_info)) {
        DestroyIcon(sii.hIcon);
        return {};
    }

    QImage image = iconColorBitmapToImage(icon_info.hbmColor);

    DeleteObject(icon_info.hbmColor);
    DeleteObject(icon_info.hbmMask);
    DestroyIcon(sii.hIcon);

    if (image.isNull()) {
        return {};  // fail closed: extraction failed, do not cache/publish partial pixels
    }

    cached = QIcon(QPixmap::fromImage(std::move(image)));
    return cached;
}

}  // namespace sak
