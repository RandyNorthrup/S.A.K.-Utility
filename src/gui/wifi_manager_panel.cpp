// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_manager_panel.cpp
/// @brief Implements the Wi-Fi network manager panel UI with QR code generation

#include "sak/wifi_manager_panel.h"

#include "sak/detachable_log_window.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/process_runner.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "sak/wifi_setup_script.h"

#include "qrcodegen.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyleOptionButton>
#include <QTableWidgetItem>
#include <QtConcurrent>
#include <QTemporaryFile>
#include <QTextStream>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

// -----------------------------------------------------------------------------
// CheckHeaderView  --  column 0 renders a tri-state "select all" checkbox
// -----------------------------------------------------------------------------
namespace {
constexpr int kHeaderCheckRadiusPx = sak::ui::kCssRadiusSmallPx;
constexpr int kHeaderCheckTickPenWidth = sak::ui::kCssBorderWidthAccentPx;
constexpr int kHeaderCheckTickX0 = 3;
constexpr int kHeaderCheckTickY0 = 8;
constexpr int kHeaderCheckTickX1 = 6;
constexpr int kHeaderCheckTickY1 = 11;
constexpr int kHeaderCheckTickX2 = 13;
constexpr int kHeaderCheckTickY2 = 4;
constexpr int kHeaderCheckDashX0 = 4;
constexpr int kHeaderCheckDashY = 8;
constexpr int kHeaderCheckDashX1 = 12;

class CheckHeaderView : public QHeaderView {
    Q_OBJECT
public:
    explicit CheckHeaderView(QWidget* parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent), m_state(Qt::Unchecked) {
        setSectionsClickable(true);
    }

    Qt::CheckState triState() const { return m_state; }

    void setTriState(Qt::CheckState state) {
        if (m_state == state) {
            return;
        }
        m_state = state;
        viewport()->update();
    }

Q_SIGNALS:
    void checkToggled(bool all_checked);

protected:
    void paintSection(QPainter* painter, const QRect& rect, int logical_index) const override {
        Q_ASSERT(painter);
        Q_ASSERT(logical_index >= 0);
        // Paint default section background first
        painter->save();
        QHeaderView::paintSection(painter, rect, logical_index);
        painter->restore();
        if (logical_index != 0) {
            return;
        }

        // Draw a custom checkbox that matches the table indicator stylesheet tokens.
        constexpr int kSide = sak::ui::kUiIconSmall;
        const int cx = rect.x() + ((rect.width() - kSide) / 2);
        const int cy = rect.y() + ((rect.height() - kSide) / 2);
        const QRect cb_rect(cx, cy, kSide, kSide);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        if (m_state == Qt::Unchecked) {
            painter->setBrush(QColor(QString::fromLatin1(sak::ui::kColorBgSurface)));
            painter->setPen(QPen(QColor(QString::fromLatin1(sak::ui::kColorBorderMuted)),
                                 sak::ui::kCssBorderWidthDefaultPx));
            painter->drawRoundedRect(cb_rect, kHeaderCheckRadiusPx, kHeaderCheckRadiusPx);
        } else {
            // Checked or PartiallyChecked  --  blue fill
            painter->setBrush(QColor(QString::fromLatin1(sak::ui::kColorPrimary)));
            painter->setPen(QPen(QColor(QString::fromLatin1(sak::ui::kColorPrimaryDark)), 1));
            painter->drawRoundedRect(cb_rect, kHeaderCheckRadiusPx, kHeaderCheckRadiusPx);
            painter->setPen(QPen(
                Qt::white, kHeaderCheckTickPenWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            if (m_state == Qt::Checked) {
                // White tick mark
                QPainterPath tick;
                tick.moveTo(cx + kHeaderCheckTickX0, cy + kHeaderCheckTickY0);
                tick.lineTo(cx + kHeaderCheckTickX1, cy + kHeaderCheckTickY1);
                tick.lineTo(cx + kHeaderCheckTickX2, cy + kHeaderCheckTickY2);
                painter->drawPath(tick);
            } else {
                // White horizontal dash for partial
                painter->drawLine(cx + kHeaderCheckDashX0,
                                  cy + kHeaderCheckDashY,
                                  cx + kHeaderCheckDashX1,
                                  cy + kHeaderCheckDashY);
            }
        }
        painter->restore();
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (logicalIndexAt(event->pos()) == 0) {
            const bool next = (m_state != Qt::Checked);
            m_state = next ? Qt::Checked : Qt::Unchecked;
            viewport()->update();
            Q_EMIT checkToggled(next);
        } else {
            QHeaderView::mousePressEvent(event);
        }
    }

private:
    Qt::CheckState m_state;
};
}  // anonymous namespace

namespace sak {

namespace {

constexpr int kWifiPasswordIconSize = ui::kUiIconSmall;
constexpr int kWifiSelectColumnWidth = kSelectColumnW;
constexpr int kQrBrowseButtonWidth = kSnapButtonW;
constexpr int kBatchQrDialogMinWidth = 400;
constexpr int kQrMultiDialogWidth = kDialogWidthMedium;
constexpr int kQrMultiDialogHeight = 530;
constexpr int kQrDisplaySize = 360;
constexpr int kQrNavButtonWidth = 70;
constexpr int kQrGalleryDialogWidth = 400;
constexpr int kQrGalleryDialogHeight = 460;
constexpr int kPasswordRevealIconSize = ui::kUiIconCompact;
constexpr int kFormSplitterStretch = 2;
constexpr int kTableSplitterStretch = 3;
constexpr int kCenteringDivisor = 2;
constexpr int kSearchButtonSpacingExtraPx = 2;
constexpr int kQrHeaderFontPointSize = 14;
constexpr int kPdfExportResolutionDpi = 150;
constexpr int kQrLargeDisplaySize = 360;
constexpr int kEscapedTextReserveMultiplier = 2;
constexpr ushort kPasswordBulletCharacter = 0x2022;
}  // namespace

// -----------------------------------------------------------------------------
// Table column indices
// -----------------------------------------------------------------------------
static constexpr int kColSelect = 0;  // checkbox
static constexpr int kColLocation = 1;
static constexpr int kColSsid = 2;
static constexpr int kColPassword = 3;
static constexpr int kColSecurity = 4;
static constexpr int kColHidden = 5;
static constexpr int kColCount = 6;

// -----------------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------------
WifiManagerPanel::WifiManagerPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
    connectSignals();
}

WifiManagerPanel::~WifiManagerPanel() {
    // Cooperatively cancel then bounded-join the in-flight "add to Windows profiles" install so the
    // netsh mutation does not run detached past teardown. installWlanProfiles checks the flag
    // between profiles, so teardown blocks for at most ONE in-flight netsh call (its own process
    // timeout), not one per selected network. The panel (and this atomic) outlive the wait.
    m_wlanInstallCancel.store(true);
    if (m_wlanInstallFuture.isRunning()) {
        // SAK-ALLOW-BLOCKING: installWlanProfiles checks the flag set above between profiles
        // and each netsh call has its own process timeout, so this blocks for at most one
        // in-flight call. Abandoning it would let a netsh mutation run detached past teardown.
        m_wlanInstallFuture.waitForFinished();
    }
}

// -----------------------------------------------------------------------------
// UI setup
// -----------------------------------------------------------------------------
void WifiManagerPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* outer_layout = new QVBoxLayout(this);
    outer_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* root_layout = new QVBoxLayout(content_widget);
    root_layout->setContentsMargins(
        sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall);
    root_layout->setSpacing(sak::ui::kSpacingSmall);

    scroll_area->setWidget(content_widget);
    outer_layout->addWidget(scroll_area);

    // Main content: horizontal splitter form | table
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    setupFormGroup();
    setupTableGroup();

    splitter->addWidget(m_form_group);
    splitter->addWidget(m_table_group);
    splitter->setStretchFactor(0, kFormSplitterStretch);
    splitter->setStretchFactor(1, kTableSplitterStretch);

    root_layout->addWidget(splitter, 1);

    setupActionButtons();

