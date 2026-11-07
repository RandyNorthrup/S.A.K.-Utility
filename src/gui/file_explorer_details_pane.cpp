// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_details_pane.h"

#include "sak/layout_constants.h"
#include "sak/portable_settings.h"
#include "sak/style_constants.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QSettings>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

namespace sak {

namespace {

// The narrowest a Details section asks to be, in average characters. Anything wider is given by
// the pane; a long path wraps to fit rather than widening the scroller (review 3, F23).
constexpr int kDetailsTextMinimumChars = 8;

// Mirrors Files InfoPaneSettingsService.SelectedTab (default Details).
constexpr const char* kInfoPaneSelectedTabKey = "FileManagementExplorer/InfoPane/SelectedTab";
constexpr const char* kInfoPaneTabDetails = "details";
constexpr const char* kInfoPaneTabPreview = "preview";

// InfoPane.xaml: pill buttons are 32px tall; the pill sits in a 12px margin.
// The preview row and details row share the pane 2*:3* (rows 2* and 3*).
constexpr int kInfoPanePillButtonHeight = 32;
constexpr int kInfoPanePillMargin = 12;
constexpr int kInfoPanePreviewStretch = 2;
constexpr int kInfoPaneDetailsStretch = 3;

}  // namespace

FileExplorerDetailsText::FileExplorerDetailsText(QWidget* parent) : QTextEdit(parent) {
    // The info pane's style sheet draws these without a box, on the scroller's own background,
    // as the labels they replace were drawn.
    setProperty("infoPaneDetailsText", true);
    setReadOnly(true);
    setAcceptRichText(false);
    setUndoRedoEnabled(false);
    setTabChangesFocus(true);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    document()->setDocumentMargin(0);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    connect(document(), &QTextDocument::contentsChanged, this, [this]() { updateGeometry(); });
}

void FileExplorerDetailsText::setDetailsText(const QString& text) {
    setPlainText(text);
}

bool FileExplorerDetailsText::hasHeightForWidth() const {
    return true;
}

QSize FileExplorerDetailsText::chromeSize() const {
    const QMargins frame = contentsMargins();
    const QMargins viewport = viewportMargins();
    return {frame.left() + frame.right() + viewport.left() + viewport.right(),
            frame.top() + frame.bottom() + viewport.top() + viewport.bottom()};
}

int FileExplorerDetailsText::heightForWidth(const int width) const {
    const QSize chrome = chromeSize();
    // Measured on a copy, so asking about one width does not lay out the text shown at another.
    const std::unique_ptr<QTextDocument> measure(document()->clone());
    measure->setTextWidth(std::max(1, width - chrome.width()));
    return static_cast<int>(std::ceil(measure->size().height())) + chrome.height();
}

QSize FileExplorerDetailsText::sizeHint() const {
    const int width = QTextEdit::sizeHint().width();
    return {width, heightForWidth(width)};
}

QSize FileExplorerDetailsText::minimumSizeHint() const {
    const QSize chrome = chromeSize();
    return {fontMetrics().averageCharWidth() * kDetailsTextMinimumChars + chrome.width(),
            fontMetrics().height() + chrome.height()};
}

FileExplorerDetailsPane::FileExplorerDetailsPane(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerInfoPane"));
    setAccessibleName(tr("Explorer preview, properties, safety, and evidence"));
    setMinimumWidth(kFileExplorerDetailsPaneMinW);
    setMaximumWidth(kFileExplorerDetailsPaneMaxW);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    root->setSpacing(ui::kSpacingNone);
    root->addWidget(buildTabPill(), 0, Qt::AlignHCenter);

    m_body_layout = new QBoxLayout(QBoxLayout::TopToBottom);
    m_body_layout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    m_body_layout->addWidget(buildPreviewRegion(), kInfoPanePreviewStretch);
    m_details_scroll = buildDetailsScroller();
    m_body_layout->addWidget(m_details_scroll, kInfoPaneDetailsStretch);
    root->addLayout(m_body_layout, 1);

    const PortableSettings settings;
    const QString saved =
        settings.value(QLatin1String(kInfoPaneSelectedTabKey), QLatin1String(kInfoPaneTabDetails))
            .toString();
    applySelectedTab(saved != QLatin1String(kInfoPaneTabPreview), false);
}

QWidget* FileExplorerDetailsPane::buildTabPill() {
    // InfoPane.xaml 154-179: a centered horizontal pill of two exclusive
    // radio-style buttons, Details then Preview.
    auto* pill = new QWidget(this);
    pill->setObjectName(QStringLiteral("fileExplorerInfoPanePill"));
    auto* layout = new QHBoxLayout(pill);
    layout->setContentsMargins(
        kInfoPanePillMargin, kInfoPanePillMargin, kInfoPanePillMargin, kInfoPanePillMargin);
    layout->setSpacing(ui::kSpacingNone);

    m_details_tab_button = new QPushButton(tr("Details"), pill);
    m_details_tab_button->setObjectName(QStringLiteral("fileExplorerInfoPaneDetailsTab"));
    m_preview_tab_button = new QPushButton(tr("Preview"), pill);
    m_preview_tab_button->setObjectName(QStringLiteral("fileExplorerInfoPanePreviewTab"));
    auto* group = new QButtonGroup(pill);
    group->setExclusive(true);
    for (auto* button : {m_details_tab_button, m_preview_tab_button}) {
        button->setCheckable(true);
        button->setFixedHeight(kInfoPanePillButtonHeight);
        button->setFocusPolicy(Qt::TabFocus);
        group->addButton(button);
        layout->addWidget(button);
    }
    m_details_tab_button->setAccessibleName(tr("Show item details"));
    m_preview_tab_button->setAccessibleName(tr("Show item preview"));
    connect(m_details_tab_button, &QPushButton::clicked, this, [this]() {
        applySelectedTab(true, true);
    });
    connect(m_preview_tab_button, &QPushButton::clicked, this, [this]() {
        applySelectedTab(false, true);
    });
    return pill;
}

QWidget* FileExplorerDetailsPane::buildPreviewRegion() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);

    m_preview_caption = new QLabel(container);
    m_preview_caption->setObjectName(QStringLiteral("fileExplorerPreviewCaption"));
    // The caption embeds the entry name, which is bytes parsed straight out of a raw ext2/3/4,
    // HFS+ or APFS image -- fully author-controlled -- so it is never interpreted as markup.
    m_preview_caption->setTextFormat(Qt::PlainText);
    m_preview_caption->setWordWrap(true);
    m_preview_caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview_caption->setAccessibleDescription(tr("Explorer preview caption"));
    // In from the edge as the preview's text is; flush left, it began 25 px left of it (X6).
    m_preview_caption->setContentsMargins(
        ui::kMarginMedium, ui::kMarginNone, ui::kMarginMedium, ui::kMarginNone);
    layout->addWidget(m_preview_caption);

    m_preview_stack = new QStackedWidget(container);
    m_preview_stack->setObjectName(QStringLiteral("fileExplorerPreviewStack"));
    m_preview_text = makeDetailsText(tr("Explorer preview details"));
    m_preview_text->setObjectName(QStringLiteral("fileExplorerPreviewText"));

    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    m_preview_image = new QLabel(scroll);
    m_preview_image->setObjectName(QStringLiteral("fileExplorerPreviewImage"));
    m_preview_image->setAlignment(Qt::AlignCenter);
    m_preview_image->setAccessibleName(tr("Explorer image preview"));
    m_preview_image->setAccessibleDescription(tr("The picture selected in the file list"));
    scroll->setWidget(m_preview_image);

    m_preview_stack->addWidget(m_preview_text);
    m_preview_stack->addWidget(scroll);
    layout->addWidget(m_preview_stack, 1);
    return container;
}

