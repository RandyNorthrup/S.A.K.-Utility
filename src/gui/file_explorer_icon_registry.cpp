// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_icon_registry.h"

#include "sak/style_constants.h"

#include <QApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

#include <algorithm>
#include <array>
#include <utility>

namespace sak {
namespace {

using IconDescriptor = FileExplorerIconDescriptor;

// Pixmap cache key packs the requested QSize and tint into one quint64: each dimension is
// masked to 12 bits, the width shifted above the height, and the tint RGBA held in the low bits.
constexpr int kIconCacheDimensionMask = 0xFFF;
constexpr int kIconCacheWidthShift = 44;
constexpr int kIconCacheHeightShift = 32;

[[nodiscard]] IconDescriptor descriptor(const char* key,
                                        const char* file_name,
                                        const char* upstream_key,
                                        const char* upstream_source) {
    return {.key = QString::fromLatin1(key),
            .resource_path = ui::kIconExplorerFilesTemplate.arg(QString::fromLatin1(file_name)),
            .upstream_key = QString::fromLatin1(upstream_key),
            .upstream_source = QString::fromLatin1(upstream_source),
            .license = QStringLiteral("MIT")};
}

[[nodiscard]] IconDescriptor fluentDescriptor(const char* key) {
    return {.key = QString::fromLatin1(key),
            .resource_path = ui::kIconExplorerFluentTemplate.arg(QString::fromLatin1(key)),
            .upstream_key = QString(),
            .upstream_source =
                QStringLiteral("S.A.K.-Utility original, Fluent-style outline glyph"),
            .license = QStringLiteral("AGPL-3.0-or-later")};
}

// The bundled SVGs carry a fixed dark fill, so a raw QIcon disappears on dark
// backgrounds. This engine recolors every rendered pixel with the palette
// foreground at paint time, which keeps icons legible in both theme modes
// without regenerating buttons on a theme switch.
class PaletteTintedIconEngine final : public QIconEngine {
public:
    explicit PaletteTintedIconEngine(QString resource_path)
        : m_resource_path(std::move(resource_path)), m_source(m_resource_path) {}

    void paint(QPainter* painter,
               const QRect& rect,
               const QIcon::Mode mode,
               const QIcon::State state) override {
        if (painter == nullptr) {
            return;
        }
        painter->drawPixmap(rect, pixmap(rect.size(), mode, state));
    }

    QPixmap pixmap(const QSize& size, const QIcon::Mode mode, const QIcon::State state) override {
        const QColor tint = tintColor(mode);
        const quint64 cache_key = (static_cast<quint64>(size.width() & kIconCacheDimensionMask)
                                   << kIconCacheWidthShift) |
                                  (static_cast<quint64>(size.height() & kIconCacheDimensionMask)
                                   << kIconCacheHeightShift) |
                                  static_cast<quint64>(tint.rgba());
        const auto it = m_cache.constFind(cache_key);
        if (it != m_cache.constEnd()) {
            return it.value();
        }
        QPixmap tinted = m_source.pixmap(size, QIcon::Normal, state);
        if (!tinted.isNull()) {
            QPainter overlay(&tinted);
            overlay.setCompositionMode(QPainter::CompositionMode_SourceIn);
            overlay.fillRect(tinted.rect(), tint);
        }
        m_cache.insert(cache_key, tinted);
        return tinted;
    }

    [[nodiscard]] QIconEngine* clone() const override {
        return new PaletteTintedIconEngine(m_resource_path);
    }

private:
    [[nodiscard]] static QColor tintColor(const QIcon::Mode mode) {
        const QPalette palette = QApplication::palette();
        switch (mode) {
        case QIcon::Disabled:
            return palette.color(QPalette::Disabled, QPalette::WindowText);
        case QIcon::Selected:
            return palette.color(QPalette::HighlightedText);
        case QIcon::Active:
        case QIcon::Normal:
            break;
        }
        return palette.color(QPalette::WindowText);
    }

