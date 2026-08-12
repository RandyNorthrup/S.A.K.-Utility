// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_storage_history.h
/// @brief Undo/redo journal for the File Management Explorer (clone of the
///        Files app Utils/Storage/History/StorageHistoryWrapper.cs).

#pragma once

#include <QString>
#include <QVector>

namespace sak {

// The Files FileOperationType values that have inverses (StorageHistory
// Operations.Undo): create, rename, copy, and move. Extract and permanent
// delete record no inverse in Files and are not journaled here either.
enum class FileExplorerHistoryOperation {
    CreateNew = 0,
    Rename,
    Copy,
    Move,
};

/// One affected entry: rename/move keep old -> new paths; copy keeps the
/// original source and the created destination; create keeps the created
/// path in destination_path.
struct FileExplorerHistoryItem {
    QString source_path;
    QString destination_path;
    bool directory{false};
};

struct FileExplorerStorageHistory {
    FileExplorerHistoryOperation operation{FileExplorerHistoryOperation::CreateNew};
    QString source_target_id;
    QString destination_target_id;
    QVector<FileExplorerHistoryItem> items;
};

/// Files StorageHistoryWrapper: a history list plus an index that starts at
/// -1; pushing a new entry truncates any redo branch beyond it. Unlike the
/// unbounded Files original, the retained length is capped at
/// kMaxRetainedEntries; once full the oldest entries are dropped, leaving
/// undo/redo behaviour within the cap unchanged (bounds long-session memory).
class FileExplorerStorageHistoryWrapper {
public:
    /// Upper bound on retained undo/redo entries; the oldest are dropped once a
    /// new entry would exceed it, bounding long-session memory (R5-P9-39).
    static constexpr int kMaxRetainedEntries = 256;

    void add(FileExplorerStorageHistory history);
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    /// The entry Ctrl+Z would revert (nullptr when there is none).
    [[nodiscard]] const FileExplorerStorageHistory* undoTarget() const;
    /// The entry Ctrl+Y would replay (nullptr when there is none).
    [[nodiscard]] const FileExplorerStorageHistory* redoTarget() const;
    void markUndone();
    void markRedone();
    void clear();
    [[nodiscard]] int count() const;
    [[nodiscard]] int index() const;

private:
    QVector<FileExplorerStorageHistory> m_histories;
    int m_index{-1};
};

}  // namespace sak
