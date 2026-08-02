// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/app_action_guards.h"

#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

/// @file leftover_cleanup_guard.h
/// @brief Fail-closed target screens for the AI assistant's software.clean_leftovers op.
///
/// clean_leftovers drives CleanupWorker, which PERMANENTLY deletes whatever it is handed: files and
/// folders, whole registry key trees (RegDeleteTreeW), registry values, Windows services (sc
/// delete), scheduled tasks (schtasks /delete /f), and firewall rules (netsh ... delete rule). That
/// is the arbitrary-destruction surface a real technician needs -- but it is also exactly what a
/// prompt-injected/confused model could weaponize to brick the OS (delete HKLM\SYSTEM, sc delete
/// RpcSs, netsh ... delete rule name=all, remove C:\Windows\System32, ...).
///
/// The op is gated CATASTROPHIC (a human must confirm every item, even in Unattended), which is the
/// primary backstop. THESE guards are the second, independent layer: they refuse OS-critical /
/// unrecoverable / injection targets OUTRIGHT, before any item reaches the worker, so the tool
/// itself will not delete something that unboots the machine even if a human rubber-stamps the
/// confirm. Each returns a human-readable refusal reason, or an empty QString when the target is
/// allowed. Rejecting empty targets here also satisfies CleanupWorker's Q_ASSERT(!x.isEmpty())
/// preconditions (an empty path/name would otherwise abort a Debug build).
///
/// These are deliberately NOT a whitelist: the technician gets broad range within the space of
/// targets whose deletion is recoverable. The denylist encodes only the "do not brick the OS /
/// wipe a shared root" judgment the injectable model cannot be trusted to make.
namespace sak {

/// A control character (< 0x20) or a shell/CLI wildcard ('*' or '?') in a name the op will pass to
/// an external tool or a Win32 API. Rule names may legitimately contain spaces/punctuation, so this
/// is applied to service/task/registry targets, not to firewall rule names (see below).
[[nodiscard]] inline bool cleanupNameHasControlOrWildcard(const QString& value) {
    for (const QChar ch : value) {
        if (ch.unicode() < 0x20 || ch == QLatin1Char('*') || ch == QLatin1Char('?')) {
            return true;
        }
    }
    return false;
}

/// Registry hives CleanupWorker actually supports (deleteRegistryKey/Value refuse anything else).
/// Returns true and fills @p subkeyOut (the remainder after the hive prefix) on a match.
[[nodiscard]] inline bool cleanupRegistryHive(const QString& fullKeyPath, QString& subkeyOut) {
    static const QStringList kHives = {QStringLiteral("HKLM\\"),
                                       QStringLiteral("HKCU\\"),
                                       QStringLiteral("HKCR\\")};
    for (const QString& hive : kHives) {
        if (fullKeyPath.startsWith(hive, Qt::CaseInsensitive)) {
            subkeyOut = fullKeyPath.mid(hive.size());
            return true;
        }
    }
    return false;
}

/// Lower-cased, backslash-normalized subkey with leading/trailing/repeated separators collapsed, so
/// denylist comparisons are canonical regardless of the exact spelling the model supplied. The
/// registry API resolves "\\SYSTEM", "SYSTEM\\\\Foo", and "SYSTEM\\" to the same subkey, so a
/// separator trick must not let one evade a prefix/exact match.
[[nodiscard]] inline QString cleanupNormalizedSubkey(const QString& subkey) {
    QString normalized = subkey;
    normalized.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const QStringList parts = normalized.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    return parts.join(QLatin1Char('\\')).toLower();
}

/// Registry subtrees where NOTHING may be deleted (prefix match at a key boundary). These are the
/// keys whose loss unboots the machine or breaks the security substrate.
[[nodiscard]] inline const QStringList& cleanupDeniedRegistryPrefixes() {
    static const QStringList kPrefixes = {
        QStringLiteral("system"),
        QStringLiteral("sam"),
        QStringLiteral("security"),
        QStringLiteral("bcd00000000"),
        QStringLiteral("hardware"),
        QStringLiteral("components"),
        QStringLiteral("software\\microsoft\\cryptography"),
        QStringLiteral("software\\microsoft\\windows defender"),
        QStringLiteral("software\\policies"),
        QStringLiteral("software\\microsoft\\windows\\currentversion\\policies"),
        QStringLiteral("software\\microsoft\\windows nt\\currentversion\\winlogon"),
        QStringLiteral("software\\wow6432node\\microsoft\\cryptography"),
    };
    return kPrefixes;
}

/// Shared registry roots blocked only as the EXACT key -- a specific deeper key (an app's own
/// subkey) is a legitimate leftover, but deleting one of these whole trees is catastrophic.
[[nodiscard]] inline const QStringList& cleanupDeniedRegistryExact() {
    static const QStringList kExact = {
        QStringLiteral("software"),
        QStringLiteral("software\\microsoft"),
        QStringLiteral("software\\microsoft\\windows"),
        QStringLiteral("software\\microsoft\\windows\\currentversion"),
        QStringLiteral("software\\microsoft\\windows\\currentversion\\run"),
        QStringLiteral("software\\microsoft\\windows\\currentversion\\runonce"),
        QStringLiteral("software\\microsoft\\windows\\currentversion\\explorer"),
        QStringLiteral("software\\microsoft\\windows\\currentversion\\uninstall"),
        QStringLiteral("software\\microsoft\\windows nt"),
        QStringLiteral("software\\microsoft\\windows nt\\currentversion"),
        QStringLiteral("software\\classes"),
        QStringLiteral("software\\clients"),
        QStringLiteral("software\\wow6432node"),
        QStringLiteral("software\\wow6432node\\microsoft"),
        QStringLiteral("software\\wow6432node\\microsoft\\windows"),
        QStringLiteral("software\\wow6432node\\microsoft\\windows\\currentversion"),
        QStringLiteral("clsid"),
        QStringLiteral("appid"),
        QStringLiteral("typelib"),
        QStringLiteral("interface"),
        QStringLiteral("*"),
        QStringLiteral("exefile"),
        QStringLiteral("directory"),
        QStringLiteral("folder"),
    };
    return kExact;
}

/// True if @p normalizedSubkey lands in an OS-critical registry subtree (prefix block).
[[nodiscard]] inline QString cleanupProtectedRegistrySubtree(const QString& normalizedSubkey) {
    for (const QString& prefix : cleanupDeniedRegistryPrefixes()) {
        if (normalizedSubkey == prefix || normalizedSubkey.startsWith(prefix + QLatin1Char('\\'))) {
            return prefix;
        }
    }
    return {};
}

/// Screen a registry KEY-tree deletion (RegDeleteTreeW deletes the key and ALL subkeys).
[[nodiscard]] inline QString registryKeyDeletionRefusal(const QString& fullKeyPath) {
    const QString path = fullKeyPath.trimmed();
    if (path.isEmpty()) {
        return QStringLiteral("empty registry key path");
    }
    if (cleanupNameHasControlOrWildcard(path)) {
        return QStringLiteral("registry key path contains a control or wildcard character");
    }
    QString subkey;
    if (!cleanupRegistryHive(path, subkey)) {
        return QStringLiteral("registry key must start with HKLM\\, HKCU\\ or HKCR\\");
    }
    const QString normalized = cleanupNormalizedSubkey(subkey);
    if (normalized.isEmpty()) {
        return QStringLiteral("refusing to delete an entire registry hive root");
    }
    const QString subtree = cleanupProtectedRegistrySubtree(normalized);
    if (!subtree.isEmpty()) {
        return QStringLiteral("refusing to delete a protected system registry subtree (%1)")
            .arg(subtree);
    }
    if (cleanupDeniedRegistryExact().contains(normalized)) {
        return QStringLiteral(
            "refusing to delete a shared registry root key; delete a specific "
            "subkey instead");
    }
    return {};
}

/// Screen a registry VALUE deletion. Deleting a single value is far narrower than deleting a key
/// tree, so a value under a shared root (e.g. a Run value = a startup leftover) is ALLOWED; only
/// the OS-critical subtrees (SYSTEM/SAM/SECURITY/...) are still refused.
[[nodiscard]] inline QString registryValueDeletionRefusal(const QString& keyPath,
                                                          const QString& valueName) {
    const QString path = keyPath.trimmed();
    if (path.isEmpty()) {
        return QStringLiteral("empty registry key path");
    }
    if (valueName.trimmed().isEmpty()) {
        return QStringLiteral("empty registry value name");
    }
    if (cleanupNameHasControlOrWildcard(path) || cleanupNameHasControlOrWildcard(valueName)) {
        return QStringLiteral("registry value target contains a control or wildcard character");
    }
    QString subkey;
    if (!cleanupRegistryHive(path, subkey)) {
        return QStringLiteral("registry key must start with HKLM\\, HKCU\\ or HKCR\\");
    }
    const QString normalized = cleanupNormalizedSubkey(subkey);
    if (normalized.isEmpty()) {
        return QStringLiteral("refusing to delete a value directly under a hive root");
    }
    const QString subtree = cleanupProtectedRegistrySubtree(normalized);
    if (!subtree.isEmpty()) {
        return QStringLiteral("refusing to modify a protected system registry subtree (%1)")
            .arg(subtree);
    }
    return {};
}

/// Critical Windows services whose deletion breaks boot, security, or core OS function.
/// Lower-cased.
[[nodiscard]] inline const QSet<QString>& cleanupCriticalServices() {
    static const QSet<QString> kServices = {
        QStringLiteral("rpcss"),
        QStringLiteral("rpceptmapper"),
        QStringLiteral("dcomlaunch"),
        QStringLiteral("lsm"),
        QStringLiteral("brokerinfrastructure"),
        QStringLiteral("power"),
        QStringLiteral("plugplay"),
        QStringLiteral("schedule"),
        QStringLiteral("winmgmt"),
        QStringLiteral("eventlog"),
        QStringLiteral("eventsystem"),
        QStringLiteral("systemeventsbroker"),
        QStringLiteral("mpssvc"),
        QStringLiteral("windefend"),
        QStringLiteral("wdnissvc"),
        QStringLiteral("wscsvc"),
        QStringLiteral("securityhealthservice"),
        QStringLiteral("dnscache"),
        QStringLiteral("dhcp"),
        QStringLiteral("nsi"),
        QStringLiteral("netprofm"),
        QStringLiteral("nlasvc"),
        QStringLiteral("bfe"),
        QStringLiteral("lanmanserver"),
        QStringLiteral("lanmanworkstation"),
        QStringLiteral("gpsvc"),
        QStringLiteral("profsvc"),
        QStringLiteral("themes"),
        QStringLiteral("audiosrv"),
        QStringLiteral("audioendpointbuilder"),
        QStringLiteral("coremessagingregistrar"),
        QStringLiteral("samss"),
        QStringLiteral("trustedinstaller"),
        QStringLiteral("wuauserv"),
        QStringLiteral("cryptsvc"),
        QStringLiteral("msiserver"),
        QStringLiteral("appinfo"),
        QStringLiteral("shellhwdetection"),
        QStringLiteral("staterepository"),
        QStringLiteral("usermanager"),
        QStringLiteral("w32time"),
        QStringLiteral("wlansvc"),
        QStringLiteral("wcmsvc"),
        QStringLiteral("termservice"),
        QStringLiteral("usosvc"),
        QStringLiteral("wdiservicehost"),
    };
    return kServices;
}

/// Screen a Windows service deletion (sc stop + sc delete).
[[nodiscard]] inline QString serviceDeletionRefusal(const QString& serviceName) {
    const QString name = serviceName.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("empty service name");
    }
    if (name.size() > 256) {
        return QStringLiteral("service name is too long");
    }
    if (cleanupNameHasControlOrWildcard(name)) {
        return QStringLiteral("service name contains a control or wildcard character");
    }
    if (name.contains(QLatin1Char('\\')) || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('"')) || name.contains(QLatin1Char('='))) {
        return QStringLiteral("service name contains an invalid character");
    }
    if (cleanupCriticalServices().contains(name.toLower())) {
        return QStringLiteral("refusing to delete a critical Windows service (%1)").arg(name);
    }
    return {};
}

