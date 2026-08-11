// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_name_delegate.h"

#include "sak/file_explorer_group_proxy_model.h"
#include "sak/file_explorer_item_model.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QToolTip>

namespace sak {

namespace {

// Halve a leftover span to center a glyph or icon within its cell.
constexpr int kCenteringDivisor = 2;
// Alpha applied to the highlight fill of a selected card (subtle tint).
constexpr int kSelectedCardFillAlpha = 60;

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
        const int cursor =
            static_cast<int>(qMax(0, cursorPosition() - (text.length() - cleaned.length())));
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

void FileExplorerRenameLineEdit::keyPressEvent(QKeyEvent* event) {
    QLineEdit::keyPressEvent(event);
    // QLineEdit deliberately ignores Return/Enter/Escape so dialog default
    // buttons can fire; here that would bubble the key to the item view,
    // whose Enter activation matrix would open the item mid-rename. The
    // delegate has already scheduled commit/revert - consume the key.
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Escape:
        event->accept();
        break;
    default:
        break;
    }
}

FileExplorerNameDelegate::FileExplorerNameDelegate(QObject* parent, const bool folder_chevron)
    : QStyledItemDelegate(parent), m_folder_chevron(folder_chevron) {}

bool FileExplorerNameDelegate::folderChevronEnabled() const {
    return m_folder_chevron;
}

void FileExplorerNameDelegate::paint(QPainter* painter,
                                     const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const {
    QStyledItemDelegate::paint(painter, option, index);
    if (!m_folder_chevron || !index.data(FileExplorerItemModel::EntryDirectoryRole).toBool()) {
        return;
    }
    // Files ColumnLayoutPage OpenFolderChevron: a right-aligned chevron on
    // folder rows marks that opening pushes the next blade.
    constexpr int kChevronEdge = 12;
    constexpr int kChevronMargin = 4;
    QStyleOption arrow;
    arrow.rect = QRect(option.rect.right() - kChevronEdge - kChevronMargin,
                       option.rect.center().y() - (kChevronEdge / kCenteringDivisor),
                       kChevronEdge,
                       kChevronEdge);
    arrow.state = option.state;
    arrow.palette = option.palette;
    QApplication::style()->drawPrimitive(QStyle::PE_IndicatorArrowRight, &arrow, painter);
}

FileExplorerCardDelegate::FileExplorerCardDelegate(QObject* parent)
    : FileExplorerNameDelegate(parent) {}

namespace {

// Files CardsBrowserTemplate: rounded card (CornerRadius 8) with a 1px
// border and the icon box on the left; palette-driven so light and dark
// themes both read. Returns the details rect to the right of the icon box.
QRect paintCardFrameAndIcon(QPainter* painter,
                            const QStyleOptionViewItem& option,
                            const QModelIndex& index) {
    constexpr int kCardMargin = 3;
    constexpr int kCardRadius = 8;
    constexpr int kDetailsSpacing = 8;
    const QRect card = option.rect.adjusted(kCardMargin, kCardMargin, -kCardMargin, -kCardMargin);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    QColor fill = option.palette.color(selected ? QPalette::Highlight : QPalette::Base);
    if (selected) {
        fill.setAlpha(kSelectedCardFillAlpha);
    }
    painter->setPen(QPen(option.palette.color(selected ? QPalette::Highlight : QPalette::Mid), 1));
    painter->setBrush(fill);
    painter->drawRoundedRect(card, kCardRadius, kCardRadius);

    const int box = card.height();
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    if (!icon.isNull()) {
        const QSize icon_size = option.decorationSize;
        icon.paint(painter,
                   QRect(card.left() + ((box - icon_size.width()) / kCenteringDivisor),
                         card.top() + ((box - icon_size.height()) / kCenteringDivisor),
                         icon_size.width(),
                         icon_size.height()));
    }
    return {card.left() + box + kDetailsSpacing,
            card.top() + (kDetailsSpacing / kCenteringDivisor),
            card.width() - box - kDetailsSpacing - kDetailsSpacing,
            card.height() - kDetailsSpacing};
}

}  // namespace

void FileExplorerCardDelegate::paint(QPainter* painter,
                                     const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const {
    // Injected group-section headers keep the default painting.
    if (index.data(FileExplorerGroupProxyModel::IsGroupHeaderRole).toBool()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect details = paintCardFrameAndIcon(painter, option, index);

    // Details block: name (BodyStrong), then type and size captions.
    constexpr int kLineSpacing = 2;
    QColor title_color = option.palette.color(QPalette::Text);
    if (const QVariant foreground = index.data(Qt::ForegroundRole); foreground.isValid()) {
        title_color = foreground.value<QBrush>().color();  // cut dimming
    }
    QFont title_font = option.font;
    title_font.setBold(true);
    const QFontMetrics title_metrics(title_font);
    painter->setFont(title_font);
    painter->setPen(title_color);
    int y = details.top();
    painter->drawText(QRect(details.left(), y, details.width(), title_metrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      title_metrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                               Qt::ElideMiddle,
                                               details.width()));
    y += title_metrics.height() + kLineSpacing;
    const QFontMetrics caption_metrics(option.font);
    painter->setFont(option.font);
    painter->setPen(option.palette.color(QPalette::PlaceholderText));
    painter->drawText(
        QRect(details.left(), y, details.width(), caption_metrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        caption_metrics.elidedText(index.data(FileExplorerItemModel::EntryTypeRole).toString(),
                                   Qt::ElideRight,
                                   details.width()));
    if (!index.data(FileExplorerItemModel::EntryDirectoryRole).toBool()) {
        y += caption_metrics.height() + kLineSpacing;
        const QString size_text = FileExplorerItemModel::sizeText(
            index.data(FileExplorerItemModel::EntrySizeRole).toULongLong());
        painter->drawText(QRect(details.left(), y, details.width(), caption_metrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          size_text);
    }
    painter->restore();
}

QWidget* FileExplorerNameDelegate::createEditor(QWidget* parent,
                                                const QStyleOptionViewItem& option,
                                                const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    return new FileExplorerRenameLineEdit(parent);
}

void FileExplorerNameDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    auto* line_edit = qobject_cast<FileExplorerRenameLineEdit*>(editor);
    if (line_edit == nullptr) {
        QStyledItemDelegate::setEditorData(editor, index);
        return;
    }
    const QString text = index.data(Qt::EditRole).toString();
    line_edit->setText(text);
    // Files StartRenameItem: files select the base name (extension excluded
    // when visible); folders select the whole name.
    const bool is_directory = index.data(FileExplorerItemModel::EntryDirectoryRole).toBool();
    int selection_length = static_cast<int>(text.length());
    if (!is_directory) {
        const int last_dot = static_cast<int>(text.lastIndexOf(QLatin1Char('.')));
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