QScrollArea* FileExplorerDetailsPane::buildDetailsScroller() {
    // InfoPane.xaml RootPropertiesScrollViewer: one vertical scroller of
    // stacked captioned sections shown only on the Details tab.
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("fileExplorerInfoPaneDetailsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);

    m_properties_text = makeDetailsSection(tr("Explorer item properties"));
    m_properties_text->setObjectName(QStringLiteral("fileExplorerPropertiesText"));
    addDetailsSection(layout, tr("Properties"), m_properties_text);
    m_safety_text = makeDetailsSection(tr("Explorer target safety"));
    m_safety_text->setObjectName(QStringLiteral("fileExplorerSafetyText"));
    addDetailsSection(layout, tr("Safety"), m_safety_text);
    m_evidence_text = makeDetailsSection(tr("Explorer evidence details"));
    m_evidence_text->setObjectName(QStringLiteral("fileExplorerEvidenceText"));
    addDetailsSection(layout, tr("Evidence"), m_evidence_text);
    layout->addStretch(1);

    scroll->setWidget(content);
    return scroll;
}

void FileExplorerDetailsPane::addDetailsSection(QVBoxLayout* layout,
                                                const QString& title,
                                                QWidget* text) {
    auto* header = new QLabel(title, this);
    header->setProperty("infoPaneSectionHeader", true);
    layout->addWidget(header);
    layout->addWidget(text);
}

FileExplorerDetailsText* FileExplorerDetailsPane::makeDetailsSection(
    const QString& accessible_name) {
    // Each section is as tall as its text and the details scroller scrolls them together. As
    // five-line text boxes each scrolled on its own inside that scroller and cut its lines
    // ("Capability: NTFS - local file API -", review 2, F23); as labels a long path could break
    // only at a space and ran past the pane's edge (review 3, F23).
    auto* section = new FileExplorerDetailsText(this);
    // A text box's name is said beside its text, not in place of it, so it can name the section.
    section->setAccessibleName(accessible_name);
    return section;
}

void FileExplorerDetailsPane::applySelectedTab(const bool details, const bool persist) {
    m_details_tab_button->setChecked(details);
    m_preview_tab_button->setChecked(!details);
    // Files keeps the preview visible on both tabs; Details adds the
    // properties scroller below it (InfoPane.xaml SelectedTab states).
    m_details_scroll->setVisible(details);
    if (persist) {
        PortableSettings settings;
        settings.setValue(QLatin1String(kInfoPaneSelectedTabKey),
                          QLatin1String(details ? kInfoPaneTabDetails : kInfoPaneTabPreview));
    }
}

void FileExplorerDetailsPane::showDetailsTab() {
    applySelectedTab(true, true);
}

void FileExplorerDetailsPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // InfoPane.xaml.cs Root_SizeChanged: side-by-side when wider than tall.
    const bool horizontal = width() >= height();
    const QBoxLayout::Direction direction = horizontal ? QBoxLayout::LeftToRight
                                                       : QBoxLayout::TopToBottom;
    if ((m_body_layout != nullptr) && m_body_layout->direction() != direction) {
        m_body_layout->setDirection(direction);
    }
}