    // A11Y-07: explicit tab order for keyboard navigation
    setTabOrder(m_ssid_input, m_location_input);
    setTabOrder(m_location_input, m_password_input);
    setTabOrder(m_password_input, m_password_toggle_btn);
    setTabOrder(m_password_toggle_btn, m_security_combo);
    setTabOrder(m_security_combo, m_hidden_checkbox);
    setTabOrder(m_hidden_checkbox, m_connect_phone_btn);
    setTabOrder(m_connect_phone_btn, m_add_table_btn);
    setTabOrder(m_add_table_btn, m_search_input);
    setTabOrder(m_search_input, m_search_up_btn);
    setTabOrder(m_search_up_btn, m_search_down_btn);
    setTabOrder(m_search_down_btn, m_network_table);
    setTabOrder(m_network_table, m_delete_selected_btn);
    setTabOrder(m_delete_selected_btn, m_add_to_windows_btn);
    setTabOrder(m_add_to_windows_btn, m_save_table_btn);
    setTabOrder(m_save_table_btn, m_load_table_btn);
    setTabOrder(m_load_table_btn, m_generate_qr_btn);
    setTabOrder(m_generate_qr_btn, m_export_script_btn);
    setTabOrder(m_export_script_btn, m_export_macos_btn);
    setTabOrder(m_export_macos_btn, m_scan_networks_btn);
}

void WifiManagerPanel::setupNetworkIdentityFields(QFormLayout* layout) {
    m_location_input = new QLineEdit(m_form_group);
    m_location_input->setPlaceholderText("e.g. Home, Office, Guest");
    m_location_input->setToolTip("Optional label for this network entry");
    m_location_input->setAccessibleName(QStringLiteral("Network Location"));
    layout->addRow("Location:", m_location_input);

    // SSID
    m_ssid_input = new QLineEdit(m_form_group);
    m_ssid_input->setPlaceholderText("Network name (SSID)");
    m_ssid_input->setToolTip("The exact name of the WiFi network");
    m_ssid_input->setAccessibleName(QStringLiteral("Network SSID"));
    layout->addRow("SSID:", m_ssid_input);
}

void WifiManagerPanel::setupNetworkPasswordField(QFormLayout* layout) {
    auto* pass_row = new QHBoxLayout();
    m_password_input = new QLineEdit(m_form_group);
    m_password_input->setPlaceholderText("Password");
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setAccessibleName(QStringLiteral("Network Password"));
    m_password_toggle_btn = new QToolButton(m_form_group);
    m_password_toggle_btn->setCheckable(true);
    m_password_toggle_btn->setIcon(ui::passwordEyeOpenToolButtonIcon());
    m_password_toggle_btn->setIconSize(QSize(kWifiPasswordIconSize, kWifiPasswordIconSize));
    m_password_toggle_btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_password_toggle_btn->setToolTip("Show/hide password");
    m_password_toggle_btn->setAccessibleName(QStringLiteral("Toggle password visibility"));
    pass_row->addWidget(m_password_input);
    pass_row->addWidget(m_password_toggle_btn);
    auto* pass_widget = new QWidget(m_form_group);
    pass_widget->setLayout(pass_row);
    layout->addRow("Password:", pass_widget);
}

void WifiManagerPanel::setupNetworkOptionsFields(QFormLayout* layout) {
    m_security_combo = new QComboBox(m_form_group);
    m_security_combo->addItems({"WPA/WPA2/WPA3", "WEP", "None (Open)"});
    m_security_combo->setToolTip("WiFi security type");
    m_security_combo->setAccessibleName(QStringLiteral("Security Type"));
    layout->addRow("Security:", m_security_combo);

    // Hidden network
    m_hidden_checkbox = new QCheckBox("This network is hidden (not broadcasting SSID)",
                                      m_form_group);
    m_hidden_checkbox->setAccessibleName(QStringLiteral("Hidden Network"));
    layout->addRow("", m_hidden_checkbox);
}

void WifiManagerPanel::setupFormActionButtons(QFormLayout* layout) {
    m_connect_phone_btn = new QPushButton("Connect with Phone/Tablet", m_form_group);
    m_connect_phone_btn->setToolTip(
        "Show a QR code of the current network for phone/tablet "
        "scanning");
    m_connect_phone_btn->setAccessibleName(QStringLiteral("Connect with Phone"));
    m_connect_phone_btn->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_add_table_btn = new QPushButton("Add to Table", m_form_group);
    m_add_table_btn->setToolTip("Add current form entry to the saved networks table");
    m_add_table_btn->setAccessibleName(QStringLiteral("Add to Table"));
    m_add_table_btn->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    auto* form_btn_row = new QHBoxLayout();
    form_btn_row->setContentsMargins(sak::ui::kMarginNone,
                                     sak::ui::kSpacingDefault,
                                     sak::ui::kMarginNone,
                                     sak::ui::kMarginNone);  // top buffer
    form_btn_row->addWidget(m_connect_phone_btn);
    form_btn_row->addWidget(m_add_table_btn);
    auto* form_btn_widget = new QWidget(m_form_group);
    form_btn_widget->setLayout(form_btn_row);
    layout->addRow("", form_btn_widget);
}

void WifiManagerPanel::setupFormGroup() {
    m_form_group = new QGroupBox("Network Details", this);
    auto* layout = new QFormLayout(m_form_group);
    layout->setSpacing(sak::ui::kSpacingMedium);
    layout->setContentsMargins(sak::ui::kSpacingDefault,
                               sak::ui::kSpacingLarge + kSearchButtonSpacingExtraPx,
                               sak::ui::kSpacingDefault,
                               sak::ui::kSpacingDefault);
    setupNetworkIdentityFields(layout);
    setupNetworkPasswordField(layout);
    setupNetworkOptionsFields(layout);
    setupFormActionButtons(layout);
}

void WifiManagerPanel::setupTableGroup() {
    m_table_group = new QGroupBox("Saved Networks", this);
    auto* layout = new QVBoxLayout(m_table_group);
    layout->setContentsMargins(sak::ui::kMarginSmall,
                               sak::ui::kMarginMedium,
                               sak::ui::kMarginSmall,
                               sak::ui::kMarginSmall);
    layout->setSpacing(sak::ui::kSpacingSmall);

    setupTableSearchRow(layout);
    setupNetworkTable(layout);
    setupTableActionButtons(layout);
}

void WifiManagerPanel::setupTableSearchRow(QVBoxLayout* layout) {
    auto* search_row = new QHBoxLayout();
    m_search_input = new QLineEdit(m_table_group);
    m_search_input->setPlaceholderText("Search networks\u2026");
    m_search_input->setAccessibleName(QStringLiteral("Search Networks"));
    m_search_up_btn = new QToolButton(m_table_group);
    m_search_up_btn->setIcon(ui::selectorChevronUpToolButtonIcon());
    m_search_up_btn->setIconSize(QSize(ui::kUiIconSmall, ui::kUiIconSmall));
    m_search_up_btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_search_up_btn->setToolTip("Previous match");
    m_search_up_btn->setAccessibleName(QStringLiteral("Previous search match"));
    m_search_up_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_search_down_btn = new QToolButton(m_table_group);
    m_search_down_btn->setIcon(ui::selectorChevronDownToolButtonIcon());
    m_search_down_btn->setIconSize(QSize(ui::kUiIconSmall, ui::kUiIconSmall));
    m_search_down_btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_search_down_btn->setToolTip("Next match");
    m_search_down_btn->setAccessibleName(QStringLiteral("Next search match"));
    m_search_down_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    search_row->addWidget(m_search_input, 1);
    search_row->addWidget(m_search_up_btn);
    search_row->addWidget(m_search_down_btn);
    layout->addLayout(search_row);
}

void WifiManagerPanel::setupNetworkTable(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    m_network_table = new QTableWidget(0, kColCount, m_table_group);
    m_network_table->setAccessibleName(QStringLiteral("Saved WiFi Networks Table"));
    auto* check_header = new CheckHeaderView(m_table_group);
    m_network_table->setHorizontalHeader(check_header);
    m_network_table->setHorizontalHeaderLabels(
        {"", "Location", "SSID", "Password", "Security", "Hidden"});
    m_network_table->horizontalHeader()->setStretchLastSection(false);
    m_network_table->horizontalHeader()->setSectionResizeMode(kColSelect, QHeaderView::Fixed);
    m_network_table->horizontalHeader()->setSectionResizeMode(kColLocation, QHeaderView::Stretch);
    m_network_table->horizontalHeader()->setSectionResizeMode(kColSsid, QHeaderView::Stretch);
    m_network_table->horizontalHeader()->setSectionResizeMode(kColPassword, QHeaderView::Stretch);
    m_network_table->horizontalHeader()->setSectionResizeMode(kColSecurity,
                                                              QHeaderView::ResizeToContents);
    m_network_table->horizontalHeader()->setSectionResizeMode(kColHidden,
                                                              QHeaderView::ResizeToContents);
    m_network_table->setColumnWidth(kColSelect, kWifiSelectColumnWidth);
    m_network_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_network_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_network_table->setAlternatingRowColors(true);
    m_network_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_network_table->setToolTip("Double-click a row to load it into the form");
    m_network_table->setStyleSheet(sak::ui::wifiIndicatorStyle());
    layout->addWidget(m_network_table, 1);
}

void WifiManagerPanel::setupTableActionButtons(QVBoxLayout* layout) {
    auto* table_actions = new QHBoxLayout();
    m_delete_selected_btn = new QPushButton("Delete Selected", m_table_group);
    m_delete_selected_btn->setAccessibleName(QStringLiteral("Delete Selected Networks"));
    m_delete_selected_btn->setStyleSheet(sak::ui::kDangerButtonStyle);
    m_add_to_windows_btn = new QPushButton("Add Selected to Windows", m_table_group);
    m_add_to_windows_btn->setAccessibleName(QStringLiteral("Add Selected to Windows"));
    m_add_to_windows_btn->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_save_table_btn = new QPushButton("Save\u2026", m_table_group);
    m_save_table_btn->setAccessibleName(QStringLiteral("Save Networks to File"));
    m_save_table_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_load_table_btn = new QPushButton("Load\u2026", m_table_group);
    m_load_table_btn->setAccessibleName(QStringLiteral("Load Networks from File"));
    m_load_table_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_delete_selected_btn->setToolTip("Remove the selected row(s) from the table");
    m_add_to_windows_btn->setToolTip(
        "Add checked networks to Windows known WiFi profiles via netsh");
    m_add_to_windows_btn->setEnabled(false);
    m_save_table_btn->setToolTip("Save table to a JSON file");
    m_save_table_btn->setEnabled(false);
    m_load_table_btn->setToolTip("Load table from a JSON file");
    table_actions->addWidget(m_delete_selected_btn);
    table_actions->addWidget(m_add_to_windows_btn);
    table_actions->addStretch();
    table_actions->addWidget(m_save_table_btn);
    table_actions->addWidget(m_load_table_btn);
    layout->addLayout(table_actions);
}

void WifiManagerPanel::setupActionButtons() {
    auto* bar = new QHBoxLayout();

    m_generate_qr_btn = new QPushButton("Generate QR Code", this);
    m_generate_qr_btn->setAccessibleName(QStringLiteral("Generate QR Code"));
    m_generate_qr_btn->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_export_script_btn = new QPushButton("Generate Windows Script", this);
    m_export_script_btn->setAccessibleName(QStringLiteral("Generate Windows Script"));
    m_export_script_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_export_macos_btn = new QPushButton("Generate macOS Profile", this);
    m_export_macos_btn->setAccessibleName(QStringLiteral("Generate macOS Profile"));
    m_export_macos_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_scan_networks_btn = new QPushButton("Scan Known Networks", this);
    m_scan_networks_btn->setAccessibleName(QStringLiteral("Scan Known Networks"));
    m_scan_networks_btn->setStyleSheet(sak::ui::kPrimaryButtonStyle);

    m_generate_qr_btn->setToolTip(
        "Export the current network as a QR code image (PNG, PDF, JPG, "
        "BMP)");
    m_export_script_btn->setToolTip("Generate a Windows netsh .cmd script for the current network");
    m_export_macos_btn->setToolTip(
        "Generate a macOS WiFi .mobileconfig profile for the current "
        "network");
    m_scan_networks_btn->setToolTip("Scan Windows known WiFi profiles and add them to the table");

    bar->addWidget(m_generate_qr_btn);
    bar->addWidget(m_export_script_btn);
    bar->addWidget(m_export_macos_btn);
    bar->addStretch();

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    bar->addWidget(m_logToggle);
    bar->addWidget(m_scan_networks_btn);

    auto* bar_widget = new QWidget(this);
    bar_widget->setLayout(bar);
    if (auto* vbox = qobject_cast<QVBoxLayout*>(layout())) {
        vbox->addWidget(bar_widget);
    } else {
        sak::logError("WiFi panel: parent layout is not a QVBoxLayout");
    }
}

void WifiManagerPanel::connectSignals() {
    Q_ASSERT(m_network_table);
    connect(m_security_combo,
            &QComboBox::currentTextChanged,
            this,
            &WifiManagerPanel::onSecurityChanged);
    connect(m_password_toggle_btn,
            &QToolButton::toggled,
            this,
            &WifiManagerPanel::onTogglePasswordVisibility);
    connect(m_add_table_btn, &QPushButton::clicked, this, &WifiManagerPanel::onAddToTableClicked);
    connect(m_delete_selected_btn,
            &QPushButton::clicked,
            this,
            &WifiManagerPanel::onDeleteSelectedClicked);
    connect(m_save_table_btn, &QPushButton::clicked, this, &WifiManagerPanel::onSaveTableClicked);
    connect(m_load_table_btn, &QPushButton::clicked, this, &WifiManagerPanel::onLoadTableClicked);
    connect(m_network_table,
            &QTableWidget::doubleClicked,
            this,
            &WifiManagerPanel::onTableDoubleClicked);
    connect(m_search_input, &QLineEdit::textChanged, this, &WifiManagerPanel::onSearchChanged);
    connect(m_search_up_btn, &QToolButton::clicked, this, &WifiManagerPanel::onFindPrev);
    connect(m_search_down_btn, &QToolButton::clicked, this, &WifiManagerPanel::onFindNext);
    connect(m_generate_qr_btn, &QPushButton::clicked, this, &WifiManagerPanel::onGenerateQrClicked);
    connect(m_export_script_btn,
            &QPushButton::clicked,
            this,
            &WifiManagerPanel::onExportWindowsScriptClicked);
    connect(m_export_macos_btn,
            &QPushButton::clicked,
            this,
            &WifiManagerPanel::onExportMacosProfileClicked);
    connect(m_connect_phone_btn,
            &QPushButton::clicked,
            this,
            &WifiManagerPanel::onConnectWithPhoneClicked);
    connect(
        m_scan_networks_btn, &QPushButton::clicked, this, &WifiManagerPanel::onScanNetworksClicked);
    connect(m_add_to_windows_btn,
            &QPushButton::clicked,
            this,
            &WifiManagerPanel::onAddToWindowsClicked);
    connect(m_network_table,
            &QTableWidget::itemSelectionChanged,
            this,
            &WifiManagerPanel::onSelectionChanged);
    connect(
        m_network_table, &QTableWidget::itemChanged, this, &WifiManagerPanel::onTableItemChanged);

    // Wire the CheckHeaderView "select all" checkbox
    auto* check_hdr = qobject_cast<CheckHeaderView*>(m_network_table->horizontalHeader());
    if (check_hdr != nullptr) {
        connect(
            check_hdr, &CheckHeaderView::checkToggled, this, &WifiManagerPanel::setAllCheckStates);
    }
}

void WifiManagerPanel::setAllCheckStates(bool all_checked) {
    m_network_table->blockSignals(true);
    for (int row_index = 0; row_index < m_network_table->rowCount(); ++row_index) {
        auto* item = m_network_table->item(row_index, kColSelect);
        if (item != nullptr) {
            item->setCheckState(all_checked ? Qt::Checked : Qt::Unchecked);
        }
    }
    m_network_table->blockSignals(false);
    onSelectionChanged();
}

// -----------------------------------------------------------------------------
// Slots
// -----------------------------------------------------------------------------

void WifiManagerPanel::onSecurityChanged(const QString& value) {
    const bool has_password = !value.contains("None", Qt::CaseInsensitive);
    m_password_input->setEnabled(has_password);
    m_password_toggle_btn->setEnabled(has_password);
    if (!has_password) {
        m_password_input->clear();
    }
}

void WifiManagerPanel::onTogglePasswordVisibility() {
    const bool showing = m_password_toggle_btn->isChecked();
    m_password_input->setEchoMode(showing ? QLineEdit::Normal : QLineEdit::Password);
    // Eye open = "click to show" (password hidden); Eye closed = "click to hide" (password shown)
    m_password_toggle_btn->setIcon(showing ? ui::passwordEyeClosedToolButtonIcon()
                                           : ui::passwordEyeOpenToolButtonIcon());
}

void WifiManagerPanel::onAddToTableClicked() {
    const QString ssid = m_ssid_input->text().trimmed();
    if (ssid.isEmpty()) {
        Q_EMIT statusMessage("SSID cannot be empty.", sak::kTimerStatusMessageMs);
        return;
    }
    addRowToTable(configFromForm());
    Q_EMIT statusMessage(QString("Added \"%1\" to table.").arg(ssid), sak::kTimerStatusMessageMs);
}

void WifiManagerPanel::onDeleteSelectedClicked() {
    Q_ASSERT(m_network_table);
    const auto selected = m_network_table->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return;
    }

