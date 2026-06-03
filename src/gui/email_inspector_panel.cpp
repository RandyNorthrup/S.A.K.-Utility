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
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
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
        const int cx = rect.x() + (rect.width() - side) / 2;
        const int cy = rect.y() + (rect.height() - side) / 2;
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
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setIcon(QIcon(icon_path));
    button->setIconSize(QSize(kRibbonIconSize, kRibbonIconSize));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setToolTip(tooltip);
    button->setAccessibleName(text);
    button->setFixedSize(kRibbonButtonWidth, kRibbonButtonHeight);
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


    m_content_browser = new QTextBrowser(this);
    m_content_browser->setOpenExternalLinks(false);
    m_content_browser->setReadOnly(true);
    m_content_browser->setAccessibleName(QStringLiteral("Email Content"));
    m_content_browser->setStyleSheet(ui::emailContentBrowserStyle());
    layout->addWidget(m_content_browser, 1);

    return container;
}

QWidget* EmailInspectorPanel::createHeadersTab() {
    m_headers_browser = new QTextBrowser(this);
    m_headers_browser->setReadOnly(true);
    m_headers_browser->setAccessibleName(QStringLiteral("Email Headers"));
    QFont mono_font(QStringLiteral("Consolas"), ui::kFontSizeNote);
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
    QString filter = tr(
        "Email Data Files (*.pst *.ost *.mbox);;"
        "PST Files (*.pst);;"
        "OST Files (*.ost);;"
        "MBOX Files (*.mbox);;"
        "All Files (*)");
    QString path =
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
    if (!item) {
        return;
    }
    bool ok = false;
    uint64_t folder_id = item->data(0, Qt::UserRole).toULongLong(&ok);
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
    if (!subject_item) {
        return;
    }
    bool ok = false;
    uint64_t item_id = subject_item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    m_pending_item_id = item_id;
    m_controller->loadItemDetail(item_id);
    m_controller->loadItemProperties(item_id);
}

void EmailInspectorPanel::onItemListContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction(tr("Open in Detail Panel"), this, [this] {
        int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                uint64_t nid = subject_item->data(Qt::UserRole).toULongLong();
                m_pending_item_id = nid;
                m_controller->loadItemDetail(nid);
            }
        }
    });
    menu.addSeparator();
    menu.addAction(tr("Export Checked Emails..."), this, [this] { onExportClicked(); });
    menu.addAction(tr("Browse Attachments..."), this, [this] { onExportAttachmentsClicked(); });
    menu.addSeparator();
    menu.addAction(tr("Copy Subject"), this, [this] {
        int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                QApplication::clipboard()->setText(subject_item->text());
            }
        }
    });
    menu.addAction(tr("View MAPI Properties"), this, [this] {
        int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                uint64_t nid = subject_item->data(Qt::UserRole).toULongLong();
                m_controller->loadItemProperties(nid);
                m_detail_tabs->setCurrentIndex(kPropertiesDetailTabIndex);
            }
        }
    });
    menu.exec(m_item_list->viewport()->mapToGlobal(pos));
}

void EmailInspectorPanel::onFolderTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_folder_tree->itemAt(pos);
    if (!item) {
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Export Folder as EML..."), this, [this] {
        exportCurrentFolderAs(sak::ExportFormat::Eml);
    });
    menu.addAction(tr("Export Folder as CSV..."), this, [this] {
        exportCurrentFolderAs(sak::ExportFormat::CsvEmails);
    });
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
    QString query = m_search_edit->text().trimmed();
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