/// Screen a scheduled-task deletion (schtasks /delete /tn <name> /f). The whole \\Microsoft\\ task
/// tree (the OS's own tasks) is refused; app leftovers live at the root or in a vendor folder.
[[nodiscard]] inline QString scheduledTaskDeletionRefusal(const QString& taskName) {
    const QString name = taskName.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("empty scheduled-task name");
    }
    if (name.size() > 512) {
        return QStringLiteral("scheduled-task name is too long");
    }
    if (cleanupNameHasControlOrWildcard(name)) {
        return QStringLiteral("scheduled-task name contains a control or wildcard character");
    }
    QString normalized = name;
    normalized.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (normalized.startsWith(QLatin1Char('\\'))) {
        normalized.remove(0, 1);
    }
    const QString lower = normalized.toLower();
    if (lower.isEmpty()) {
        return QStringLiteral("refusing to delete the scheduled-task root");
    }
    if (lower == QLatin1String("microsoft") || lower.startsWith(QLatin1String("microsoft\\"))) {
        return QStringLiteral("refusing to delete a Windows/Microsoft scheduled task");
    }
    return {};
}

/// Screen a firewall-rule deletion (netsh advfirewall firewall delete rule name=<name>). Rule names
/// legitimately contain spaces and punctuation, so only control chars are rejected -- but the
/// special token "all" deletes EVERY rule and is refused (a specific rule must be named).
[[nodiscard]] inline QString firewallRuleDeletionRefusal(const QString& ruleName) {
    const QString name = ruleName.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("empty firewall rule name");
    }
    if (name.size() > 512) {
        return QStringLiteral("firewall rule name is too long");
    }
    for (const QChar ch : name) {
        if (ch.unicode() < 0x20) {
            return QStringLiteral("firewall rule name contains a control character");
        }
    }
    if (name.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral(
            "refusing firewall wildcard name=all (deletes ALL rules); name a "
            "specific rule");
    }
    return {};
}