void FileExplorerDetailsPane::showImagePreview(bool image) {
    if (m_preview_stack != nullptr) {
        m_preview_stack->setCurrentIndex(image ? 1 : 0);
    }
}

QPlainTextEdit* FileExplorerDetailsPane::makeDetailsText(const QString& accessible_name) {
    auto* editor = new QPlainTextEdit(this);
    editor->setReadOnly(true);
    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor->setAccessibleName(accessible_name);
    return editor;
}

QPlainTextEdit* FileExplorerDetailsPane::previewText() const {
    return m_preview_text;
}

QLabel* FileExplorerDetailsPane::previewImage() const {
    return m_preview_image;
}

QLabel* FileExplorerDetailsPane::previewCaption() const {
    return m_preview_caption;
}

FileExplorerDetailsText* FileExplorerDetailsPane::propertiesText() const {
    return m_properties_text;
}

FileExplorerDetailsText* FileExplorerDetailsPane::safetyText() const {
    return m_safety_text;
}

FileExplorerDetailsText* FileExplorerDetailsPane::evidenceText() const {
    return m_evidence_text;
}

QPushButton* FileExplorerDetailsPane::detailsTabButton() const {
    return m_details_tab_button;
}

QPushButton* FileExplorerDetailsPane::previewTabButton() const {
    return m_preview_tab_button;
}

}  // namespace sak