uint64_t EmailInspectorPanel::itemIdForRow(int row) const {
    if (m_item_list == nullptr || row < 0 || row >= m_item_list->rowCount()) {
        return 0;
    }
    auto* subject_item = m_item_list->item(row, ColSubject);
    if (subject_item == nullptr) {
        return 0;
    }
    bool ok = false;
    const uint64_t item_id = subject_item->data(Qt::UserRole).toULongLong(&ok);
    return ok ? item_id : 0;
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
        const uint64_t item_id = itemIdForRow(row);
        if (item_id != 0) {
            ids.append(item_id);
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

    QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Export Directory"));
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

void EmailInspectorPanel::exportCurrentFolderAs(sak::ExportFormat format) {
    if (!m_controller->isFileOpen()) {
        return;
    }
    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Export Directory"));
    if (dir_path.isEmpty()) {
        return;
    }

    sak::EmailExportConfig config;
    config.format = format;
    config.output_path = dir_path;
    config.folder_id = m_current_folder_id;
    config.save_attachments_with_messages = true;
    m_controller->exportItems(config);
}

void EmailInspectorPanel::onExportAttachmentsClicked() {
    if (!m_controller->isFileOpen()) {
        return;
    }
    m_dialog_active = true;
    disconnectDialogSignals();
    EmailAttachmentsBrowserDialog dialog(m_controller.get(), m_cached_folder_tree, this);
    const auto result = dialog.exec();
    reconnectDialogSignals();
    m_dialog_active = false;
    if (result == QDialog::Accepted) {
        uint64_t folder_id = dialog.navigateFolderId();
        uint64_t message_id = dialog.navigateMessageId();
        if (folder_id != 0 && message_id != 0) {
            m_current_folder_id = folder_id;
            m_current_page = 0;
            m_current_total = 0;
            reloadCurrentPage();
            m_pending_item_id = message_id;
            m_controller->loadItemDetail(message_id);
            m_controller->loadItemProperties(message_id);
        }
    }
}

// ============================================================================
// Attachment Slots
// ============================================================================

void EmailInspectorPanel::onSaveAttachmentClicked() {
    int row = m_attachments_table->currentRow();
    if (row < 0 || row >= m_current_detail.attachments.size()) {
        return;
    }
    QString dir = QFileDialog::getExistingDirectory(this, tr("Save Attachment"));
    if (dir.isEmpty()) {
        return;
    }
    m_batch_save.begin(dir, 1);
    const auto& attachment = m_current_detail.attachments.at(row);
    updateStatusBar(tr("Saving attachment..."));
    m_controller->loadAttachmentContent(m_current_item_id, attachment.index);
}

void EmailInspectorPanel::onSaveAllAttachmentsClicked() {
    if (m_current_detail.attachments.isEmpty()) {
        return;
    }
    QString dir = QFileDialog::getExistingDirectory(this, tr("Save All Attachments"));
    if (dir.isEmpty()) {
        return;
    }
    int count = m_current_detail.attachments.size();
    m_batch_save.begin(dir, count);
    for (const auto& attachment : m_current_detail.attachments) {
        m_controller->loadAttachmentContent(m_current_item_id, attachment.index);
    }
    updateStatusBar(tr("Saving %1 attachments...").arg(count));
}

// ============================================================================
// Controller Signal Handlers
// ============================================================================

void EmailInspectorPanel::onStateChanged(EmailInspectorController::State state) {
    if (m_dialog_active) {
        return;
    }
    bool idle = (state == EmailInspectorController::State::Idle);
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
    m_current_item_id = 0;
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
    const int first = (items.isEmpty()) ? 0 : (m_current_page * page_size + 1);
    const int last = m_current_page * page_size + static_cast<int>(items.size());
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
    // New item — the per-message inline-image cache is invalid.
    m_inline_images.clear();
    m_remote_images.clear();
    m_redraw_timer->stop();

    // Commit the pending item ID now that the load succeeded
    m_current_item_id = m_pending_item_id;
    m_current_detail = detail;
    displayItemDetail(detail);
    displayAttachments(detail.attachments);
    m_save_all_attachments_button->setEnabled(!detail.attachments.isEmpty());

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

void EmailInspectorPanel::onItemPropertiesLoaded(uint64_t /*item_id*/,
                                                 QVector<sak::MapiProperty> properties) {
    if (m_dialog_active) {
        return;
    }
    displayProperties(properties);
}

void EmailInspectorPanel::onAttachmentContentReady(uint64_t /*message_id*/,
                                                   int index,
                                                   QByteArray attachment_data,
                                                   QString filename) {
    if (m_dialog_active) {
        return;
    }

    // Save operation — delegate entirely to shared batch saver
    if (m_batch_save.isActive()) {
        m_batch_save.recordOne(filename, attachment_data);
        if (m_batch_save.isComplete()) {
            updateStatusBar(m_batch_save.summaryText());
            m_batch_save.reset();
        }
        return;
    }

    // Not a save — check for inline CID image
    for (const auto& att : m_current_detail.attachments) {
        if (att.index == index && !att.content_id.isEmpty()) {
            // Cache by Content-Id so the HTML builder can inline as data URI.
            m_inline_images.insert(att.content_id, attachment_data);
            // Coalesce: multiple inline images commonly arrive in rapid
            // succession — debounce the full-HTML repaint so we only
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

    // For MBOX, create a single root folder item
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

    // Filter blank messages
    QVector<int> visible_indices;
    visible_indices.reserve(messages.size());
    for (int idx = 0; idx < messages.size(); ++idx) {
        const auto& msg = messages.at(idx);
        if (!msg.subject.trimmed().isEmpty() || !msg.from.trimmed().isEmpty()) {
            visible_indices.append(idx);
        }
    }

    const int count = visible_indices.size();
    m_item_list->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& msg = messages.at(visible_indices.at(row));
        m_item_list->setItem(row,
                             ColSelect,
                             makeEmailSelectItem(static_cast<uint64_t>(visible_indices.at(row)),
                                                 tr("Select email for export")));

        auto* subject_cell = new QTableWidgetItem(msg.subject);
        subject_cell->setData(Qt::UserRole, QVariant::fromValue<uint64_t>(visible_indices.at(row)));
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
    int filtered = total - count;
    QString label = tr("%1 messages (showing %2)").arg(total).arg(count);
    if (filtered > 0) {
        label += tr("  [%1 blank filtered]").arg(filtered);
    }
    m_item_count_label->setText(label);
}

void EmailInspectorPanel::onMboxMessageDetailLoaded(sak::MboxMessageDetail detail) {
    // Display in content tab respecting HTML/plain text toggle.
    // No cross-format fallback: if the requested format is absent, the pane
    // is cleared so the missing data is visible rather than hidden.
    if (m_show_html) {
        if (detail.body_html.isEmpty()) {
            m_content_browser->clear();
        } else {
            QString wrapped = QString::fromLatin1(ui::kHtmlEmailPreviewDocument)
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

    // Display headers
    m_headers_browser->setPlainText(detail.raw_headers);

    // Attachments
    displayAttachments(detail.attachments);
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

void EmailInspectorPanel::populateFolderTree(const sak::PstFolderTree& tree) {
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

    // Sort folders by display priority
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
                  int order_lhs = folderSortOrder(lhs->display_name, lhs->container_class);
                  int order_rhs = folderSortOrder(rhs->display_name, rhs->container_class);
                  if (order_lhs != order_rhs) {
                      return order_lhs < order_rhs;
                  }
                  return lhs->display_name.compare(rhs->display_name, Qt::CaseInsensitive) < 0;
              });

    for (const auto* folder : sorted) {
        auto* item = new QTreeWidgetItem(m_folder_tree);
        item->setIcon(0, folderIcon(folder->display_name));
        item->setText(
            0, QStringLiteral("%1 (%2)").arg(folder->display_name).arg(folder->content_count));
        item->setData(0, Qt::UserRole, QVariant::fromValue(folder->node_id));
        for (const auto& child : folder->children) {
            addFolderToTree(item, child);
        }
    }
    m_folder_tree->expandToDepth(0);
}

void EmailInspectorPanel::addFolderToTree(QTreeWidgetItem* parent, const sak::PstFolder& folder) {
    if (isHiddenFolder(folder.display_name, folder.container_class)) {
        return;
    }
    auto* item = new QTreeWidgetItem(parent);
    item->setIcon(0, folderIcon(folder.display_name));
    item->setText(0, QStringLiteral("%1 (%2)").arg(folder.display_name).arg(folder.content_count));
    item->setData(0, Qt::UserRole, QVariant::fromValue(folder.node_id));
    for (const auto& child : folder.children) {
        addFolderToTree(item, child);
    }
}

// static
const sak::PstFolder* EmailInspectorPanel::findIpmSubtree(const QVector<sak::PstFolder>& folders) {
    // OST files have two IPM_SUBTREE nodes — one under Root - Public
    // (empty) and one under Root - Mailbox (with all user folders).
    // We want the one that actually has children.
    const sak::PstFolder* best = nullptr;

    for (const auto& folder : folders) {
        if (folder.display_name == QLatin1String("IPM_SUBTREE") ||
            folder.display_name == QLatin1String("Top of Information Store")) {
            if (!folder.children.isEmpty()) {
                return &folder;
            }
            if (best == nullptr) {
                best = &folder;
            }
        }
        const sak::PstFolder* found = findIpmSubtree(folder.children);
        if (found != nullptr && !found->children.isEmpty()) {
            return found;
        }
        if (found != nullptr && best == nullptr) {
            best = found;
        }
    }
    return best;
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
    // Contact and calendar folders are shown in their own modals
    if (container_class.startsWith(QLatin1String("IPF.Contact"))) {
        return true;
    }
    if (container_class.startsWith(QLatin1String("IPF.Appointment"))) {
        return true;
    }

    // Task, Note, and Journal folders are not email — hide them
    if (container_class.startsWith(QLatin1String("IPF.Task"))) {
        return true;
    }
    if (container_class.startsWith(QLatin1String("IPF.StickyNote"))) {
        return true;
    }
    if (container_class.startsWith(QLatin1String("IPF.Journal"))) {
        return true;
    }

    // System/configuration folders that clutter the tree
    if (container_class == QLatin1String("IPF.Configuration")) {
        return true;
    }

    // Known system folder names to hide
    static const QStringList kHiddenNames = {
        QStringLiteral("PersonMetadata"),
        QStringLiteral("MeContact"),
        QStringLiteral("ExternalContacts"),
        QStringLiteral("Quick Step Settings"),
        QStringLiteral("Conversation Action Settings"),
        QStringLiteral("Yammer Root"),
        QStringLiteral("Social Activity Notifications"),
        QStringLiteral("Conversation History"),
        QStringLiteral("Files"),
        QStringLiteral("Sync Issues"),
        QStringLiteral("Conflicts"),
        QStringLiteral("Local Failures"),
        QStringLiteral("Server Failures"),
    };
    for (const auto& hidden : kHiddenNames) {
        if (name.compare(hidden, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
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
        QString path = dialog.selectedFilePath();
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
        html += detail.body_html;
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
    int color_idx = (detail.note_color >= 0 && detail.note_color < kNoteColorCount)
                        ? detail.note_color
                        : kDefaultNoteColorIndex;
    QString html = QString::fromLatin1(ui::kHtmlNoteDetailTemplate)
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
    // Inline every image `src` we can resolve offline — both `cid:`
    // attachment references and any already-downloaded http(s) URLs.  The
    // result is self-contained HTML that `QTextBrowser` can render without
    // a network stack.  Anything we cannot resolve gets blanked out so the
    // browser never attempts a fetch (tracking-pixel + privacy defence).
    static const QRegularExpression kImgSrc(QStringLiteral(R"((src\s*=\s*)(["'])([^"']+)\2)"),
                                            QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(body_html.size());
    int pos = 0;
    auto it = kImgSrc.globalMatch(body_html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out.append(body_html.mid(pos, m.capturedStart() - pos));
        pos = m.capturedEnd();

        const QString src = m.captured(kInlineImageSrcCaptureGroup).trimmed();
        const QString lower = src.toLower();

        if (lower.startsWith(QStringLiteral("data:"))) {
            // Already self-contained — pass through unchanged.
            out.append(m.captured(0));
            continue;
        }

        const QByteArray image_data = resolveInlineImageData(src, lower);
        appendInlineImageAttr(out, m, image_data);
    }
    out.append(body_html.mid(pos));

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

void EmailInspectorPanel::fetchRemoteImages(const QString& body_html) {
    if (!m_remote_image_nam) {
        m_remote_image_nam = new QNetworkAccessManager(this);
    }

    static const QRegularExpression kImgSrc(QStringLiteral(R"((src\s*=\s*)(["'])([^"']+)\2)"),
                                            QRegularExpression::CaseInsensitiveOption);

    auto it = kImgSrc.globalMatch(body_html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QString src = m.captured(kInlineImageSrcCaptureGroup).trimmed();
        const QString lower = src.toLower();
        if (lower.startsWith(QStringLiteral("//"))) {
            src = QStringLiteral("https:") + src;
        } else if (!lower.startsWith(QStringLiteral("http://")) &&
                   !lower.startsWith(QStringLiteral("https://"))) {
            continue;
        }
        if (m_remote_images.contains(src)) {
            continue;
        }
        // Reserve the slot so duplicate `src` references in the body only
        // dispatch one request per URL.
        m_remote_images.insert(src, {});

        const uint64_t message_id = m_current_detail.node_id;
        QNetworkRequest request{QUrl(src)};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = m_remote_image_nam->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, src, message_id]() {
            reply->deleteLater();
            // Drop stale results when the user has already moved on.
            if (m_current_detail.node_id != message_id) {
                m_remote_images.remove(src);
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                m_remote_images.remove(src);
                return;
            }
            const QByteArray payload = reply->readAll();
            if (payload.isEmpty()) {
                m_remote_images.remove(src);
                return;
            }
            m_remote_images.insert(src, payload);
            m_redraw_timer->start();
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
            QString wrapped = QString::fromLatin1(ui::kHtmlEmailPreviewDocument)
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
        QString display = prop.display_value.isEmpty() ? QStringLiteral("<empty>")
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
        QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
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
    m_save_attachment_button->setEnabled(!attachments.isEmpty());
    m_save_all_attachments_button->setEnabled(!attachments.isEmpty());
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
    if (m_current_folder_id == 0) {
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