/// True if @p path contains an NTFS 8.3 short-name component: a tilde followed by a digit (e.g.
/// PROGRA~1). Win32 resolves such a component to its long-name target, which the guard CANNOT
/// expand lexically -- so "C:\\PROGRA~1" would slip the long-name shared-root denylist below yet
/// the worker still deletes the real Program Files tree. Model input from scan_leftovers is always
/// long-form, so a short-name component is refused fail-closed (the technician supplies the full
/// long path).
[[nodiscard]] inline bool cleanupHasShortNameComponent(const QString& path) {
    static const QRegularExpression kShortName(QStringLiteral("~[0-9]"));
    return kShortName.match(path).hasMatch();
}

/// Lower-cased path with each component's trailing dots and spaces stripped, mirroring how Win32
/// resolves a path (it discards trailing '.' and ' ' from every component). Without this,
/// "C:\\Windows." or "C:\\Program Files " would slip the root/system denylist below while the
/// worker still deletes the real protected target.
[[nodiscard]] inline QString cleanupCanonicalLower(const QString& cleanedAbsolutePath) {
    QStringList segments = cleanedAbsolutePath.split(QLatin1Char('/'));
    for (QString& segment : segments) {
        while (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' '))) {
            segment.chop(1);
        }
    }
    return segments.join(QLatin1Char('/')).toLower();
}

