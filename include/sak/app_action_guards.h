// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QChar>
#include <QFileInfo>
#include <QLatin1Char>
#include <QString>

/// @file app_action_guards.h
/// @brief Path guards shared by every AI-assistant app-action module.
///
/// These live in one header (not duplicated per module) on purpose: they are
/// security-critical (prompt-injection defenses) and MUST NOT drift between the
/// read-only and mutating op modules. Both include this and call the same code.
namespace sak {

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
    return path.size() >= 2 && isSeparator(path.at(0)) && isSeparator(path.at(1));
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
    return info.isSymLink() || info.isJunction();
}
[[nodiscard]] inline bool pathIsReparsePoint(const QString& path) {
    return pathIsReparsePoint(QFileInfo(path));
}

/// Cap on the MBOX size a headless op will index. MboxParser::readMessages builds
/// an offset for EVERY "From " line across the whole file regardless of the
/// requested limit, so an adversarially dense multi-GB file (a prompt-injected
/// path) could OOM the process. Real single-folder mailboxes fit well under this;
/// larger ones use the GUI panel.
inline constexpr qint64 kMaxMboxBytes = 512LL * 1024 * 1024;

}  // namespace sak
