// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_inspector_panel.cpp
/// @brief UI construction and signal/slot handlers for the Email Tools

#include "sak/email_inspector_panel.h"

#include "sak/detachable_log_window.h"
#include "sak/email_attachment_saver.h"
#include "sak/email_attachments_browser_dialog.h"
#include "sak/email_calendar_dialog.h"
#include "sak/email_constants.h"
#include "sak/email_contacts_dialog.h"
#include "sak/email_file_scanner_dialog.h"
#include "sak/email_folder_selection.h"
#include "sak/email_html_sanitizer.h"
#include "sak/email_safe_text_browser.h"
#include "sak/email_view_ids.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/ribbon_tool_button.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStorageInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace sak {

namespace {

constexpr int kEmailHeaderCheckRadiusPx = ui::kCssRadiusSmallPx;
constexpr int kEmailHeaderCheckTickPenWidth = ui::kCssBorderWidthAccentPx;
constexpr int kEmailHeaderCheckTickX0 = 3;
constexpr int kEmailHeaderCheckTickY0 = 8;
constexpr int kEmailHeaderCheckTickX1 = 6;
constexpr int kEmailHeaderCheckTickY1 = 11;
constexpr int kEmailHeaderCheckTickX2 = 13;
constexpr int kEmailHeaderCheckTickY2 = 4;
constexpr int kEmailHeaderCheckDashX0 = 4;
constexpr int kEmailHeaderCheckDashY = 8;
constexpr int kEmailHeaderCheckDashX1 = 12;
constexpr int kEmailSelectHeaderColumn = 0;
constexpr double kRibbonPrimaryHoverAlpha = ui::kHoverAlphaSubtle;
constexpr double kRibbonPrimaryHoverBorderAlpha = 0.25;
constexpr double kRibbonPrimaryPressedAlpha = ui::kPressedAlphaSubtle;
constexpr double kRibbonPrimaryPressedBorderAlpha = 0.40;
constexpr int kPropertiesTableColumnCount = 2;
constexpr int kAttachmentTableColumnFilename = 0;
constexpr int kAttachmentTableColumnSize = 1;
constexpr int kAttachmentTableColumnMimeType = 2;
constexpr int kAttachmentTableColumnContentId = 3;
constexpr int kAttachmentTableColumnCount = 4;
constexpr int kPropertiesDetailTabIndex = 2;
constexpr int kFolderSortSentItems = 2;
constexpr int kFolderSortDeletedItems = 3;
constexpr int kFolderSortArchive = 4;
constexpr int kFolderSortJunkEmail = 5;
constexpr int kFolderSortOutbox = 6;
constexpr int kFolderSortUserFolder = 10;
constexpr int kTaskStatusCount = 5;
constexpr int kDefaultNoteColorIndex = 3;
constexpr int kInlineImageAttrPrefixCaptureGroup = 1;
constexpr int kInlineImageAttrQuoteCaptureGroup = 2;
constexpr int kInlineImageSrcCaptureGroup = 3;
constexpr int kLargeByteDisplayPrecision = 2;

class EmailSelectHeaderView : public QHeaderView {
public:
    explicit EmailSelectHeaderView(QWidget* parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent) {
        setSectionsClickable(true);
    }

    void setTriState(Qt::CheckState state) {
        if (m_state == state) {
            return;
        }
        m_state = state;
        viewport()->update();
    }