    QString m_resource_path;
    QIcon m_source;
    QHash<quint64, QPixmap> m_cache;
};

[[nodiscard]] QVector<IconDescriptor> fileActionDescriptors() {
    return {
        descriptor("open",
                   "open",
                   "App.ThemedIcons.Open",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Open.xaml"),
        descriptor("open-in-new-tab",
                   "open-in-new-tab",
                   "App.ThemedIcons.OpenInTab",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Open.xaml"),
        descriptor("copy-path",
                   "copy-path",
                   "App.ThemedIcons.CopyAsPath",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Common.xaml"),
        descriptor("refresh",
                   "refresh",
                   "App.ThemedIcons.Refresh",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Common.xaml"),
        descriptor("new-folder",
                   "new-folder",
                   "App.ThemedIcons.New.Folder",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.New.xaml"),
        descriptor("write-file",
                   "write-file",
                   "App.ThemedIcons.New.File",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.New.xaml"),
        descriptor("rename",
                   "rename",
                   "App.ThemedIcons.Rename",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Common.xaml"),
        descriptor("delete",
                   "delete",
                   "App.ThemedIcons.Delete",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Common.xaml"),
    };
}

[[nodiscard]] QVector<IconDescriptor> viewLayoutDescriptors() {
    return {
        descriptor("view-details",
                   "view-details",
                   "App.ThemedIcons.IconLayout.Details",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout.xaml"),
        descriptor("view-list",
                   "view-list",
                   "App.ThemedIcons.IconLayout.List",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout.xaml"),
        descriptor("view-grid",
                   "view-grid",
                   "App.ThemedIcons.IconSize.Small",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout.xaml"),
        descriptor("view-cards",
                   "view-cards",
                   "App.ThemedIcons.IconLayout.Tiles",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout.xaml"),
        descriptor("view-columns",
                   "view-columns",
                   "App.ThemedIcons.IconLayout.Columns",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout.xaml"),
    };
}

[[nodiscard]] QVector<IconDescriptor> viewLayout28Descriptors() {
    return {
        descriptor("view-details-28",
                   "view-details-28",
                   "App.ThemedIcons.IconLayout.Details.28",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout28.xaml"),
        descriptor("view-list-28",
                   "view-list-28",
                   "App.ThemedIcons.IconLayout.List.28",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout28.xaml"),
        descriptor("view-grid-28",
                   "view-grid-28",
                   "App.ThemedIcons.IconLayout.Grid.28",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout28.xaml"),
        descriptor("view-cards-28",
                   "view-cards-28",
                   "App.ThemedIcons.IconLayout.Tiles.28",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout28.xaml"),
        descriptor("view-columns-28",
                   "view-columns-28",
                   "App.ThemedIcons.IconLayout.Columns.28",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout28.xaml"),
    };
}

[[nodiscard]] QVector<IconDescriptor> panelAndStatusDescriptors() {
    return {
        descriptor("panel-left",
                   "panel-left",
                   "App.ThemedIcons.PanelLeft",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Panel.xaml"),
        descriptor("details-pane",
                   "details-pane",
                   "App.ThemedIcons.PanelRight",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Panel.xaml"),
        descriptor("dual-pane",
                   "dual-pane",
                   "App.ThemedIcons.Panes.Vertical",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.TabPane.xaml"),
        descriptor("favorite",
                   "favorite",
                   "App.ThemedIcons.Favorite",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Favorite.xaml"),
        descriptor("status-warning",
                   "status-warning",
                   "App.ThemedIcons.Status.Warning",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Status.xaml"),
        descriptor("properties-general",
                   "properties-general",
                   "App.ThemedIcons.Properties.General",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Properties.Dialog.xaml"),
        descriptor("properties-security",
                   "properties-security",
                   "App.ThemedIcons.Properties.Security",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Properties.Dialog.xaml"),
        descriptor("more",
                   "more",
                   "App.ThemedIcons.More",
                   "src/Files.App.Controls/ThemedIcon/Styles/Icons.Common.xaml"),
    };
}

[[nodiscard]] QVector<IconDescriptor> fluentGlyphDescriptors() {
    QVector<IconDescriptor> items;
    static constexpr std::array kFluentKeys = {
        "nav-back", "nav-forward", "nav-up",     "search", "plus",        "chevron-right",
        "home",     "drive",       "folder",     "file",   "image-file",  "recent",
        "tag",      "shield",      "scan-disks", "close",  "select-mode", "sort",
        "copy",     "paste",       "cut"};
    items.reserve(static_cast<qsizetype>(kFluentKeys.size()));
    for (const char* key : kFluentKeys) {
        items.append(fluentDescriptor(key));
    }
    return items;
}

[[nodiscard]] QVector<IconDescriptor> buildIconDescriptors() {
    QVector<IconDescriptor> items;
    items += fileActionDescriptors();
    items += viewLayoutDescriptors();
    items += viewLayout28Descriptors();
    items += panelAndStatusDescriptors();
    items += fluentGlyphDescriptors();
    return items;
}

[[nodiscard]] const QVector<IconDescriptor>& iconDescriptors() {
    static const QVector<IconDescriptor> items = buildIconDescriptors();
    return items;
}

}  // namespace

QIcon FileExplorerIconRegistry::iconForCommand(const FileExplorerCommandId command) {
    const QString key = iconKeyForCommand(command);
    return iconForKey(key);
}

QIcon FileExplorerIconRegistry::iconForKey(const QString& key) {
    if (key.isEmpty()) {
        return {};
    }

    const FileExplorerIconDescriptor icon = descriptorForKey(key);
    if (icon.resource_path.isEmpty()) {
        return {};
    }
    return QIcon(new PaletteTintedIconEngine(icon.resource_path));
}

QString FileExplorerIconRegistry::iconKeyForCommand(const FileExplorerCommandId command) {
    using Id = FileExplorerCommandId;
    static constexpr auto kIconKeys = std::to_array<std::pair<Id, const char*>>({
        {Id::Back, "nav-back"},
        {Id::Forward, "nav-forward"},
        {Id::Up, "nav-up"},
        {Id::Open, "open"},
        {Id::OpenInNewTab, "open-in-new-tab"},
        {Id::CopyPath, "copy-path"},
        {Id::CopyItemPath, "copy-path"},
        {Id::Refresh, "refresh"},
        {Id::NewFolder, "new-folder"},
        {Id::WriteFile, "write-file"},
        {Id::Rename, "rename"},
        {Id::Delete, "delete"},
        {Id::ViewDetails, "view-details"},
        {Id::ViewList, "view-list"},
        {Id::ViewGrid, "view-grid"},
        {Id::ViewCards, "view-cards"},
        {Id::ViewColumns, "view-columns"},
        {Id::ViewAdaptive, "view-grid"},
        {Id::TogglePreviewPane, "details-pane"},
        {Id::ToggleDualPane, "dual-pane"},
        {Id::OpenInSecondPane, "dual-pane"},
        {Id::CopyItems, "copy"},
        {Id::CutItems, "cut"},
        {Id::Paste, "paste"},
        {Id::PasteIntoSelection, "paste"},
        {Id::Properties, "properties-general"},
        {Id::SelectAll, "select-mode"},
        {Id::Home, "home"},
    });
    const auto it = std::ranges::find(kIconKeys, command, &std::pair<Id, const char*>::first);
    return it != kIconKeys.end() ? QString::fromLatin1(it->second) : QString();
}

FileExplorerIconDescriptor FileExplorerIconRegistry::descriptorForKey(const QString& key) {
    const auto& items = iconDescriptors();
    for (const auto& item : items) {
        if (item.key == key) {
            return item;
        }
    }
    return {};
}

QVector<FileExplorerIconDescriptor> FileExplorerIconRegistry::descriptors() {
    return iconDescriptors();
}

}  // namespace sak
