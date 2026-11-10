// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

/// @file app_action_guards.h
/// @brief Path guards shared by every AI-assistant app-action module.
///
/// These live in one header (not duplicated per module) on purpose: they are
/// security-critical (prompt-injection defenses) and MUST NOT drift between the
/// read-only and mutating op modules. Both include this and call the same code.
namespace sak {

/// A UNC / device-namespace root begins with two leading separators, so a path
/// must be at least this long to carry that double-separator prefix.
inline constexpr qsizetype kDoubleSeparatorPrefixLength = 2;

/// True if @p path begins with two separators of any kind (\\, //, \/, /\).
///
/// Windows treats ANY two leading separators as a UNC / device namespace root
/// (\\server\share, \\.\PhysicalDrive0, \\?\...). An ungated or model-steered op
/// must never be pointed at such a path: even a bare QFileInfo::exists() on a UNC
/// path triggers an SMB/NTLM handshake to an attacker-controlled host and leaks
/// the user's credential hash, and a device path bypasses normal file semantics.
/// Qt normalizes '/' to '\\', so mixed forms are rejected too.
[[nodiscard]] inline bool isNetworkOrDevicePath(const QString& path) {
    const auto isSeparator = [](QChar ch) {
        return ch == QLatin1Char('\\') || ch == QLatin1Char('/');
    };
    return path.size() >= kDoubleSeparatorPrefixLength && isSeparator(path.at(0)) &&
           isSeparator(path.at(1));
}

/// True if @p info is a reparse point (symbolic link or junction).
///
/// This reads the LINK's own attributes and does NOT stat the target -- unlike exists()/isFile()/
/// isDir()/size(), which FOLLOW the link. isNetworkOrDevicePath above only inspects the literal
/// string, so a normal drive-letter path that is actually a symlink to \\host\share (or a .lnk
/// whose target is a UNC path) slips past it; the following stat would then perform the SMB/NTLM
/// handshake this module exists to prevent. So every model-supplied path is screened here BEFORE
/// any following stat, and a reparse point is refused (fail closed -- an on-box symlink target is
/// still refused, but a headless op has no need to follow one).
[[nodiscard]] inline bool pathIsReparsePoint(const QFileInfo& info) {
    // QFileInfo CACHES its metadata at the first query, so a caller-supplied info can answer
    // from a snapshot taken before the path was swapped for a link. Re-stat a copy so this
    // screen always reads the CURRENT attributes (refreshing a never-queried QFileInfo costs
    // nothing -- it only drops a cache that has not been filled yet).
    QFileInfo fresh(info);
    fresh.refresh();
    return fresh.isSymLink() || fresh.isJunction();
}
/// True if @p path must be REFUSED: it is a reparse point, or it is a literal that cannot be
/// screened safely at all (empty/blank, or UNC/device). The literal rejection happens BEFORE
/// the QFileInfo query on purpose: even an attribute read on \\host\share performs the
/// SMB/NTLM handshake this module exists to prevent, so no caller may reach the filesystem
/// through this helper with such a path, whether or not it screened the literal itself.
[[nodiscard]] inline bool pathIsReparsePoint(const QString& path) {
    if (path.trimmed().isEmpty() || isNetworkOrDevicePath(path)) {
        return true;
    }
    return pathIsReparsePoint(QFileInfo(path));
}

/// True if any ANCESTOR directory component of @p path is a reparse point (symlink/junction).
///
/// pathIsReparsePoint above screens only the LEAF. But if an ANCESTOR is a symlink/junction to a
/// UNC target (C:\link\sub\file where C:\link -> \\host\share), the first target-FOLLOWING stat
/// (exists/isFile/isDir/open) resolves the ancestor during path traversal and performs the SMB/NTLM
/// handshake, leaking the credential hash -- exactly what the leaf guard is meant to stop, evaded
/// via an intermediate component. This closes that endemic gap: every model-supplied path is
/// screened for a reparse ANCESTOR too, before any following stat.
///
/// The walk goes ROOT -> LEAF on purpose (SAFETY, not just style): isSymLink()/isJunction() read a
/// component's OWN reparse attribute WITHOUT following it, and by checking a shallower prefix
/// before a deeper one, a reparse ancestor is caught BEFORE we ever stat a deeper path that would
/// traverse it (and trigger the very handshake we prevent). Each stat therefore only traverses
/// ancestors already verified clean. The path is lexically cleaned first (Windows canonicalizes
/// ".." without following links, so a cleaned ancestor set matches what CreateFile will actually
/// traverse).
[[nodiscard]] inline bool pathHasReparsePointAncestor(const QString& path) {
    // Fail closed on a literal that cannot be screened, BEFORE any filesystem query: an empty
    // path has no ancestors to prove clean (reporting it "safe" would be fail-open), and a
    // UNC/device literal would make the walk below stat \\host\share itself.
    if (path.trimmed().isEmpty() || isNetworkOrDevicePath(path)) {
        return true;
    }
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    // A relative input resolves through the process CWD, which can itself be a UNC root --
    // re-screen the RESOLVED form before the attribute walk traverses it.
    if (absolute.isEmpty() || isNetworkOrDevicePath(absolute)) {
        return true;
    }
    const QStringList parts = absolute.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const bool absoluteRoot = absolute.startsWith(QLatin1Char('/'));
    QString prefix;
    // Stop at parts.size() - 1: the last component is the LEAF, covered by pathIsReparsePoint.
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        if (i == 0) {
            prefix = absoluteRoot ? QLatin1Char('/') + parts.at(0) : parts.at(0);
            // A bare drive root ("C:") has no reparse attribute of its own -- skip it.
            if (parts.at(0).endsWith(QLatin1Char(':'))) {
                continue;
            }
        } else {
            prefix += QLatin1Char('/') + parts.at(i);
        }
        if (pathIsReparsePoint(QFileInfo(prefix))) {
            return true;
        }
    }
    return false;
}