/// Directories that are shared system/user roots: blocked as the EXACT path (a specific subfolder
/// under them -- e.g. Program Files\\Vendor or Documents\\SomeApp -- is a legitimate leftover and
/// stays allowed). Includes the per-user shell-data folder ROOTS (Documents/Desktop/Downloads/...)
/// and the Public shell roots, so a poisoned batch cannot wipe a user's entire Documents/Desktop
/// tree while a genuine app subfolder under them stays cleanable.
[[nodiscard]] inline const QRegularExpression& cleanupSharedRootDirRegex() {
    static const QRegularExpression kRegex(QStringLiteral(
        "^[a-z]:/(program files|program files \\(x86\\)|programdata|users|"
        "users/public|windows|users/[^/]+|users/[^/]+/appdata|"
        "users/[^/]+/appdata/local|users/[^/]+/appdata/roaming|"
        "users/[^/]+/appdata/locallow|"
        "users/[^/]+/(documents|desktop|downloads|pictures|music|videos|favorites|contacts|"
        "links|searches|saved games|onedrive)|"
        "users/public/(documents|desktop|downloads|pictures|music|videos|libraries))$"));
    return kRegex;
}

/// Boot / system-critical roots that are NEVER a legitimate application leftover: refused as the
/// path AND its whole subtree (unlike the shared roots, no subfolder under these is ever
/// cleanable).
[[nodiscard]] inline const QRegularExpression& cleanupCriticalTreeRegex() {
    static const QRegularExpression kRegex(
        QStringLiteral("^[a-z]:/(bootmgr|boot|efi|recovery|\\$recycle\\.bin|"
                       "system volume information|hiberfil\\.sys|pagefile\\.sys|swapfile\\.sys|"
                       "bootsect\\.bak|bootnxt|config\\.msi)(/.*)?$"));
    return kRegex;
}

