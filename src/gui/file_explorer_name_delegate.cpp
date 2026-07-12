// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_name_delegate.h"

#include "sak/file_explorer_item_model.h"

#include <QEvent>
#include <QToolTip>

namespace sak {

namespace {

// Files FilesystemHelpers.RestrictedCharacters. Applied on every target so
// names stay portable between raw Mac volumes and Windows hosts.
const QString kRestrictedCharacters = QStringLiteral("\\/:*?\"<>|");

QString stripRestrictedCharacters(const QString& text) {
    QString cleaned;
    cleaned.reserve(text.size());
    for (const QChar character : text) {
        if (!kRestrictedCharacters.contains(character)) {
            cleaned.append(character);
        }
    }
    return cleaned;
}

}  // namespace

FileExplorerRenameLineEdit::FileExplorerRenameLineEdit(QWidget* parent) : QLineEdit(parent) {
    setObjectName(QStringLiteral("fileExplorerRenameEditor"));
    setAccessibleName(tr("Rename item"));
    // Files ItemNameTextBox_BeforeTextChanging: restricted characters are
    // stripped live and a warning tip is shown instead of blocking input.
    connect(this, &QLineEdit::textEdited, this, [this](const QString& text) {
        const QString cleaned = stripRestrictedCharacters(text);
        if (cleaned == text) {
            return;
        }
        const int cursor = qMax(0, cursorPosition() - (text.length() - cleaned.length()));
        setText(cleaned);
        setCursorPosition(cursor);
        QToolTip::showText(mapToGlobal(rect().bottomLeft()),
                           tr("A name can't contain any of these characters: %1")
                               .arg(QStringLiteral("\\ / : * ? \" < > |")),
                           this);
    });
}

bool FileExplorerRenameLineEdit::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QLineEdit::event(event);
}

FileExplorerNameDelegate::FileExplorerNameDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QWidget* FileExplorerNameDelegate::createEditor(QWidget* parent,
                                                const QStyleOptionViewItem& option,
                                                const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    return new FileExplorerRenameLineEdit(parent);
}

void FileExplorerNameDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    auto* line_edit = qobject_cast<FileExplorerRenameLineEdit*>(editor);
    if (!line_edit) {
        QStyledItemDelegate::setEditorData(editor, index);
        return;
    }
    const QString text = index.data(Qt::EditRole).toString();
    line_edit->setText(text);
    // Files StartRenameItem: files select the base name (extension excluded
    // when visible); folders select the whole name.
    const bool is_directory = index.data(FileExplorerItemModel::EntryDirectoryRole).toBool();
    int selection_length = text.length();
    if (!is_directory) {
        const int last_dot = text.lastIndexOf(QLatin1Char('.'));
        if (last_dot > 0) {
            selection_length = last_dot;
        }
    }
    // Deferred: QAbstractItemView::openEditor select-alls the line edit right
    // after this call, so the Files-style base-name selection must win later.
    QMetaObject::invokeMethod(
        line_edit,
        [line_edit, selection_length]() { line_edit->setSelection(0, selection_length); },
        Qt::QueuedConnection);
}

}  // namespace sak