    void setToggleHandler(std::function<void(bool)> handler) {
        m_toggle_handler = std::move(handler);
    }

protected:
    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override {
        Q_ASSERT(painter != nullptr);
        QHeaderView::paintSection(painter, rect, logicalIndex);
        if (logicalIndex != kEmailSelectHeaderColumn) {
            return;
        }

        constexpr int side = ui::kUiIconSmall;
        const int cx = rect.x() + ((rect.width() - side) / 2);
        const int cy = rect.y() + ((rect.height() - side) / 2);
        const QRect check_rect(cx, cy, side, side);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (m_state == Qt::Unchecked) {
            painter->setBrush(QColor(QString::fromLatin1(ui::kColorBgSurface)));
            painter->setPen(QPen(QColor(QString::fromLatin1(ui::kColorBorderMuted)),
                                 ui::kCssBorderWidthDefaultPx));
            painter->drawRoundedRect(check_rect,
                                     kEmailHeaderCheckRadiusPx,
                                     kEmailHeaderCheckRadiusPx);
        } else {
            painter->setBrush(QColor(QString::fromLatin1(ui::kColorPrimary)));
            painter->setPen(QPen(QColor(QString::fromLatin1(ui::kColorPrimaryDark)),
                                 ui::kCssBorderWidthDefaultPx));
            painter->drawRoundedRect(check_rect,
                                     kEmailHeaderCheckRadiusPx,
                                     kEmailHeaderCheckRadiusPx);
            painter->setPen(QPen(Qt::white,
                                 kEmailHeaderCheckTickPenWidth,
                                 Qt::SolidLine,
                                 Qt::RoundCap,
                                 Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            if (m_state == Qt::Checked) {
                QPainterPath tick;
                tick.moveTo(cx + kEmailHeaderCheckTickX0, cy + kEmailHeaderCheckTickY0);
                tick.lineTo(cx + kEmailHeaderCheckTickX1, cy + kEmailHeaderCheckTickY1);
                tick.lineTo(cx + kEmailHeaderCheckTickX2, cy + kEmailHeaderCheckTickY2);
                painter->drawPath(tick);
            } else {
                painter->drawLine(cx + kEmailHeaderCheckDashX0,
                                  cy + kEmailHeaderCheckDashY,
                                  cx + kEmailHeaderCheckDashX1,
                                  cy + kEmailHeaderCheckDashY);
            }
        }
        painter->restore();
    }

    void mousePressEvent(QMouseEvent* event) override {
        Q_ASSERT(event != nullptr);
        if (logicalIndexAt(event->pos()) == kEmailSelectHeaderColumn) {
            const bool should_check_all = (m_state != Qt::Checked);
            m_state = should_check_all ? Qt::Checked : Qt::Unchecked;
            viewport()->update();
            if (m_toggle_handler) {
                m_toggle_handler(should_check_all);
            }
            event->accept();
            return;
        }
        QHeaderView::mousePressEvent(event);
    }

private:
    Qt::CheckState m_state{Qt::Unchecked};
    std::function<void(bool)> m_toggle_handler;
};

/// Detect an image MIME type from raw bytes using `QImageReader`.
/// Returns the IANA name (e.g. `image/png`) or an empty `QByteArray` if the
/// format is not recognised.
QByteArray detectImageMime(const QByteArray& data) {
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray fmt = QImageReader::imageFormat(&buffer).toLower();
    if (fmt.isEmpty()) {
        return {};
    }
    // Qt returns "jpeg" for JPEG; map everything else as "image/<fmt>".
    if (fmt == "jpg" || fmt == "jpeg") {
        return QByteArrayLiteral("image/jpeg");
    }
    return QByteArrayLiteral("image/") + fmt;
}

QTableWidgetItem* makeEmailSelectItem(uint64_t item_id, const QString& tooltip) {
    auto* select_cell = new QTableWidgetItem();
    select_cell->setFlags((select_cell->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) &
                          ~Qt::ItemIsEditable);
    select_cell->setCheckState(Qt::Unchecked);
    select_cell->setTextAlignment(Qt::AlignCenter);
    select_cell->setToolTip(tooltip);
    select_cell->setData(Qt::UserRole, QVariant::fromValue(item_id));
    return select_cell;
}

/// Neutralise remote image/resource references in an HTML body so that
/// `QTextBrowser` cannot reach out to the network.  `cid:` and `data:`
/// references are preserved.
QString stripRemoteContent(QString html) {
    static const QRegularExpression kRemoteAttr(
        QStringLiteral("(\\bsrc|\\bbackground|\\bposter)\\s*=\\s*[\"'](?:https?:|//)[^\"']*[\"']"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(kRemoteAttr, QStringLiteral("\\1=\"\""));

    static const QRegularExpression kCssRemoteUrl(
        QStringLiteral("url\\(\\s*['\"]?(?:https?:|//)[^)'\"]*['\"]?\\s*\\)"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(kCssRemoteUrl, QStringLiteral("none"));
    return html;
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

EmailInspectorPanel::EmailInspectorPanel(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<EmailInspectorController>(this)) {
    setupRedrawTimer();
    setupUi();
    connectController();
}

void EmailInspectorPanel::setupRedrawTimer() {
    constexpr int kRedrawDebounceMs = 50;
    m_redraw_timer = new QTimer(this);
    m_redraw_timer->setSingleShot(true);
    m_redraw_timer->setInterval(kRedrawDebounceMs);
    connect(m_redraw_timer, &QTimer::timeout, this, [this] {
        if (!m_dialog_active) {
            displayItemDetail(m_current_detail);
        }
    });
}

EmailInspectorPanel::~EmailInspectorPanel() {
    m_controller->cancelOperation();
}

// ============================================================================
// UI Construction
// ============================================================================

void EmailInspectorPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    root_layout->setSpacing(ui::kSpacingSmall);

    // Dynamic panel header
    m_header_widgets =
        createDynamicPanelHeader(this,
                                 QStringLiteral(":/icons/icons/panel_email.svg"),
                                 tr("Email Tools"),
                                 tr("Browse and export emails, contacts, calendar "
                                    "items, tasks, and notes from PST/OST/MBOX files"),
                                 root_layout);

    // Ribbon toolbar
    root_layout->addWidget(createRibbon());

    // Main splitter: folder tree | content area
    m_main_splitter = new QSplitter(Qt::Horizontal, this);
    m_main_splitter->setChildrenCollapsible(false);
    m_main_splitter->addWidget(createFolderTreePanel());
    m_main_splitter->addWidget(createContentArea());
    m_main_splitter->setSizes({email::kFolderTreeDefaultWidth, email::kContentPaneDefaultWidth});
    root_layout->addWidget(m_main_splitter, 1);

    // Status bar
    auto* status_row = new QHBoxLayout();
    m_status_label = new QLabel(tr("Ready"), this);
    m_status_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    status_row->addWidget(m_status_label, 1);

    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setMaximumWidth(email::kInspectorProgressMaxWidth);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setFormat(QStringLiteral("%p%"));
    m_progress_bar->setVisible(false);
    m_progress_bar->setAccessibleName(QStringLiteral("Email Operation Progress"));
    status_row->addWidget(m_progress_bar);
    root_layout->addLayout(status_row);

    addPreviewControlRow(root_layout);
}

void EmailInspectorPanel::addPreviewControlRow(QVBoxLayout* root_layout) {
    Q_ASSERT(root_layout);

    m_log_toggle = new LogToggleSwitch(tr("Log"), this);
    m_images_toggle_switch = new LogToggleSwitch(tr("Images"), this);
    m_images_toggle_switch->setToolTip(
        tr("Load images and remote content in the preview (off by default for security)"));
    connect(m_images_toggle_switch, &LogToggleSwitch::toggled, this, [this](bool on) {
        m_show_images = on;
        if (on && !m_current_detail.body_html.isEmpty()) {
            fetchRemoteImages(m_current_detail.body_html);
        }
        displayItemDetail(m_current_detail);
    });
    m_html_toggle_switch = new LogToggleSwitch(tr("HTML"), this);
    m_html_toggle_switch->setToolTip(tr("Render HTML body (off shows plain text)"));
    m_html_toggle_switch->setChecked(true);
    connect(m_html_toggle_switch, &LogToggleSwitch::toggled, this, [this](bool on) {
        m_show_html = on;
        displayItemDetail(m_current_detail);
    });
    auto* log_toggle_layout = new QHBoxLayout();
    log_toggle_layout->addWidget(m_log_toggle);
    log_toggle_layout->addStretch();
    m_export_emails_button = new QPushButton(tr("Save Selected Email"), this);
    m_export_emails_button->setEnabled(false);
    m_export_emails_button->setAccessibleName(QStringLiteral("Save Selected Email"));
    m_export_emails_button->setToolTip(tr("Save checked emails as HTML, text, EML, or PDF"));
    m_export_emails_button->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(
        m_export_emails_button, &QPushButton::clicked, this, &EmailInspectorPanel::onExportClicked);
    log_toggle_layout->addWidget(m_export_emails_button);
    log_toggle_layout->addSpacing(sak::ui::kSpacingLarge);
    log_toggle_layout->addWidget(m_images_toggle_switch);
    log_toggle_layout->addSpacing(sak::ui::kSpacingLarge);
    log_toggle_layout->addWidget(m_html_toggle_switch);
    root_layout->addLayout(log_toggle_layout);
}

// ============================================================================
// Ribbon Toolbar
// ============================================================================

namespace {

constexpr int kRibbonIconSize = 28;
constexpr int kRibbonSearchIconInset = 6;
constexpr int kRibbonButtonWidth = 64;
constexpr int kRibbonButtonHeight = 56;

QString ribbonStyle() {
    return QString::fromLatin1(ui::kEmailRibbonStyle)
        .arg(ui::cssColor(ui::kColorBgSurface),
             ui::cssColor(ui::kColorBgPageHover),
             QString::number(ui::kCssBorderWidthDefaultPx),
             ui::cssColor(ui::kColorBorderDefault),
             QString::number(ui::kCssRadiusLargePx));
}

QString ribbonButtonStyle() {
    return QString::fromLatin1(ui::kEmailRibbonButtonStyle)
        .arg(ui::kCssBorderWidthDefaultPx)
        .arg(ui::kCssRadiusMediumPx)
        .arg(ui::kCssPaddingSmallPx)
        .arg(ui::kCssPaddingTinyPx)
        .arg(ui::kFontSizeNote)
        .arg(ui::kFontWeightMedium)
        .arg(ui::cssColor(ui::kColorTextBody),
             ui::colorWithAlpha(ui::kColorPrimary, kRibbonPrimaryHoverAlpha),
             ui::colorWithAlpha(ui::kColorPrimary, kRibbonPrimaryHoverBorderAlpha),
             ui::colorWithAlpha(ui::kColorPrimary, kRibbonPrimaryPressedAlpha),
             ui::colorWithAlpha(ui::kColorPrimary, kRibbonPrimaryPressedBorderAlpha),
             ui::cssColor(ui::kColorTextDisabled));
}

QString ribbonSeparatorStyle() {
    return QString::fromLatin1(ui::kEmailRibbonSeparatorStyle)
        .arg(ui::cssColor(ui::kColorBorderDefault))
        .arg(ui::kSpacingSmall);
}

QToolButton* makeRibbonButton(const QString& text,
                              const QString& icon_path,
                              const QString& tooltip,
                              QWidget* parent) {
    auto* button = new ui::RibbonToolButton(parent, QSize(kRibbonButtonWidth, kRibbonButtonHeight));
    button->setText(text);
    button->setIcon(QIcon(icon_path));
    button->setIconSize(QSize(kRibbonIconSize, kRibbonIconSize));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setToolTip(tooltip);
    button->setAccessibleName(text);
    button->setStyleSheet(ribbonButtonStyle());
    return button;
}

QFrame* makeRibbonSeparator(QWidget* parent) {
    auto* sep = new QFrame(parent);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedWidth(ui::kUiSeparatorWidth);
    sep->setStyleSheet(ribbonSeparatorStyle());
    return sep;
}

}  // anonymous namespace

void EmailInspectorPanel::addRibbonFileGroup(QHBoxLayout* layout, QWidget* ribbon) {
    m_scan_files_button = makeRibbonButton(tr("Scan"),
                                           QStringLiteral(":/icons/icons/icons8-radar.svg"),
                                           tr("Scan common locations for email data files"),
                                           ribbon);
    connect(m_scan_files_button,
            &QToolButton::clicked,
            this,
            &EmailInspectorPanel::onScanForFilesClicked);
    layout->addWidget(m_scan_files_button);

    m_open_button = makeRibbonButton(tr("Open"),
                                     QStringLiteral(":/icons/icons/icons8-opened-folder.svg"),
                                     tr("Open a PST, OST, or MBOX file"),
                                     ribbon);
    connect(m_open_button, &QToolButton::clicked, this, &EmailInspectorPanel::onOpenFileClicked);
    layout->addWidget(m_open_button);

    m_close_button = makeRibbonButton(tr("Close"),
                                      QStringLiteral(":/icons/icons/icons8-close-window.svg"),
                                      tr("Close the current file"),
                                      ribbon);
    m_close_button->setEnabled(false);
    connect(m_close_button, &QToolButton::clicked, this, &EmailInspectorPanel::onCloseFileClicked);
    layout->addWidget(m_close_button);

    layout->addWidget(makeRibbonSeparator(ribbon));
}

void EmailInspectorPanel::addRibbonSearchGroup(QHBoxLayout* layout, QWidget* ribbon) {
    m_search_edit = new QLineEdit(ribbon);
    m_search_edit->setPlaceholderText(tr("Search emails..."));
    m_search_edit->setMinimumWidth(ui::kUiSearchMinWidth);
    m_search_edit->setMaximumWidth(ui::kUiSearchMaxWidth);
    m_search_edit->setEnabled(false);
    m_search_edit->setStyleSheet(ui::emailInspectorSearchStyle());
    m_search_edit->setAccessibleName(QStringLiteral("Email Search"));
    layout->addWidget(m_search_edit);

    m_search_button = new QPushButton(ribbon);
    m_search_button->setIcon(QIcon(QStringLiteral(":/icons/icons/icons8-search.svg")));
    m_search_button->setIconSize(
        QSize(kRibbonIconSize - kRibbonSearchIconInset, kRibbonIconSize - kRibbonSearchIconInset));
    m_search_button->setFixedSize(ui::kUiButtonHeightDialog, ui::kUiButtonHeightDialog);
    m_search_button->setEnabled(false);
    m_search_button->setToolTip(tr("Search"));
    m_search_button->setStyleSheet(ui::transparentHoverButtonStyle(ui::kColorBgInfoPanel));
    m_search_button->setAccessibleName(QStringLiteral("Search Emails"));
    connect(m_search_button, &QPushButton::clicked, this, &EmailInspectorPanel::onSearchClicked);
    connect(m_search_edit, &QLineEdit::returnPressed, this, &EmailInspectorPanel::onSearchClicked);
    layout->addWidget(m_search_button);

    layout->addWidget(makeRibbonSeparator(ribbon));
}

void EmailInspectorPanel::addRibbonActionsGroup(QHBoxLayout* layout, QWidget* ribbon) {
    m_attachments_button = makeRibbonButton(tr("Attachments"),
                                            QStringLiteral(":/icons/icons/icons8-attachment.svg"),
                                            tr("Browse all attachments in this mailbox"),
                                            ribbon);
    m_attachments_button->setEnabled(false);
    connect(m_attachments_button,
            &QToolButton::clicked,
            this,
            &EmailInspectorPanel::onExportAttachmentsClicked);
    layout->addWidget(m_attachments_button);

    layout->addWidget(makeRibbonSeparator(ribbon));
}

void EmailInspectorPanel::addRibbonPeopleGroup(QHBoxLayout* layout, QWidget* ribbon) {
    m_contacts_button = makeRibbonButton(tr("Contacts"),
                                         QStringLiteral(":/icons/icons/icons8-address-book.svg"),
                                         tr("Open the address book to view and export contacts"),
                                         ribbon);
    m_contacts_button->setEnabled(false);
    connect(
        m_contacts_button, &QToolButton::clicked, this, &EmailInspectorPanel::onContactsClicked);
    layout->addWidget(m_contacts_button);

    m_calendar_button = makeRibbonButton(tr("Calendar"),
                                         QStringLiteral(":/icons/icons/icons8-calendar.svg"),
                                         tr("Open calendar viewer to view and export events"),
                                         ribbon);
    m_calendar_button->setEnabled(false);
    connect(
        m_calendar_button, &QToolButton::clicked, this, &EmailInspectorPanel::onCalendarClicked);
    layout->addWidget(m_calendar_button);
}

QWidget* EmailInspectorPanel::createRibbon() {
    auto* ribbon = new QWidget(this);
    ribbon->setObjectName(QStringLiteral("ribbonBar"));
    ribbon->setStyleSheet(ribbonStyle());
    auto* layout = new QHBoxLayout(ribbon);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginTight, ui::kMarginSmall, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingTight);

    addRibbonFileGroup(layout, ribbon);
    addRibbonSearchGroup(layout, ribbon);
    addRibbonActionsGroup(layout, ribbon);
    addRibbonPeopleGroup(layout, ribbon);
    layout->addStretch();
    return ribbon;
}

QWidget* EmailInspectorPanel::createFolderTreePanel() {
    auto* group = new QGroupBox(tr("Folders"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);
    group->setMinimumWidth(email::kFolderTreeMinWidth);

    m_folder_tree = new QTreeWidget(group);
    m_folder_tree->setHeaderHidden(true);
    m_folder_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_folder_tree->setAccessibleName(QStringLiteral("Email Folder Tree"));
    connect(m_folder_tree,
            &QTreeWidget::itemClicked,
            this,
            &EmailInspectorPanel::onFolderTreeItemClicked);
    connect(m_folder_tree,
            &QTreeWidget::customContextMenuRequested,
            this,
            &EmailInspectorPanel::onFolderTreeContextMenu);
    layout->addWidget(m_folder_tree, 1);

    return group;
}

QWidget* EmailInspectorPanel::createContentArea() {
    // Horizontal splitter: item list on left, preview on right
    m_content_splitter = new QSplitter(Qt::Horizontal, this);
    m_content_splitter->setChildrenCollapsible(false);
    m_content_splitter->addWidget(createItemListPanel());
    m_content_splitter->addWidget(createDetailPanel());
    m_content_splitter->setSizes({email::kItemListDefaultWidth, email::kPreviewPaneDefaultWidth});
    m_content_splitter->setStretchFactor(0, 0);
    m_content_splitter->setStretchFactor(1, 1);
    return m_content_splitter;
}

QHBoxLayout* EmailInspectorPanel::createItemPagingRow(QWidget* group) {
    m_item_count_label = new QLabel(group);
    m_item_count_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));

    m_page_size_combo = new QComboBox(group);
    m_page_size_combo->addItems(
        {tr("50"), tr("100"), tr("250"), tr("500"), tr("1000"), tr("5000")});
    m_page_size_combo->setCurrentIndex(1);  // Default 100
    m_page_size_combo->setToolTip(tr("Items per page"));
    m_page_size_combo->setAccessibleName(tr("Email items per page"));
    m_page_size_combo->setFixedWidth(email::kPageSizeComboWidth);
    connect(m_page_size_combo, &QComboBox::currentIndexChanged, this, [this] { applyPageSize(); });

    m_prev_page_button = new QToolButton(group);
    m_prev_page_button->setIcon(ui::selectorChevronLeftToolButtonIcon());
    m_prev_page_button->setIconSize(QSize(ui::kUiIconSmall, ui::kUiIconSmall));
    m_prev_page_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_prev_page_button->setToolTip(tr("Previous page"));
    m_prev_page_button->setAccessibleName(tr("Previous email page"));
    m_prev_page_button->setEnabled(false);
    connect(m_prev_page_button, &QToolButton::clicked, this, [this] {
        if (m_current_page > 0) {
            --m_current_page;
            reloadCurrentPage();
        }
    });

    m_page_label = new QLabel(group);
    m_page_label->setMinimumWidth(email::kPageIndicatorMinWidth);
    m_page_label->setAlignment(Qt::AlignCenter);

    m_next_page_button = new QToolButton(group);
    m_next_page_button->setIcon(ui::selectorChevronRightToolButtonIcon());
    m_next_page_button->setIconSize(QSize(ui::kUiIconSmall, ui::kUiIconSmall));
    m_next_page_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_next_page_button->setToolTip(tr("Next page"));
    m_next_page_button->setAccessibleName(tr("Next email page"));
    m_next_page_button->setEnabled(false);
    connect(m_next_page_button, &QToolButton::clicked, this, [this] {
        const int page_size = currentPageSize();
        const int max_page = (m_current_total > 0) ? (m_current_total - 1) / page_size : 0;
        if (m_current_page < max_page) {
            ++m_current_page;
            reloadCurrentPage();
        }
    });

    auto* filter_row = new QHBoxLayout();
    filter_row->addWidget(m_item_count_label, 1);
    filter_row->addWidget(m_prev_page_button);
    filter_row->addWidget(m_page_label);
    filter_row->addWidget(m_next_page_button);
    filter_row->addSpacing(sak::ui::kSpacingMedium);
    filter_row->addWidget(new QLabel(tr("Per page:"), group));
    filter_row->addWidget(m_page_size_combo);
    return filter_row;
}

void EmailInspectorPanel::configureItemListTable(QWidget* group) {
    m_item_list = new QTableWidget(group);
    m_item_list->setColumnCount(ColCount);
    m_item_list->setAccessibleName(QStringLiteral("Email Items List"));
    auto* select_header = new EmailSelectHeaderView(m_item_list);
    select_header->setAccessibleName(tr("Select all emails"));
    select_header->setToolTip(tr("Select or deselect all emails for export"));
    select_header->setToggleHandler([this](bool checked) { setAllItemListChecks(checked); });
    m_item_list->setHorizontalHeader(select_header);
    m_item_list->setHorizontalHeaderLabels(
        {QString(), tr("Subject"), tr("From / Name"), tr("Date"), tr("Size"), tr("Type")});
    m_item_list->horizontalHeaderItem(ColSelect)->setToolTip(
        tr("Select or deselect all emails for export"));
    m_item_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_item_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_item_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_item_list->setSortingEnabled(true);
    m_item_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_item_list->horizontalHeader()->setStretchLastSection(true);
    m_item_list->horizontalHeader()->setSectionResizeMode(ColSelect, QHeaderView::Fixed);
    m_item_list->setColumnWidth(ColSelect, email::kAttachmentIndicatorColumnWidth);
    m_item_list->setColumnWidth(ColSubject, email::kSubjectColumnWidth);
    m_item_list->setColumnWidth(ColFrom, email::kSenderColumnWidth);
    m_item_list->setColumnWidth(ColDate, email::kDateColumnWidth);
    m_item_list->setColumnWidth(ColSize, email::kSizeColumnWidth);
    m_item_list->setColumnWidth(ColType, email::kTypeColumnWidth);
    m_item_list->verticalHeader()->setVisible(false);

    connect(
        m_item_list, &QTableWidget::cellClicked, this, &EmailInspectorPanel::onItemListCellClicked);
    connect(m_item_list,
            &QTableWidget::customContextMenuRequested,
            this,
            &EmailInspectorPanel::onItemListContextMenu);
    connect(m_item_list, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item != nullptr && item->column() == ColSelect) {
            updateItemListHeaderCheckState();
        }
    });
}

QWidget* EmailInspectorPanel::createItemListPanel() {
    auto* group = new QGroupBox(tr("Items"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingTight);

    layout->addLayout(createItemPagingRow(group));
    configureItemListTable(group);
    layout->addWidget(m_item_list, 1);
    return group;
}

QWidget* EmailInspectorPanel::createDetailPanel() {
    auto* group = new QGroupBox(tr("Preview"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    group->setMinimumHeight(email::kDetailPanelMinHeight);
    group->setMinimumWidth(email::kDetailPanelMinWidth);

    m_detail_tabs = new QTabWidget(group);
    m_detail_tabs->setAccessibleName(tr("Email preview detail tabs"));
    m_detail_tabs->addTab(createContentTab(), tr("Content"));
    m_detail_tabs->addTab(createHeadersTab(), tr("Headers"));
    m_detail_tabs->addTab(createPropertiesTab(), tr("Properties"));
    m_detail_tabs->addTab(createAttachmentsTab(), tr("Attachments"));
    layout->addWidget(m_detail_tabs, 1);

    return group;
}

QWidget* EmailInspectorPanel::createContentTab() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    layout->setSpacing(ui::kSpacingTight);


    // Renders attacker-authored message markup: EmailSafeTextBrowser denies every non-data:
    // resource load (no file:/// disclosure, no remote tracking pixel) and refuses to navigate
    // on a link activation. See sak/email_safe_text_browser.h.
    m_content_browser = new EmailSafeTextBrowser(this);
    m_content_browser->setAccessibleName(QStringLiteral("Email Content"));
    m_content_browser->setStyleSheet(ui::emailContentBrowserStyle());
    layout->addWidget(m_content_browser, 1);

    return container;
}

QWidget* EmailInspectorPanel::createHeadersTab() {
    m_headers_browser = new QTextBrowser(this);
    m_headers_browser->setReadOnly(true);
    m_headers_browser->setAccessibleName(QStringLiteral("Email Headers"));
    const QFont mono_font(QStringLiteral("Consolas"), ui::kFontSizeNote);
    m_headers_browser->setFont(mono_font);
    return m_headers_browser;
}

QWidget* EmailInspectorPanel::createPropertiesTab() {
    m_properties_table = new QTableWidget(this);
    m_properties_table->setColumnCount(kPropertiesTableColumnCount);
    m_properties_table->setAccessibleName(QStringLiteral("MAPI Properties Table"));
    m_properties_table->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
    m_properties_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_properties_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_properties_table->setSortingEnabled(true);
    m_properties_table->horizontalHeader()->setStretchLastSection(true);
    m_properties_table->verticalHeader()->setVisible(false);
    return m_properties_table;
}

QWidget* EmailInspectorPanel::createAttachmentsTab() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    m_attachments_table = new QTableWidget(container);
    m_attachments_table->setColumnCount(kAttachmentTableColumnCount);
    m_attachments_table->setAccessibleName(QStringLiteral("Email Attachments Table"));
    m_attachments_table->setHorizontalHeaderLabels(
        {tr("Filename"), tr("Size"), tr("MIME Type"), tr("Content ID")});
    m_attachments_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_attachments_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attachments_table->horizontalHeader()->setStretchLastSection(true);
    m_attachments_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_attachments_table, 1);

    auto* button_row = new QHBoxLayout();
    m_save_attachment_button = new QPushButton(tr("Save Selected"), container);
    m_save_attachment_button->setEnabled(false);
    m_save_attachment_button->setStyleSheet(ui::kSecondaryButtonStyle);
    m_save_attachment_button->setAccessibleName(QStringLiteral("Save Selected Attachment"));
    connect(m_save_attachment_button,
            &QPushButton::clicked,
            this,
            &EmailInspectorPanel::onSaveAttachmentClicked);
    button_row->addWidget(m_save_attachment_button);

    m_save_all_attachments_button = new QPushButton(tr("Save All"), container);
    m_save_all_attachments_button->setEnabled(false);
    m_save_all_attachments_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_save_all_attachments_button->setAccessibleName(QStringLiteral("Save All Attachments"));
    connect(m_save_all_attachments_button,
            &QPushButton::clicked,
            this,
            &EmailInspectorPanel::onSaveAllAttachmentsClicked);
    button_row->addWidget(m_save_all_attachments_button);
    button_row->addStretch();
    layout->addLayout(button_row);

    return container;
}

// ============================================================================
// Dialog Signal Isolation
// ============================================================================

void EmailInspectorPanel::disconnectDialogSignals() {
    disconnect(m_controller.get(),
               &EmailInspectorController::folderItemsLoaded,
               this,
               &EmailInspectorPanel::onFolderItemsLoaded);
    disconnect(m_controller.get(),
               &EmailInspectorController::itemDetailLoaded,
               this,
               &EmailInspectorPanel::onItemDetailLoaded);
    disconnect(m_controller.get(),
               &EmailInspectorController::itemPropertiesLoaded,
               this,
               &EmailInspectorPanel::onItemPropertiesLoaded);
    disconnect(m_controller.get(),
               &EmailInspectorController::attachmentContentReady,
               this,
               &EmailInspectorPanel::onAttachmentContentReady);
    disconnect(m_controller.get(),
               &EmailInspectorController::stateChanged,
               this,
               &EmailInspectorPanel::onStateChanged);
    disconnect(m_controller.get(),
               &EmailInspectorController::errorOccurred,
               this,
               &EmailInspectorPanel::onErrorOccurred);
}

void EmailInspectorPanel::reconnectDialogSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::folderItemsLoaded,
            this,
            &EmailInspectorPanel::onFolderItemsLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemDetailLoaded,
            this,
            &EmailInspectorPanel::onItemDetailLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemPropertiesLoaded,
            this,
            &EmailInspectorPanel::onItemPropertiesLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::attachmentContentReady,
            this,
            &EmailInspectorPanel::onAttachmentContentReady);
    connect(m_controller.get(),
            &EmailInspectorController::stateChanged,
            this,
            &EmailInspectorPanel::onStateChanged);
    connect(m_controller.get(),
            &EmailInspectorController::errorOccurred,
            this,
            &EmailInspectorPanel::onErrorOccurred);
}

// ============================================================================
// Controller Connections
// ============================================================================

void EmailInspectorPanel::connectController() {
    Q_ASSERT(m_controller);
    connectControllerFileSignals();
    connectControllerNavigationSignals();
    connectControllerSearchSignals();
    connectControllerExportSignals();
    connectControllerCommonSignals();
    connectControllerMboxSignals();
}

void EmailInspectorPanel::connectControllerFileSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::stateChanged,
            this,
            &EmailInspectorPanel::onStateChanged);

    // File
    connect(m_controller.get(),
            &EmailInspectorController::fileOpened,
            this,
            &EmailInspectorPanel::onFileOpened);
    connect(m_controller.get(),
            &EmailInspectorController::folderTreeLoaded,
            this,
            &EmailInspectorPanel::onFolderTreeLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::fileClosed,
            this,
            &EmailInspectorPanel::onFileClosed);
}

