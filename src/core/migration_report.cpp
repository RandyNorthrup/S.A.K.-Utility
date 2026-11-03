// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/migration_report.h"

#include <QSysInfo>

#include <algorithm>

#include <windows.h>

#include <lmcons.h>

namespace sak {

MigrationReport::MigrationReport() {
    // Initialize metadata
    m_metadata.source_machine = getComputerName();
    m_metadata.source_os = QSysInfo::productType();
    m_metadata.source_os_version = QSysInfo::productVersion();
    m_metadata.created_by = getCurrentUser();
    m_metadata.created_at = QDateTime::currentDateTime();
    m_metadata.report_version = "1.0";
}

void MigrationReport::addEntry(const MigrationEntry& entry) {
    m_entries.push_back(entry);
    m_metadata.total_apps = static_cast<int>(m_entries.size());
}

void MigrationReport::selectAll() {
    for (auto& entry : m_entries) {
        entry.selected = true;
    }
    m_metadata.selected_apps = getSelectedCount();
}

int MigrationReport::getSelectedCount() const {
    return static_cast<int>(std::count_if(
        m_entries.begin(), m_entries.end(), [](const MigrationEntry& e) { return e.selected; }));
}

int MigrationReport::getMatchedCount() const {
    return static_cast<int>(
        std::count_if(m_entries.begin(), m_entries.end(), [](const MigrationEntry& e) {
            return !e.choco_package.isEmpty();
        }));
}

int MigrationReport::getUnmatchedCount() const {
    return static_cast<int>(m_entries.size()) - getMatchedCount();
}

void MigrationReport::clear() {
    m_entries.clear();
    m_metadata.total_apps = 0;
    m_metadata.matched_apps = 0;
    m_metadata.selected_apps = 0;
    m_metadata.match_rate = 0.0;
}

QString MigrationReport::getComputerName() {
    WCHAR buffer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &size) != 0) {
        return QString::fromWCharArray(buffer);
    }
    return QSysInfo::machineHostName();
}

QString MigrationReport::getCurrentUser() {
    WCHAR buffer[UNLEN + 1];
    DWORD size = UNLEN + 1;
    if (GetUserNameW(buffer, &size) != 0) {
        return QString::fromWCharArray(buffer);
    }
    return qEnvironmentVariable("USERNAME");
}

}  // namespace sak
