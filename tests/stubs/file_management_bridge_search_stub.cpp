// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Test-only stub. The panel tool-dispatch cert links AdvancedSearchWorker (for the
// search.find_in_files read-only app op) but the op never sets
// SearchConfig::use_file_system_target, so the worker's raw/image-filesystem bridge
// path (listDirectory/readFile) is unreachable at runtime. Providing these two
// symbols here avoids linking the ENTIRE APFS/HFS+/ext raw-filesystem reader+writer
// driver into this test. They are marked ok=false with a blocker so any accidental
// invocation is loud, never a silent empty result.

#include "sak/file_management_file_system.h"

namespace sak {

FileManagementListResult FileManagementFileSystemBridge::listDirectory(const FileManagementTarget&,
                                                                       const QString&,
                                                                       int) {
    FileManagementListResult result;
    result.ok = false;
    result.blockers.append(
        QStringLiteral("file-system-target listing is stubbed out in this test build"));
    return result;
}

FileManagementReadResult FileManagementFileSystemBridge::readFile(const FileManagementTarget&,
                                                                  const QString&,
                                                                  uint64_t) {
    FileManagementReadResult result;
    result.ok = false;
    result.blockers.append(
        QStringLiteral("file-system-target read is stubbed out in this test build"));
    return result;
}

}  // namespace sak