    QList<int> rows;
    for (const auto& idx : selected) {
        rows.prepend(idx.row());
    }

    std::ranges::sort(rows, std::greater<int>());
    for (const int row : rows) {
        m_network_table->removeRow(row);
    }

    onSelectionChanged();
    Q_EMIT statusMessage(QString("Deleted %1 row(s).").arg(rows.size()),
                         sak::kTimerStatusMessageMs);
}

void WifiManagerPanel::onTableDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    loadConfigToForm(configFromRow(index.row()));
}

void WifiManagerPanel::onSearchChanged(const QString& text) {
    updateSearchMatches(text);
    highlightSearchMatches();
    m_search_index = m_search_matches.isEmpty() ? -1 : 0;
    if (!m_search_matches.isEmpty()) {
        m_network_table->scrollToItem(m_network_table->item(m_search_matches.first(), kColSsid));
    }
}

void WifiManagerPanel::onFindNext() {
    if (m_search_matches.isEmpty()) {
        return;
    }
    m_search_index = static_cast<int>((m_search_index + 1) % m_search_matches.size());
    m_network_table->scrollToItem(
        m_network_table->item(m_search_matches.at(m_search_index), kColSsid));
    m_network_table->selectRow(m_search_matches.at(m_search_index));
}

void WifiManagerPanel::onFindPrev() {
    if (m_search_matches.isEmpty()) {
        return;
    }
    m_search_index =
        static_cast<int>((m_search_index - 1 + m_search_matches.size()) % m_search_matches.size());
    m_network_table->scrollToItem(
        m_network_table->item(m_search_matches.at(m_search_index), kColSsid));
    m_network_table->selectRow(m_search_matches.at(m_search_index));
}

// -----------------------------------------------------------------------------
// QR wizard helpers (TigerStyle decomposition of onGenerateQrClicked)
// -----------------------------------------------------------------------------

QImage WifiManagerPanel::renderQrWithHeader(const QString& payload,
                                            const QString& location,
                                            bool show_header) {
    const QImage base = generateQrImage(payload);
    if (!show_header || location.isEmpty()) {
        return base;
    }
    constexpr int kHeaderH = 52;
    QImage out(base.width(), base.height() + kHeaderH, QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter p(&out);
    QFont f = p.font();
    f.setPointSize(kQrHeaderFontPointSize);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(QRect(0, 0, out.width(), kHeaderH), Qt::AlignHCenter | Qt::AlignVCenter, location);
    p.drawImage(0, kHeaderH, base);
    p.end();
    return out;
}

bool WifiManagerPanel::exportQrToPdf(const QImage& image,
                                     const QString& path,
                                     const QString& title) {
    QPdfWriter writer(path);
    writer.setTitle(title);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(kPdfExportResolutionDpi);
    const QRect page_rect = writer.pageLayout().paintRectPixels(writer.resolution());
    const int side = std::min(page_rect.width(), page_rect.height());
    const QImage scaled = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&writer);
    if (!painter.isActive()) {
        return false;
    }
    painter.drawImage((page_rect.width() - scaled.width()) / kCenteringDivisor, 0, scaled);
    painter.end();
    return true;
}

QWidget* WifiManagerPanel::buildQrFormatPage(const QString& payload,
                                             const QString& location,
                                             QrWizardControls& ctl) {
    auto* page = new QWidget;
    auto* top_row = new QHBoxLayout;

    const QImage preview_img = renderQrWithHeader(payload, location, true)
                                   .scaled(sak::kQrImageSize,
                                           sak::kQrImageSize,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
    ctl.previewLabel = new QLabel(page);
    ctl.previewLabel->setPixmap(QPixmap::fromImage(preview_img));
    ctl.previewLabel->setFixedSize(sak::kQrImageSize, sak::kQrImageSize);
    ctl.previewLabel->setAlignment(Qt::AlignCenter);
    ctl.previewLabel->setStyleSheet(
        sak::ui::borderedPreviewStyle(sak::ui::kColorBorderDefault, sak::ui::kColorBgWhite));
    ctl.previewLabel->setAccessibleName(QStringLiteral("QR code preview"));

    auto* opt_widget = new QWidget(page);
    auto* opt_layout = new QVBoxLayout(opt_widget);
    opt_layout->setContentsMargins(
        sak::ui::kMarginSmall, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    opt_layout->addWidget(new QLabel("Select export format(s):"));
    ctl.chkPng = new QCheckBox("PNG");
    ctl.chkPng->setChecked(true);
    ctl.chkPdf = new QCheckBox("PDF");
    ctl.chkJpg = new QCheckBox("JPG");
    ctl.chkBmp = new QCheckBox("BMP");
    opt_layout->addWidget(ctl.chkPng);
    opt_layout->addWidget(ctl.chkPdf);
    opt_layout->addWidget(ctl.chkJpg);
    opt_layout->addWidget(ctl.chkBmp);
    opt_layout->addSpacing(sak::ui::kSpacingDefault);
    ctl.headerToggle = new LogToggleSwitch(tr("Location header"), opt_widget);
    ctl.headerToggle->setChecked(true);
    opt_layout->addWidget(ctl.headerToggle);
    opt_layout->addStretch();

    top_row->addWidget(ctl.previewLabel);
    top_row->addWidget(opt_widget, 1);

    auto* outer_layout = new QVBoxLayout(page);
    auto* btn_row = new QHBoxLayout;
    ctl.btnCancel0 = new QPushButton(tr("Cancel"));
    ctl.btnCancel0->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    ctl.btnNext = new QPushButton(tr("Next >"));
    ctl.btnNext->setDefault(true);
    ctl.btnNext->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    btn_row->addStretch();
    btn_row->addWidget(ctl.btnCancel0);
    btn_row->addWidget(ctl.btnNext);
    outer_layout->addLayout(top_row);
    outer_layout->addLayout(btn_row);
    return page;
}

QWidget* WifiManagerPanel::buildQrOutputPage(QrWizardControls& ctl) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Choose output directory:")));
    auto* dir_row = new QHBoxLayout;
    ctl.dirEdit = new QLineEdit;
    ctl.dirEdit->setReadOnly(true);
    ctl.dirEdit->setPlaceholderText(tr("Click Browse to select a folder..."));
    ctl.btnBrowse = new QPushButton(tr("Browse..."));
    ctl.btnBrowse->setMinimumWidth(kQrBrowseButtonWidth);
    ctl.btnBrowse->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    dir_row->addWidget(ctl.dirEdit, 1);
    dir_row->addWidget(ctl.btnBrowse);
    layout->addLayout(dir_row);
    ctl.subLabel = new QLabel(tr("Files will be saved to: (select a folder first)"));
    ctl.subLabel->setWordWrap(true);
    ctl.subLabel->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeSmall));
    layout->addWidget(ctl.subLabel);
    layout->addStretch();
    auto* btn_row = new QHBoxLayout;
    ctl.btnBack = new QPushButton("< Back");
    ctl.btnBack->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    ctl.btnCancel1 = new QPushButton("Cancel");
    ctl.btnCancel1->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    ctl.btnGenerate = new QPushButton("Generate");
    ctl.btnGenerate->setDefault(true);
    ctl.btnGenerate->setEnabled(false);
    ctl.btnGenerate->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    btn_row->addWidget(ctl.btnBack);
    btn_row->addStretch();
    btn_row->addWidget(ctl.btnCancel1);
    btn_row->addWidget(ctl.btnGenerate);
    layout->addLayout(btn_row);
    return page;
}

void WifiManagerPanel::connectSingleQrWizard(QDialog* dlg,
                                             QStackedWidget* stack,
                                             QrWizardControls ctl,
                                             const QrExportContent& content) {
    QObject::connect(ctl.headerToggle,
                     &LogToggleSwitch::toggled,
                     [ctl, payload = content.payload, location = content.location](bool on) {
                         const QImage updated = renderQrWithHeader(payload, location, on)
                                                    .scaled(sak::kQrImageSize,
                                                            sak::kQrImageSize,
                                                            Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation);
                         ctl.previewLabel->setPixmap(QPixmap::fromImage(updated));
                     });
    QObject::connect(ctl.btnCancel0, &QPushButton::clicked, dlg, &QDialog::reject);
    QObject::connect(ctl.btnNext, &QPushButton::clicked, [this, ctl, stack]() {
        if (!ctl.chkPng->isChecked() && !ctl.chkPdf->isChecked() && !ctl.chkJpg->isChecked() &&
            !ctl.chkBmp->isChecked()) {
            Q_EMIT statusMessage("Select at least one format.", sak::kTimerServiceDelayMs);
            return;
        }
        stack->setCurrentIndex(1);
    });
    QObject::connect(ctl.btnCancel1, &QPushButton::clicked, dlg, &QDialog::reject);
    QObject::connect(ctl.btnBack, &QPushButton::clicked, [stack]() { stack->setCurrentIndex(0); });
    QObject::connect(
        ctl.btnBrowse, &QPushButton::clicked, [dlg, ctl, sub_name = content.sub_name]() {
            const QString start =
                ctl.dirEdit->text().isEmpty()
                    ? QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                    : ctl.dirEdit->text();
            const QString chosen =
                QFileDialog::getExistingDirectory(dlg, "Select Output Directory", start);
            if (chosen.isEmpty()) {
                return;
            }
            ctl.dirEdit->setText(chosen);
            ctl.subLabel->setText(QString("Files will be saved to: %1/%2/").arg(chosen, sub_name));
            ctl.btnGenerate->setEnabled(true);
        });
    QObject::connect(ctl.btnGenerate, &QPushButton::clicked, [this, dlg, ctl, content]() {
        executeSingleQrExport(dlg, ctl, content);
    });
}

void WifiManagerPanel::saveCheckedFormats(QDialog* dlg,
                                          QrWizardControls ctl,
                                          const QImage& final_img,
                                          const QrExportContent& content,
                                          QStringList& saved) {
    const QString out_dir = ctl.dirEdit->text() + "/" + content.sub_name;
    auto save_plain = [&](const QString& ext, const QString& fmt) {
        const QString path = out_dir + "/" + content.sub_name + "." + ext;
        if (!final_img.save(path, fmt.toUtf8().constData())) {
            sak::logWarning(("Failed to save " + ext.toUpper() + ": " + path).toStdString());
            sak::showWarningLogged(dlg,
                                   "Export Error",
                                   "Failed to save " + ext.toUpper() + ":\n" + path);
        } else {
            saved.append(ext.toUpper());
        }
    };
    if (ctl.chkPng->isChecked()) {
        save_plain("png", "PNG");
    }
    if (ctl.chkJpg->isChecked()) {
        save_plain("jpg", "JPEG");
    }
    if (ctl.chkBmp->isChecked()) {
        save_plain("bmp", "BMP");
    }
    if (ctl.chkPdf->isChecked()) {
        const QString pdf_path = out_dir + "/" + content.sub_name + ".pdf";
        const QString title = content.ssid.isEmpty() ? QStringLiteral("WiFi QR Code")
                                                     : content.ssid + " WiFi QR Code";
        if (exportQrToPdf(final_img, pdf_path, title)) {
            saved.append("PDF");
        }
    }
}