void EmailInspectorPanel::connectControllerNavigationSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::folderItemsLoaded,
            this,
            &EmailInspectorPanel::onFolderItemsLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemDetailLoaded,
            this,
            &EmailInspectorPanel::onItemDetailLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemPropertiesLoaded,
            this,
            &EmailInspectorPanel::onItemPropertiesLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::attachmentContentReady,
            this,
            &EmailInspectorPanel::onAttachmentContentReady);
}

void EmailInspectorPanel::connectControllerSearchSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::searchHit,
            this,
            &EmailInspectorPanel::onSearchHit);
    connect(m_controller.get(),
            &EmailInspectorController::searchComplete,
            this,
            &EmailInspectorPanel::onSearchComplete);
}

void EmailInspectorPanel::connectControllerExportSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::exportStarted,
            this,
            &EmailInspectorPanel::onExportStarted);
    connect(m_controller.get(),
            &EmailInspectorController::exportProgress,
            this,
            &EmailInspectorPanel::onExportProgress);
    connect(m_controller.get(),
            &EmailInspectorController::exportComplete,
            this,
            &EmailInspectorPanel::onExportComplete);
}

void EmailInspectorPanel::connectControllerCommonSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::errorOccurred,
            this,
            &EmailInspectorPanel::onErrorOccurred);
    connect(m_controller.get(),
            &EmailInspectorController::logOutput,
            this,
            &EmailInspectorPanel::logOutput);
    connect(m_controller.get(),
            &EmailInspectorController::progressUpdated,
            this,
            [this](int percent, QString status) {
                if (m_dialog_active) {
                    return;
                }
                m_progress_bar->setValue(percent);
                updateStatusBar(status);
            });
}

