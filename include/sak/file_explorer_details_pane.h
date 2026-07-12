// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_details_pane.h
/// @brief File Explorer info pane: Details|Preview segmented tabs over a
/// preview area and a stacked details scroller (Files InfoPane.xaml clone).

#pragma once

#include <QBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QWidget>

namespace sak {

/// Files InfoPane.xaml anatomy: a centered Details|Preview segmented pill,
/// a preview region that is always visible, and a details scroller shown
/// only on the Details tab (rows 2*/3*). The selected tab persists across
/// sessions (InfoPaneSettingsService.SelectedTab default Details) and the
/// preview/details split flips horizontal when the pane is wider than tall
/// (InfoPane.xaml.cs Root_SizeChanged).
class FileExplorerDetailsPane : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerDetailsPane(QWidget* parent = nullptr);

    [[nodiscard]] QPlainTextEdit* previewText() const;
    [[nodiscard]] QLabel* previewImage() const;
    [[nodiscard]] QLabel* previewCaption() const;
    [[nodiscard]] QPlainTextEdit* propertiesText() const;
    [[nodiscard]] QPlainTextEdit* safetyText() const;
    [[nodiscard]] QPlainTextEdit* evidenceText() const;
    [[nodiscard]] QPushButton* detailsTabButton() const;
    [[nodiscard]] QPushButton* previewTabButton() const;

    /// Switch the preview region between the text/hex view (false) and the
    /// image view (true).
    void showImagePreview(bool image);

    /// Select the Details tab (used when the user explicitly asks for
    /// item properties).
    void showDetailsTab();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QPlainTextEdit* makeDetailsText(const QString& accessible_name);
    QWidget* buildTabPill();
    QWidget* buildPreviewRegion();
    QScrollArea* buildDetailsScroller();
    void addDetailsSection(QVBoxLayout* layout, const QString& title, QPlainTextEdit* editor);
    void applySelectedTab(bool details, bool persist);

    QPushButton* m_details_tab_button{nullptr};
    QPushButton* m_preview_tab_button{nullptr};
    QBoxLayout* m_body_layout{nullptr};
    QScrollArea* m_details_scroll{nullptr};
    QPlainTextEdit* m_preview_text{nullptr};
    QLabel* m_preview_caption{nullptr};
    QLabel* m_preview_image{nullptr};
    QStackedWidget* m_preview_stack{nullptr};
    QPlainTextEdit* m_properties_text{nullptr};
    QPlainTextEdit* m_safety_text{nullptr};
    QPlainTextEdit* m_evidence_text{nullptr};
};

}  // namespace sak
