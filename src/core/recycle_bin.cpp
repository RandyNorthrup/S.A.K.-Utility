// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file recycle_bin.cpp
/// @brief Shared Windows Recycle Bin helper.

#include "sak/recycle_bin.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>

#include <shellapi.h>
#endif

namespace sak {

bool pathVolumeHasRecycleBin(const QString& path) {
#ifdef Q_OS_WIN
    if (path.isEmpty()) {
        return false;
    }
    // The "\\" native prefix covers UNC shares ("\\server\share") and the device
    // namespaces ("\\?\", "\\.\"); none of them has a Recycle Bin the shell will
    // use. Checked on the RAW string first -- before any path resolution or volume
    // call -- so an unreachable host can never stall this on a network round trip.
    if (QDir::toNativeSeparators(path).startsWith(QLatin1String("\\\\"))) {
        return false;
    }
    // Re-check after resolution: a relative path can still resolve onto a UNC (or
    // device) root, which the raw-string check cannot see.
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QLatin1String("\\\\"))) {
        return false;
    }
    // Resolve the volume the path actually lives on (a mount point under C:\ can
    // be a different volume than C:), then require a fixed volume -- the proxy the
    // leftover-cleanup recycle gate already relies on for "has a Recycle Bin".
    wchar_t volume_root[MAX_PATH] = {};
    if (GetVolumePathNameW(reinterpret_cast<LPCWSTR>(native.utf16()), volume_root, MAX_PATH) == 0) {
        return false;
    }
    return GetDriveTypeW(volume_root) == DRIVE_FIXED;
#else
    Q_UNUSED(path)
    return false;
#endif
}

bool sendPathToRecycleBin(const QString& path) {
#ifdef Q_OS_WIN
    // Guard the shell call before it runs. SHFileOperationW EXPANDS wildcards in
    // pFrom, so a '*' or '?' would recycle every match rather than the literal
    // path; an embedded NUL would split pFrom's double-null list into extra
    // unintended targets; and an empty path has an undefined target. Refuse all
    // three -- a mass/erroneous recycle from a crafted or garbled path (B8-23).
    if (path.isEmpty() || path.contains(QLatin1Char('*')) || path.contains(QLatin1Char('?')) ||
        path.contains(QChar(QChar::Null))) {
        return false;
    }

    // Fail closed on a volume with no Recycle Bin. FOF_ALLOWUNDO silently degrades
    // to a PERMANENT delete there, so a "recycle" would destroy the item while the
    // caller reported a recoverable move to the bin; refuse and let the caller ask
    // for an explicit permanent delete instead (R5 p9_filemgmt-7).
    if (!pathVolumeHasRecycleBin(path)) {
        return false;
    }

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