void EmailInspectorPanel::connectControllerMboxSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::mboxOpened,
            this,
            &EmailInspectorPanel::onMboxOpened);
    connect(m_controller.get(),
            &EmailInspectorController::mboxMessagesLoaded,
            this,
            &EmailInspectorPanel::onMboxMessagesLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::mboxMessageDetailLoaded,
            this,
            &EmailInspectorPanel::onMboxMessageDetailLoaded);
}

// ============================================================================
// File Operation Slots
// ============================================================================

void EmailInspectorPanel::onOpenFileClicked() {
    const QString filter = tr(
        "Email Data Files (*.pst *.ost *.mbox);;"
        "PST Files (*.pst);;"
        "OST Files (*.ost);;"
        "MBOX Files (*.mbox);;"
        "All Files (*)");
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Open Email Data File"), QString(), filter);
    if (path.isEmpty()) {
        return;
    }
    m_controller->openFile(path);
}

void EmailInspectorPanel::onCloseFileClicked() {
    m_controller->closeFile();
}

// ============================================================================
// Navigation Slots
// ============================================================================

void EmailInspectorPanel::onFolderTreeItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }
    bool ok = false;
    const uint64_t folder_id = item->data(0, Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    m_current_folder_id = folder_id;
    m_current_page = 0;
    m_current_total = 0;
    reloadCurrentPage();
}

void EmailInspectorPanel::onItemListCellClicked(int row, int column) {
    if (column == ColSelect) {
        return;
    }
    if (row < 0 || row >= m_item_list->rowCount()) {
        return;
    }
    auto* subject_item = m_item_list->item(row, ColSubject);
    if (subject_item == nullptr) {
        return;
    }
    bool ok = false;
    const uint64_t item_id = subject_item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    m_controller->loadItemDetail(item_id);
    m_controller->loadItemProperties(item_id);
}

void EmailInspectorPanel::onItemListContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction(tr("Open in Detail Panel"), this, [this] {
        const int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                const uint64_t nid = subject_item->data(Qt::UserRole).toULongLong();
                m_controller->loadItemDetail(nid);
            }
        }
    });
    menu.addSeparator();
    menu.addAction(tr("Export Checked Emails..."), this, [this] { onExportClicked(); });
    menu.addAction(tr("Browse Attachments..."), this, [this] { onExportAttachmentsClicked(); });
    menu.addSeparator();
    menu.addAction(tr("Copy Subject"), this, [this] {
        const int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                QApplication::clipboard()->setText(subject_item->text());
            }
        }
    });
    menu.addAction(tr("View MAPI Properties"), this, [this] {
        const int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                const uint64_t nid = subject_item->data(Qt::UserRole).toULongLong();
                m_controller->loadItemProperties(nid);
                m_detail_tabs->setCurrentIndex(kPropertiesDetailTabIndex);
            }
        }
    });
    menu.exec(m_item_list->viewport()->mapToGlobal(pos));
}

