// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file recycle_bin.cpp
/// @brief Shared Windows Recycle Bin helper.

#include "sak/recycle_bin.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>

#include <shellapi.h>
#endif

namespace sak {

bool sendPathToRecycleBin(const QString& path) {
#ifdef Q_OS_WIN
    // SHFileOperationW requires a native, double-null terminated path.
    std::wstring widePath = QDir::toNativeSeparators(path).toStdWString();
    widePath.push_back(L'\0');  // Extra null terminator

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = widePath.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
#else
    Q_UNUSED(path)
    return false;
#endif
}

}  // namespace sak