void WifiManagerPanel::executeSingleQrExport(QDialog* dlg,
                                             QrWizardControls ctl,
                                             const QrExportContent& content) {
    const QString base_dir = ctl.dirEdit->text();
    if (base_dir.isEmpty()) {
        return;
    }
    const QString out_dir = base_dir + "/" + content.sub_name;
    if (!QDir().mkpath(out_dir)) {
        sak::logWarning(("Could not create output folder: " + out_dir).toStdString());
        sak::showWarningLogged(dlg, "Error", "Could not create output folder:\n" + out_dir);
        return;
    }
    const bool show_header = ctl.headerToggle->isChecked();
    const QImage final_img = renderQrWithHeader(content.payload, content.location, show_header);
    QStringList saved;
    saveCheckedFormats(dlg, ctl, final_img, content, saved);
    dlg->accept();
    if (!saved.isEmpty()) {
        Q_EMIT statusMessage(QString("Saved %1 to: %2").arg(saved.join(", "), out_dir),
                             sak::kTimerStatusMessageMs);
    }
}

void WifiManagerPanel::showSingleQrWizard(const WifiConfig& cfg) {
    const QString payload = buildWifiPayloadFromConfig(cfg);
    const QString ssid = cfg.ssid;
    const QString location = cfg.location;

    if (payload.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", sak::kTimerStatusMessageMs);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Generate QR Code");
    dlg.setMinimumWidth(sak::kDialogWidthMedium);

    auto* main_layout = new QVBoxLayout(&dlg);
    auto* stack = new QStackedWidget(&dlg);
    main_layout->addWidget(stack);

    QrWizardControls ctl;
    stack->addWidget(buildQrFormatPage(payload, location, ctl));
    stack->addWidget(buildQrOutputPage(ctl));

    const QString raw_name = location.isEmpty() ? ssid : location + "_" + ssid;
    const QString sub_name = QString(raw_name).replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");

    connectSingleQrWizard(
        &dlg,
        stack,
        ctl,
        {.payload = payload, .ssid = ssid, .location = location, .sub_name = sub_name});
    dlg.exec();
}

bool WifiManagerPanel::executeSingleQrNetwork(const WifiConfig& cfg,
                                              const QString& base_dir,
                                              bool show_header,
                                              const QrExportFormats& formats) {
    const QString cfg_payload = buildWifiPayloadFromConfig(cfg);
    const QString raw_name = cfg.location.isEmpty() ? cfg.ssid : cfg.location + "_" + cfg.ssid;
    const QString sub_name = QString(raw_name).replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
    const QString out_dir = base_dir + "/" + sub_name;
    if (!QDir().mkpath(out_dir)) {
        return false;
    }
    const QImage img = renderQrWithHeader(cfg_payload, cfg.location, show_header);
    bool any_saved = false;
    if (formats.png && img.save(out_dir + "/" + sub_name + ".png", "PNG")) {
        any_saved = true;
    }
    if (formats.jpg && img.save(out_dir + "/" + sub_name + ".jpg", "JPEG")) {
        any_saved = true;
    }
    if (formats.bmp && img.save(out_dir + "/" + sub_name + ".bmp", "BMP")) {
        any_saved = true;
    }
    if (formats.pdf) {
        exportQrToPdf(img, out_dir + "/" + sub_name + ".pdf", cfg.ssid + " WiFi QR Code");
        any_saved = true;
    }
    return any_saved;
}

void WifiManagerPanel::executeBatchQrExport(QDialog* dlg,
                                            const QList<WifiConfig>& sources,
                                            const QString& base_dir,
                                            bool show_header,
                                            const QrExportFormats& formats) {
    Q_ASSERT(!sources.isEmpty());
    Q_ASSERT(!base_dir.isEmpty());
    int saved = 0;
    int failed = 0;
    for (const WifiConfig& cfg : sources) {
        if (cfg.ssid.isEmpty()) {
            continue;
        }
        executeSingleQrNetwork(cfg, base_dir, show_header, formats) ? ++saved : ++failed;
    }
    dlg->accept();
    const QString msg =
        failed > 0 ? QString("Batch QR: saved %1 network(s) to %2 (%3 failed).")
                         .arg(saved)
                         .arg(base_dir)
                         .arg(failed)
                   : QString("Batch QR: saved %1 network(s) to: %2").arg(saved).arg(base_dir);
    Q_EMIT statusMessage(msg, sak::kTimerStatusExtendedMs);
}

namespace {

struct BatchQrDialogUi {
    QCheckBox* chk_png{nullptr};
    QCheckBox* chk_pdf{nullptr};
    QCheckBox* chk_jpg{nullptr};
    QCheckBox* chk_bmp{nullptr};
    LogToggleSwitch* header_toggle{nullptr};
    QLineEdit* dir_edit{nullptr};
    QLabel* sub_label{nullptr};
    QPushButton* btn_browse{nullptr};
    QPushButton* btn_cancel{nullptr};
    QPushButton* btn_gen{nullptr};
};

BatchQrDialogUi buildBatchQrDialogUi(QDialog* dlg, int network_count) {
    Q_ASSERT(network_count >= 0);
    Q_ASSERT(dlg);

    BatchQrDialogUi ui;
    dlg->setWindowTitle(QString("Batch QR Export (%1 networks)").arg(network_count));
    dlg->setMinimumWidth(kBatchQrDialogMinWidth);

    auto* layout = new QVBoxLayout(dlg);
    layout->addWidget(
        new QLabel(QString("Generate QR codes for %1 selected networks:").arg(network_count), dlg));
    layout->addSpacing(sak::ui::kSpacingSmall);
    layout->addWidget(new QLabel("Export format(s):", dlg));

    ui.chk_png = new QCheckBox("PNG", dlg);
    ui.chk_png->setChecked(true);
    ui.chk_pdf = new QCheckBox("PDF", dlg);
    ui.chk_jpg = new QCheckBox("JPG", dlg);
    ui.chk_bmp = new QCheckBox("BMP", dlg);
    for (auto* chk : {ui.chk_png, ui.chk_pdf, ui.chk_jpg, ui.chk_bmp}) {
        layout->addWidget(chk);
    }

    layout->addSpacing(sak::ui::kSpacingMedium);
    ui.header_toggle = new LogToggleSwitch(QObject::tr("Location header"), dlg);
    ui.header_toggle->setChecked(true);
    layout->addWidget(ui.header_toggle);
    layout->addSpacing(sak::ui::kSpacingMedium);

    layout->addWidget(
        new QLabel(QObject::tr("Output directory (one subfolder per network):"), dlg));
    auto* dir_row = new QHBoxLayout;
    ui.dir_edit = new QLineEdit(dlg);
    ui.dir_edit->setReadOnly(true);
    ui.dir_edit->setPlaceholderText(QObject::tr("Click Browse..."));
    ui.btn_browse = new QPushButton(QObject::tr("Browse..."), dlg);
    ui.btn_browse->setMinimumWidth(kQrBrowseButtonWidth);
    ui.btn_browse->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    dir_row->addWidget(ui.dir_edit, 1);
    dir_row->addWidget(ui.btn_browse);
    layout->addLayout(dir_row);

    ui.sub_label = new QLabel("", dlg);
    ui.sub_label->setWordWrap(true);
    ui.sub_label->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeSmall));
    layout->addWidget(ui.sub_label);

    layout->addStretch();
    auto* btn_row = new QHBoxLayout;
    ui.btn_cancel = new QPushButton("Cancel", dlg);
    ui.btn_cancel->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    ui.btn_gen = new QPushButton("Batch Generate", dlg);
    ui.btn_gen->setDefault(true);
    ui.btn_gen->setEnabled(false);
    ui.btn_gen->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    btn_row->addStretch();
    btn_row->addWidget(ui.btn_cancel);
    btn_row->addWidget(ui.btn_gen);
    layout->addLayout(btn_row);

    return ui;
}

struct MultiNetworkQrDialogUi {
    QLabel* idx_lbl{nullptr};
    QLabel* title_lbl{nullptr};
    QLabel* img_lbl{nullptr};
    QPushButton* prev_btn{nullptr};
    QPushButton* next_btn{nullptr};
    QPushButton* close_btn{nullptr};
};

MultiNetworkQrDialogUi buildMultiNetworkQrDialogUi(QDialog* dlg, int network_count) {
    Q_ASSERT(network_count >= 0);
    Q_ASSERT(dlg);

    MultiNetworkQrDialogUi ui;
    dlg->setWindowTitle(QString("Connect with Phone / Tablet (%1 networks)").arg(network_count));
    dlg->setMinimumSize(kQrMultiDialogWidth, kQrMultiDialogHeight);
    dlg->resize(kQrMultiDialogWidth, kQrMultiDialogHeight);

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(sak::ui::kMarginXLarge,
                               sak::ui::kMarginLarge,
                               sak::ui::kMarginXLarge,
                               sak::ui::kMarginLarge);
    layout->setSpacing(sak::ui::kSpacingMedium);

    ui.idx_lbl = new QLabel(dlg);
    ui.idx_lbl->setAlignment(Qt::AlignCenter);
    ui.idx_lbl->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeNote));
    layout->addWidget(ui.idx_lbl);

    ui.title_lbl = new QLabel(dlg);
    ui.title_lbl->setAlignment(Qt::AlignCenter);
    ui.title_lbl->setStyleSheet(sak::ui::fontSizeStyle(sak::ui::kFontSizeSection));
    layout->addWidget(ui.title_lbl);

    ui.img_lbl = new QLabel(dlg);
    ui.img_lbl->setAlignment(Qt::AlignCenter);
    ui.img_lbl->setStyleSheet(
        sak::ui::imagePreviewStyle(sak::ui::kColorBgWhite, sak::ui::kColorBorderDefault));
    ui.img_lbl->setFixedSize(kQrDisplaySize, kQrDisplaySize);
    ui.img_lbl->setAccessibleName(QStringLiteral("WiFi QR code"));
    layout->addWidget(ui.img_lbl, 0, Qt::AlignHCenter);

    auto* hint_lbl = new QLabel(
        "Scan this QR code with your phone or tablet\n"
        "to connect to the network.",
        dlg);
    hint_lbl->setAlignment(Qt::AlignCenter);
    hint_lbl->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextSecondary, sak::ui::kFontSizeNote));
    layout->addWidget(hint_lbl);

    auto* nav_bar = new QHBoxLayout();
    ui.prev_btn = new QPushButton(QObject::tr("< Prev"), dlg);
    ui.next_btn = new QPushButton(QObject::tr("Next >"), dlg);
    ui.close_btn = new QPushButton("Close", dlg);
    ui.prev_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    ui.next_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    ui.close_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    ui.prev_btn->setMinimumWidth(kQrNavButtonWidth);
    ui.next_btn->setMinimumWidth(kQrNavButtonWidth);
    nav_bar->addWidget(ui.prev_btn);
    nav_bar->addStretch();
    nav_bar->addWidget(ui.close_btn);
    nav_bar->addStretch();
    nav_bar->addWidget(ui.next_btn);
    layout->addLayout(nav_bar);

    return ui;
}

}  // namespace

