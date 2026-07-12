// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_name_delegate.h
/// @brief Inline-rename delegate for the File Explorer name column
/// (Files BaseGroupableLayoutPage rename textbox clone).

#pragma once

#include <QLineEdit>
#include <QStyledItemDelegate>

namespace sak {

/// Rename editor line edit. Claims every ShortcutOverride so panel-level
/// shortcuts (Esc clear-selection, Backspace back, Space preview, ...) do
/// not steal keys while a rename is in progress -- while renaming, the
/// keyboard belongs to the editor, exactly like the Files rename textbox.
class FileExplorerRenameLineEdit : public QLineEdit {
    Q_OBJECT

public:
    explicit FileExplorerRenameLineEdit(QWidget* parent = nullptr);

protected:
    bool event(QEvent* event) override;
};

/// Name-column delegate: rename editor with Files semantics -- base name
/// selected for files when extensions are shown, restricted characters
/// stripped live with a warning tip (FilesystemHelpers.RestrictedCharacters).
class FileExplorerNameDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit FileExplorerNameDelegate(QObject* parent = nullptr);

    [[nodiscard]] QWidget* createEditor(QWidget* parent,
                                        const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
};

}  // namespace sak
