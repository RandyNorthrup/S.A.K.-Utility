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
    void keyPressEvent(QKeyEvent* event) override;
};

/// Name-column delegate: rename editor with Files semantics -- base name
/// selected for files when extensions are shown, restricted characters
/// stripped live with a warning tip (FilesystemHelpers.RestrictedCharacters).
/// With @p folder_chevron on, folder rows paint the Files columns-view
/// chevron (ColumnLayoutPage OpenFolderChevron, glyph E76C) right-aligned.
class FileExplorerNameDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit FileExplorerNameDelegate(QObject* parent = nullptr, bool folder_chevron = false);

    [[nodiscard]] QWidget* createEditor(QWidget* parent,
                                        const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] bool folderChevronEnabled() const;

private:
    bool m_folder_chevron{false};
};

/// Cards-layout delegate (Files CardsBrowserTemplate): a rounded card with
/// the icon box on the left and a details block of name (bold), type
/// caption, and size caption. Inherits the rename editor.
class FileExplorerCardDelegate : public FileExplorerNameDelegate {
    Q_OBJECT

public:
    explicit FileExplorerCardDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

}  // namespace sak