void WifiManagerPanel::showBatchQrDialog(const QList<WifiConfig>& sources) {
    Q_ASSERT(!sources.isEmpty());
    QDialog dlg(this);
    const BatchQrDialogUi ui = buildBatchQrDialogUi(&dlg, static_cast<int>(sources.size()));

    QObject::connect(ui.btn_cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(ui.btn_browse, &QPushButton::clicked, &dlg, [&]() {
        const QString start = ui.dir_edit->text().isEmpty() ? QStandardPaths::writableLocation(
                                                                  QStandardPaths::DesktopLocation)
                                                            : ui.dir_edit->text();
        const QString chosen =
            QFileDialog::getExistingDirectory(&dlg, "Select Output Directory", start);
        if (chosen.isEmpty()) {
            return;
        }
        ui.dir_edit->setText(chosen);
        ui.sub_label->setText(
            QString("One subfolder will be created per network under: %1").arg(chosen));
        ui.btn_gen->setEnabled(true);
    });

    QObject::connect(ui.btn_gen, &QPushButton::clicked, &dlg, [&]() {
        const QString base_dir = ui.dir_edit->text();
        if (base_dir.isEmpty()) {
            return;
        }
        if (!ui.chk_png->isChecked() && !ui.chk_pdf->isChecked() && !ui.chk_jpg->isChecked() &&
            !ui.chk_bmp->isChecked()) {
            sak::logWarning("Select at least one export format.");
            sak::showWarningLogged(&dlg, "No Format", "Select at least one export format.");
            return;
        }
        executeBatchQrExport(&dlg,
                             sources,
                             base_dir,
                             ui.header_toggle->isChecked(),
                             {.png = ui.chk_png->isChecked(),
                              .pdf = ui.chk_pdf->isChecked(),
                              .jpg = ui.chk_jpg->isChecked(),
                              .bmp = ui.chk_bmp->isChecked()});
    });
    dlg.exec();
}

// -----------------------------------------------------------------------------

void WifiManagerPanel::onGenerateQrClicked() {
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", sak::kTimerStatusMessageMs);
        return;
    }

    if (sources.size() == 1) {
        showSingleQrWizard(sources.first());
    } else {
        showBatchQrDialog(sources);
    }
}

void WifiManagerPanel::onExportWindowsScriptClicked() {
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", sak::kTimerStatusMessageMs);
        return;
    }

    if (sources.size() == 1) {
        exportSingleWindowsScript(sources.first());
    } else {
        exportMultipleWindowsScripts(sources);
    }
}

void WifiManagerPanel::exportSingleWindowsScript(const WifiConfig& cfg) {
    const QString script = buildWindowsScript(cfg.ssid, cfg.password, cfg.security, cfg.hidden);
    if (script.isEmpty()) {
        sak::showWarningLogged(this,
                               "Export Error",
                               "This SSID contains characters that cannot be safely written to a "
                               "Windows script (a quote or a control character).");
        return;
    }
    const QString default_name = cfg.ssid + "_wifi_connect.cmd";
    const QString path = QFileDialog::getSaveFileName(
        this,
        "Save Windows Script",
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) + "/" + default_name,
        "Windows Batch Script (*.cmd *.bat)");
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logWarning("Export Error: Failed to open file for writing.");
        sak::showWarningLogged(this, "Export Error", "Failed to open file for writing.");
        return;
    }
    QTextStream out(&file);
    out << script;
    if (!flushTextStreamOk(out, file)) {
        file.close();
        QFile::remove(path);  // never leave a truncated script behind
        sak::showWarningLogged(this, "Export Error", "Failed to write the script:\n" + path);
        return;
    }
    Q_EMIT statusMessage(QString("Saved Windows script: %1").arg(path), sak::kTimerStatusDefaultMs);
}

void WifiManagerPanel::exportMultipleWindowsScripts(const QList<WifiConfig>& sources) {
    Q_ASSERT(!sources.isEmpty());

    const QString out_dir = QFileDialog::getExistingDirectory(
        this,
        "Select Output Folder for Windows Scripts",
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    if (out_dir.isEmpty()) {
        return;
    }

    int saved = 0, failed = 0;
    for (const WifiConfig& cfg : sources) {
        if (cfg.ssid.isEmpty()) {
            continue;
        }
        const QString script = buildWindowsScript(cfg.ssid, cfg.password, cfg.security, cfg.hidden);
        if (script.isEmpty()) {
            ++failed;
            continue;
        }
        const QString safe_name = QString(cfg.ssid).replace(QRegularExpression("[\\\\/:*?\"<>|]"),
                                                            "_");
        const QString path = out_dir + "/" + safe_name + "_wifi_connect.cmd";
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            ++failed;
            continue;
        }
        QTextStream out(&file);
        out << script;
        if (!flushTextStreamOk(out, file)) {
            file.close();
            QFile::remove(path);  // never count a truncated script as saved
            ++failed;
            continue;
        }
        ++saved;
    }
    const QString msg =
        failed > 0
            ? QString("Saved %1 script(s) to %2 (%3 failed).").arg(saved).arg(out_dir).arg(failed)
            : QString("Saved %1 Windows script(s) to: %2").arg(saved).arg(out_dir);
    Q_EMIT statusMessage(msg, sak::kTimerStatusLongMs);
}

void WifiManagerPanel::onExportMacosProfileClicked() {
    // Multi-modal: operate on checked rows when any are checked, otherwise use form
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", sak::kTimerStatusMessageMs);
        return;
    }

    const QString xml = buildMacosProfile(sources);

    // Derive a sensible default filename
    const QString default_name = (sources.size() == 1)
                                     ? sources.first().ssid + "_wifi.mobileconfig"
                                     : QString("wifi_%1_networks.mobileconfig").arg(sources.size());

    const QString path = QFileDialog::getSaveFileName(
        this,
        "Save macOS Profile",
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) + "/" + default_name,
        "macOS Configuration Profile (*.mobileconfig)");
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logWarning("Export Error: Failed to open file for writing.");
        sak::showWarningLogged(this, "Export Error", "Failed to open file for writing.");
        return;
    }
    QTextStream out(&file);
    out << xml;
    if (!flushTextStreamOk(out, file)) {
        file.close();
        QFile::remove(path);  // never leave a truncated profile behind
        sak::showWarningLogged(this, "Export Error", "Failed to write the profile:\n" + path);
        return;
    }

    const QString label =
        (sources.size() == 1)
            ? QString("Saved macOS profile: %1").arg(path)
            : QString("Saved macOS profile with %1 networks: %2").arg(sources.size()).arg(path);
    Q_EMIT statusMessage(label, sak::kTimerStatusDefaultMs);
}

void WifiManagerPanel::onSaveTableClicked() {
    Q_ASSERT(m_network_table);
    // Collect checked rows
    QList<int> checked_rows;
    for (int row_index = 0; row_index < m_network_table->rowCount(); ++row_index) {
        auto* item = m_network_table->item(row_index, kColSelect);
        if ((item != nullptr) && item->checkState() == Qt::Checked) {
            checked_rows.append(row_index);
        }
    }

    const QString path = QFileDialog::getSaveFileName(this,
                                                      "Save Network Table",
                                                      m_save_path.isEmpty()
                                                          ? QStandardPaths::writableLocation(
                                                                QStandardPaths::DocumentsLocation) +
                                                                "/wifi_networks"
                                                                ".json"
                                                          : m_save_path + "/wifi_networks.json",
                                                      "JSON Files (*.json)");
    if (path.isEmpty()) {
        return;
    }
    m_save_path = QFileInfo(path).absolutePath();

    if (!checked_rows.isEmpty()) {
        saveCheckedRowsToJson(path, checked_rows);
    } else {
        saveTableToJson(path);
    }
}

void WifiManagerPanel::saveCheckedRowsToJson(const QString& path, const QList<int>& checked_rows) {
    QJsonArray arr;
    for (const int row_index : checked_rows) {
        const WifiConfig cfg = configFromRow(row_index);
        QJsonObject obj;
        obj["location"] = cfg.location;
        obj["ssid"] = cfg.ssid;
        obj["password"] = cfg.password;
        obj["security"] = cfg.security;
        obj["hidden"] = cfg.hidden;
        arr.append(obj);
    }
    const QJsonDocument doc(arr);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        sak::logWarning(("Could not open file for writing: " + path).toStdString());
        sak::showWarningLogged(this, "Save Error", "Could not open file for writing:\n" + path);
        return;
    }
    const QByteArray json_bytes = doc.toJson();
    const qint64 written = f.write(json_bytes);
    // Fail closed: a short write or failed atomic commit must leave the original file untouched
    // and must NOT report the checked networks as saved.
    const bool committed = (written == json_bytes.size()) && f.commit();
    if (!jsonWriteSucceeded(written, json_bytes.size(), committed)) {
        f.cancelWriting();
        sak::logWarning("Incomplete write to checked network file: {}", path.toStdString());
        sak::showWarningLogged(this,
                               "Save Error",
                               "Failed to write the network table (file left unchanged):\n" + path);
        return;
    }
    Q_EMIT statusMessage(QString("Saved %1 checked network(s) to %2").arg(arr.size()).arg(path),
                         sak::kTimerStatusDefaultMs);
}

void WifiManagerPanel::onLoadTableClicked() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Load Network Table",
        m_save_path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                              : m_save_path,
        "JSON Files (*.json)");
    if (path.isEmpty()) {
        return;
    }
    m_save_path = QFileInfo(path).absolutePath();
    loadTableFromJson(path);
}

void WifiManagerPanel::onConnectWithPhoneClicked() {
    // Multi-modal: operate on checked rows when any are checked, otherwise use form
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", sak::kTimerStatusMessageMs);
        return;
    }

    if (sources.size() == 1) {
        showSingleNetworkQrDialog(sources.first());
    } else {
        showMultiNetworkQrDialog(sources);
    }
}

void WifiManagerPanel::showSingleNetworkQrDialog(const WifiConfig& cfg) {
    Q_ASSERT(!cfg.ssid.isEmpty());
    const QString payload = buildWifiPayloadFromConfig(cfg);
    const QImage qr_img = generateQrImage(payload).scaled(
        kQrLargeDisplaySize, kQrLargeDisplaySize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QDialog dlg(this);
    dlg.setWindowTitle("Connect with Phone / Tablet");
    dlg.setMinimumSize(kQrGalleryDialogWidth, kQrGalleryDialogHeight);
    dlg.resize(kQrGalleryDialogWidth, kQrGalleryDialogHeight);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(sak::ui::kMarginXLarge,
                               sak::ui::kMarginXLarge,
                               sak::ui::kMarginXLarge,
                               sak::ui::kMarginXLarge);
    layout->setSpacing(sak::ui::kSpacingDefault);

    auto* title_lbl = new QLabel(QString("<b>%1</b>").arg(cfg.ssid.toHtmlEscaped()), &dlg);
    title_lbl->setAlignment(Qt::AlignCenter);
    title_lbl->setStyleSheet(sak::ui::fontSizeStyle(sak::ui::kFontSizeSection));
    layout->addWidget(title_lbl);

    auto* img_lbl = new QLabel(&dlg);
    img_lbl->setPixmap(QPixmap::fromImage(qr_img));
    img_lbl->setAlignment(Qt::AlignCenter);
    img_lbl->setStyleSheet(
        sak::ui::imagePreviewStyle(sak::ui::kColorBgWhite, sak::ui::kColorBorderDefault));
    img_lbl->setAccessibleName(QStringLiteral("WiFi QR code"));
    layout->addWidget(img_lbl);

    auto* hint_lbl = new QLabel(
        "Scan this QR code with your phone or tablet\nto connect to the "
        "network.",
        &dlg);
    hint_lbl->setAlignment(Qt::AlignCenter);
    hint_lbl->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextSecondary, sak::ui::kFontSizeNote));
    layout->addWidget(hint_lbl);

    auto* close_btn = new QPushButton("Close", &dlg);
    close_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(close_btn, &QPushButton::clicked, &dlg, &QDialog::accept);
    layout->addWidget(close_btn);

    dlg.exec();
}

