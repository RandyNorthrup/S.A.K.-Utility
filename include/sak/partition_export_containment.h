// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_export_containment.h
/// @brief The one containment guard every raw-filesystem exporter writes through.
///
/// The APFS, ext and HFS+ directory exporters all take a foreign, attacker-authorable volume and
/// write its contents into a directory the operator chose. Each carried its OWN byte-identical
/// copy of this logic, and that is not a hypothetical cost: R5-P4-44 (and R4 M-A4-27 before it)
/// exist because HFS+ was the copy that lagged, shipping without the containment re-check while
/// the other two had it. A guard maintained in triplicate is a guard that will diverge again, so
/// there is now one definition and the readers call it.
///
/// Header-only on purpose. The core sources are enumerated in the top-level CMakeLists AND
/// re-enumerated by roughly six test targets, so a new translation unit would have to be added to
/// each of them -- and a target that silently missed it would fail to link rather than quietly
/// lose the guard, but the churn buys nothing that `inline` does not.

#pragma once

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

namespace sak::partition_export {

/// @brief True when @p child resolves (junctions/symlinks followed) to a real path at or under
/// @p canonicalRoot.
///
/// Fails closed on an empty root or an unresolvable/vanished child: a reparse point planted at an
/// ancestor AFTER the root was checked would otherwise redirect the write outside the export
/// directory. canonicalFilePath yields '/'-separated paths, so the boundary test uses '/'; a
/// sibling-prefix (Root vs RootX) is rejected by requiring that separator.
[[nodiscard]] inline bool pathWithinRoot(const QString& canonical_root, const QString& child) {
    if (canonical_root.isEmpty()) {
        return false;
    }
    const QString canonical_child = QFileInfo(child).canonicalFilePath();
    if (canonical_child.isEmpty()) {
        return false;
    }
    return canonical_child == canonical_root ||
           canonical_child.startsWith(canonical_root + QLatin1Char('/'));
}

/// @brief Write one exported file, refusing anything that escapes the export root.
///
/// @param label names the SOURCE item for the blocker text (a volume path, or "<path> resource
/// fork"), so a refusal says which item was refused rather than only where it would have landed.
[[nodiscard]] inline bool writeFile(const QString& path,
                                    const QByteArray& data,
                                    QStringList* blockers,
                                    const QString& label,
                                    const QString& canonical_root) {
    // Re-check the leaf's PARENT directory on every write: mkpath created the ancestor chain, but
    // a junction swapped in afterwards would redirect this file outside the export root. The
    // NewOnly guard below only protects the final component.
    if (!pathWithinRoot(canonical_root, QFileInfo(path).absolutePath())) {
        blockers->append(
            QStringLiteral("Export target escapes the export root (reparse point): %1").arg(label));
        return false;
    }
    QFile output(path);
    // NewOnly (O_EXCL/CREATE_NEW) fails closed if anything already exists at `path`: the caller's
    // uniquePath returned a non-existent candidate, so a file or symlink appearing here is a
    // TOCTOU race, and Truncate would have followed it to overwrite an arbitrary target.
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        output.write(data) != data.size()) {
        blockers->append(QStringLiteral("Unable to write exported %1: %2").arg(label, path));
        return false;
    }
    return true;
}

}  // namespace sak::partition_export