void EmailInspectorPanel::onFolderTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_folder_tree->itemAt(pos);
    if (item == nullptr) {
        return;
    }
    // Resolve the RIGHT-CLICKED folder and export that id. A right-click does not
    // emit itemClicked, so m_current_folder_id still names the last LEFT-clicked
    // folder: exporting through it exported a folder the user never pointed at.
    // Captured by value so the export actions do not have to reach back into the
    // tree once the menu is open. An item with no usable id yields nullopt, and
    // exportFolderAs refuses rather than falling back to the selection.
    const std::optional<uint64_t> folder_id = sak::decodeEmailViewId(item->data(0, Qt::UserRole));

    QMenu menu(this);
    menu.addAction(tr("Export Folder as EML..."), this, [this, folder_id] {
        exportFolderAs(sak::ExportFormat::Eml, folder_id);
    });
    menu.addAction(tr("Export Folder as CSV..."), this, [this, folder_id] {
        exportFolderAs(sak::ExportFormat::CsvEmails, folder_id);
    });
    menu.addSeparator();
    // Whole-store export. The OST Converter produces full mailbox files (MBOX); turning an
    // entire store into per-message files belongs here, next to the per-folder exports.
    QMenu* all_menu = menu.addMenu(tr("Export ALL Mail Folders as"));
    struct AllExportChoice {
        QString label;
        sak::ExportFormat format;
    };
    const QVector<AllExportChoice> all_choices{
        {.label = tr("EML..."), .format = sak::ExportFormat::Eml},
        {.label = tr("HTML..."), .format = sak::ExportFormat::Html},
        {.label = tr("Text..."), .format = sak::ExportFormat::Text},
        {.label = tr("PDF..."), .format = sak::ExportFormat::Pdf},
        {.label = tr("CSV..."), .format = sak::ExportFormat::CsvEmails}};
    for (const AllExportChoice& choice : all_choices) {
        all_menu->addAction(choice.label, this, [this, format = choice.format] {
            exportAllMailFoldersAs(format);
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Browse Attachments..."), this, [this] { onExportAttachmentsClicked(); });
    menu.addSeparator();
    menu.addAction(tr("Search in This Folder..."), this, [this] { m_search_edit->setFocus(); });
    menu.addSeparator();
    menu.addAction(tr("Expand All Subfolders"), this, [item] {
        item->setExpanded(true);
        for (int idx = 0; idx < item->childCount(); ++idx) {
            item->child(idx)->setExpanded(true);
        }
    });
    menu.addAction(tr("Collapse All"), this, [item] { item->setExpanded(false); });
    menu.exec(m_folder_tree->viewport()->mapToGlobal(pos));
}

// ============================================================================
// Search Slots
// ============================================================================

void EmailInspectorPanel::onSearchClicked() {
    const QString query = m_search_edit->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    sak::EmailSearchCriteria criteria;
    criteria.query_text = query;
    criteria.search_subject = true;
    criteria.search_body = true;
    criteria.search_sender = true;
    m_controller->startSearch(criteria);
}

void EmailInspectorPanel::onSearchTextChanged() {
    // Placeholder for debounced incremental search
}

std::optional<uint64_t> EmailInspectorPanel::itemIdForRow(int row) const {
    // Returns nullopt for "this row carries no id", never 0. A PST node id is never 0, but
    // an MBOX message_index legitimately IS 0 for the first message in the file, so using 0
    // as the not-found sentinel silently dropped that message from every checkbox-driven
    // export.
    if (m_item_list == nullptr || row < 0 || row >= m_item_list->rowCount()) {
        return std::nullopt;
    }
    auto* subject_item = m_item_list->item(row, ColSubject);
    if (subject_item == nullptr) {
        return std::nullopt;
    }
    bool ok = false;
    const uint64_t item_id = subject_item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return item_id;
}

void EmailInspectorPanel::setAllItemListChecks(bool checked) {
    if (m_item_list == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_item_list);
    const Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    for (int row = 0; row < m_item_list->rowCount(); ++row) {
        auto* select_item = m_item_list->item(row, ColSelect);
        if (select_item != nullptr) {
            select_item->setCheckState(state);
        }
    }
    updateItemListHeaderCheckState();
}

void EmailInspectorPanel::updateItemListHeaderCheckState() {
    if (m_item_list == nullptr) {
        return;
    }
    auto* select_header = dynamic_cast<EmailSelectHeaderView*>(m_item_list->horizontalHeader());
    if (select_header == nullptr) {
        return;
    }

    int selectable_count = 0;
    int checked_count = 0;
    for (int row = 0; row < m_item_list->rowCount(); ++row) {
        auto* select_item = m_item_list->item(row, ColSelect);
        if (select_item == nullptr || !select_item->flags().testFlag(Qt::ItemIsUserCheckable)) {
            continue;
        }
        ++selectable_count;
        if (select_item->checkState() == Qt::Checked) {
            ++checked_count;
        }
    }

    Qt::CheckState state = Qt::Unchecked;
    if (selectable_count > 0 && checked_count == selectable_count) {
        state = Qt::Checked;
    } else if (checked_count > 0) {
        state = Qt::PartiallyChecked;
    }
    select_header->setTriState(state);
}

QVector<uint64_t> EmailInspectorPanel::checkedItemIds() const {
    QVector<uint64_t> ids;
    if (m_item_list == nullptr) {
        return ids;
    }
    ids.reserve(m_item_list->rowCount());
    for (int row = 0; row < m_item_list->rowCount(); ++row) {
        auto* select_item = m_item_list->item(row, ColSelect);
        if (select_item == nullptr || select_item->checkState() != Qt::Checked) {
            continue;
        }
        if (const auto item_id = itemIdForRow(row)) {
            ids.append(*item_id);
        }
    }
    return ids;
}

// ============================================================================
// Export Slots
// ============================================================================

void EmailInspectorPanel::onExportClicked() {
    if (!m_controller->isFileOpen()) {
        return;
    }
    const QVector<uint64_t> checked_ids = checkedItemIds();
    if (checked_ids.isEmpty()) {
        sak::showWarningLogged(this,
                               tr("No Emails Checked"),
                               tr("Check one or more emails before saving selected email."));
        return;
    }
    const QStringList formats = {
        tr("HTML"),
        tr("Text"),
        tr("EML"),
        tr("PDF"),
    };
    bool accepted = false;
    const QString selected_format = QInputDialog::getItem(this,
                                                          tr("Export Email Format"),
                                                          tr("Save selected emails as:"),
                                                          formats,
                                                          0,
                                                          false,
                                                          &accepted);
    if (!accepted || selected_format.isEmpty()) {
        return;
    }

    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Export Directory"));
    if (dir_path.isEmpty()) {
        return;
    }

    sak::EmailExportConfig config;
    if (selected_format == tr("HTML")) {
        config.format = sak::ExportFormat::Html;
    } else if (selected_format == tr("Text")) {
        config.format = sak::ExportFormat::Text;
    } else if (selected_format == tr("PDF")) {
        config.format = sak::ExportFormat::Pdf;
    } else {
        config.format = sak::ExportFormat::Eml;
    }
    config.output_path = dir_path;
    config.item_ids = checked_ids;
    config.save_attachments_with_messages = true;
    m_controller->exportItems(config);
}

void EmailInspectorPanel::exportAllMailFoldersAs(sak::ExportFormat format) {
    if (!m_controller->isFileOpen()) {
        return;
    }
    // The same function populateFolderTree filters with, so "all mail folders" is exactly
    // the set the technician can see in the tree.
    const QVector<uint64_t> folder_ids = sak::mailFolderNodeIds(m_cached_folder_tree);
    if (folder_ids.isEmpty()) {
        // Refuse rather than launch an export that can only produce an empty directory the
        // technician would read as "this store has no mail".
        sak::showWarningLogged(this,
                               tr("No Mail Folders"),
                               tr("This store has no mail folders to export. Contacts, calendar "
                                  "and task folders are exported from their own windows."));
        return;
    }
    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Export Directory"));
    if (dir_path.isEmpty()) {
        return;
    }

    sak::EmailExportConfig config;
    config.format = format;
    config.output_path = dir_path;
    // Every folder is listed explicitly rather than relying on recurse_subfolders, because
    // the list is already filtered: recursing from the roots would pull the non-mail
    // subfolders back in.
    config.folder_ids = folder_ids;
    config.save_attachments_with_messages = true;
    m_controller->exportItems(config);
}

void EmailInspectorPanel::exportFolderAs(sak::ExportFormat format,
                                         std::optional<uint64_t> folder_id) {
    if (!m_controller->isFileOpen()) {
        return;
    }
    if (!folder_id.has_value()) {
        // Fail visibly. Falling back to the selected folder here would export a
        // folder other than the one the user asked for.
        sak::showWarningLogged(this,
                               tr("Folder Not Identified"),
                               tr("This folder carries no identifier, so nothing was exported."));
        return;
    }
    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Export Directory"));
    if (dir_path.isEmpty()) {
        return;
    }

    sak::EmailExportConfig config;
    config.format = format;
    config.output_path = dir_path;
    config.folder_id = *folder_id;
    config.save_attachments_with_messages = true;
    m_controller->exportItems(config);
}

void EmailInspectorPanel::onExportAttachmentsClicked() {
    if (!m_controller->isFileOpen()) {
        return;
    }
    if (m_batch_save.isActive()) {
        // The dialog takes over the controller's attachment signals for as long as
        // it is open, so a batch running here would never see its remaining
        // arrivals and would never complete. Refuse rather than strand it.
        sak::showWarningLogged(this,
                               tr("Save In Progress"),
                               tr("An attachment save is already running. Wait for it to "
                                  "finish before browsing attachments."));
        return;
    }
    m_dialog_active = true;
    disconnectDialogSignals();
    EmailAttachmentsBrowserDialog dialog(m_controller.get(), m_cached_folder_tree, this);
    const auto result = dialog.exec();
    reconnectDialogSignals();
    m_dialog_active = false;
    if (result == QDialog::Accepted) {
        const uint64_t folder_id = dialog.navigateFolderId();
        const uint64_t message_id = dialog.navigateMessageId();
        if (folder_id != 0 && message_id != 0) {
            m_current_folder_id = folder_id;
            m_current_page = 0;
            m_current_total = 0;
            reloadCurrentPage();
            m_controller->loadItemDetail(message_id);
            m_controller->loadItemProperties(message_id);
        }
    }
}

// ============================================================================
// Attachment Slots
// ============================================================================

std::optional<uint64_t> EmailInspectorPanel::attachmentSaveSource() {
    if (m_batch_save.isActive()) {
        sak::showWarningLogged(this,
                               tr("Save In Progress"),
                               tr("An attachment save is already running. Wait for it to "
                                  "finish before starting another."));
        return std::nullopt;
    }
    if (!m_current_item_id.has_value()) {
        // Without a committed message identity the request cannot name the message
        // the attachment belongs to, and would otherwise be issued against whatever
        // id happened to be left in place.
        sak::showWarningLogged(this,
                               tr("No Email Open"),
                               tr("Open an email before saving its attachments."));
        return std::nullopt;
    }
    return m_current_item_id;
}

void EmailInspectorPanel::startAttachmentBatch(const QString& dir,
                                               const QVector<sak::AttachmentRef>& refs) {
    if (!m_batch_save.begin(dir, refs)) {
        sak::showWarningLogged(this,
                               tr("Cannot Save Attachments"),
                               tr("The attachment save could not be started. "
                                  "Nothing was written."));
        return;
    }
    // Lock the controls for the duration: arrivals from a second batch begun now
    // would interleave with this one's and land in each other's slots.
    setAttachmentSaveControlsEnabled(false);
    updateStatusBar(tr("Saving %1 attachment(s)...").arg(refs.size()));
    for (const auto& ref : refs) {
        m_controller->loadAttachmentContent(ref.message_id, ref.index);
    }
}

void EmailInspectorPanel::finishAttachmentBatchIfComplete() {
    if (!m_batch_save.isComplete()) {
        return;
    }
    updateStatusBar(m_batch_save.summaryText());
    m_batch_save.reset();
    setAttachmentSaveControlsEnabled(!m_current_detail.attachments.isEmpty());
}

void EmailInspectorPanel::onSaveAttachmentClicked() {
    const std::optional<uint64_t> source = attachmentSaveSource();
    if (!source.has_value()) {
        return;
    }
    const int row = m_attachments_table->currentRow();
    if (row < 0 || row >= m_current_detail.attachments.size()) {
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Save Attachment"));
    if (dir.isEmpty()) {
        return;
    }
    const QVector<sak::AttachmentRef> refs{sak::AttachmentRef{
        .message_id = *source, .index = m_current_detail.attachments.at(row).index}};
    startAttachmentBatch(dir, refs);
}

void EmailInspectorPanel::onSaveAllAttachmentsClicked() {
    const std::optional<uint64_t> source = attachmentSaveSource();
    if (!source.has_value()) {
        return;
    }
    if (m_current_detail.attachments.isEmpty()) {
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Save All Attachments"));
    if (dir.isEmpty()) {
        return;
    }
    QVector<sak::AttachmentRef> refs;
    refs.reserve(m_current_detail.attachments.size());
    for (const auto& attachment : m_current_detail.attachments) {
        refs.append(sak::AttachmentRef{.message_id = *source, .index = attachment.index});
    }
    startAttachmentBatch(dir, refs);
}

// ============================================================================
// Controller Signal Handlers
// ============================================================================

void EmailInspectorPanel::onStateChanged(EmailInspectorController::State state) {
    if (m_dialog_active) {
        return;
    }
    const bool idle = (state == EmailInspectorController::State::Idle);
    setOperationRunning(!idle);
}

void EmailInspectorPanel::onFileOpened(sak::PstFileInfo info) {
    m_close_button->setEnabled(true);
    m_search_edit->setEnabled(true);
    m_search_button->setEnabled(true);
    m_export_emails_button->setEnabled(true);
    m_contacts_button->setEnabled(true);
    m_calendar_button->setEnabled(true);
    m_attachments_button->setEnabled(true);
    updateFileInfoBar(info);
}

void EmailInspectorPanel::onFolderTreeLoaded(sak::PstFolderTree tree) {
    m_cached_folder_tree = tree;
    populateFolderTree(tree);
}

void EmailInspectorPanel::onFileClosed() {
    m_folder_tree->clear();
    m_item_list->setRowCount(0);
    updateItemListHeaderCheckState();
    m_content_browser->clear();
    m_headers_browser->clear();
    m_properties_table->setRowCount(0);
    m_attachments_table->setRowCount(0);
    m_status_label->clear();
    m_item_count_label->clear();
    m_current_items.clear();
    m_current_folder_id = 0;
    m_current_item_id.reset();
    m_properties_item_id.reset();
    m_current_detail = {};
    m_current_page = 0;
    m_current_total = 0;
    m_contact_folder_ids.clear();
    m_calendar_folder_ids.clear();
    m_cached_folder_tree.clear();
    m_batch_save.reset();

    m_close_button->setEnabled(false);
    m_search_edit->setEnabled(false);
    m_search_button->setEnabled(false);
    m_export_emails_button->setEnabled(false);
    m_contacts_button->setEnabled(false);
    m_calendar_button->setEnabled(false);
    m_attachments_button->setEnabled(false);
    m_save_attachment_button->setEnabled(false);
    m_save_all_attachments_button->setEnabled(false);

    updateStatusBar(tr("Ready"));
}

void EmailInspectorPanel::onFolderItemsLoaded(uint64_t /*folder_id*/,
                                              QVector<sak::PstItemSummary> items,
                                              int total) {
    if (m_dialog_active) {
        return;
    }
    m_current_items = items;
    m_current_total = total;
    populateItemList(items);

    const int page_size = currentPageSize();
    const int first = (items.isEmpty()) ? 0 : ((m_current_page * page_size) + 1);
    const int last = (m_current_page * page_size) + static_cast<int>(items.size());
    if (total > items.size()) {
        m_item_count_label->setText(tr("Showing %1\u2013%2 of %3").arg(first).arg(last).arg(total));
    } else {
        m_item_count_label->setText(tr("%1 items").arg(total));
    }
    updatePageControls();
}

void EmailInspectorPanel::onItemDetailLoaded(sak::PstItemDetail detail) {
    if (m_dialog_active) {
        return;
    }
    // New item -- the per-message inline-image cache is invalid.
    m_inline_images.clear();
    m_remote_images.clear();
    m_redraw_timer->stop();

    // Commit the identity the response itself carries, not the most recently
    // clicked row. Under rapid navigation an out-of-order response would otherwise
    // pair message A's content with message B's identity, and every later action
    // that reads the committed id -- attachment saves above all -- would address
    // the wrong message.
    m_current_item_id = detail.node_id;
    m_current_detail = detail;
    displayItemDetail(detail);
    displayAttachments(detail.attachments);

    // The properties tab is filled by a separate response. Until the one for THIS
    // message arrives, whatever is on screen describes the previous message, so
    // clear it rather than leave it captioned by the message now displayed.
    if (m_properties_item_id != m_current_item_id) {
        m_properties_item_id.reset();
        displayProperties({});
    }

    // Request inline image attachments so they can be rendered once data
    // arrives.  Always requested regardless of the HTML toggle: flipping the
    // toggle later must not require a round-trip to the parser.
    if (!detail.body_html.isEmpty()) {
        for (const auto& attachment : detail.attachments) {
            if (!attachment.content_id.isEmpty()) {
                m_controller->loadAttachmentContent(detail.node_id, attachment.index);
            }
        }
        if (m_show_images) {
            fetchRemoteImages(detail.body_html);
        }
    }
}

void EmailInspectorPanel::onItemPropertiesLoaded(uint64_t item_id,
                                                 QVector<sak::MapiProperty> properties) {
    if (m_dialog_active) {
        return;
    }
    // Apply only the response that describes the message the detail view has
    // committed. Responses arrive out of order under rapid navigation, and
    // applying whichever one lands last would caption message A's properties with
    // message B's identity.
    if (m_current_item_id != item_id) {
        return;
    }
    m_properties_item_id = item_id;
    displayProperties(properties);
}

void EmailInspectorPanel::onAttachmentContentReady(uint64_t message_id,
                                                   int index,
                                                   QByteArray attachment_data,
                                                   QString filename) {
    if (m_dialog_active) {
        return;
    }
    const sak::AttachmentRef ref{.message_id = message_id, .index = index};

    // Save operation: record only what this batch actually asked for. A stray
    // arrival -- an inline-image fetch, or a leftover from an earlier batch --
    // would otherwise be written into one of the batch's slots, saving the wrong
    // payload while the batch still reported a full count.
    if (m_batch_save.expects(ref)) {
        const auto result = m_batch_save.recordOne(ref, filename, attachment_data);
        if (!result.success) {
            Q_EMIT logOutput(tr("Attachment save failed: %1").arg(result.error_message));
        }
        finishAttachmentBatchIfComplete();
        return;
    }

    // Not part of a save: inline CID image for the message on screen. A late
    // arrival for a previously opened message must not be cached under this
    // message's Content-Ids.
    if (m_current_item_id != message_id) {
        return;
    }
    for (const auto& att : m_current_detail.attachments) {
        if (att.index == index && !att.content_id.isEmpty()) {
            // Cache by Content-Id so the HTML builder can inline as data URI.
            m_inline_images.insert(att.content_id, attachment_data);
            // Coalesce: multiple inline images commonly arrive in rapid
            // succession -- debounce the full-HTML repaint so we only
            // re-parse once per burst.
            m_redraw_timer->start();
            return;
        }
    }
}

void EmailInspectorPanel::onSearchHit(sak::EmailSearchHit /*hit*/) {
    // Incrementally update result count in status bar
}

void EmailInspectorPanel::onSearchComplete(int total_hits) {
    updateStatusBar(tr("Search complete: %1 hits").arg(total_hits));
}

void EmailInspectorPanel::onExportStarted(int total) {
    m_progress_bar->setVisible(true);
    m_progress_bar->setRange(0, total);
    m_progress_bar->setValue(0);
    updateStatusBar(tr("Exporting %1 items...").arg(total));
}

void EmailInspectorPanel::onExportProgress(int done, int total, qint64 /*bytes*/) {
    m_progress_bar->setRange(0, total);
    m_progress_bar->setValue(done);
}

void EmailInspectorPanel::onExportComplete(sak::EmailExportResult result) {
    m_progress_bar->setVisible(false);
    if (result.items_failed > 0 || !result.errors.isEmpty()) {
        // A partial or failed export must not read as a clean success. Surface the failed
        // count and every recorded error instead of the flat "Export complete".
        m_status_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorError));
        updateStatusBar(tr("Export INCOMPLETE: %1 exported, %2 failed (to %3)")
                            .arg(result.items_exported)
                            .arg(result.items_failed)
                            .arg(result.export_path));
        Q_EMIT logOutput(tr("Export INCOMPLETE: %1 exported, %2 failed (%3 bytes)")
                             .arg(result.items_exported)
                             .arg(result.items_failed)
                             .arg(formatBytes(result.total_bytes)));
        for (const QString& err : result.errors) {
            Q_EMIT logOutput(tr("  Export error: %1").arg(err));
        }
        return;
    }
    updateStatusBar(
        tr("Export complete: %1 items to %2").arg(result.items_exported).arg(result.export_path));
    Q_EMIT logOutput(tr("Exported %1 items (%2 bytes)")
                         .arg(result.items_exported)
                         .arg(formatBytes(result.total_bytes)));
}

void EmailInspectorPanel::onErrorOccurred(QString message) {
    m_status_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorError));
    updateStatusBar(tr("Error: %1").arg(message));
    sak::logError("Email Tools: {}", message.toStdString());
    Q_EMIT logOutput(tr("Error: %1").arg(message));

    // A failed attachment read is reported through this signal and names no
    // attachment, so an in-flight batch has to count it. Without this the batch
    // never reaches its expected total, and the save controls -- locked for the
    // duration of a batch -- would stay locked for the rest of the session.
    if (m_batch_save.isActive()) {
        m_batch_save.recordError();
        finishAttachmentBatchIfComplete();
    }
}

