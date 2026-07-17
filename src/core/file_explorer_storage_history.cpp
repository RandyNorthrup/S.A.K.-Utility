// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_storage_history.cpp
/// @brief Undo/redo journal cloned from the Files StorageHistoryWrapper.

#include "sak/file_explorer_storage_history.h"

#include <utility>

namespace sak {

void FileExplorerStorageHistoryWrapper::add(FileExplorerStorageHistory history) {
    // Files AddHistory: advance the index, insert there, and drop the stale
    // redo branch that followed.
    ++m_index;
    m_histories.insert(m_index, std::move(history));
    if (m_index + 1 < m_histories.size()) {
        m_histories.remove(m_index + 1, m_histories.size() - m_index - 1);
    }
}

bool FileExplorerStorageHistoryWrapper::canUndo() const {
    return m_index >= 0 && !m_histories.isEmpty();
}

bool FileExplorerStorageHistoryWrapper::canRedo() const {
    return m_index + 1 < m_histories.size();
}

const FileExplorerStorageHistory* FileExplorerStorageHistoryWrapper::undoTarget() const {
    return canUndo() ? &m_histories.at(m_index) : nullptr;
}

const FileExplorerStorageHistory* FileExplorerStorageHistoryWrapper::redoTarget() const {
    return canRedo() ? &m_histories.at(m_index + 1) : nullptr;
}

void FileExplorerStorageHistoryWrapper::markUndone() {
    if (canUndo()) {
        --m_index;
    }
}

void FileExplorerStorageHistoryWrapper::markRedone() {
    if (canRedo()) {
        ++m_index;
    }
}

void FileExplorerStorageHistoryWrapper::clear() {
    m_histories.clear();
    m_index = -1;
}

int FileExplorerStorageHistoryWrapper::count() const {
    return static_cast<int>(m_histories.size());
}

int FileExplorerStorageHistoryWrapper::index() const {
    return m_index;
}

}  // namespace sak