/// Combined leaf + ancestor reparse screen: true if @p path itself OR any ancestor directory is a
/// symlink/junction. Use this before any target-following stat on a model-supplied path.
///
/// ORDER IS SECURITY-CRITICAL: the ancestor walk MUST run first. The leaf check pathIsReparsePoint
/// statically reads only the leaf's OWN attribute, but the OS still has to RESOLVE the full path to
/// reach the leaf -- and path resolution FOLLOWS reparse points in ancestor (non-final) components
/// (FILE_FLAG_OPEN_REPARSE_POINT spares only the final component). So statting the leaf of
/// C:\link\sub\file where C:\link is a junction to \\host\share would itself trigger the SMB/NTLM
/// handshake this guard exists to prevent. Running pathHasReparsePointAncestor first short-circuits
/// (it reads each ancestor's own attribute root->leaf without following) so a bad ancestor is
/// refused WITHOUT ever statting the leaf; the leaf stat runs only once every ancestor is proven
/// clean, so it traverses only safe components.
///
/// This is also the single entrypoint that performs the LITERAL rejection (empty/blank,
/// UNC/device) before any filesystem query, so a caller that reaches here without screening
/// the literal itself still cannot trigger a UNC stat.
[[nodiscard]] inline bool pathReparseUnsafe(const QString& path) {
    if (path.trimmed().isEmpty() || isNetworkOrDevicePath(path)) {
        return true;
    }
    return pathHasReparsePointAncestor(path) || pathIsReparsePoint(path);
}

/// Cap on the MBOX size a headless op will index. MboxParser::readMessages builds
/// an offset for EVERY "From " line across the whole file regardless of the
/// requested limit, so an adversarially dense multi-GB file (a prompt-injected
/// path) could OOM the process. Real single-folder mailboxes fit well under this;
/// larger ones use the GUI panel.
inline constexpr qint64 kMaxMboxBytes = 512LL * 1024 * 1024;

}  // namespace sak
