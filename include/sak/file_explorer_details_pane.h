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
#include <QTextEdit>
#include <QWidget>

class QMimeData;

namespace sak {

/// One Details section: read-only, selectable text with no frame and no scroll bar of its own,
/// as tall as its text at the width it is given, so the details scroller scrolls every section
/// together. It wraps anywhere a line has to break: a long path has no spaces, and a label could
/// break only at spaces and cut it at the pane's edge (review 3, F23). A path breaks after a
/// backslash rather than at a hyphen (review 4, Y28): the marks that allow that are taken out of
/// what is copied and of detailsText(), so what a technician copies is the path as it is.
class FileExplorerDetailsText : public QTextEdit {
    Q_OBJECT

public:
    explicit FileExplorerDetailsText(QWidget* parent = nullptr);

    /// The section's text, plain: a path or name from the browsed disk is never read as markup.
    void setDetailsText(const QString& text);

    /// The text as it was given, without the marks that let a path break after its backslashes.
    [[nodiscard]] QString detailsText() const;

    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    [[nodiscard]] QMimeData* createMimeDataFromSelection() const override;

private:
    /// Left plus right, and top plus bottom, of everything around the text: frame, margins and
    /// the document's own margin.
    [[nodiscard]] QSize chromeSize() const;
};

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
    /// The line above the preview: shown with text, gone without, so the preview card sits one
    /// gap below the Details/Preview switch either way.
    void setPreviewCaption(const QString& text);
    /// The Details tab's sections, each as tall as its text, so the one details scroller scrolls
    /// them together: as text boxes each had a scroll bar of its own and cut its text (review 2,
    /// F23), and as labels a long path could not wrap (review 3, F23).
    [[nodiscard]] FileExplorerDetailsText* propertiesText() const;
    [[nodiscard]] FileExplorerDetailsText* safetyText() const;
    [[nodiscard]] FileExplorerDetailsText* evidenceText() const;
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
    FileExplorerDetailsText* makeDetailsSection(const QString& accessible_name);
    QWidget* buildTabPill();
    QWidget* buildPreviewRegion();
    QScrollArea* buildDetailsScroller();
    void addDetailsSection(QVBoxLayout* layout, const QString& title, QWidget* text);
    void applySelectedTab(bool details, bool persist);

    QPushButton* m_details_tab_button{nullptr};
    QPushButton* m_preview_tab_button{nullptr};
    QBoxLayout* m_body_layout{nullptr};
    QScrollArea* m_details_scroll{nullptr};
    QPlainTextEdit* m_preview_text{nullptr};
    QLabel* m_preview_caption{nullptr};
    QLabel* m_preview_image{nullptr};
    QStackedWidget* m_preview_stack{nullptr};
    FileExplorerDetailsText* m_properties_text{nullptr};
    FileExplorerDetailsText* m_safety_text{nullptr};
    FileExplorerDetailsText* m_evidence_text{nullptr};
};

}  // namespace sak