// MBOX-specific handlers
void EmailInspectorPanel::onMboxOpened(int message_count) {
    m_close_button->setEnabled(true);
    m_search_edit->setEnabled(true);
    m_search_button->setEnabled(true);
    m_export_emails_button->setEnabled(true);
    m_contacts_button->setEnabled(false);
    m_calendar_button->setEnabled(false);
    m_attachments_button->setEnabled(true);

    // For MBOX, create a single root folder item. Its folder id is 0, which the PST
    // paging path treats as "no folder selected"; m_mbox_active tells reloadCurrentPage
    // that 0 is a real, loadable folder here so clicking the Inbox loads messages.
    m_mbox_active = true;
    m_folder_tree->clear();
    auto* root = new QTreeWidgetItem(m_folder_tree);
    root->setText(0, tr("Inbox (%1 messages)").arg(message_count));
    root->setData(0, Qt::UserRole, QVariant::fromValue<uint64_t>(0));
    m_folder_tree->expandAll();

    updateStatusBar(tr("Opened MBOX: %1 messages").arg(message_count));
}

void EmailInspectorPanel::onMboxMessagesLoaded(QVector<sak::MboxMessage> messages, int total) {
    m_item_list->setSortingEnabled(false);
    m_item_list->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_item_list);

    m_current_total = total;

    // Filter only messages with no subject, no sender AND no attachments -- empty MBOX
    // delimiters. A message that carries attachments (or a subject/sender) is real
    // forensic content and stays visible even when the remaining headers are blank.
    QVector<int> visible_indices;
    visible_indices.reserve(messages.size());
    for (int idx = 0; idx < messages.size(); ++idx) {
        const auto& msg = messages.at(idx);
        if (!msg.subject.trimmed().isEmpty() || !msg.from.trimmed().isEmpty() ||
            msg.has_attachments) {
            visible_indices.append(idx);
        }
    }

    const int count = visible_indices.size();
    m_item_list->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& msg = messages.at(visible_indices.at(row));
        // Identify the row by the message's absolute index in the MBOX file, NOT by its
        // position within this page's vector. visible_indices holds offsets into the
        // current page after blank-message filtering, so on any page beyond the first --
        // and on any page where a blank message was filtered out -- that offset addresses
        // a different message than the user selected. Opening, exporting or saving
        // attachments then acted on the wrong email.
        const auto message_id = static_cast<uint64_t>(msg.message_index);
        m_item_list->setItem(row,
                             ColSelect,
                             makeEmailSelectItem(message_id, tr("Select email for export")));

        auto* subject_cell = new QTableWidgetItem(msg.subject);
        subject_cell->setData(Qt::UserRole, QVariant::fromValue<uint64_t>(message_id));
        m_item_list->setItem(row, ColSubject, subject_cell);

        m_item_list->setItem(row, ColFrom, new QTableWidgetItem(msg.from));
        m_item_list->setItem(row, ColDate, new QTableWidgetItem(msg.date.toString(Qt::ISODate)));
        m_item_list->setItem(row, ColSize, new QTableWidgetItem(formatBytes(msg.message_size)));

        auto* type_cell = new QTableWidgetItem(tr("Email"));
        type_cell->setIcon(itemTypeQIcon(sak::EmailItemType::Email));
        if (msg.has_attachments) {
            type_cell->setToolTip(tr("Has attachments"));
        }
        m_item_list->setItem(row, ColType, type_cell);
    }
    m_item_list->setUpdatesEnabled(true);
    m_item_list->setSortingEnabled(true);
    updateItemListHeaderCheckState();
    // Blanks removed from THIS page only. `total` counts the whole store across every
    // page, so total-count would mislabel every other page's messages as "blank filtered".
    const int filtered = static_cast<int>(messages.size()) - count;
    QString label = tr("%1 messages (showing %2)").arg(total).arg(count);
    if (filtered > 0) {
        label += tr("  [%1 blank filtered]").arg(filtered);
    }
    m_item_count_label->setText(label);
    updatePageControls();
}

void EmailInspectorPanel::onMboxMessageDetailLoaded(sak::MboxMessageDetail detail) {
    if (m_dialog_active) {
        return;
    }
    // New message -- the per-message inline/remote image caches and any pending coalesced
    // redraw belong to the PREVIOUS message. Without clearing them a matching Content-Id or
    // URL would render bytes from the message just closed.
    m_inline_images.clear();
    m_remote_images.clear();
    m_redraw_timer->stop();

    // Commit the identity the response carries (attachment saves read it) and keep the full
    // body + headers in m_current_detail so the HTML/Images toggles -- which re-render from
    // m_current_detail -- do not lose the body, exactly as the PST path does. Without this
    // the MBOX flow left m_current_detail at stale PST values, so Save Attachment(s) and the
    // toggles acted on a previously-selected PST message.
    m_current_item_id = static_cast<uint64_t>(detail.message_index);
    sak::PstItemDetail normalized;
    normalized.node_id = *m_current_item_id;
    normalized.item_type = sak::EmailItemType::Email;
    normalized.subject = detail.subject;
    normalized.body_plain = detail.body_plain;
    normalized.body_html = detail.body_html;
    normalized.transport_headers = detail.raw_headers;
    normalized.attachments = detail.attachments;
    m_current_detail = normalized;

    displayItemDetail(m_current_detail);
    displayAttachments(detail.attachments);

    // MBOX carries no MAPI properties. Clear the properties tab rather than leave it under a
    // previously opened PST message's identity.
    m_properties_item_id.reset();
    displayProperties({});

    // Request inline CID image attachments (mirrors the PST path) so cid: images render once
    // their bytes arrive; always requested regardless of the HTML toggle so flipping it later
    // needs no re-fetch.
    if (!detail.body_html.isEmpty()) {
        for (const auto& attachment : detail.attachments) {
            if (!attachment.content_id.isEmpty()) {
                m_controller->loadAttachmentContent(*m_current_item_id, attachment.index);
            }
        }
        if (m_show_images) {
            fetchRemoteImages(detail.body_html);
        }
    }
}

// ============================================================================
// Helpers
// ============================================================================

void EmailInspectorPanel::setOperationRunning(bool running) {
    m_open_button->setEnabled(!running);
    m_progress_bar->setVisible(running);
    if (running) {
        m_progress_bar->setRange(0, 0);
    }
}

namespace {
// Upper bound on folder-tree items built from an untrusted PST/OST hierarchy. A real
// mailbox has at most a few hundred folders; the cap only trips on a hostile store
// declaring an unbounded breadth/depth, and stops it from exhausting UI-thread memory.
constexpr int kMaxFolderTreeNodes = 50'000;
}  // namespace

// static
// The visible top-level folders in display order: hidden (non-mail) folders dropped, the
// rest sorted by folder priority then case-insensitive name. Returned pointers alias into
// `source`, which must outlive them.
QVector<const sak::PstFolder*> EmailInspectorPanel::sortedVisibleFolders(
    const QVector<sak::PstFolder>& source) {
    QVector<const sak::PstFolder*> sorted;
    sorted.reserve(source.size());
    for (const auto& folder : source) {
        if (!isHiddenFolder(folder.display_name, folder.container_class)) {
            sorted.append(&folder);
        }
    }
    std::sort(sorted.begin(),
              sorted.end(),
              [](const sak::PstFolder* lhs, const sak::PstFolder* rhs) {
                  const int order_lhs = folderSortOrder(lhs->display_name, lhs->container_class);
                  const int order_rhs = folderSortOrder(rhs->display_name, rhs->container_class);
                  if (order_lhs != order_rhs) {
                      return order_lhs < order_rhs;
                  }
                  return lhs->display_name.compare(rhs->display_name, Qt::CaseInsensitive) < 0;
              });
    return sorted;
}

void EmailInspectorPanel::populateFolderTree(const sak::PstFolderTree& tree) {
    // A PST/OST folder tree is being built, so an MBOX is not open.
    m_mbox_active = false;
    m_folder_tree_budget = kMaxFolderTreeNodes;
    m_folder_tree->clear();
    m_contact_folder_ids.clear();
    m_calendar_folder_ids.clear();

    // Find IPM_SUBTREE to show user-facing folders at the top level.
    const sak::PstFolder* ipm = findIpmSubtree(tree);

    const auto& source = (ipm != nullptr && !ipm->children.isEmpty()) ? ipm->children : tree;

    // Collect contacts/calendar folder IDs before filtering
    for (const auto& folder : source) {
        collectSpecialFolderIds(folder);
    }

    // Folders to show at the top level, hidden ones dropped and the rest in display order.
    const QVector<const sak::PstFolder*> sorted = sortedVisibleFolders(source);

    for (const auto* folder : sorted) {
        if (m_folder_tree_budget <= 0) {
            break;
        }
        auto* item = new QTreeWidgetItem(m_folder_tree);
        --m_folder_tree_budget;
        item->setIcon(0, folderIcon(folder->display_name));
        item->setText(
            0, QStringLiteral("%1 (%2)").arg(folder->display_name).arg(folder->content_count));
        item->setData(0, Qt::UserRole, QVariant::fromValue(folder->node_id));
        for (const auto& child : folder->children) {
            addFolderToTree(item, child);
        }
    }
    if (m_folder_tree_budget <= 0) {
        logMessage(tr("Folder tree truncated: too many folders in this store"));
    }
    m_folder_tree->expandToDepth(0);
}

void EmailInspectorPanel::addFolderToTree(QTreeWidgetItem* parent, const sak::PstFolder& folder) {
    if (isHiddenFolder(folder.display_name, folder.container_class)) {
        return;
    }
    if (m_folder_tree_budget <= 0) {
        return;
    }
    auto* item = new QTreeWidgetItem(parent);
    --m_folder_tree_budget;
    item->setIcon(0, folderIcon(folder.display_name));
    item->setText(0, QStringLiteral("%1 (%2)").arg(folder.display_name).arg(folder.content_count));
    item->setData(0, Qt::UserRole, QVariant::fromValue(folder.node_id));
    for (const auto& child : folder.children) {
        addFolderToTree(item, child);
    }
}

// static
const sak::PstFolder* EmailInspectorPanel::findIpmSubtree(const QVector<sak::PstFolder>& folders) {
    return sak::findIpmSubtree(folders);
}

// static
QIcon EmailInspectorPanel::folderIcon(const QString& name) {
    const QString lower = name.toLower();
    if (lower == QLatin1String("inbox")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-inbox.svg"));
    }
    if (lower == QLatin1String("sent items")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-sent.svg"));
    }
    if (lower == QLatin1String("drafts")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-edit-file.svg"));
    }
    if (lower == QLatin1String("deleted items")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-trash.svg"));
    }
    if (lower == QLatin1String("junk email")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-spam.svg"));
    }
    if (lower == QLatin1String("outbox")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-clock.svg"));
    }
    if (lower == QLatin1String("archive")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-folder.svg"));
    }
    return QIcon(QStringLiteral(":/icons/icons/icons8-folder.svg"));
}

