// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/migration_report.h"

namespace sak {

void MigrationReport::addEntry(const MigrationEntry& entry) {
    m_entries.push_back(entry);
}

}  // namespace sak