void WifiManagerPanel::showMultiNetworkQrDialog(const QList<WifiConfig>& sources) {
    Q_ASSERT(!sources.isEmpty());
    QDialog dlg(this);
    const MultiNetworkQrDialogUi ui = buildMultiNetworkQrDialogUi(&dlg,
                                                                  static_cast<int>(sources.size()));

    int current_idx = 0;

    auto update_page = [&sources, &ui](int idx) {
        Q_ASSERT(idx >= 0);
        Q_ASSERT(idx < sources.size());

        const WifiConfig& cfg = sources[idx];
        const QString payload = buildWifiPayloadFromConfig(cfg);
        const QImage qr_img = generateQrImage(payload).scaled(
            360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        ui.img_lbl->setPixmap(QPixmap::fromImage(qr_img));

        ui.title_lbl->setText(QString("<b>%1</b>")
                                  .arg(cfg.ssid.isEmpty() ? QStringLiteral("WiFi Network")
                                                          : cfg.ssid.toHtmlEscaped()));
        ui.idx_lbl->setText(QString("Network %1 of %2").arg(idx + 1).arg(sources.size()));
        ui.prev_btn->setEnabled(idx > 0);
        ui.next_btn->setEnabled(idx < static_cast<int>(sources.size()) - 1);
    };

    connect(ui.prev_btn, &QPushButton::clicked, &dlg, [&current_idx, &update_page]() {
        update_page(--current_idx);
    });
    connect(ui.next_btn, &QPushButton::clicked, &dlg, [&current_idx, &update_page]() {
        update_page(++current_idx);
    });
    connect(ui.close_btn, &QPushButton::clicked, &dlg, &QDialog::accept);

    update_page(0);
    dlg.exec();
}


void WifiManagerPanel::onScanNetworksClicked() {
#ifndef Q_OS_WIN
    Q_EMIT statusMessage("Scan Known Networks is only supported on Windows.",
                         sak::kTimerStatusWarnMs);
    return;
#else
    m_scan_networks_btn->setEnabled(false);
    Q_EMIT statusMessage("Scanning known Windows WiFi profiles...", 0);

    auto* watcher = new QFutureWatcher<QList<WifiConfig>>(this);
    const QPointer<WifiManagerPanel> panel(this);
    connect(watcher, &QFutureWatcher<QList<WifiConfig>>::finished, this, [watcher, panel]() {
        watcher->deleteLater();
        if (!panel) {
            return;
        }

        panel->m_scan_networks_btn->setEnabled(true);
        const QList<WifiConfig> configs = watcher->result();
        if (configs.isEmpty()) {
            Q_EMIT panel->statusMessage("No known WiFi profiles found.",
                                        sak::kTimerStatusMessageMs);
            return;
        }

        int added = 0;
        for (const WifiConfig& cfg : configs) {
            if (cfg.ssid.isEmpty()) {
                continue;
            }
            panel->addRowToTable(cfg);
            ++added;
        }

        Q_EMIT panel->statusMessage(QString("Added %1 known network(s) to table.").arg(added),
                                    sak::kTimerStatusDefaultMs);
    });

    watcher->setFuture(QtConcurrent::run([]() {
        QList<WifiConfig> configs;
        const QStringList profile_names = scanWindowsProfileNames();
        configs.reserve(profile_names.size());
        for (const QString& name : profile_names) {
            const WifiConfig cfg = parseWindowsWifiProfile(name);
            if (!cfg.ssid.isEmpty()) {
                configs.append(cfg);
            }
        }
        return configs;
    }));
#endif
}

QStringList WifiManagerPanel::scanWindowsProfileNames() {
    // System32-qualified netsh, never the bare name: CreateProcess searches the current
    // directory ahead of System32, so a planted netsh could feed us fabricated profiles
    // (or run with our token). Unresolvable -> FAILED scan, never a PATH-found binary.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        sak::logError("Cannot resolve the System32 netsh.exe path; WiFi profile scan aborted");
        return {};
    }
    const auto result = sak::runProcess(
        netsh_exe,
        {QStringLiteral("wlan"), QStringLiteral("show"), QStringLiteral("profiles")},
        sak::kTimerNetshWaitMs);
    if (!result.succeeded()) {
        return {};
    }
    const QString output = result.std_out;
    QStringList profile_names;
    const QRegularExpression name_re(R"(:\s+(.+)$)");
    for (const QString& line : output.split('\n')) {
        const QString trimmed = line.trimmed();
        if (!trimmed.contains("All User Profile", Qt::CaseInsensitive) &&
            !trimmed.contains("Current User Profile", Qt::CaseInsensitive)) {
            continue;
        }
        const auto match = name_re.match(trimmed);
        if (!match.hasMatch()) {
            continue;
        }
        const QString name = match.captured(1).trimmed();
        if (!name.isEmpty()) {
            profile_names.append(name);
        }
    }
    return profile_names;
}

WifiManagerPanel::WifiConfig WifiManagerPanel::parseWindowsWifiProfile(
    const QString& profile_name) {
    Q_ASSERT(!profile_name.isEmpty());
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        sak::logError("Cannot resolve the System32 netsh.exe path; WiFi profile read aborted");
        return {};
    }
    const auto result = sak::runProcess(netsh_exe,
                                        {QStringLiteral("wlan"),
                                         QStringLiteral("show"),
                                         QStringLiteral("profile"),
                                         QStringLiteral("name=") + profile_name,
                                         QStringLiteral("key=clear")},
                                        sak::kTimeoutProcessShortMs);
    if (!result.succeeded()) {
        sak::logWarning("Timed out parsing WiFi profile: {}", profile_name.toStdString());
        return {};
    }
    const QString detail = result.std_out;

    QString password;
    QString security = "WPA/WPA2/WPA3";
    bool hidden = false;

    const QRegularExpression key_re(R"(Key Content\s*:\s+(.+))",
                                    QRegularExpression::CaseInsensitiveOption);
    const auto key_match = key_re.match(detail);
    if (key_match.hasMatch()) {
        password = key_match.captured(1).trimmed();
    }

    const QRegularExpression auth_re(R"(Authentication\s*:\s+(.+))",
                                     QRegularExpression::CaseInsensitiveOption);
    const auto auth_match = auth_re.match(detail);
    if (auth_match.hasMatch()) {
        const QString auth = auth_match.captured(1).trimmed().toUpper();
        if (auth.contains("WEP")) {
            security = "WEP";
        } else if (auth == "OPEN" || auth.contains("NONE")) {
            security = "None (Open)";
        }
    }

    const QRegularExpression non_bc_re(R"(Network broadcast\s*:\s+(.+))",
                                       QRegularExpression::CaseInsensitiveOption);
    const auto nb_match = non_bc_re.match(detail);
    if (nb_match.hasMatch()) {
        hidden =
            nb_match.captured(1).trimmed().compare("Don't broadcast", Qt::CaseInsensitive) == 0 ||
            nb_match.captured(1).trimmed().compare("Not broadcasting", Qt::CaseInsensitive) == 0;
    }

    WifiConfig cfg;
    cfg.location = {};
    cfg.ssid = profile_name;
    cfg.password = password;
    cfg.security = security;
    cfg.hidden = hidden;
    return cfg;
}

void WifiManagerPanel::onSelectionChanged() {
    Q_ASSERT(m_save_table_btn);
    Q_ASSERT(m_network_table);
    const int total = m_network_table->rowCount();
    int checked = 0;
    for (int row_index = 0; row_index < total; ++row_index) {
        auto* item = m_network_table->item(row_index, kColSelect);
        if ((item != nullptr) && item->checkState() == Qt::Checked) {
            ++checked;
        }
    }

    // Update header checkbox tri-state
    auto* check_hdr = qobject_cast<CheckHeaderView*>(m_network_table->horizontalHeader());
    if (check_hdr != nullptr) {
        if (total == 0 || checked == 0) {
            check_hdr->setTriState(Qt::Unchecked);
        } else if (checked == total) {
            check_hdr->setTriState(Qt::Checked);
        } else {
            check_hdr->setTriState(Qt::PartiallyChecked);
        }
    }

    if (total == 0) {
        m_save_table_btn->setEnabled(false);
        m_save_table_btn->setText("Save\u2026");
        m_add_to_windows_btn->setEnabled(false);
    } else if (checked > 0) {
        m_save_table_btn->setEnabled(true);
        m_save_table_btn->setText(QString("Save Checked (%1)\u2026").arg(checked));
        m_add_to_windows_btn->setEnabled(true);
    } else {
        m_save_table_btn->setEnabled(true);
        m_save_table_btn->setText("Save\u2026");
        m_add_to_windows_btn->setEnabled(false);
    }

    // Update multi-modal button tooltips to indicate source
    if (checked > 0) {
        const QString src = QString("checked %1 network(s)").arg(checked);
        m_generate_qr_btn->setToolTip(QString("Export QR code(s) for %1").arg(src));
        m_export_script_btn->setToolTip(QString("Generate Windows script(s) for %1").arg(src));
        m_export_macos_btn->setToolTip(QString("Generate macOS profile for %1").arg(src));
        m_connect_phone_btn->setToolTip(QString("Show QR dialog for %1").arg(src));
    } else {
        m_generate_qr_btn->setToolTip(
            "Export the current network form as a"
            " QR code image (PNG, PDF, JPG, BMP)");
        m_export_script_btn->setToolTip(
            "Generate a Windows netsh .cmd script"
            " for the current network form");
        m_export_macos_btn->setToolTip(
            "Generate a macOS WiFi .mobileconfig"
            " profile for the current network form");
        m_connect_phone_btn->setToolTip(
            "Show a QR code of the current network"
            " form for phone/tablet scanning");
    }
}

void WifiManagerPanel::onTableItemChanged(QTableWidgetItem* item) {
    if ((item != nullptr) && item->column() == kColSelect) {
        onSelectionChanged();
    }
}

// onSelectAllClicked and onSelectNoneClicked removed  --  replaced by CheckHeaderView

void WifiManagerPanel::onAddToWindowsClicked() {
    Q_ASSERT(m_network_table);
#ifndef Q_OS_WIN
    Q_EMIT statusMessage("Add to Windows is only supported on Windows.", sak::kTimerStatusWarnMs);
    return;
#else
    const QList<int> checked_rows = checkedWifiRows();
    if (checked_rows.isEmpty()) {
        Q_EMIT statusMessage("Check at least one network row first.", sak::kTimerStatusMessageMs);
        return;
    }

    const QList<WifiConfig> configs = configsFromRows(checked_rows);
    if (configs.isEmpty()) {
        Q_EMIT statusMessage("Checked rows do not contain any SSIDs.", sak::kTimerStatusMessageMs);
        return;
    }

    startAddToWindowsProfiles(configs);
#endif
}

QList<int> WifiManagerPanel::checkedWifiRows() const {
    QList<int> checked_rows;
    for (int row_index = 0; row_index < m_network_table->rowCount(); ++row_index) {
        auto* item = m_network_table->item(row_index, kColSelect);
        if ((item != nullptr) && item->checkState() == Qt::Checked) {
            checked_rows.append(row_index);
        }
    }
    return checked_rows;
}

QList<WifiManagerPanel::WifiConfig> WifiManagerPanel::configsFromRows(
    const QList<int>& rows) const {
    QList<WifiConfig> configs;
    configs.reserve(rows.size());
    for (const int row : rows) {
        const WifiConfig cfg = configFromRow(row);
        if (!cfg.ssid.isEmpty()) {
            configs.append(cfg);
        }
    }
    return configs;
}

void WifiManagerPanel::startAddToWindowsProfiles(const QList<WifiConfig>& configs) {
    // Refuse a second concurrent install: m_wlanInstallFuture holds a SINGLE future, and both tasks
    // would capture &m_wlanInstallCancel -- overwriting it would leave the older task tracked by
    // nothing, so the destructor could not join it and it could deref the destroyed atomic. One
    // install at a time (the button is also disabled during a run; this guards a re-entrant path).
    if (m_wlanInstallFuture.isRunning()) {
        Q_EMIT statusMessage("A WiFi profile install is already in progress.", 0);
        return;
    }
    m_add_to_windows_btn->setEnabled(false);
    Q_EMIT statusMessage("Adding selected network(s) to Windows WiFi profiles...", 0);

    auto* watcher = new QFutureWatcher<QPair<int, int>>(this);
    const QPointer<WifiManagerPanel> panel(this);
    connect(watcher, &QFutureWatcher<QPair<int, int>>::finished, this, [watcher, panel]() {
        watcher->deleteLater();
        if (!panel) {
            return;
        }
        panel->m_add_to_windows_btn->setEnabled(true);
        const auto [added, failed] = watcher->result();
        if (failed == 0) {
            Q_EMIT panel->statusMessage(
                QString("Added %1 network(s) to Windows WiFi profiles.").arg(added),
                sak::kTimerStatusDefaultMs);
        } else {
            Q_EMIT panel->statusMessage(QString("Added %1 network(s); %2 failed"
                                                " (try running as Administrator).")
                                            .arg(added)
                                            .arg(failed),
                                        sak::kTimerStatusLongMs);
        }
    });

    m_wlanInstallCancel.store(false);
    const std::atomic<bool>* cancel = &m_wlanInstallCancel;
    m_wlanInstallFuture = QtConcurrent::run(
        [configs, cancel]() { return WifiManagerPanel::installWlanProfiles(configs, cancel); });
    watcher->setFuture(m_wlanInstallFuture);
}

