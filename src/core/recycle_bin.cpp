// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file recycle_bin.cpp
/// @brief Shared Windows Recycle Bin helper.

#include "sak/recycle_bin.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>

#ifdef Q_OS_WIN
#include <windows.h>

#include <shellapi.h>
#endif

namespace sak {

namespace {
#ifdef Q_OS_WIN

// True only when the shell reports a USABLE Recycle Bin for @p volume_root. DRIVE_FIXED is
// necessary but NOT sufficient: a fixed volume whose bin is disabled by policy still looks
// fixed, yet FOF_ALLOWUNDO would then silently and PERMANENTLY delete. SHQueryRecycleBinW is
// the authority; ANY failure fails closed. Cached per volume root for the process run -- the
// query can enumerate the bin and policy does not change mid-run -- and the mutex guards the
// worker threads that share this predicate (R5-G22-4).
bool volumeRecycleBinIsUsable(LPCWSTR volume_root) {
    static QMutex cache_mutex;
    static QHash<QString, bool> cache;
    const QString key = QString::fromWCharArray(volume_root).toLower();
    const QMutexLocker locker(&cache_mutex);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) {
        return cached.value();
    }
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    const bool usable = SUCCEEDED(SHQueryRecycleBinW(volume_root, &info));
    cache.insert(key, usable);
    return usable;
}

#endif  // Q_OS_WIN
}  // namespace

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
    // be a different volume than C:), then require a fixed volume: removable, optical
    // and network volumes have no Recycle Bin the shell will use.
    wchar_t volume_root[MAX_PATH] = {};
    if (GetVolumePathNameW(reinterpret_cast<LPCWSTR>(native.utf16()), volume_root, MAX_PATH) == 0) {
        return false;
    }
    if (GetDriveTypeW(volume_root) != DRIVE_FIXED) {
        return false;
    }
    // DRIVE_FIXED alone would still pass a fixed volume whose Recycle Bin is disabled by
    // policy, where FOF_ALLOWUNDO silently becomes a permanent delete; ask the shell for
    // the volume's actual bin and fail closed when it has none.
    return volumeRecycleBinIsUsable(volume_root);
#else
    Q_UNUSED(path)
    return false;
#endif
}

namespace {

// Report the REAL reason for a refusal instead of collapsing every path to a bare false.
void setRecycleError(QString* error, const QString& reason) {
    if (error != nullptr) {
        *error = reason;
    }
}

#ifdef Q_OS_WIN

// Decide whether the RAW path may reach the shell at all; empty means it may.
QString recycleInputRefusal(const QString& path) {
    // SHFileOperationW EXPANDS wildcards in pFrom, so a '*' or '?' would recycle
    // every match rather than the literal path; an embedded NUL would split pFrom's
    // double-null list into extra unintended targets; and an empty path has an
    // undefined target. Refuse all three -- a mass/erroneous recycle from a crafted
    // or garbled path (B8-23).
    if (path.isEmpty() || path.contains(QLatin1Char('*')) || path.contains(QLatin1Char('?')) ||
        path.contains(QChar(QChar::Null))) {
        return QStringLiteral(
            "Refusing to recycle a path that is empty or carries a "
            "wildcard or embedded NUL.");
    }
    return {};
}

// Decide whether the RESOLVED target may be recycled; empty means it may.
QString recycleTargetRefusal(const QString& path, const QString& absolute) {
    // SHFileOperationW IGNORES FOF_ALLOWUNDO when pFrom is not fully qualified: a relative or
    // drive-relative spelling would be PERMANENTLY deleted while this reported a recoverable
    // recycle, and it would also resolve against the process-wide current directory, which
    // another thread can change under us.
    if (absolute.isEmpty() || !QDir::isAbsolutePath(absolute)) {
        return QStringLiteral(
                   "Cannot resolve '%1' to a fully qualified path; refusing "
                   "to recycle it.")
            .arg(path);
    }

    // Fail closed on a volume with no Recycle Bin. FOF_ALLOWUNDO silently degrades
    // to a PERMANENT delete there, so a "recycle" would destroy the item while the
    // caller reported a recoverable move to the bin; refuse and let the caller ask
    // for an explicit permanent delete instead (R5 p9_filemgmt-7).
    if (!pathVolumeHasRecycleBin(absolute)) {
        return QStringLiteral(
                   "'%1' is not on a volume with a Recycle Bin; refusing to "
                   "turn a recycle into a permanent delete.")
            .arg(absolute);
    }
    return {};
}

// Base used to format the SHFileOperation error code as hexadecimal for the message.
constexpr int kHexBase = 16;

// Decide whether the finished shell call really delivered the item; empty means it did.
QString recycleOutcomeError(const SHFILEOPSTRUCTW& op, int shell_result, const QString& absolute) {
    if (shell_result != 0) {
        return QStringLiteral(
                   "The shell refused to recycle '%1' (SHFileOperation "
                   "error 0x%2).")
            .arg(absolute, QString::number(shell_result, kHexBase));
    }
    // A user or the shell can abort mid-flight with a zero return, so the item may never
    // have reached the bin even though the call "succeeded".
    if (op.fAnyOperationsAborted != 0) {
        return QStringLiteral("The recycle of '%1' was aborted before it completed.").arg(absolute);
    }
    return {};
}

#endif  // Q_OS_WIN

}  // namespace

bool sendPathToRecycleBin(const QString& path, QString* error) {
    setRecycleError(error, QString());
#ifdef Q_OS_WIN
    // Guard the shell call before it runs.
    const QString input_refusal = recycleInputRefusal(path);
    if (!input_refusal.isEmpty()) {
        setRecycleError(error, input_refusal);
        return false;
    }

    // Resolve ONCE here and operate on that exact string, so the target gate below and the
    // shell call target the same object.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString target_refusal = recycleTargetRefusal(path, absolute);
    if (!target_refusal.isEmpty()) {
        setRecycleError(error, target_refusal);
        return false;
    }

    // SHFileOperationW requires a native, double-null terminated path.
    std::wstring widePath = QDir::toNativeSeparators(absolute).toStdWString();
    widePath.push_back(L'\0');  // Extra null terminator

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = widePath.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int shell_result = SHFileOperationW(&op);
    const QString outcome_error = recycleOutcomeError(op, shell_result, absolute);
    if (!outcome_error.isEmpty()) {
        setRecycleError(error, outcome_error);
        return false;
    }
    return true;
#else
    Q_UNUSED(path)
    setRecycleError(error, QStringLiteral("The Recycle Bin is only available on Windows."));
    return false;
#endif
}

}  // namespace sak