/// Screen a file/folder deletion. Reuses the shared UNC/device + reparse screens (so a
/// symlink/junction target -- leaf or ancestor -- cannot leak an NTLM hash or escape the check),
/// then refuses drive roots, the Windows system tree, and shared system/user root directories.
// Lexical (string-only) screens that need no path resolution: empty, over-length, non-drive-letter,
// UNC/device, symlink/junction (leaf or ancestor), or an 8.3 short-name component. Split out so
// filePathDeletionRefusal stays within the complexity budget.
[[nodiscard]] inline QString cleanupFilePathStringRefusal(const QString& path) {
    if (path.isEmpty()) {
        return QStringLiteral("empty path");
    }
    if (path.size() > 4096) {
        return QStringLiteral("path is too long");
    }
    static const QRegularExpression kRawDriveRe(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    if (!kRawDriveRe.match(path).hasMatch()) {
        return QStringLiteral("path must be an absolute drive-letter path");
    }
    if (isNetworkOrDevicePath(path)) {
        return QStringLiteral("refusing a UNC/device path");
    }
    if (pathReparseUnsafe(path)) {
        return QStringLiteral("refusing a symlink/junction path (or one nested under one)");
    }
    if (cleanupHasShortNameComponent(path)) {
        return QStringLiteral(
            "path contains an 8.3 short-name component; supply the full long path");
    }
    return {};
}

[[nodiscard]] inline QString filePathDeletionRefusal(const QString& rawPath) {
    const QString path = rawPath.trimmed();
    const QString string_refusal = cleanupFilePathStringRefusal(path);
    if (!string_refusal.isEmpty()) {
        return string_refusal;
    }
    // Resolve DOS-device / SUBST aliases (and symlinks) to the REAL target when the path exists, so
    // an alias such as `subst X: C:\Windows` cannot smuggle a protected path past the drive-letter
    // screens below (the kernel resolves X:\ to C:\Windows at delete time, but a lexical
    // absoluteFilePath would keep "x:/..." and slip every check). canonicalFilePath() is empty for
    // a nonexistent path -- deleting one is a no-op -- so fall back to the lexical absolute path.
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    const QString clean = QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                                              : canonical);
    const QString lower = cleanupCanonicalLower(clean);
    static const QRegularExpression kDriveRootRe(QStringLiteral("^[a-z]:/?$"));
    if (kDriveRootRe.match(lower).hasMatch()) {
        return QStringLiteral("refusing a drive root");
    }
    QString systemRoot = QDir::fromNativeSeparators(
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")));
    systemRoot = QDir::cleanPath(systemRoot).toLower();
    if (lower == systemRoot || lower.startsWith(systemRoot + QLatin1Char('/'))) {
        return QStringLiteral("refusing a path inside the Windows system directory");
    }
    if (cleanupCriticalTreeRegex().match(lower).hasMatch()) {
        return QStringLiteral("refusing a boot/system-critical path");
    }
    if (cleanupSharedRootDirRegex().match(lower).hasMatch()) {
        return QStringLiteral(
            "refusing a shared system/user root directory; delete a specific "
            "subfolder instead");
    }
    return {};
}

/// Delete-TIME re-verification for the file/folder cleanup path (the ancestor-junction-swap TOCTOU
/// close). filePathDeletionRefusal screens a path STRING at validate time, but CleanupWorker
/// deletes later by re-resolving that string. A local attacker who swaps an ANCESTOR directory into
/// a junction between validate and delete makes the same string resolve to a DIFFERENT real target
/// (a leftover subfolder "cache" replaced by a junction to C:\\Windows\\System32). Leaf reparse is
/// already screened; the ancestor swap is not. So at delete time the worker opens a handle to the
/// item (FILE_FLAG_OPEN_REPARSE_POINT, no leaf-follow) and reads the object's REAL path via
/// GetFinalPathNameByHandleW -- with every ancestor junction/symlink resolved to its true target --
/// and passes it here as @p handleFinalPath (\\?\ already stripped). This refuses the deletion when
/// that real path (a) cannot be resolved, (b) lands in a protected/critical/root/UNC location, or
/// (c) no longer matches the validated @p requestedPath (ANY ancestor was swapped, even to a benign
/// but wrong target the human never confirmed). Empty QString => the real target is exactly the
/// validated path and is safe to delete; the worker then deletes BY HANDLE so no third path
/// resolution can re-open the swap window. Pure/string-only so it is unit-testable without a live
/// junction.
[[nodiscard]] inline QString cleanupHandleRedirectRefusal(const QString& requestedPath,
                                                          const QString& handleFinalPath) {
    if (handleFinalPath.trimmed().isEmpty()) {
        return QStringLiteral("could not resolve the real deletion target");
    }
    const QString protectedRefusal = filePathDeletionRefusal(handleFinalPath);
    if (!protectedRefusal.isEmpty()) {
        return QStringLiteral("delete target resolved to a protected location (%1)")
            .arg(protectedRefusal);
    }
    const QString wantLower = cleanupCanonicalLower(QDir::cleanPath(requestedPath));
    const QString gotLower = cleanupCanonicalLower(QDir::cleanPath(handleFinalPath));
    if (wantLower != gotLower) {
        return QStringLiteral(
            "delete target was redirected from the validated path (possible junction/symlink "
            "swap)");
    }
    return {};
}

}  // namespace sak