QPair<int, int> WifiManagerPanel::installWlanProfiles(const QList<WifiConfig>& configs,
                                                      const std::atomic<bool>* cancel) {
    int added = 0;
    int failed = 0;
    for (int index = 0; index < configs.size(); ++index) {
        // Cooperative cancellation: stop between profiles so teardown does not wait for every
        // remaining netsh call. Already-installed profiles stay (a partial install is honest).
        if ((cancel != nullptr) && cancel->load()) {
            break;
        }
        const QString xml = WifiManagerPanel::buildWlanProfileXml(configs.at(index));
        // Empty XML means the security type was refused (unsupported/unknown): count it as a
        // failure rather than installing a downgraded profile.
        if (!xml.isEmpty() && WifiManagerPanel::installWlanProfile(xml, index)) {
            ++added;
        } else {
            ++failed;
        }
    }
    return QPair<int, int>{added, failed};
}

std::pair<QString, QString> WifiManagerPanel::wlanAuthEncForSecurity(const QString& security) {
    const QString upper = security.toUpper();
    // Enterprise (802.1X/EAP) profiles need EAP configuration this PSK/open builder cannot
    // produce; refuse rather than emit a broken or downgraded profile.
    if (upper.contains(QStringLiteral("ENTERPRISE"))) {
        return {};
    }
    if (upper.contains(QStringLiteral("WEP"))) {
        return {QStringLiteral("open"), QStringLiteral("WEP")};
    }
    if (upper.contains(QStringLiteral("NONE")) || upper.contains(QStringLiteral("OPEN"))) {
        return {QStringLiteral("open"), QStringLiteral("none")};
    }
    // WPA3-Personal uses SAE: map it to WPA3SAE rather than silently downgrading it to WPA2-PSK.
    if (upper.contains(QStringLiteral("WPA3")) || upper.contains(QStringLiteral("SAE"))) {
        return {QStringLiteral("WPA3SAE"), QStringLiteral("AES")};
    }
    if (upper.contains(QStringLiteral("WPA"))) {  // WPA/WPA2 Personal (PSK)
        return {QStringLiteral("WPA2PSK"), QStringLiteral("AES")};
    }
    return {};  // unknown / malformed -> refuse (fail closed)
}

QString WifiManagerPanel::buildWlanProfileXml(const WifiConfig& cfg) {
    Q_ASSERT(!cfg.ssid.isEmpty());
    const auto [authType, encType] = wlanAuthEncForSecurity(cfg.security);
    // Empty authType == unsupported/unknown security: return an empty document so the caller
    // refuses the install instead of writing a downgraded or malformed profile.
    if (authType.isEmpty()) {
        return {};
    }

    QString xml;
    xml += "<?xml version=\"1.0\"?>\r\n";
    xml += "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\r\n";
    xml += "  <name>" + cfg.ssid.toHtmlEscaped() + "</name>\r\n";
    xml += "  <SSIDConfig>\r\n";
    xml += "    <SSID><name>" + cfg.ssid.toHtmlEscaped() + "</name></SSID>\r\n";
    xml += QString("    <nonBroadcast>%1</nonBroadcast>\r\n").arg(cfg.hidden ? "true" : "false");
    xml += "  </SSIDConfig>\r\n";
    xml += "  <connectionType>ESS</connectionType>\r\n";
    xml += "  <connectionMode>auto</connectionMode>\r\n";
    xml += "  <MSM><security><authEncryption>\r\n";
    xml += "    <authentication>" + authType + "</authentication>\r\n";
    xml += "    <encryption>" + encType + "</encryption>\r\n";
    xml += "    <useOneX>false</useOneX>\r\n";
    xml += "  </authEncryption>\r\n";
    // WEP maps to auth "open" but still carries a key: emit it as <networkKey> so a WEP key is NOT
    // silently discarded (a keyless WEP profile cannot connect). WPA keys stay passPhrase; a
    // genuinely open network (no key) emits no <sharedKey>.
    const bool is_wep = encType == QLatin1String("WEP");
    if (!cfg.password.isEmpty() && (authType != QLatin1String("open") || is_wep)) {
        xml += "  <sharedKey>\r\n";
        xml += is_wep ? "    <keyType>networkKey</keyType>\r\n"
                      : "    <keyType>passPhrase</keyType>\r\n";
        xml += "    <protected>false</protected>\r\n";
        xml += "    <keyMaterial>" + cfg.password.toHtmlEscaped() + "</keyMaterial>\r\n";
        xml += "  </sharedKey>\r\n";
    }
    xml += "  </security></MSM>\r\n";
    xml += "</WLANProfile>\r\n";
    return xml;
}

bool WifiManagerPanel::installWlanProfile(const QString& xml, int row) {
    Q_ASSERT(row >= 0);
    Q_ASSERT(!xml.isEmpty());
    (void)row;

    // Adding a WLAN profile is a privileged mutation: resolve the System32 netsh and
    // refuse outright when it cannot be resolved rather than let CreateProcess search.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        sak::logError("Cannot resolve the System32 netsh.exe path; WLAN profile install refused");
        return false;
    }

    // Use QTemporaryFile with .xml suffix for secure unique temp file
    QTemporaryFile tmp_file(QDir::tempPath() + QStringLiteral("/sak_wifi_XXXXXX.xml"));
    tmp_file.setAutoRemove(true);
    if (!tmp_file.open()) {
        return false;
    }
    {
        QTextStream ts(&tmp_file);
        ts.setEncoding(QStringConverter::Utf8);
        ts << xml;
    }
    tmp_file.close();

    const QString tmp_path = tmp_file.fileName();

    const auto result = sak::runProcess(netsh_exe,
                                        {QStringLiteral("wlan"),
                                         QStringLiteral("add"),
                                         QStringLiteral("profile"),
                                         QStringLiteral("filename=") + tmp_path,
                                         QStringLiteral("user=all")},
                                        sak::kTimerNetshWaitMs);
    if (result.timed_out) {
        sak::logError("Timed out installing WLAN profile");
        return false;
    }
    return result.succeeded();
}

// -----------------------------------------------------------------------------
// WiFi payload helpers
// -----------------------------------------------------------------------------
// static
QString WifiManagerPanel::escapeWifiField(const QString& value) {
    QString result;
    result.reserve(value.size() * kEscapedTextReserveMultiplier);
    for (const QChar c : value) {
        if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':') {
            result += '\\';
        }
        result += c;
    }
    return result;
}

// static
QString WifiManagerPanel::normalizeSecurityForQr(const QString& security) {
    const QString upper = security.toUpper().trimmed();
    if (upper == "WEP") {
        return "WEP";
    }
    if (upper.contains("NONE") || upper.contains("OPEN") || upper == "NO PASSWORD") {
        return "nopass";
    }
    return "WPA";
}

QString WifiManagerPanel::buildWifiPayload() const {
    Q_ASSERT(m_ssid_input);
    Q_ASSERT(m_security_combo);
    const QString ssid = m_ssid_input->text().trimmed();
    if (ssid.isEmpty()) {
        return {};
    }

    const QString sec = normalizeSecurityForQr(m_security_combo->currentText());
    const bool hidden = m_hidden_checkbox->isChecked();

    QString payload = "WIFI:T:" + sec + ";";
    payload += "S:" + escapeWifiField(ssid) + ";";
    if (sec != "nopass") {
        const QString pass = m_password_input->text();
        payload += pass.isEmpty() ? "P:;" : "P:" + escapeWifiField(pass) + ";";
    }
    payload += QString("H:%1;;").arg(hidden ? "true" : "false");
    return payload;
}

// static
QString WifiManagerPanel::buildWifiPayloadFromConfig(const WifiConfig& cfg) {
    if (cfg.ssid.isEmpty()) {
        return {};
    }
    const QString sec = normalizeSecurityForQr(cfg.security);
    QString payload = "WIFI:T:" + sec + ";";
    payload += "S:" + escapeWifiField(cfg.ssid) + ";";
    if (sec != "nopass") {
        payload += cfg.password.isEmpty() ? "P:;" : "P:" + escapeWifiField(cfg.password) + ";";
    }
    payload += QString("H:%1;;").arg(cfg.hidden ? "true" : "false");
    return payload;
}

// -----------------------------------------------------------------------------
// QR generation
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// QR drawing helper (extracted to keep generateQrImage nesting = 3)
// -----------------------------------------------------------------------------
static void drawQrModules(QImage& out, const QString& payload, int image_size) {
    Q_ASSERT(!payload.isEmpty());
    Q_ASSERT(image_size >= 0);
    constexpr int kBorder = 4;
    // HIGH ECC trades capacity for resilience -- critical because phone cameras
    // often scan QR codes at oblique angles or in poor lighting.
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(payload.toUtf8().constData(),
                                                               qrcodegen::QrCode::Ecc::HIGH);

    const int modules = qr.getSize();
    // The quiet zone (BORDER) around the QR is required by the spec so
    // scanners can reliably detect the code boundaries.
    const int total_modules = modules + (kBorder * 2);
    const double cell_size = static_cast<double>(image_size) / total_modules;

    QPainter painter(&out);
    // Antialiasing must be OFF -- sub-pixel blending produces grey edges
    // that confuse QR decoders.
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::black);
    painter.setPen(Qt::NoPen);

    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qr.getModule(x, y)) {
                continue;
            }
            // Compute each cell's pixel rect from (x+1)*cellSize - x*cellSize
            // rather than using a fixed integer cell width; this prevents
            // cumulative rounding drift that would create visible gaps.
            const int px = static_cast<int>((x + kBorder) * cell_size);
            const int py = static_cast<int>((y + kBorder) * cell_size);
            const int pw = static_cast<int>((x + kBorder + 1) * cell_size) - px;
            const int ph = static_cast<int>((y + kBorder + 1) * cell_size) - py;
            painter.drawRect(px, py, pw, ph);
        }
    }
}

// static
QImage WifiManagerPanel::generateQrImage(const QString& payload) {
    constexpr int kImageSize = 640;

    QImage out(kImageSize, kImageSize, QImage::Format_RGB32);
    out.fill(Qt::white);
    if (payload.isEmpty()) {
        return out;
    }

    try {
        drawQrModules(out, payload, kImageSize);
    } catch (const std::exception& ex) {
        sak::logWarning("QR code generation failed: {}", ex.what());
        QPainter p(&out);
        p.setPen(Qt::red);
        p.drawText(out.rect(), Qt::AlignCenter, "QR Error");
    }
    return out;
}

// -----------------------------------------------------------------------------
// Export script builders
// -----------------------------------------------------------------------------

// static
QString WifiManagerPanel::buildWindowsScript(const QString& ssid,
                                             const QString& password,
                                             const QString& security,
                                             bool hidden) {
    // Delegates to the shared, injection-hardened core builder (sak::buildWifiSetupScriptWindows),
    // which also backs the headless network.generate_wifi_setup_script tool. Returns empty for an
    // empty/unsafe SSID; callers treat empty as an error.
    return sak::buildWifiSetupScriptWindows(ssid, password, security, hidden);
}