// static
int EmailInspectorPanel::folderSortOrder(const QString& name, const QString& /*container_class*/) {
    // Priority ordering for common Outlook folders.
    // Lower number = appears higher in the folder tree.
    if (name.compare(QLatin1String("Inbox"), Qt::CaseInsensitive) == 0) {
        return 0;
    }
    if (name.compare(QLatin1String("Drafts"), Qt::CaseInsensitive) == 0) {
        return 1;
    }
    if (name.compare(QLatin1String("Sent Items"), Qt::CaseInsensitive) == 0) {
        return kFolderSortSentItems;
    }
    if (name.compare(QLatin1String("Deleted Items"), Qt::CaseInsensitive) == 0) {
        return kFolderSortDeletedItems;
    }
    if (name.compare(QLatin1String("Archive"), Qt::CaseInsensitive) == 0) {
        return kFolderSortArchive;
    }
    if (name.compare(QLatin1String("Junk Email"), Qt::CaseInsensitive) == 0) {
        return kFolderSortJunkEmail;
    }
    if (name.compare(QLatin1String("Outbox"), Qt::CaseInsensitive) == 0) {
        return kFolderSortOutbox;
    }
    return kFolderSortUserFolder;
}

// static
bool EmailInspectorPanel::isHiddenFolder(const QString& name, const QString& container_class) {
    // One definition, shared with the "Export ALL Mail Folders" walk: if the tree hid a
    // folder the export still visited, the technician would get calendar entries written
    // out as .eml files.
    return !sak::isMailFolder(name, container_class);
}

void EmailInspectorPanel::collectSpecialFolderIds(const sak::PstFolder& folder) {
    if (folder.container_class.startsWith(QLatin1String("IPF.Contact"))) {
        m_contact_folder_ids.append(folder.node_id);
    }
    if (folder.container_class.startsWith(QLatin1String("IPF.Appointment"))) {
        m_calendar_folder_ids.append(folder.node_id);
    }
    for (const auto& child : folder.children) {
        collectSpecialFolderIds(child);
    }
}

void EmailInspectorPanel::onScanForFilesClicked() {
    EmailFileScannerDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        const QString path = dialog.selectedFilePath();
        if (!path.isEmpty()) {
            m_controller->openFile(path);
        }
    }
}

void EmailInspectorPanel::onContactsClicked() {
    if (m_contact_folder_ids.isEmpty()) {
        sak::showInformationLogged(this,
                                   tr("No Contacts"),
                                   tr("No contact folders were found in this file."));
        return;
    }
    m_dialog_active = true;
    disconnectDialogSignals();
    EmailContactsDialog dialog(m_controller.get(), m_contact_folder_ids, this);
    dialog.exec();
    reconnectDialogSignals();
    m_dialog_active = false;
}

void EmailInspectorPanel::onCalendarClicked() {
    if (m_calendar_folder_ids.isEmpty()) {
        sak::showInformationLogged(this,
                                   tr("No Calendar"),
                                   tr("No calendar folders were found in this file."));
        return;
    }
    m_dialog_active = true;
    disconnectDialogSignals();
    EmailCalendarDialog dialog(m_controller.get(), m_calendar_folder_ids, this);
    dialog.exec();
    reconnectDialogSignals();
    m_dialog_active = false;
}

void EmailInspectorPanel::populateItemList(const QVector<sak::PstItemSummary>& items) {
    m_item_list->setSortingEnabled(false);
    m_item_list->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_item_list);

    const int count = static_cast<int>(items.size());
    m_item_list->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& item = items.at(row);

        m_item_list->setItem(row,
                             ColSelect,
                             makeEmailSelectItem(item.node_id, tr("Select email for export")));

        auto* subject_cell = new QTableWidgetItem(item.subject);
        subject_cell->setData(Qt::UserRole, QVariant::fromValue(item.node_id));
        m_item_list->setItem(row, ColSubject, subject_cell);

        m_item_list->setItem(row, ColFrom, new QTableWidgetItem(item.sender_name));
        m_item_list->setItem(row, ColDate, new QTableWidgetItem(item.date.toString(Qt::ISODate)));
        m_item_list->setItem(row, ColSize, new QTableWidgetItem(formatBytes(item.size_bytes)));

        auto* type_cell = new QTableWidgetItem(itemTypeLabel(item.item_type));
        type_cell->setIcon(itemTypeQIcon(item.item_type));
        if (item.has_attachments) {
            type_cell->setToolTip(tr("Has attachments"));
        }
        m_item_list->setItem(row, ColType, type_cell);
    }
    m_item_list->setUpdatesEnabled(true);
    m_item_list->setSortingEnabled(true);
    updateItemListHeaderCheckState();
}

void EmailInspectorPanel::displayTaskDetail(const sak::PstItemDetail& detail) {
    QString html = QString::fromLatin1(ui::kHtmlDetailHeading2Open)
                       .arg(ui::kHtmlDetailPaddingPx)
                       .arg(ui::htmlColor(ui::kColorTextHeading))
                       .arg(detail.subject.toHtmlEscaped());
    static const char* const kTaskStatus[] = {
        "Not Started", "In Progress", "Complete", "Waiting", "Deferred"};
    if (detail.task_status >= 0 && detail.task_status < kTaskStatusCount) {
        html += QStringLiteral("<p><b>Status:</b> %1 (%2%)</p>")
                    .arg(QLatin1String(kTaskStatus[detail.task_status]))
                    .arg(static_cast<int>(detail.task_percent_complete * sak::kPercentMax));
    }
    if (detail.task_due_date.isValid()) {
        html += QStringLiteral("<p><b>Due:</b> %1</p>")
                    .arg(detail.task_due_date.toString(Qt::RFC2822Date));
    }
    if (detail.task_start_date.isValid()) {
        html += QStringLiteral("<p><b>Start:</b> %1</p>")
                    .arg(detail.task_start_date.toString(Qt::RFC2822Date));
    }
    html += QString::fromLatin1(ui::kHtmlHorizontalRule)
                .arg(ui::kCssBorderWidthDefaultPx)
                .arg(ui::htmlColor(ui::kColorBorderDefault));
    if (!detail.body_html.isEmpty()) {
        // A task body is untrusted markup exactly like a mail body; route it through the same
        // choke point instead of splicing it in raw (this path previously skipped both the
        // sanitizer and the inline/remote image neutralization).
        html += buildPreviewHtml(detail.body_html);
    } else if (!detail.body_plain.isEmpty()) {
        html += QString::fromLatin1(ui::kHtmlPreWrap).arg(detail.body_plain.toHtmlEscaped());
    }
    html += QStringLiteral("</div>");
    m_content_browser->setHtml(html);
    if (!detail.transport_headers.isEmpty()) {
        m_headers_browser->setPlainText(detail.transport_headers);
    } else {
        m_headers_browser->setPlainText(tr("No transport headers available"));
    }
}

void EmailInspectorPanel::displayNoteDetail(const sak::PstItemDetail& detail) {
    static const char* const kNoteColors[] = {ui::kColorNoteBlue,
                                              ui::kColorNoteGreen,
                                              ui::kColorNotePink,
                                              ui::kColorNoteYellow,
                                              ui::kColorNoteGray};
    constexpr int kNoteColorCount = 5;
    const int color_idx = (detail.note_color >= 0 && detail.note_color < kNoteColorCount)
                              ? detail.note_color
                              : kDefaultNoteColorIndex;
    const QString html = QString::fromLatin1(ui::kHtmlNoteDetailTemplate)
                             .arg(ui::kHtmlDetailLargePaddingPx)
                             .arg(ui::htmlColor(QLatin1String(kNoteColors[color_idx])))
                             .arg(ui::kCssRadiusLargePx)
                             .arg(ui::kHtmlNoteMinHeightPx)
                             .arg(ui::htmlColor(ui::kColorTextHeading))
                             .arg(detail.subject.toHtmlEscaped())
                             .arg(ui::htmlColor(ui::kColorTextBody))
                             .arg(detail.body_plain.toHtmlEscaped());
    m_content_browser->setHtml(html);
    m_headers_browser->setPlainText(tr("No transport headers available"));
}

QString EmailInspectorPanel::buildPreviewHtml(const QString& body_html) const {
    // Single choke point for every untrusted body this panel renders. Strip active content
    // first, with the SAME sanitizer the HTML and PDF export writers use -- one authority, not a
    // second dialect -- so script blocks, inline event handlers, javascript:/vbscript: URIs and
    // framing/loading tags (iframe/object/embed/link/meta/...) never reach the document.
    //
    // Bound the untrusted body first: a pathologically large PST/MBOX body would otherwise
    // drive several full-size allocations and a long global-regex pass on the UI thread.
    // Truncate with a visible notice rather than process an unbounded blob (fail closed
    // against UI-thread DoS).
    constexpr int kMaxPreviewHtmlChars = 4 * 1024 * 1024;
    QString bounded_body = body_html;
    if (bounded_body.size() > kMaxPreviewHtmlChars) {
        bounded_body.truncate(kMaxPreviewHtmlChars);
        bounded_body += QStringLiteral("<p>[preview truncated]</p>");
    }
    const QString safe_html = sanitizeEmailBodyHtml(bounded_body);

    // Then inline every image `src` we can resolve offline -- both `cid:`
    // attachment references and any already-downloaded http(s) URLs.  The
    // result is self-contained HTML that `QTextBrowser` can render without
    // a network stack.  Anything we cannot resolve gets blanked out so the
    // browser never attempts a fetch (tracking-pixel + privacy defence).
    static const QRegularExpression kImgSrc(QStringLiteral(R"((src\s*=\s*)(["'])([^"']+)\2)"),
                                            QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(safe_html.size());
    int pos = 0;
    auto it = kImgSrc.globalMatch(safe_html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out.append(safe_html.mid(pos, m.capturedStart() - pos));
        pos = m.capturedEnd();

        const QString src = m.captured(kInlineImageSrcCaptureGroup).trimmed();
        const QString lower = src.toLower();

        if (lower.startsWith(QStringLiteral("data:"))) {
            // Already self-contained -- pass through unchanged.
            out.append(m.captured(0));
            continue;
        }

        const QByteArray image_data = resolveInlineImageData(src, lower);
        appendInlineImageAttr(out, m, image_data);
    }
    out.append(safe_html.mid(pos));

    // Neutralise any lingering remote refs that are not `src=...` attributes
    // (CSS url(...), background/poster attributes) when images are off.
    if (!m_show_images) {
        out = stripRemoteContent(out);
    }
    return out;
}

QByteArray EmailInspectorPanel::resolveInlineImageData(const QString& src,
                                                       const QString& lower_src) const {
    if (lower_src.startsWith(QStringLiteral("cid:"))) {
        const QString cid = src.mid(4).trimmed();
        return m_inline_images.value(cid);
    }
    if (!m_show_images) {
        return {};
    }
    const bool is_remote = lower_src.startsWith(QStringLiteral("http://")) ||
                           lower_src.startsWith(QStringLiteral("https://")) ||
                           lower_src.startsWith(QStringLiteral("//"));
    if (!is_remote) {
        return {};
    }
    QString key = src;
    if (key.startsWith(QStringLiteral("//"))) {
        key = QStringLiteral("https:") + key;
    }
    return m_remote_images.value(key);
}

void EmailInspectorPanel::appendInlineImageAttr(QString& out,
                                                const QRegularExpressionMatch& m,
                                                const QByteArray& image_data) {
    out.append(m.captured(kInlineImageAttrPrefixCaptureGroup));
    out.append(m.captured(kInlineImageAttrQuoteCaptureGroup));
    if (image_data.isEmpty()) {
        // Unknown scheme, or data not yet loaded, or images disabled.
        out.append(m.captured(kInlineImageAttrQuoteCaptureGroup));
        return;
    }
    QByteArray mime = detectImageMime(image_data);
    if (mime.isEmpty()) {
        mime = QByteArrayLiteral("application/octet-stream");
    }
    out.append(QStringLiteral("data:"));
    out.append(QString::fromLatin1(mime));
    out.append(QStringLiteral(";base64,"));
    out.append(QString::fromLatin1(image_data.toBase64()));
    out.append(m.captured(kInlineImageAttrQuoteCaptureGroup));
}

