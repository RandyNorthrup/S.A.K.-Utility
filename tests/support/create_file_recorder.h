// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file create_file_recorder.h
/// @brief Records the flags each CreateFileW call in the test binary passed, by path.
///
/// A check that reads back what was just written has to bypass Windows' file cache, and nothing a
/// test can see in the bytes says whether it did: the cache holds the same bytes. What does say is
/// the flags the handle was opened with. This stands in for CreateFileW in the test binary's own
/// import table, where the product code linked into it calls it, the same way
/// test_hardware_guard.cpp does (and passes each call on to the guard), and puts it back when it
/// goes out of scope. Only one may exist at a time.

#pragma once

#include "test_hardware_guard.h"

#include <QDir>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <cstdint>
#include <cstring>
#include <tuple>

#include <windows.h>

namespace sak::test {

class CreateFileRecorder {
public:
    struct Open {
        QString path;  ///< as asked for, without a \\?\ prefix, in native separators
        DWORD flags;   ///< dwFlagsAndAttributes
    };

    CreateFileRecorder() {
        {
            const QMutexLocker lock(&mutex());
            opens().clear();
        }
        visitImports(GetModuleHandleW(nullptr), install);
    }
    ~CreateFileRecorder() { visitImports(GetModuleHandleW(nullptr), remove); }

    CreateFileRecorder(const CreateFileRecorder&) = delete;
    CreateFileRecorder& operator=(const CreateFileRecorder&) = delete;
    CreateFileRecorder(CreateFileRecorder&&) = delete;
    CreateFileRecorder& operator=(CreateFileRecorder&&) = delete;

    /// Every open of @p path so far (compared without case, in native separators).
    [[nodiscard]] static QList<Open> opensOf(const QString& path) {
        const QString wanted = QDir::toNativeSeparators(path);
        QList<Open> found;
        const QMutexLocker lock(&mutex());
        for (const Open& open : opens()) {
            if (open.path.compare(wanted, Qt::CaseInsensitive) == 0) {
                found.append(open);
            }
        }
        return found;
    }

    /// True when @p path was opened at least once and every open bypassed the file cache.
    [[nodiscard]] static bool readOnlyUnbuffered(const QString& path) {
        const QList<Open> found = opensOf(path);
        if (found.isEmpty()) {
            return false;
        }
        for (const Open& open : found) {
            if ((open.flags & FILE_FLAG_NO_BUFFERING) == 0) {
                return false;
            }
        }
        return true;
    }

private:
    using CreateFile = decltype(&CreateFileW);

    /// The path as it was asked for, without the \\?\ prefix a long-path open adds.
    static QString plainPath(LPCWSTR name) {
        const QString path = QString::fromWCharArray(name);
        const QString unc = QStringLiteral("\\\\?\\UNC\\");
        const QString local = QStringLiteral("\\\\?\\");
        if (path.startsWith(unc)) {
            return QStringLiteral("\\\\") + path.mid(unc.size());
        }
        return path.startsWith(local) ? path.mid(local.size()) : path;
    }
    // The rest of the signature is Windows' own and passes straight through, as the hardware
    // guard's does; the flags are its third item, after the security attributes and disposition.
    template <typename... Rest>
    static HANDLE WINAPI
    recordingCreateFileW(LPCWSTR name, DWORD access, DWORD share, Rest... rest) {
        if (name != nullptr) {
            const DWORD flags = std::get<2>(std::tuple<Rest...>(rest...));
            const QMutexLocker lock(&mutex());
            opens().append({QDir::toNativeSeparators(plainPath(name)), flags});
        }
        return s_previous(name, access, share, rest...);
    }
    static void install(const char* name, std::uintptr_t* slot) {
        if (std::strcmp(name, "CreateFileW") == 0) {
            s_previous = reinterpret_cast<CreateFile>(*slot);
            const CreateFile recording = recordingCreateFileW;
            setImportSlot(slot, reinterpret_cast<std::uintptr_t>(recording));
        }
    }
    static void remove(const char* name, std::uintptr_t* slot) {
        if (std::strcmp(name, "CreateFileW") == 0) {
            setImportSlot(slot, reinterpret_cast<std::uintptr_t>(s_previous));
        }
    }
    static QMutex& mutex() {
        static QMutex lock;
        return lock;
    }
    static QList<Open>& opens() {
        static QList<Open> list;
        return list;
    }

    static inline CreateFile s_previous = nullptr;
};

}  // namespace sak::test