// static
QString WifiManagerPanel::buildMacosProfile(const QList<WifiConfig>& networks) {
    const QString profile_uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    const QString payload_id = "com.sak.wifi." + profile_uuid.left(8).toLower();
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QString plist;
    plist += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    plist +=
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist += "<plist version=\"1.0\">\n<dict>\n";
    plist += "  <key>PayloadContent</key>\n  <array>\n";

    for (const auto& cfg : networks) {
        const QString net_uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
        const QString upper = cfg.security.toUpper();
        QString enc_type;
        if (upper.contains("WEP")) {
            enc_type = "WEP";
        } else if (upper.contains("NONE") || upper.contains("OPEN")) {
            enc_type = "None";
        } else {
            enc_type = "WPA";
        }

        plist += "    <dict>\n";
        plist += "      <key>AutoJoin</key><true/>\n";
        plist += "      <key>EncryptionType</key><string>" + enc_type + "</string>\n";
        plist += "      <key>HIDDEN_NETWORK</key>";
        plist += cfg.hidden ? "<true/>\n" : "<false/>\n";
        if (enc_type != "None" && !cfg.password.isEmpty()) {
            plist += "      <key>Password</key><string>" + cfg.password.toHtmlEscaped() +
                     "</"
                     "string>\n";
        }
        plist += "      <key>PayloadDisplayName</key><string>WiFi (" + cfg.ssid.toHtmlEscaped() +
                 ")</string>\n";
        plist += "      <key>PayloadIdentifier</key><string>com.sak.wifi." + net_uuid.toLower() +
                 "</string>\n";
        plist += "      <key>PayloadType</key><string>com.apple.wifi.managed</string>\n";
        plist += "      <key>PayloadUUID</key><string>" + net_uuid + "</string>\n";
        plist += "      <key>PayloadVersion</key><integer>1</integer>\n";
        plist += "      <key>SSID_STR</key><string>" + cfg.ssid.toHtmlEscaped() + "</string>\n";
        plist += "    </dict>\n";
    }

    plist += "  </array>\n";
    plist += "  <key>PayloadDescription</key><string>WiFi config by S.A.K. Utility on " + now +
             "</string>\n";
    const QString display_name = networks.isEmpty() ? "WiFi Networks" : networks.first().ssid;
    plist += "  <key>PayloadDisplayName</key><string>" + display_name.toHtmlEscaped() +
             " WiFi</"
             "string>\n";
    plist += "  <key>PayloadIdentifier</key><string>" + payload_id + "</string>\n";
    plist += "  <key>PayloadRemovalDisallowed</key><false/>\n";
    plist += "  <key>PayloadType</key><string>Configuration</string>\n";
    plist += "  <key>PayloadUUID</key><string>" + profile_uuid + "</string>\n";
    plist += "  <key>PayloadVersion</key><integer>1</integer>\n";
    plist += "</dict>\n</plist>\n";
    return plist;
}

// -----------------------------------------------------------------------------
// Table helpers
// -----------------------------------------------------------------------------
WifiManagerPanel::WifiConfig WifiManagerPanel::configFromForm() const {
    WifiConfig cfg;
    cfg.location = m_location_input->text().trimmed();
    cfg.ssid = m_ssid_input->text().trimmed();
    cfg.password = m_password_input->text();
    cfg.security = m_security_combo->currentText();
    cfg.hidden = m_hidden_checkbox->isChecked();
    return cfg;
}

void WifiManagerPanel::loadConfigToForm(const WifiConfig& cfg) {
    m_location_input->setText(cfg.location);
    m_ssid_input->setText(cfg.ssid);
    m_password_input->setText(cfg.password);
    m_hidden_checkbox->setChecked(cfg.hidden);
    const int idx = m_security_combo->findText(cfg.security);
    m_security_combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void WifiManagerPanel::addRowToTable(const WifiConfig& cfg) {
    Q_ASSERT(m_network_table);
    const int row = m_network_table->rowCount();
    m_network_table->insertRow(row);

    // Checkbox cell (COL_SELECT)
    auto* check_item = new QTableWidgetItem();
    check_item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
    check_item->setCheckState(Qt::Unchecked);
    m_network_table->setItem(row, kColSelect, check_item);

    m_network_table->setItem(row, kColLocation, new QTableWidgetItem(cfg.location));
    m_network_table->setItem(row, kColSsid, new QTableWidgetItem(cfg.ssid));

    // Password cell: store real value in item's UserRole; cell widget shows dots + eye toggle
    {
        const QString pwd = cfg.password;
        const QString dots = pwd.isEmpty() ? QString{}
                                           : QString(pwd.length(), QChar(kPasswordBulletCharacter));

        // Item holds the real password for configFromRow() retrieval
        auto* pw_item = new QTableWidgetItem(QString{});
        pw_item->setData(Qt::UserRole, pwd);
        m_network_table->setItem(row, kColPassword, pw_item);

        // Visual widget: [dots label] [eye button]
        auto* container = new QWidget;
        container->setProperty("wifi_password", pwd);
        auto* hbox = new QHBoxLayout(container);
        hbox->setContentsMargins(sak::ui::kSpacingTight,
                                 sak::ui::kMarginNone,
                                 sak::ui::kCssPaddingTinyPx,
                                 sak::ui::kMarginNone);
        hbox->setSpacing(sak::ui::kCssPaddingTinyPx);

        auto* lbl = new QLabel(dots, container);
        lbl->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

        auto* eye_btn = new QToolButton(container);
        eye_btn->setIcon(QIcon(ui::kIconPasswordEyeOpen));
        eye_btn->setIconSize(QSize(kPasswordRevealIconSize, kPasswordRevealIconSize));
        eye_btn->setCheckable(true);
        eye_btn->setFixedSize(sak::kEyeButtonSize, sak::kEyeButtonSize);
        eye_btn->setToolTip("Show/hide password");
        eye_btn->setAccessibleName(QStringLiteral("Toggle password visibility"));
        eye_btn->setStyleSheet(sak::ui::passwordRevealButtonStyle());
        eye_btn->setEnabled(!pwd.isEmpty());

        QObject::connect(eye_btn, &QToolButton::toggled, [lbl, eye_btn, pwd](bool showing) {
            lbl->setText(showing ? pwd
                                 : (pwd.isEmpty()
                                        ? QString{}
                                        : QString(pwd.length(), QChar(kPasswordBulletCharacter))));
            eye_btn->setIcon(
                QIcon(showing ? ui::kIconPasswordEyeClosed : ui::kIconPasswordEyeOpen));
        });

        hbox->addWidget(lbl, 1);
        hbox->addWidget(eye_btn);
        m_network_table->setCellWidget(row, kColPassword, container);
    }

    m_network_table->setItem(row, kColSecurity, new QTableWidgetItem(cfg.security));
    m_network_table->setItem(row, kColHidden, new QTableWidgetItem(cfg.hidden ? "Yes" : "No"));
    m_network_table->scrollToBottom();
    onSelectionChanged();
}

WifiManagerPanel::WifiConfig WifiManagerPanel::configFromRow(int row) const {
    Q_ASSERT(m_network_table);
    Q_ASSERT(row >= 0);
    auto text = [&](int col) -> QString {
        auto* item = m_network_table->item(row, col);
        return item ? item->text() : QString{};
    };
    WifiConfig cfg;
    cfg.location = text(kColLocation);
    cfg.ssid = text(kColSsid);
    // Real password is stored in UserRole (display shows dots)
    auto* pw_item = m_network_table->item(row, kColPassword);
    cfg.password = (pw_item != nullptr) ? pw_item->data(Qt::UserRole).toString() : QString{};
    cfg.security = text(kColSecurity);
    cfg.hidden = text(kColHidden).compare("Yes", Qt::CaseInsensitive) == 0;
    return cfg;
}

QList<WifiManagerPanel::WifiConfig> WifiManagerPanel::allConfigs() const {
    QList<WifiConfig> list;
    list.reserve(m_network_table->rowCount());
    for (int i = 0; i < m_network_table->rowCount(); ++i) {
        list.append(configFromRow(i));
    }
    return list;
}

QList<WifiManagerPanel::WifiConfig> WifiManagerPanel::checkedConfigs() const {
    QList<WifiConfig> list;
    for (int row_index = 0; row_index < m_network_table->rowCount(); ++row_index) {
        auto* item = m_network_table->item(row_index, kColSelect);
        if ((item != nullptr) && item->checkState() == Qt::Checked) {
            list.append(configFromRow(row_index));
        }
    }
    return list;
}

// -----------------------------------------------------------------------------
// Search
// -----------------------------------------------------------------------------
bool WifiManagerPanel::rowMatchesSearch(int row, const QString& text) const {
    for (int col = kColLocation; col < kColCount; ++col) {
        auto* item = m_network_table->item(row, col);
        if ((item != nullptr) && item->text().contains(text, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

void WifiManagerPanel::updateSearchMatches(const QString& text) {
    m_search_matches.clear();
    if (text.isEmpty()) {
        return;
    }
    for (int row = 0; row < m_network_table->rowCount(); ++row) {
        if (rowMatchesSearch(row, text)) {
            m_search_matches.append(row);
        }
    }
}

static void setRowBackground(QTableWidget* table, int row, const QBrush& brush) {
    for (int col = kColLocation; col < kColCount; ++col) {
        auto* item = table->item(row, col);
        if (item != nullptr) {
            item->setBackground(brush);
        }
    }
}

void WifiManagerPanel::highlightSearchMatches() {
    for (int row = 0; row < m_network_table->rowCount(); ++row) {
        setRowBackground(m_network_table, row, QBrush());
    }

    for (const int row : m_search_matches) {
        setRowBackground(m_network_table,
                         row,
                         QColor(QString::fromLatin1(ui::kColorWifiSearchMatchHighlight)));
    }
}

// -----------------------------------------------------------------------------
// Persistence
// -----------------------------------------------------------------------------
void WifiManagerPanel::saveTableToJson(const QString& path) {
    Q_ASSERT(m_network_table);
    Q_ASSERT(!path.isEmpty());
    QJsonArray arr;
    for (int i = 0; i < m_network_table->rowCount(); ++i) {
        const WifiConfig cfg = configFromRow(i);
        QJsonObject obj;
        obj["location"] = cfg.location;
        obj["ssid"] = cfg.ssid;
        obj["password"] = cfg.password;
        obj["security"] = cfg.security;
        obj["hidden"] = cfg.hidden;
        arr.append(obj);
    }
    const QJsonDocument doc(arr);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        sak::logWarning(("Could not open file for writing: " + path).toStdString());
        sak::showWarningLogged(this, "Save Error", "Could not open file for writing:\n" + path);
        return;
    }
    const QByteArray json_bytes = doc.toJson();
    const qint64 written = f.write(json_bytes);
    // Fail closed: a short write or a failed atomic commit must leave the
    // original credential table untouched and must NOT report success. The
    // '&&' short-circuit means commit() is only attempted after a full write;
    // otherwise the temp file is discarded via cancelWriting().
    const bool committed = (written == json_bytes.size()) && f.commit();
    if (!jsonWriteSucceeded(written, json_bytes.size(), committed)) {
        f.cancelWriting();
        sak::logWarning("Incomplete write to network table file: {}", path.toStdString());
        sak::showWarningLogged(this,
                               "Save Error",
                               "Failed to write the network table (file left unchanged):\n" + path);
        return;
    }
    Q_EMIT statusMessage(QString("Saved %1 network(s) to %2").arg(arr.size()).arg(path),
                         sak::kTimerStatusDefaultMs);
}

bool WifiManagerPanel::jsonWriteSucceeded(qint64 written, qint64 expected, bool committed) {
    return written == expected && committed;
}

bool WifiManagerPanel::flushTextStreamOk(QTextStream& stream, const QFileDevice& file) {
    // Fail-closed finalize: flush the buffered text and verify neither the stream nor its file
    // device reported an error (disk full / IO failure). Callers must not report success on false.
    stream.flush();
    return stream.status() == QTextStream::Ok && file.error() == QFileDevice::NoError;
}

void WifiManagerPanel::loadTableFromJson(const QString& path) {
    Q_ASSERT(m_network_table);
    Q_ASSERT(!path.isEmpty());
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        sak::logWarning(("Could not open file: " + path).toStdString());
        sak::showWarningLogged(this, "Load Error", "Could not open file:\n" + path);
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) {
        sak::logWarning("Load Error: Invalid JSON format (expected array).");
        sak::showWarningLogged(this, "Load Error", "Invalid JSON format (expected array).");
        return;
    }
    m_network_table->setRowCount(0);
    for (const QJsonValue& val : doc.array()) {
        const QJsonObject obj = val.toObject();
        WifiConfig cfg;
        cfg.location = obj["location"].toString();
        cfg.ssid = obj["ssid"].toString();
        cfg.password = obj["password"].toString();
        cfg.security = obj["security"].toString();
        cfg.hidden = obj["hidden"].toBool();
        addRowToTable(cfg);
    }
    Q_EMIT statusMessage(
        QString("Loaded %1 network(s) from %2").arg(m_network_table->rowCount()).arg(path),
        sak::kTimerStatusDefaultMs);
    onSelectionChanged();
}

}  // namespace sak

// Required for Q_OBJECT classes defined in .cpp files
#include "wifi_manager_panel.moc"