namespace {

// Remote-image fetch bounds: cap the requests dispatched per message and the
// bytes read per reply so a hostile mail server cannot open unbounded
// connections or exhaust memory with a giant response.
constexpr int kMaxRemoteImageRequests = 64;
constexpr qint64 kMaxRemoteImageBytes = 8LL * 1024 * 1024;

// Normalize a raw img src to an http(s) URL, or empty when it is not an eligible
// remote reference (data:, cid:, relative, unknown scheme).
QString normalizeRemoteImageSrc(const QString& raw) {
    const QString lower = raw.toLower();
    if (lower.startsWith(QStringLiteral("//"))) {
        return QStringLiteral("https:") + raw;
    }
    if (lower.startsWith(QStringLiteral("http://")) ||
        lower.startsWith(QStringLiteral("https://"))) {
        return raw;
    }
    return QString();
}

bool remoteImageOverBudget(const qint64 received, const qint64 total) {
    return received > kMaxRemoteImageBytes || total > kMaxRemoteImageBytes;
}

// Store a finished reply's payload only when it is current, succeeded, non-empty,
// and within the byte cap; otherwise release the reserved slot. Fail-closed: a
// stale, failed, empty, or oversized fetch is dropped, never rendered.
void applyRemoteImageReply(QNetworkReply* reply,
                           const QString& src,
                           const bool current,
                           QHash<QString, QByteArray>& images,
                           QTimer* redraw) {
    reply->deleteLater();
    if (!current || reply->error() != QNetworkReply::NoError) {
        images.remove(src);
        return;
    }
    const QByteArray payload = reply->readAll();
    if (payload.isEmpty() || payload.size() > kMaxRemoteImageBytes) {
        images.remove(src);
        return;
    }
    images.insert(src, payload);
    redraw->start();
}

}  // namespace

void EmailInspectorPanel::fetchRemoteImages(const QString& body_html) {
    if (m_remote_image_nam == nullptr) {
        m_remote_image_nam = new QNetworkAccessManager(this);
    }

    static const QRegularExpression kImgSrc(QStringLiteral(R"((src\s*=\s*)(["'])([^"']+)\2)"),
                                            QRegularExpression::CaseInsensitiveOption);

    auto it = kImgSrc.globalMatch(body_html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString src =
            normalizeRemoteImageSrc(m.captured(kInlineImageSrcCaptureGroup).trimmed());
        if (src.isEmpty() || m_remote_images.contains(src)) {
            continue;
        }
        if (m_remote_images.size() >= kMaxRemoteImageRequests) {
            // Per-message request cap: stop dispatching once the budget is spent.
            break;
        }
        // Reserve the slot so duplicate `src` references in the body only
        // dispatch one request per URL.
        m_remote_images.insert(src, {});

        const uint64_t message_id = m_current_detail.node_id;
        QNetworkRequest request{QUrl(src)};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = m_remote_image_nam->get(request);
        // Bound in-flight buffering and abort a stream that blows the byte budget.
        reply->setReadBufferSize(kMaxRemoteImageBytes + 1);
        connect(reply,
                &QNetworkReply::downloadProgress,
                this,
                [reply](const qint64 received, const qint64 total) {
                    if (remoteImageOverBudget(received, total)) {
                        reply->abort();
                    }
                });
        connect(reply, &QNetworkReply::finished, this, [this, reply, src, message_id]() {
            applyRemoteImageReply(reply,
                                  src,
                                  m_current_detail.node_id == message_id,
                                  m_remote_images,
                                  m_redraw_timer);
        });
    }
}

void EmailInspectorPanel::displayItemDetail(const sak::PstItemDetail& detail) {
    if (detail.item_type == sak::EmailItemType::Task) {
        displayTaskDetail(detail);
        return;
    }

    if (detail.item_type == sak::EmailItemType::StickyNote) {
        displayNoteDetail(detail);
        return;
    }

    // Default: email / journal / RSS / conversation history.
    // No cross-format fallback: the toggle is authoritative.
    if (m_show_html) {
        if (detail.body_html.isEmpty()) {
            m_content_browser->clear();
        } else {
            const QString wrapped = QString::fromLatin1(ui::kHtmlEmailPreviewDocument)
                                        .arg(ui::kHtmlPreviewBodyFontPx)
                                        .arg(ui::kHtmlPreviewBodyPaddingPx)
                                        .arg(buildPreviewHtml(detail.body_html));
            m_content_browser->setHtml(wrapped);
        }
    } else {
        if (detail.body_plain.isEmpty()) {
            m_content_browser->clear();
        } else {
            m_content_browser->setPlainText(detail.body_plain);
        }
    }

    if (!detail.transport_headers.isEmpty()) {
        m_headers_browser->setPlainText(detail.transport_headers);
    } else {
        m_headers_browser->setPlainText(tr("No transport headers available"));
    }
}

void EmailInspectorPanel::displayProperties(const QVector<sak::MapiProperty>& props) {
    m_properties_table->setSortingEnabled(false);
    m_properties_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_properties_table);
    const int count = props.size();
    m_properties_table->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& prop = props.at(row);
        m_properties_table->setItem(row, 0, new QTableWidgetItem(prop.property_name));
        const QString display = prop.display_value.isEmpty() ? QStringLiteral("<empty>")
                                                             : prop.display_value;
        m_properties_table->setItem(row, 1, new QTableWidgetItem(display));
    }
    m_properties_table->setUpdatesEnabled(true);
    m_properties_table->setSortingEnabled(true);
}

void EmailInspectorPanel::displayAttachments(const QVector<sak::PstAttachmentInfo>& attachments) {
    m_attachments_table->setSortingEnabled(false);
    m_attachments_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_attachments_table);
    const int count = attachments.size();
    m_attachments_table->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& att = attachments.at(row);
        const QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        m_attachments_table->setItem(row, 0, new QTableWidgetItem(name));
        m_attachments_table->setItem(row, 1, new QTableWidgetItem(formatBytes(att.size_bytes)));
        m_attachments_table->setItem(row,
                                     kAttachmentTableColumnMimeType,
                                     new QTableWidgetItem(att.mime_type));
        m_attachments_table->setItem(row,
                                     kAttachmentTableColumnContentId,
                                     new QTableWidgetItem(att.content_id));
    }
    m_attachments_table->setUpdatesEnabled(true);
    m_attachments_table->setSortingEnabled(true);
    // Never unlock the save controls while a batch is still running: a second
    // batch started now would interleave its arrivals with the first one's.
    setAttachmentSaveControlsEnabled(!attachments.isEmpty() && !m_batch_save.isActive());
}

void EmailInspectorPanel::setAttachmentSaveControlsEnabled(bool enabled) {
    m_save_attachment_button->setEnabled(enabled);
    m_save_all_attachments_button->setEnabled(enabled);
}

void EmailInspectorPanel::updateFileInfoBar(const sak::PstFileInfo& info) {
    updateStatusBar(QStringLiteral("%1 \u2014 %2 \u2014 %3 items")
                        .arg(info.display_name)
                        .arg(formatBytes(info.file_size_bytes))
                        .arg(info.total_items));
}

void EmailInspectorPanel::updateStatusBar(const QString& message) {
    m_status_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    m_status_label->setText(message);
    Q_EMIT statusMessage(message, kTimerStatusDefaultMs);
}

void EmailInspectorPanel::logMessage(const QString& message) {
    Q_EMIT logOutput(message);
}

QIcon EmailInspectorPanel::itemTypeQIcon(sak::EmailItemType type) {
    switch (type) {
    case sak::EmailItemType::Email:
        return QIcon(QStringLiteral(":/icons/icons/icons8-sent.svg"));
    case sak::EmailItemType::Contact:
        return QIcon(QStringLiteral(":/icons/icons/icons8-address-book.svg"));
    case sak::EmailItemType::Calendar:
        return QIcon(QStringLiteral(":/icons/icons/icons8-calendar.svg"));
    case sak::EmailItemType::Task:
        return QIcon(QStringLiteral(":/icons/icons/icons8-edit-file.svg"));
    case sak::EmailItemType::StickyNote:
        return QIcon(QStringLiteral(":/icons/icons/icons8-star.svg"));
    case sak::EmailItemType::JournalEntry:
        return QIcon(QStringLiteral(":/icons/icons/icons8-source.svg"));
    case sak::EmailItemType::DistList:
        return QIcon(QStringLiteral(":/icons/icons/icons8-address-book.svg"));
    case sak::EmailItemType::MeetingRequest:
        return QIcon(QStringLiteral(":/icons/icons/icons8-calendar.svg"));
    default:
        return QIcon(QStringLiteral(":/icons/icons/icons8-folder.svg"));
    }
}

QString EmailInspectorPanel::itemTypeLabel(sak::EmailItemType type) {
    switch (type) {
    case sak::EmailItemType::Email:
        return tr("Email");
    case sak::EmailItemType::Contact:
        return tr("Contact");
    case sak::EmailItemType::Calendar:
        return tr("Calendar");
    case sak::EmailItemType::Task:
        return tr("Task");
    case sak::EmailItemType::StickyNote:
        return tr("Note");
    case sak::EmailItemType::JournalEntry:
        return tr("Journal");
    case sak::EmailItemType::DistList:
        return tr("Dist List");
    case sak::EmailItemType::MeetingRequest:
        return tr("Meeting");
    default:
        return tr("Unknown");
    }
}

bool EmailInspectorPanel::isBlankItem(const sak::PstItemSummary& /*item*/) {
    // Retained as a non-filtering hook for API compatibility. Row filtering
    // is no longer needed: the PST parser now enumerates only live rows from
    // the TCROWID BTH (MS-PST section 2.3.4.3.1), so stale/deleted/padding slots
    // never reach the GUI in the first place.
    return false;
}

void EmailInspectorPanel::applyPageSize() {
    if (m_current_folder_id == 0) {
        return;
    }
    m_current_page = 0;
    reloadCurrentPage();
}

void EmailInspectorPanel::reloadCurrentPage() {
    // Folder id 0 means "no folder selected" for a PST/OST store, but it is the MBOX
    // root's real id. Only bail on 0 when no MBOX is open, otherwise the MBOX message
    // list can never load.
    if (m_current_folder_id == 0 && !m_mbox_active) {
        return;
    }
    const int page_size = currentPageSize();
    m_controller->loadFolderItems(m_current_folder_id, m_current_page * page_size, page_size);
}

int EmailInspectorPanel::currentPageSize() const {
    const int raw = m_page_size_combo->currentText().toInt();
    return (raw > 0) ? raw : email::kMaxItemsPerLoad;
}

void EmailInspectorPanel::updatePageControls() {
    const int page_size = currentPageSize();
    const int max_page = (m_current_total > 0) ? (m_current_total - 1) / page_size : 0;
    const int total_pages = max_page + 1;
    m_prev_page_button->setEnabled(m_current_page > 0);
    m_next_page_button->setEnabled(m_current_page < max_page);
    if (m_current_total <= page_size) {
        m_page_label->setText(QString());
    } else {
        m_page_label->setText(tr("Page %1 of %2").arg(m_current_page + 1).arg(total_pages));
    }
}

QString EmailInspectorPanel::formatBytes(qint64 bytes) {
    if (bytes < kBytesPerKB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < kBytesPerMB) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / kBytesPerKBf, 0, 'f', 1);
    }
    if (bytes < kBytesPerGB) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / kBytesPerMBf, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(
        static_cast<double>(bytes) / kBytesPerGBf, 0, 'f', kLargeByteDisplayPrecision);
}

}  // namespace sak
