// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_manager_panel.cpp
/// @brief Implements the Wi-Fi network manager panel UI with QR code generation

#include "sak/wifi_manager_panel.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "qrcodegen.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QScrollArea>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QToolButton>
#include <QMouseEvent>
#include <QProcess>
#include <QStyleOptionButton>
#include <QUrl>
#include <QIcon>
#include <QUuid>
#include <QVBoxLayout>
#include "sak/detachable_log_window.h"

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// CheckHeaderView â€” column 0 renders a tri-state "select all" checkbox
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
namespace {
class CheckHeaderView : public QHeaderView {
    Q_OBJECT
public:
    explicit CheckHeaderView(QWidget* parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent), m_state(Qt::Unchecked)
    {
        setSectionsClickable(true);
    }

    Qt::CheckState triState() const { return m_state; }

    void setTriState(Qt::CheckState state) {
        if (m_state == state) return;
        m_state = state;
        viewport()->update();
    }

Q_SIGNALS:
    void checkToggled(bool allChecked);

protected:
    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override {
        // Paint default section background first
        painter->save();
        QHeaderView::paintSection(painter, rect, logicalIndex);
        painter->restore();
        if (logicalIndex == 0) {
            // Draw a custom checkbox that matches the table indicator stylesheet:
            // unchecked:        #f8fafc bg, #94a3b8 border, 4px radius
            // checked:          #3b82f6 bg, #2563eb border + white tick
            // partially-checked:#3b82f6 bg, #2563eb border + white dash
            constexpr int side = 16;
            const int cx = rect.x() + (rect.width()  - side) / 2;
            const int cy = rect.y() + (rect.height() - side) / 2;
            const QRect cbRect(cx, cy, side, side);

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            if (m_state == Qt::Unchecked) {
                painter->setBrush(QColor(0xf8, 0xfa, 0xfc));
                painter->setPen(QPen(QColor(0x94, 0xa3, 0xb8), 1));
                painter->drawRoundedRect(cbRect, 4, 4);
            } else {
                // Checked or PartiallyChecked â€” blue fill
                painter->setBrush(QColor(0x3b, 0x82, 0xf6));
                painter->setPen(QPen(QColor(0x25, 0x63, 0xeb), 1));
                painter->drawRoundedRect(cbRect, 4, 4);
                painter->setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->setBrush(Qt::NoBrush);
                if (m_state == Qt::Checked) {
                    // White tick mark
                    QPainterPath tick;
                    tick.moveTo(cx + 3,  cy + 8);
                    tick.lineTo(cx + 6,  cy + 11);
                    tick.lineTo(cx + 13, cy + 4);
                    painter->drawPath(tick);
                } else {
                    // White horizontal dash for partial
                    painter->drawLine(cx + 4, cy + 8, cx + 12, cy + 8);
                }
            }
            painter->restore();
        }
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
} // anonymous namespace

namespace sak {

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Table column indices
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static constexpr int COL_SELECT   = 0;  // checkbox
static constexpr int COL_LOCATION = 1;
static constexpr int COL_SSID     = 2;
static constexpr int COL_PASSWORD = 3;
static constexpr int COL_SECURITY = 4;
static constexpr int COL_HIDDEN   = 5;
static constexpr int COL_COUNT    = 6;

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Construction / destruction
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
WifiManagerPanel::WifiManagerPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    connectSignals();
}

WifiManagerPanel::~WifiManagerPanel() = default;

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// UI setup
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void WifiManagerPanel::setupUi()
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* rootLayout = new QVBoxLayout(contentWidget);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    scrollArea->setWidget(contentWidget);
    outerLayout->addWidget(scrollArea);

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(contentWidget, tr("WiFi Manager"),
        tr("Manage, share, and deploy Wi-Fi network profiles"), rootLayout);

    // Main content: horizontal splitter form | table
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    setupFormGroup();
    setupTableGroup();

    splitter->addWidget(m_form_group);
    splitter->addWidget(m_table_group);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    rootLayout->addWidget(splitter, 1);

    setupActionButtons();

    // A11Y-07: explicit tab order for keyboard navigation
    setTabOrder(m_ssid_input,            m_location_input);
    setTabOrder(m_location_input,        m_password_input);
    setTabOrder(m_password_input,        m_password_toggle_btn);
    setTabOrder(m_password_toggle_btn,   m_security_combo);
    setTabOrder(m_security_combo,        m_hidden_checkbox);
    setTabOrder(m_hidden_checkbox,       m_connect_phone_btn);
    setTabOrder(m_connect_phone_btn,     m_add_table_btn);
    setTabOrder(m_add_table_btn,         m_search_input);
    setTabOrder(m_search_input,          m_search_up_btn);
    setTabOrder(m_search_up_btn,         m_search_down_btn);
    setTabOrder(m_search_down_btn,       m_network_table);
    setTabOrder(m_network_table,         m_delete_selected_btn);
    setTabOrder(m_delete_selected_btn,   m_add_to_windows_btn);
    setTabOrder(m_add_to_windows_btn,    m_save_table_btn);
    setTabOrder(m_save_table_btn,        m_load_table_btn);
    setTabOrder(m_load_table_btn,        m_generate_qr_btn);
    setTabOrder(m_generate_qr_btn,       m_export_script_btn);
    setTabOrder(m_export_script_btn,     m_export_macos_btn);
    setTabOrder(m_export_macos_btn,      m_scan_networks_btn);
}

void WifiManagerPanel::setupFormGroup()
{
    m_form_group = new QGroupBox("Network Details", this);
    auto* layout = new QFormLayout(m_form_group);
    layout->setSpacing(8);
    layout->setContentsMargins(10, 14, 10, 10);

    // Location
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

    // Password with show/hide toggle
    auto* passRow = new QHBoxLayout();
    m_password_input = new QLineEdit(m_form_group);
    m_password_input->setPlaceholderText("Password");
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setAccessibleName(QStringLiteral("Network Password"));
    m_password_toggle_btn = new QToolButton(m_form_group);
    m_password_toggle_btn->setCheckable(true);
    m_password_toggle_btn->setIcon(QIcon(":/icons/eye_open.svg"));
    m_password_toggle_btn->setIconSize(QSize(16, 16));
    m_password_toggle_btn->setToolTip("Show/hide password");
    m_password_toggle_btn->setAccessibleName(QStringLiteral("Toggle password visibility"));
    passRow->addWidget(m_password_input);
    passRow->addWidget(m_password_toggle_btn);
    auto* passWidget = new QWidget(m_form_group);
    passWidget->setLayout(passRow);
    layout->addRow("Password:", passWidget);

    // Security type
    m_security_combo = new QComboBox(m_form_group);
    m_security_combo->addItems({"WPA/WPA2/WPA3", "WEP", "None (Open)"});
    m_security_combo->setToolTip("WiFi security type");
    m_security_combo->setAccessibleName(QStringLiteral("Security Type"));
    layout->addRow("Security:", m_security_combo);

    // Hidden network
    m_hidden_checkbox = new QCheckBox("This network is hidden (not broadcasting SSID)", m_form_group);
    m_hidden_checkbox->setAccessibleName(QStringLiteral("Hidden Network"));
    layout->addRow("", m_hidden_checkbox);

    // Action buttons at the bottom of the form â€” side by side
    m_connect_phone_btn = new QPushButton("Connect with Phone/Tablet", m_form_group);
    m_connect_phone_btn->setToolTip("Show a QR code of the current network for phone/tablet scanning");
    m_connect_phone_btn->setAccessibleName(QStringLiteral("Connect with Phone"));
    m_add_table_btn = new QPushButton("Add to Table", m_form_group);
    m_add_table_btn->setToolTip("Add current form entry to the saved networks table");
    m_add_table_btn->setAccessibleName(QStringLiteral("Add to Table"));
    auto* formBtnRow = new QHBoxLayout();
    formBtnRow->setContentsMargins(0, 10, 0, 0);  // 10px top buffer
    formBtnRow->addWidget(m_connect_phone_btn);
    formBtnRow->addWidget(m_add_table_btn);
    auto* formBtnWidget = new QWidget(m_form_group);
    formBtnWidget->setLayout(formBtnRow);
    layout->addRow("", formBtnWidget);
}

void WifiManagerPanel::setupTableGroup()
{
    m_table_group = new QGroupBox("Saved Networks", this);
    auto* layout = new QVBoxLayout(m_table_group);
    layout->setContentsMargins(8, 12, 8, 8);
    layout->setSpacing(6);

    setupTableSearchRow(layout);
    setupNetworkTable(layout);
    setupTableActionButtons(layout);
}

void WifiManagerPanel::setupTableSearchRow(QVBoxLayout* layout)
{
    auto* searchRow   = new QHBoxLayout();
    m_search_input    = new QLineEdit(m_table_group);
    m_search_input->setPlaceholderText("Search networks\u2026");
    m_search_input->setAccessibleName(QStringLiteral("Search Networks"));
    m_search_up_btn   = new QToolButton(m_table_group);
    m_search_up_btn->setText("\u25b2");
    m_search_up_btn->setToolTip("Previous match");
    m_search_up_btn->setAccessibleName(QStringLiteral("Previous search match"));
    m_search_up_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_search_down_btn = new QToolButton(m_table_group);
    m_search_down_btn->setText("\u25bc");
    m_search_down_btn->setToolTip("Next match");
    m_search_down_btn->setAccessibleName(QStringLiteral("Next search match"));
    m_search_down_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    searchRow->addWidget(m_search_input, 1);
    searchRow->addWidget(m_search_up_btn);
    searchRow->addWidget(m_search_down_btn);
    layout->addLayout(searchRow);
}

void WifiManagerPanel::setupNetworkTable(QVBoxLayout* layout)
{
    m_network_table = new QTableWidget(0, COL_COUNT, m_table_group);
    m_network_table->setAccessibleName(QStringLiteral("Saved WiFi Networks Table"));
    auto* checkHeader = new CheckHeaderView(m_table_group);
    m_network_table->setHorizontalHeader(checkHeader);
    m_network_table->setHorizontalHeaderLabels({"", "Location", "SSID", "Password", "Security", "Hidden"});
    m_network_table->horizontalHeader()->setStretchLastSection(false);
    m_network_table->horizontalHeader()->setSectionResizeMode(COL_SELECT,   QHeaderView::Fixed);
    m_network_table->horizontalHeader()->setSectionResizeMode(COL_LOCATION, QHeaderView::Stretch);
    m_network_table->horizontalHeader()->setSectionResizeMode(COL_SSID,     QHeaderView::Stretch);
    m_network_table->horizontalHeader()->setSectionResizeMode(COL_PASSWORD, QHeaderView::Stretch);
    m_network_table->horizontalHeader()->setSectionResizeMode(COL_SECURITY, QHeaderView::ResizeToContents);
    m_network_table->horizontalHeader()->setSectionResizeMode(COL_HIDDEN,   QHeaderView::ResizeToContents);
    m_network_table->setColumnWidth(COL_SELECT, 36);
    m_network_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_network_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_network_table->setAlternatingRowColors(true);
    m_network_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_network_table->setToolTip("Double-click a row to load it into the form");
    m_network_table->setStyleSheet(
        QString("QTableWidget::indicator { width: 16px; height: 16px; border: 1px solid %1; border-radius: 4px; background: %2; }"
                "QTableWidget::indicator:checked { background: %3; border: 1px solid %4; }"
                "QTableWidget::indicator:unchecked { background: %2; border: 1px solid %1; }")
            .arg(sak::ui::kColorBorderMuted)
            .arg(sak::ui::kColorBgSurface)
            .arg(sak::ui::kColorPrimary)
            .arg(sak::ui::kColorPrimaryDark)
    );
    layout->addWidget(m_network_table, 1);
}

void WifiManagerPanel::setupTableActionButtons(QVBoxLayout* layout)
{
    auto* tableActions    = new QHBoxLayout();
    m_delete_selected_btn = new QPushButton("Delete Selected", m_table_group);
    m_delete_selected_btn->setAccessibleName(QStringLiteral("Delete Selected Networks"));
    m_add_to_windows_btn  = new QPushButton("Add Selected to Windows", m_table_group);
    m_add_to_windows_btn->setAccessibleName(QStringLiteral("Add Selected to Windows"));
    m_save_table_btn      = new QPushButton("Save\u2026",      m_table_group);
    m_save_table_btn->setAccessibleName(QStringLiteral("Save Networks to File"));
    m_load_table_btn      = new QPushButton("Load\u2026",      m_table_group);
    m_load_table_btn->setAccessibleName(QStringLiteral("Load Networks from File"));
    m_delete_selected_btn->setToolTip("Remove the selected row(s) from the table");
    m_add_to_windows_btn->setToolTip("Add checked networks to Windows known WiFi profiles via netsh");
    m_add_to_windows_btn->setEnabled(false);
    m_save_table_btn->setToolTip("Save table to a JSON file");
    m_save_table_btn->setEnabled(false);
    m_load_table_btn->setToolTip("Load table from a JSON file");
    tableActions->addWidget(m_delete_selected_btn);
    tableActions->addWidget(m_add_to_windows_btn);
    tableActions->addStretch();
    tableActions->addWidget(m_save_table_btn);
    tableActions->addWidget(m_load_table_btn);
    layout->addLayout(tableActions);
}

void WifiManagerPanel::setupActionButtons()
{
    auto* bar = new QHBoxLayout();

    m_generate_qr_btn   = new QPushButton("Generate QR Code",      this);
    m_generate_qr_btn->setAccessibleName(QStringLiteral("Generate QR Code"));
    m_export_script_btn = new QPushButton("Generate Windows Script", this);
    m_export_script_btn->setAccessibleName(QStringLiteral("Generate Windows Script"));
    m_export_macos_btn  = new QPushButton("Generate macOS Profile",  this);
    m_export_macos_btn->setAccessibleName(QStringLiteral("Generate macOS Profile"));
    m_scan_networks_btn = new QPushButton("Scan Known Networks",     this);
    m_scan_networks_btn->setAccessibleName(QStringLiteral("Scan Known Networks"));

    m_generate_qr_btn->setToolTip("Export the current network as a QR code image (PNG, PDF, JPG, BMP)");
    m_export_script_btn->setToolTip("Generate a Windows netsh .cmd script for the current network");
    m_export_macos_btn->setToolTip("Generate a macOS WiFi .mobileconfig profile for the current network");
    m_scan_networks_btn->setToolTip("Scan Windows known WiFi profiles and add them to the table");

    bar->addWidget(m_generate_qr_btn);
    bar->addWidget(m_export_script_btn);
    bar->addWidget(m_export_macos_btn);
    bar->addStretch();
    bar->addWidget(m_scan_networks_btn);

    auto* barWidget = new QWidget(this);
    barWidget->setLayout(bar);
    qobject_cast<QVBoxLayout*>(layout())->addWidget(barWidget);
}

void WifiManagerPanel::connectSignals()
{
    connect(m_security_combo,      &QComboBox::currentTextChanged,    this, &WifiManagerPanel::onSecurityChanged);
    connect(m_password_toggle_btn, &QToolButton::toggled,             this, &WifiManagerPanel::onTogglePasswordVisibility);
    connect(m_add_table_btn,       &QPushButton::clicked,             this, &WifiManagerPanel::onAddToTableClicked);
    connect(m_delete_selected_btn, &QPushButton::clicked,             this, &WifiManagerPanel::onDeleteSelectedClicked);
    connect(m_save_table_btn,      &QPushButton::clicked,             this, &WifiManagerPanel::onSaveTableClicked);
    connect(m_load_table_btn,      &QPushButton::clicked,             this, &WifiManagerPanel::onLoadTableClicked);
    connect(m_network_table,       &QTableWidget::doubleClicked,      this, &WifiManagerPanel::onTableDoubleClicked);
    connect(m_search_input,        &QLineEdit::textChanged,           this, &WifiManagerPanel::onSearchChanged);
    connect(m_search_up_btn,       &QToolButton::clicked,             this, &WifiManagerPanel::onFindPrev);
    connect(m_search_down_btn,     &QToolButton::clicked,             this, &WifiManagerPanel::onFindNext);
    connect(m_generate_qr_btn,     &QPushButton::clicked,               this, &WifiManagerPanel::onGenerateQrClicked);
    connect(m_export_script_btn,   &QPushButton::clicked,               this, &WifiManagerPanel::onExportWindowsScriptClicked);
    connect(m_export_macos_btn,    &QPushButton::clicked,               this, &WifiManagerPanel::onExportMacosProfileClicked);
    connect(m_connect_phone_btn,   &QPushButton::clicked,               this, &WifiManagerPanel::onConnectWithPhoneClicked);
    connect(m_scan_networks_btn,   &QPushButton::clicked,               this, &WifiManagerPanel::onScanNetworksClicked);
    connect(m_add_to_windows_btn,  &QPushButton::clicked,               this, &WifiManagerPanel::onAddToWindowsClicked);
    connect(m_network_table,       &QTableWidget::itemSelectionChanged, this, &WifiManagerPanel::onSelectionChanged);
    connect(m_network_table,       &QTableWidget::itemChanged,         this, &WifiManagerPanel::onTableItemChanged);

    // Wire the CheckHeaderView "select all" checkbox
    auto* checkHdr = qobject_cast<CheckHeaderView*>(m_network_table->horizontalHeader());
    if (checkHdr) {
        connect(checkHdr, &CheckHeaderView::checkToggled, this, [this](bool allChecked) {
            m_network_table->blockSignals(true);
            for (int r = 0; r < m_network_table->rowCount(); ++r) {
                auto* item = m_network_table->item(r, COL_SELECT);
                if (item) item->setCheckState(allChecked ? Qt::Checked : Qt::Unchecked);
            }
            m_network_table->blockSignals(false);
            onSelectionChanged();
        });
    }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Slots
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void WifiManagerPanel::onSecurityChanged(const QString& value)
{
    bool hasPassword = !value.contains("None", Qt::CaseInsensitive);
    m_password_input->setEnabled(hasPassword);
    m_password_toggle_btn->setEnabled(hasPassword);
    if (!hasPassword)
        m_password_input->clear();
}

void WifiManagerPanel::onTogglePasswordVisibility()
{
    const bool showing = m_password_toggle_btn->isChecked();
    m_password_input->setEchoMode(showing ? QLineEdit::Normal : QLineEdit::Password);
    // Eye open = "click to show" (password hidden); Eye closed = "click to hide" (password shown)
    m_password_toggle_btn->setIcon(QIcon(showing ? ":/icons/eye_closed.svg" : ":/icons/eye_open.svg"));
}

void WifiManagerPanel::onAddToTableClicked()
{
    const QString ssid = m_ssid_input->text().trimmed();
    if (ssid.isEmpty()) {
        Q_EMIT statusMessage("SSID cannot be empty.", 3000);
        return;
    }
    addRowToTable(configFromForm());
    Q_EMIT statusMessage(QString("Added \"%1\" to table.").arg(ssid), 3000);
}

void WifiManagerPanel::onDeleteSelectedClicked()
{
    const auto selected = m_network_table->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;

    QList<int> rows;
    for (const auto& idx : selected)
        rows.prepend(idx.row());

    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows)
        m_network_table->removeRow(row);

    onSelectionChanged();
    Q_EMIT statusMessage(QString("Deleted %1 row(s).").arg(rows.size()), 3000);
}

void WifiManagerPanel::onTableDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    loadConfigToForm(configFromRow(index.row()));
}

void WifiManagerPanel::onSearchChanged(const QString& text)
{
    updateSearchMatches(text);
    highlightSearchMatches();
    m_search_index = m_search_matches.isEmpty() ? -1 : 0;
    if (!m_search_matches.isEmpty())
        m_network_table->scrollToItem(m_network_table->item(m_search_matches.first(), COL_SSID));
}

void WifiManagerPanel::onFindNext()
{
    if (m_search_matches.isEmpty()) return;
    m_search_index = (m_search_index + 1) % m_search_matches.size();
    m_network_table->scrollToItem(m_network_table->item(m_search_matches.at(m_search_index), COL_SSID));
    m_network_table->selectRow(m_search_matches.at(m_search_index));
}

void WifiManagerPanel::onFindPrev()
{
    if (m_search_matches.isEmpty()) return;
    m_search_index = (m_search_index - 1 + m_search_matches.size()) % m_search_matches.size();
    m_network_table->scrollToItem(m_network_table->item(m_search_matches.at(m_search_index), COL_SSID));
    m_network_table->selectRow(m_search_matches.at(m_search_index));
}

// ─────────────────────────────────────────────────────────────────────────────
// QR wizard helpers (TigerStyle decomposition of onGenerateQrClicked)
// ─────────────────────────────────────────────────────────────────────────────

QImage WifiManagerPanel::renderQrWithHeader(const QString& payload,
                                            const QString& location,
                                            bool showHeader)
{
    const QImage base = generateQrImage(payload);
    if (!showHeader || location.isEmpty()) return base;
    constexpr int HEADER_H = 52;
    QImage out(base.width(), base.height() + HEADER_H, QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter p(&out);
    QFont f = p.font();
    f.setPointSize(14);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(QRect(0, 0, out.width(), HEADER_H),
               Qt::AlignHCenter | Qt::AlignVCenter, location);
    p.drawImage(0, HEADER_H, base);
    p.end();
    return out;
}

bool WifiManagerPanel::exportQrToPdf(const QImage& image,
                                     const QString& path,
                                     const QString& title)
{
    QPdfWriter writer(path);
    writer.setTitle(title);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(150);
    const QRect  pageRect = writer.pageLayout().paintRectPixels(writer.resolution());
    const int    side     = std::min(pageRect.width(), pageRect.height());
    const QImage scaled   = image.scaled(side, side, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
    QPainter painter(&writer);
    painter.drawImage((pageRect.width() - scaled.width()) / 2, 0, scaled);
    painter.end();
    return true;
}

QWidget* WifiManagerPanel::buildQrFormatPage(const QString& payload,
                                             const QString& location,
                                             QrWizardControls& ctl)
{
    auto* page   = new QWidget;
    auto* topRow = new QHBoxLayout;

    const QImage previewImg = renderQrWithHeader(payload, location, true)
        .scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ctl.previewLabel = new QLabel(page);
    ctl.previewLabel->setPixmap(QPixmap::fromImage(previewImg));
    ctl.previewLabel->setFixedSize(180, 180);
    ctl.previewLabel->setAlignment(Qt::AlignCenter);
    ctl.previewLabel->setStyleSheet("border: 1px solid #ccc; background: white;");
    ctl.previewLabel->setAccessibleName(QStringLiteral("QR code preview"));

    auto* optWidget = new QWidget(page);
    auto* optLayout = new QVBoxLayout(optWidget);
    optLayout->setContentsMargins(8, 0, 0, 0);
    optLayout->addWidget(new QLabel("Select export format(s):"));
    ctl.chkPng = new QCheckBox("PNG");  ctl.chkPng->setChecked(true);
    ctl.chkPdf = new QCheckBox("PDF");
    ctl.chkJpg = new QCheckBox("JPG");
    ctl.chkBmp = new QCheckBox("BMP");
    optLayout->addWidget(ctl.chkPng);
    optLayout->addWidget(ctl.chkPdf);
    optLayout->addWidget(ctl.chkJpg);
    optLayout->addWidget(ctl.chkBmp);
    optLayout->addSpacing(10);
    ctl.headerToggle = new LogToggleSwitch("Location header", optWidget);
    ctl.headerToggle->setFixedWidth(180);
    ctl.headerToggle->setChecked(true);
    optLayout->addWidget(ctl.headerToggle);
    optLayout->addStretch();

    topRow->addWidget(ctl.previewLabel);
    topRow->addWidget(optWidget, 1);

    auto* outerLayout = new QVBoxLayout(page);
    auto* btnRow      = new QHBoxLayout;
    ctl.btnCancel0 = new QPushButton("Cancel");
    ctl.btnNext    = new QPushButton("Next >");
    ctl.btnNext->setDefault(true);
    btnRow->addStretch();
    btnRow->addWidget(ctl.btnCancel0);
    btnRow->addWidget(ctl.btnNext);
    outerLayout->addLayout(topRow);
    outerLayout->addLayout(btnRow);
    return page;
}

QWidget* WifiManagerPanel::buildQrOutputPage(QrWizardControls& ctl)
{
    auto* page   = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel("Choose output directory:"));
    auto* dirRow = new QHBoxLayout;
    ctl.dirEdit  = new QLineEdit;
    ctl.dirEdit->setReadOnly(true);
    ctl.dirEdit->setPlaceholderText("Click Browse to select a folder...");
    ctl.btnBrowse = new QPushButton("Browse...");
    ctl.btnBrowse->setFixedWidth(80);
    dirRow->addWidget(ctl.dirEdit, 1);
    dirRow->addWidget(ctl.btnBrowse);
    layout->addLayout(dirRow);
    ctl.subLabel = new QLabel("Files will be saved to: (select a folder first)");
    ctl.subLabel->setWordWrap(true);
    ctl.subLabel->setStyleSheet("color: #666; font-size: 8pt;");
    layout->addWidget(ctl.subLabel);
    layout->addStretch();
    auto* btnRow    = new QHBoxLayout;
    ctl.btnBack     = new QPushButton("< Back");
    ctl.btnCancel1  = new QPushButton("Cancel");
    ctl.btnGenerate = new QPushButton("Generate");
    ctl.btnGenerate->setDefault(true);
    ctl.btnGenerate->setEnabled(false);
    btnRow->addWidget(ctl.btnBack);
    btnRow->addStretch();
    btnRow->addWidget(ctl.btnCancel1);
    btnRow->addWidget(ctl.btnGenerate);
    layout->addLayout(btnRow);
    return page;
}

void WifiManagerPanel::connectSingleQrWizard(
    QDialog* dlg, QStackedWidget* stack, QrWizardControls ctl,
    const QString& payload, const QString& ssid,
    const QString& location, const QString& subName)
{
    QObject::connect(ctl.headerToggle, &LogToggleSwitch::toggled,
        [ctl, payload, location](bool on) {
            const QImage updated = renderQrWithHeader(payload, location, on)
                .scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            ctl.previewLabel->setPixmap(QPixmap::fromImage(updated));
        });
    QObject::connect(ctl.btnCancel0, &QPushButton::clicked, dlg, &QDialog::reject);
    QObject::connect(ctl.btnNext, &QPushButton::clicked,
        [this, ctl, stack]() {
            if (!ctl.chkPng->isChecked() && !ctl.chkPdf->isChecked() &&
                !ctl.chkJpg->isChecked() && !ctl.chkBmp->isChecked()) {
                Q_EMIT statusMessage("Select at least one format.", 2000);
                return;
            }
            stack->setCurrentIndex(1);
        });
    QObject::connect(ctl.btnCancel1, &QPushButton::clicked, dlg, &QDialog::reject);
    QObject::connect(ctl.btnBack, &QPushButton::clicked,
        [stack]() { stack->setCurrentIndex(0); });
    QObject::connect(ctl.btnBrowse, &QPushButton::clicked,
        [dlg, ctl, subName]() {
            const QString start = ctl.dirEdit->text().isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                : ctl.dirEdit->text();
            const QString chosen = QFileDialog::getExistingDirectory(
                dlg, "Select Output Directory", start);
            if (chosen.isEmpty()) return;
            ctl.dirEdit->setText(chosen);
            ctl.subLabel->setText(
                QString("Files will be saved to: %1/%2/").arg(chosen, subName));
            ctl.btnGenerate->setEnabled(true);
        });
    QObject::connect(ctl.btnGenerate, &QPushButton::clicked,
        [this, dlg, ctl, payload, ssid, location, subName]() {
            executeSingleQrExport(dlg, ctl, payload, ssid, location, subName);
        });
}

void WifiManagerPanel::executeSingleQrExport(
    QDialog* dlg, QrWizardControls ctl,
    const QString& payload, const QString& ssid,
    const QString& location, const QString& subName)
{
    const QString baseDir = ctl.dirEdit->text();
    if (baseDir.isEmpty()) return;
    const QString outDir = baseDir + "/" + subName;
    if (!QDir().mkpath(outDir)) {
        sak::logWarning(("Could not create output folder: " + outDir).toStdString());
        QMessageBox::warning(dlg, "Error",
                             "Could not create output folder:\n" + outDir);
        return;
    }
    const bool   showHeader = ctl.headerToggle->isChecked();
    const QImage finalImg   = renderQrWithHeader(payload, location, showHeader);
    QStringList  saved;
    auto savePlain = [&](const QString& ext, const QString& fmt) {
        const QString path = outDir + "/" + subName + "." + ext;
        if (!finalImg.save(path, fmt.toUtf8().constData())) {
            sak::logWarning(
                ("Failed to save " + ext.toUpper() + ": " + path).toStdString());
            QMessageBox::warning(dlg, "Export Error",
                                 "Failed to save " + ext.toUpper() + ":\n" + path);
        } else {
            saved.append(ext.toUpper());
        }
    };
    if (ctl.chkPng->isChecked()) savePlain("png", "PNG");
    if (ctl.chkJpg->isChecked()) savePlain("jpg", "JPEG");
    if (ctl.chkBmp->isChecked()) savePlain("bmp", "BMP");
    if (ctl.chkPdf->isChecked()) {
        const QString pdfPath = outDir + "/" + subName + ".pdf";
        const QString title   = ssid.isEmpty()
            ? QStringLiteral("WiFi QR Code") : ssid + " WiFi QR Code";
        if (exportQrToPdf(finalImg, pdfPath, title))
            saved.append("PDF");
    }
    dlg->accept();
    if (!saved.isEmpty())
        Q_EMIT statusMessage(
            QString("Saved %1 to: %2").arg(saved.join(", "), outDir), 6000);
}

void WifiManagerPanel::showSingleQrWizard(const WifiConfig& cfg)
{
    const QString payload  = buildWifiPayloadFromConfig(cfg);
    const QString ssid     = cfg.ssid;
    const QString location = cfg.location;

    if (payload.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", 3000);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Generate QR Code");
    dlg.setMinimumWidth(420);

    auto* mainLayout = new QVBoxLayout(&dlg);
    auto* stack      = new QStackedWidget(&dlg);
    mainLayout->addWidget(stack);

    QrWizardControls ctl;
    stack->addWidget(buildQrFormatPage(payload, location, ctl));
    stack->addWidget(buildQrOutputPage(ctl));

    const QString rawName = location.isEmpty() ? ssid : location + "_" + ssid;
    const QString subName = QString(rawName).replace(
        QRegularExpression("[\\\\/:*?\"<>|]"), "_");

    connectSingleQrWizard(&dlg, stack, ctl, payload, ssid, location, subName);
    dlg.exec();
}

void WifiManagerPanel::executeBatchQrExport(
    QDialog* dlg, const QList<WifiConfig>& sources,
    const QString& baseDir, bool showHeader,
    bool png, bool pdf, bool jpg, bool bmp)
{
    int saved = 0, failed = 0;
    for (const WifiConfig& cfg : sources) {
        if (cfg.ssid.isEmpty()) continue;
        const QString cfgPayload = buildWifiPayloadFromConfig(cfg);
        const QString rawName = cfg.location.isEmpty()
            ? cfg.ssid : cfg.location + "_" + cfg.ssid;
        const QString subName = QString(rawName).replace(
            QRegularExpression("[\\\\/:*?\"<>|]"), "_");
        const QString outDir = baseDir + "/" + subName;
        if (!QDir().mkpath(outDir)) { ++failed; continue; }
        const QImage img = renderQrWithHeader(cfgPayload, cfg.location, showHeader);
        bool anySaved = false;
        if (png) { img.save(outDir + "/" + subName + ".png", "PNG"); anySaved = true; }
        if (jpg) { img.save(outDir + "/" + subName + ".jpg", "JPEG"); anySaved = true; }
        if (bmp) { img.save(outDir + "/" + subName + ".bmp", "BMP"); anySaved = true; }
        if (pdf) {
            exportQrToPdf(img, outDir + "/" + subName + ".pdf",
                          cfg.ssid + " WiFi QR Code");
            anySaved = true;
        }
        anySaved ? ++saved : ++failed;
    }
    dlg->accept();
    const QString msg = failed > 0
        ? QString("Batch QR: saved %1 network(s) to %2 (%3 failed).")
              .arg(saved).arg(baseDir).arg(failed)
        : QString("Batch QR: saved %1 network(s) to: %2")
              .arg(saved).arg(baseDir);
    Q_EMIT statusMessage(msg, 7000);
}

void WifiManagerPanel::showBatchQrDialog(const QList<WifiConfig>& sources)
{
    QDialog dlg(this);
    dlg.setWindowTitle(QString("Batch QR Export (%1 networks)").arg(sources.size()));
    dlg.setMinimumWidth(400);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(
        QString("Generate QR codes for %1 selected networks:").arg(sources.size())));
    layout->addSpacing(6);
    layout->addWidget(new QLabel("Export format(s):"));
    auto* chkPng = new QCheckBox("PNG");  chkPng->setChecked(true);
    auto* chkPdf = new QCheckBox("PDF");
    auto* chkJpg = new QCheckBox("JPG");
    auto* chkBmp = new QCheckBox("BMP");
    for (auto* chk : {chkPng, chkPdf, chkJpg, chkBmp}) layout->addWidget(chk);
    layout->addSpacing(8);
    auto* headerToggle = new LogToggleSwitch("Location header", &dlg);
    headerToggle->setFixedWidth(180); headerToggle->setChecked(true);
    layout->addWidget(headerToggle);
    layout->addSpacing(8);
    layout->addWidget(new QLabel("Output directory (one subfolder per network):"));
    auto* dirRow   = new QHBoxLayout;
    auto* dirEdit  = new QLineEdit;
    dirEdit->setReadOnly(true); dirEdit->setPlaceholderText("Click Browse...");
    auto* btnBrowse = new QPushButton("Browse...");
    btnBrowse->setFixedWidth(80);
    dirRow->addWidget(dirEdit, 1); dirRow->addWidget(btnBrowse);
    layout->addLayout(dirRow);
    auto* subLabel = new QLabel("");
    subLabel->setWordWrap(true);
    subLabel->setStyleSheet("color: #666; font-size: 8pt;");
    layout->addWidget(subLabel);
    layout->addStretch();
    auto* btnRow    = new QHBoxLayout;
    auto* btnCancel = new QPushButton("Cancel");
    auto* btnGen    = new QPushButton("Batch Generate");
    btnGen->setDefault(true);
    btnGen->setEnabled(false);
    btnRow->addStretch();
    btnRow->addWidget(btnCancel);
    btnRow->addWidget(btnGen);
    layout->addLayout(btnRow);
    QObject::connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(btnBrowse, &QPushButton::clicked, [&]() {
        const QString start = dirEdit->text().isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
            : dirEdit->text();
        const QString chosen = QFileDialog::getExistingDirectory(
            &dlg, "Select Output Directory", start);
        if (chosen.isEmpty()) return;
        dirEdit->setText(chosen);
        subLabel->setText(
            QString("One subfolder will be created per network under: %1").arg(chosen));
        btnGen->setEnabled(true);
    });
    QObject::connect(btnGen, &QPushButton::clicked, [&]() {
        const QString baseDir = dirEdit->text();
        if (baseDir.isEmpty()) return;
        if (!chkPng->isChecked() && !chkPdf->isChecked() &&
            !chkJpg->isChecked() && !chkBmp->isChecked()) {
            sak::logWarning("Select at least one export format.");
            QMessageBox::warning(&dlg, "No Format", "Select at least one export format.");
            return;
        }
        executeBatchQrExport(&dlg, sources, baseDir, headerToggle->isChecked(),
                             chkPng->isChecked(), chkPdf->isChecked(),
                             chkJpg->isChecked(), chkBmp->isChecked());
    });
    dlg.exec();
}

// ─────────────────────────────────────────────────────────────────────────────

void WifiManagerPanel::onGenerateQrClicked()
{
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", 3000);
        return;
    }

    if (sources.size() == 1)
        showSingleQrWizard(sources.first());
    else
        showBatchQrDialog(sources);
}

void WifiManagerPanel::onExportWindowsScriptClicked()
{
    // Multi-modal: operate on checked rows when any are checked, otherwise use form
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", 3000);
        return;
    }

    if (sources.size() == 1) {
        // Single network â€” original save-dialog behavior
        const WifiConfig& cfg     = sources.first();
        const QString script      = buildWindowsScript(cfg.ssid, cfg.password, cfg.security, cfg.hidden);
        const QString defaultName = cfg.ssid + "_wifi_connect.cmd";
        const QString path = QFileDialog::getSaveFileName(
            this, "Save Windows Script",
            QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) + "/" + defaultName,
            "Windows Batch Script (*.cmd *.bat)");
        if (path.isEmpty()) return;

        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            sak::logWarning("Export Error: Failed to open file for writing.");
            QMessageBox::warning(this, "Export Error", "Failed to open file for writing.");
            return;
        }
        QTextStream out(&file);
        out << script;
        Q_EMIT statusMessage(QString("Saved Windows script: %1").arg(path), 5000);
    } else {
        // Multiple networks â€” ask for a folder, save one .cmd per network
        const QString outDir = QFileDialog::getExistingDirectory(
            this, "Select Output Folder for Windows Scripts",
            QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        if (outDir.isEmpty()) return;

        int saved = 0, failed = 0;
        for (const WifiConfig& cfg : sources) {
            if (cfg.ssid.isEmpty()) continue;
            const QString script = buildWindowsScript(cfg.ssid, cfg.password, cfg.security, cfg.hidden);
            const QString safeName = QString(cfg.ssid).replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
            const QString path = outDir + "/" + safeName + "_wifi_connect.cmd";
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { ++failed; continue; }
            QTextStream out(&file);
            out << script;
            ++saved;
        }
        const QString msg = failed > 0
            ? QString("Saved %1 script(s) to %2 (%3 failed).").arg(saved).arg(outDir).arg(failed)
            : QString("Saved %1 Windows script(s) to: %2").arg(saved).arg(outDir);
        Q_EMIT statusMessage(msg, 6000);
    }
}

void WifiManagerPanel::onExportMacosProfileClicked()
{
    // Multi-modal: operate on checked rows when any are checked, otherwise use form
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", 3000);
        return;
    }

    const QString xml = buildMacosProfile(sources);

    // Derive a sensible default filename
    const QString defaultName = (sources.size() == 1)
        ? sources.first().ssid + "_wifi.mobileconfig"
        : QString("wifi_%1_networks.mobileconfig").arg(sources.size());

    const QString path = QFileDialog::getSaveFileName(
        this, "Save macOS Profile",
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) + "/" + defaultName,
        "macOS Configuration Profile (*.mobileconfig)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logWarning("Export Error: Failed to open file for writing.");
        QMessageBox::warning(this, "Export Error", "Failed to open file for writing.");
        return;
    }
    QTextStream out(&file);
    out << xml;

    const QString label = (sources.size() == 1)
        ? QString("Saved macOS profile: %1").arg(path)
        : QString("Saved macOS profile with %1 networks: %2").arg(sources.size()).arg(path);
    Q_EMIT statusMessage(label, 5000);
}

void WifiManagerPanel::onSaveTableClicked()
{
    // Collect checked rows
    QList<int> checkedRows;
    for (int r = 0; r < m_network_table->rowCount(); ++r) {
        auto* item = m_network_table->item(r, COL_SELECT);
        if (item && item->checkState() == Qt::Checked)
            checkedRows.append(r);
    }

    const QString path = QFileDialog::getSaveFileName(
        this, "Save Network Table",
        m_save_path.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/wifi_networks.json"
            : m_save_path + "/wifi_networks.json",
        "JSON Files (*.json)");
    if (path.isEmpty()) return;
    m_save_path = QFileInfo(path).absolutePath();

    if (!checkedRows.isEmpty()) {
        QJsonArray arr;
        for (int r : checkedRows) {
            const WifiConfig cfg = configFromRow(r);
            QJsonObject obj;
            obj["location"] = cfg.location;
            obj["ssid"]     = cfg.ssid;
            obj["password"] = cfg.password;
            obj["security"] = cfg.security;
            obj["hidden"]   = cfg.hidden;
            arr.append(obj);
        }
        QJsonDocument doc(arr);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            sak::logWarning(("Could not open file for writing: " + path).toStdString());
            QMessageBox::warning(this, "Save Error", "Could not open file for writing:\n" + path);
            return;
        }
        f.write(doc.toJson());
        Q_EMIT statusMessage(QString("Saved %1 checked network(s) to %2").arg(arr.size()).arg(path), 5000);
    } else {
        saveTableToJson(path);
    }
}

void WifiManagerPanel::onLoadTableClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Load Network Table",
        m_save_path.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            : m_save_path,
        "JSON Files (*.json)");
    if (path.isEmpty()) return;
    m_save_path = QFileInfo(path).absolutePath();
    loadTableFromJson(path);
}

void WifiManagerPanel::onConnectWithPhoneClicked()
{
    // Multi-modal: operate on checked rows when any are checked, otherwise use form
    const QList<WifiConfig> sources = [&] {
        auto checked = checkedConfigs();
        return checked.isEmpty() ? QList<WifiConfig>{configFromForm()} : checked;
    }();

    if (sources.isEmpty() || sources.first().ssid.isEmpty()) {
        Q_EMIT statusMessage("Fill in at least the SSID first.", 3000);
        return;
    }

    if (sources.size() == 1) {
        showSingleNetworkQrDialog(sources.first());
    } else {
        showMultiNetworkQrDialog(sources);
    }
}

void WifiManagerPanel::showSingleNetworkQrDialog(const WifiConfig& cfg)
{
    const QString     payload = buildWifiPayloadFromConfig(cfg);
    const QImage qrImg = generateQrImage(payload).scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QDialog dlg(this);
    dlg.setWindowTitle("Connect with Phone / Tablet");
    dlg.setFixedSize(400, 460);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(10);

    auto* titleLbl = new QLabel(QString("<b>%1</b>").arg(cfg.ssid.toHtmlEscaped()), &dlg);
    titleLbl->setAlignment(Qt::AlignCenter);
    titleLbl->setStyleSheet("font-size: 13pt;");
    layout->addWidget(titleLbl);

    auto* imgLbl = new QLabel(&dlg);
    imgLbl->setPixmap(QPixmap::fromImage(qrImg));
    imgLbl->setAlignment(Qt::AlignCenter);
    imgLbl->setStyleSheet("background: white; border: 1px solid #ccc; padding: 4px;");
    imgLbl->setAccessibleName(QStringLiteral("WiFi QR code"));
    layout->addWidget(imgLbl);

    auto* hintLbl = new QLabel("Scan this QR code with your phone or tablet\nto connect to the network.", &dlg);
    hintLbl->setAlignment(Qt::AlignCenter);
    hintLbl->setStyleSheet("color: #555; font-size: 9pt;");
    layout->addWidget(hintLbl);

    auto* closeBtn = new QPushButton("Close", &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    layout->addWidget(closeBtn);

    dlg.exec();
}

void WifiManagerPanel::showMultiNetworkQrDialog(const QList<WifiConfig>& sources)
{
    QDialog dlg(this);
    dlg.setWindowTitle(QString("Connect with Phone / Tablet (%1 networks)").arg(sources.size()));
    dlg.setFixedSize(420, 530);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(8);

    auto* idxLbl = new QLabel(&dlg);
    idxLbl->setAlignment(Qt::AlignCenter);
    idxLbl->setStyleSheet("color: #666; font-size: 9pt;");
    layout->addWidget(idxLbl);

    auto* titleLbl = new QLabel(&dlg);
    titleLbl->setAlignment(Qt::AlignCenter);
    titleLbl->setStyleSheet("font-size: 13pt;");
    layout->addWidget(titleLbl);

    auto* imgLbl = new QLabel(&dlg);
    imgLbl->setAlignment(Qt::AlignCenter);
    imgLbl->setStyleSheet("background: white; border: 1px solid #ccc; padding: 4px;");
    imgLbl->setFixedSize(360, 360);
    imgLbl->setAccessibleName(QStringLiteral("WiFi QR code"));
    layout->addWidget(imgLbl, 0, Qt::AlignHCenter);

    auto* hintLbl = new QLabel("Scan this QR code with your phone or tablet\nto connect to the network.", &dlg);
    hintLbl->setAlignment(Qt::AlignCenter);
    hintLbl->setStyleSheet("color: #555; font-size: 9pt;");
    layout->addWidget(hintLbl);

    auto* navBar   = new QHBoxLayout();
    auto* prevBtn  = new QPushButton("< Prev", &dlg);
    auto* nextBtn  = new QPushButton("Next >", &dlg);
    auto* closeBtn = new QPushButton("Close",  &dlg);
    prevBtn->setFixedWidth(70);
    nextBtn->setFixedWidth(70);
    navBar->addWidget(prevBtn);
    navBar->addStretch();
    navBar->addWidget(closeBtn);
    navBar->addStretch();
    navBar->addWidget(nextBtn);
    layout->addLayout(navBar);

    int currentIdx = 0;

    auto updatePage = [&sources, imgLbl, titleLbl, idxLbl, prevBtn, nextBtn, this](int idx) {
        const WifiConfig& cfg     = sources[idx];
        const QString     payload = buildWifiPayloadFromConfig(cfg);
        const QImage qrImg = generateQrImage(payload).scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        imgLbl->setPixmap(QPixmap::fromImage(qrImg));
        titleLbl->setText(QString("<b>%1</b>").arg(cfg.ssid.isEmpty() ? "WiFi Network" : cfg.ssid.toHtmlEscaped()));
        idxLbl->setText(QString("Network %1 of %2").arg(idx + 1).arg(sources.size()));
        prevBtn->setEnabled(idx > 0);
        nextBtn->setEnabled(idx < static_cast<int>(sources.size()) - 1);
    };

    connect(prevBtn,  &QPushButton::clicked, &dlg, [&currentIdx, &updatePage]() { updatePage(--currentIdx); });
    connect(nextBtn,  &QPushButton::clicked, &dlg, [&currentIdx, &updatePage]() { updatePage(++currentIdx); });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    updatePage(0);
    dlg.exec();
}


void WifiManagerPanel::onScanNetworksClicked()
{
#ifndef Q_OS_WIN
    Q_EMIT statusMessage("Scan Known Networks is only supported on Windows.", 4000);
    return;
#else
    QStringList profileNames = scanWindowsProfileNames();

    if (profileNames.isEmpty()) {
        Q_EMIT statusMessage("No known WiFi profiles found.", 3000);
        return;
    }

    int added = 0;
    for (const QString& name : profileNames) {
        WifiConfig cfg = parseWindowsWifiProfile(name);
        addRowToTable(cfg);
        ++added;
    }

    Q_EMIT statusMessage(QString("Added %1 known network(s) to table.").arg(added), 5000);
#endif
}

QStringList WifiManagerPanel::scanWindowsProfileNames() const
{
    QProcess proc;
    proc.start("netsh", QStringList{"wlan", "show", "profiles"});
    if (!proc.waitForFinished(8000)) {
        return {};
    }

    const QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QStringList profileNames;
    const QRegularExpression nameRe(R"(:\s+(.+)$)");
    for (const QString& line : output.split('\n')) {
        const QString trimmed = line.trimmed();
        if (trimmed.contains("All User Profile", Qt::CaseInsensitive) ||
            trimmed.contains("Current User Profile", Qt::CaseInsensitive)) {
            const auto match = nameRe.match(trimmed);
            if (match.hasMatch()) {
                const QString name = match.captured(1).trimmed();
                if (!name.isEmpty())
                    profileNames.append(name);
            }
        }
    }
    return profileNames;
}

WifiManagerPanel::WifiConfig WifiManagerPanel::parseWindowsWifiProfile(const QString& profileName) const
{
    QProcess p2;
    p2.start("netsh", QStringList{"wlan", "show", "profile", "name=" + profileName, "key=clear"});
    p2.waitForFinished(5000);
    const QString detail = QString::fromLocal8Bit(p2.readAllStandardOutput());

    QString password;
    QString security = "WPA/WPA2/WPA3";
    bool hidden = false;

    const QRegularExpression keyRe(R"(Key Content\s*:\s+(.+))", QRegularExpression::CaseInsensitiveOption);
    const auto keyMatch = keyRe.match(detail);
    if (keyMatch.hasMatch())
        password = keyMatch.captured(1).trimmed();

    const QRegularExpression authRe(R"(Authentication\s*:\s+(.+))", QRegularExpression::CaseInsensitiveOption);
    const auto authMatch = authRe.match(detail);
    if (authMatch.hasMatch()) {
        const QString auth = authMatch.captured(1).trimmed().toUpper();
        if (auth.contains("WEP"))
            security = "WEP";
        else if (auth == "OPEN" || auth.contains("NONE"))
            security = "None (Open)";
    }

    const QRegularExpression nonBcRe(R"(Network broadcast\s*:\s+(.+))", QRegularExpression::CaseInsensitiveOption);
    const auto nbMatch = nonBcRe.match(detail);
    if (nbMatch.hasMatch())
        hidden = nbMatch.captured(1).trimmed().compare("Don't broadcast", Qt::CaseInsensitive) == 0 ||
                 nbMatch.captured(1).trimmed().compare("Not broadcasting", Qt::CaseInsensitive) == 0;

    WifiConfig cfg;
    cfg.location = {};
    cfg.ssid     = profileName;
    cfg.password = password;
    cfg.security = security;
    cfg.hidden   = hidden;
    return cfg;
}

void WifiManagerPanel::onSelectionChanged()
{
    const int total   = m_network_table->rowCount();
    int checked = 0;
    for (int r = 0; r < total; ++r) {
        auto* item = m_network_table->item(r, COL_SELECT);
        if (item && item->checkState() == Qt::Checked)
            ++checked;
    }

    // Update header checkbox tri-state
    auto* checkHdr = qobject_cast<CheckHeaderView*>(m_network_table->horizontalHeader());
    if (checkHdr) {
        if   (total == 0 || checked == 0)    checkHdr->setTriState(Qt::Unchecked);
        else if (checked == total)           checkHdr->setTriState(Qt::Checked);
        else                                 checkHdr->setTriState(Qt::PartiallyChecked);
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
        m_generate_qr_btn->setToolTip("Export the current network form as a QR code image (PNG, PDF, JPG, BMP)");
        m_export_script_btn->setToolTip("Generate a Windows netsh .cmd script for the current network form");
        m_export_macos_btn->setToolTip("Generate a macOS WiFi .mobileconfig profile for the current network form");
        m_connect_phone_btn->setToolTip("Show a QR code of the current network form for phone/tablet scanning");
    }
}

void WifiManagerPanel::onTableItemChanged(QTableWidgetItem* item)
{
    if (item && item->column() == COL_SELECT)
        onSelectionChanged();
}

// onSelectAllClicked and onSelectNoneClicked removed â€” replaced by CheckHeaderView

void WifiManagerPanel::onAddToWindowsClicked()
{
#ifndef Q_OS_WIN
    Q_EMIT statusMessage("Add to Windows is only supported on Windows.", 4000);
    return;
#else
    // Collect checked rows
    QList<int> checkedRows;
    for (int r = 0; r < m_network_table->rowCount(); ++r) {
        auto* item = m_network_table->item(r, COL_SELECT);
        if (item && item->checkState() == Qt::Checked)
            checkedRows.append(r);
    }
    if (checkedRows.isEmpty()) {
        Q_EMIT statusMessage("Check at least one network row first.", 3000);
        return;
    }

    int added = 0, failed = 0;
    for (int row : checkedRows) {
        const WifiConfig cfg = configFromRow(row);
        if (cfg.ssid.isEmpty()) continue;

        const QString xml = buildWlanProfileXml(cfg);
        if (installWlanProfile(xml, row))
            ++added;
        else
            ++failed;
    }

    if (failed == 0)
        Q_EMIT statusMessage(
            QString("Added %1 network(s) to Windows WiFi profiles.").arg(added), 5000);
    else
        Q_EMIT statusMessage(
            QString("Added %1 network(s); %2 failed (try running as Administrator).").arg(added).arg(failed), 6000);
#endif
}

QString WifiManagerPanel::buildWlanProfileXml(const WifiConfig& cfg)
{
    const QString upper = cfg.security.toUpper();
    QString authType, encType;
    if (upper.contains("WEP")) {
        authType = "open";    encType = "WEP";
    } else if (upper.contains("NONE") || upper.contains("OPEN")) {
        authType = "open";    encType = "none";
    } else {
        authType = "WPA2PSK"; encType = "AES";
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
    if (!cfg.password.isEmpty() && authType != "open") {
        xml += "  <sharedKey>\r\n";
        xml += "    <keyType>passPhrase</keyType>\r\n";
        xml += "    <protected>false</protected>\r\n";
        xml += "    <keyMaterial>" + cfg.password.toHtmlEscaped() + "</keyMaterial>\r\n";
        xml += "  </sharedKey>\r\n";
    }
    xml += "  </security></MSM>\r\n";
    xml += "</WLANProfile>\r\n";
    return xml;
}

bool WifiManagerPanel::installWlanProfile(const QString& xml, int row)
{
    const QString tmpPath = QDir::tempPath() + QString("/sak_wifi_%1.xml").arg(row);
    {
        QFile f(tmpPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return false;
        }
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << xml;
    }

    QProcess proc;
    proc.start("netsh", QStringList{"wlan", "add", "profile",
                                    "filename=" + tmpPath,
                                    "user=all"});
    proc.waitForFinished(8000);
    const int exitCode = proc.exitCode();
    QFile::remove(tmpPath);
    return exitCode == 0;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// WiFi payload helpers
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// static
QString WifiManagerPanel::escapeWifiField(const QString& value)
{
    QString result;
    result.reserve(value.size() * 2);
    for (const QChar c : value) {
        if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':')
            result += '\\';
        result += c;
    }
    return result;
}

// static
QString WifiManagerPanel::normalizeSecurityForQr(const QString& security)
{
    const QString upper = security.toUpper().trimmed();
    if (upper == "WEP")
        return "WEP";
    if (upper.contains("NONE") || upper.contains("OPEN") || upper == "NO PASSWORD")
        return "nopass";
    return "WPA";
}

QString WifiManagerPanel::buildWifiPayload() const
{
    const QString ssid = m_ssid_input->text().trimmed();
    if (ssid.isEmpty()) return {};

    const QString sec    = normalizeSecurityForQr(m_security_combo->currentText());
    const bool    hidden = m_hidden_checkbox->isChecked();

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
QString WifiManagerPanel::buildWifiPayloadFromConfig(const WifiConfig& cfg)
{
    if (cfg.ssid.isEmpty()) return {};
    const QString sec    = normalizeSecurityForQr(cfg.security);
    QString payload = "WIFI:T:" + sec + ";";
    payload += "S:" + escapeWifiField(cfg.ssid) + ";";
    if (sec != "nopass") {
        payload += cfg.password.isEmpty() ? "P:;" : "P:" + escapeWifiField(cfg.password) + ";";
    }
    payload += QString("H:%1;;").arg(cfg.hidden ? "true" : "false");
    return payload;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// QR generation
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// static
QImage WifiManagerPanel::generateQrImage(const QString& payload)
{
    constexpr int IMAGE_SIZE = 640;
    constexpr int BORDER     = 4;

    QImage out(IMAGE_SIZE, IMAGE_SIZE, QImage::Format_RGB32);
    out.fill(Qt::white);
    if (payload.isEmpty()) return out;

    try {
        // HIGH ECC trades capacity for resilience — critical because phone cameras
        // often scan QR codes at oblique angles or in poor lighting.
        const qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(
                payload.toUtf8().constData(),
                qrcodegen::QrCode::Ecc::HIGH);

        const int    modules      = qr.getSize();
        // The quiet zone (BORDER) around the QR is required by the spec so
        // scanners can reliably detect the code boundaries.
        const int    totalModules = modules + BORDER * 2;
        const double cellSize     = static_cast<double>(IMAGE_SIZE) / totalModules;

        QPainter painter(&out);
        // Antialiasing must be OFF — sub-pixel blending produces grey edges
        // that confuse QR decoders.
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setBrush(Qt::black);
        painter.setPen(Qt::NoPen);

        for (int y = 0; y < modules; ++y) {
            for (int x = 0; x < modules; ++x) {
                if (qr.getModule(x, y)) {
                    // Compute each cell's pixel rect from (x+1)*cellSize - x*cellSize
                    // rather than using a fixed integer cell width; this prevents
                    // cumulative rounding drift that would create visible gaps.
                    const int px = static_cast<int>((x + BORDER) * cellSize);
                    const int py = static_cast<int>((y + BORDER) * cellSize);
                    const int pw = static_cast<int>((x + BORDER + 1) * cellSize) - px;
                    const int ph = static_cast<int>((y + BORDER + 1) * cellSize) - py;
                    painter.drawRect(px, py, pw, ph);
                }
            }
        }
    } catch (const std::exception& ex) {
        sak::logWarning("QR code generation failed: {}", ex.what());
        QPainter p(&out);
        p.setPen(Qt::red);
        p.drawText(out.rect(), Qt::AlignCenter, "QR Error");
    }
    return out;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Export script builders
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// static
QString WifiManagerPanel::buildWindowsScript(const QString& ssid,
                                        const QString& password,
                                        const QString& security,
                                        bool hidden)
{
    const QString upper = security.toUpper();
    QString authType, encType;
    if (upper.contains("WEP")) {
        authType = "open";   encType = "WEP";
    } else if (upper.contains("NONE") || upper.contains("OPEN")) {
        authType = "open";   encType = "none";
    } else {
        authType = "WPA2PSK"; encType = "AES";
    }

    const QString hiddenStr = hidden ? "true" : "false";

    QString xml;
    xml += "<?xml version=\"1.0\"?>\r\n";
    xml += "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\r\n";
    xml += "  <name>" + ssid.toHtmlEscaped() + "</name>\r\n";
    xml += "  <SSIDConfig>\r\n";
    xml += "    <SSID><name>" + ssid.toHtmlEscaped() + "</name></SSID>\r\n";
    xml += "    <nonBroadcast>" + hiddenStr + "</nonBroadcast>\r\n";
    xml += "  </SSIDConfig>\r\n";
    xml += "  <connectionType>ESS</connectionType>\r\n";
    xml += "  <connectionMode>auto</connectionMode>\r\n";
    xml += "  <MSM><security><authEncryption>\r\n";
    xml += "    <authentication>" + authType + "</authentication>\r\n";
    xml += "    <encryption>" + encType + "</encryption>\r\n";
    xml += "    <useOneX>false</useOneX>\r\n";
    xml += "  </authEncryption>\r\n";
    if (!password.isEmpty() && authType != "open") {
        xml += "  <sharedKey>\r\n";
        xml += "    <keyType>passPhrase</keyType>\r\n";
        xml += "    <protected>false</protected>\r\n";
        xml += "    <keyMaterial>" + password.toHtmlEscaped() + "</keyMaterial>\r\n";
        xml += "  </sharedKey>\r\n";
    }
    xml += "  </security></MSM>\r\n";
    xml += "</WLANProfile>\r\n";

    const QString xmlB64      = QString::fromLatin1(xml.toUtf8().toBase64());
    const QString escapedSsid = ssid.contains(' ') ? "\"" + ssid + "\"" : ssid;

    QString script;
    script += "@echo off\r\n";
    script += "echo S.A.K. Utility - WiFi Network Setup Script\r\n";
    script += "echo Network: " + ssid + "\r\n";
    script += "echo.\r\n";
    script += "set PROFILE_XML=%TEMP%\\wifi_profile_sak.xml\r\n";
    script += "powershell -Command \"[System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('"
              + xmlB64 + "')) | Set-Content -Path '%PROFILE_XML%' -Encoding UTF8\"\r\n";
    script += "netsh wlan add profile filename=\"%PROFILE_XML%\" user=all\r\n";
    script += "if %errorlevel% neq 0 (\r\n";
    script += "    echo Failed to add WiFi profile. Run as Administrator.\r\n";
    script += "    del \"%PROFILE_XML%\" 2>nul\r\n";
    script += "    pause\r\n";
    script += "    exit /b 1\r\n";
    script += ")\r\n";
    script += "del \"%PROFILE_XML%\" 2>nul\r\n";
    script += "netsh wlan connect name=" + escapedSsid + "\r\n";
    script += "if %errorlevel% neq 0 (\r\n";
    script += "    echo Network profile added but could not connect immediately.\r\n";
    script += "    echo The network will connect automatically when in range.\r\n";
    script += ") else (\r\n";
    script += "    echo Successfully connected to " + ssid + "!\r\n";
    script += ")\r\n";
    script += "pause\r\n";
    return script;
}

// static
QString WifiManagerPanel::buildMacosProfile(const QList<WifiConfig>& networks)
{
    const QString profileUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    const QString payloadId   = "com.sak.wifi." + profileUuid.left(8).toLower();
    const QString now         = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QString plist;
    plist += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    plist += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist += "<plist version=\"1.0\">\n<dict>\n";
    plist += "  <key>PayloadContent</key>\n  <array>\n";

    for (const auto& cfg : networks) {
        const QString netUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
        const QString upper   = cfg.security.toUpper();
        QString encType;
        if (upper.contains("WEP"))                           encType = "WEP";
        else if (upper.contains("NONE") || upper.contains("OPEN")) encType = "None";
        else                                                 encType = "WPA";

        plist += "    <dict>\n";
        plist += "      <key>AutoJoin</key><true/>\n";
        plist += "      <key>EncryptionType</key><string>" + encType + "</string>\n";
        plist += "      <key>HIDDEN_NETWORK</key>";
        plist += cfg.hidden ? "<true/>\n" : "<false/>\n";
        if (encType != "None" && !cfg.password.isEmpty())
            plist += "      <key>Password</key><string>" + cfg.password.toHtmlEscaped() + "</string>\n";
        plist += "      <key>PayloadDisplayName</key><string>WiFi (" + cfg.ssid.toHtmlEscaped() + ")</string>\n";
        plist += "      <key>PayloadIdentifier</key><string>com.sak.wifi." + netUuid.toLower() + "</string>\n";
        plist += "      <key>PayloadType</key><string>com.apple.wifi.managed</string>\n";
        plist += "      <key>PayloadUUID</key><string>" + netUuid + "</string>\n";
        plist += "      <key>PayloadVersion</key><integer>1</integer>\n";
        plist += "      <key>SSID_STR</key><string>" + cfg.ssid.toHtmlEscaped() + "</string>\n";
        plist += "    </dict>\n";
    }

    plist += "  </array>\n";
    plist += "  <key>PayloadDescription</key><string>WiFi config by S.A.K. Utility on " + now + "</string>\n";
    const QString displayName = networks.isEmpty() ? "WiFi Networks" : networks.first().ssid;
    plist += "  <key>PayloadDisplayName</key><string>" + displayName.toHtmlEscaped() + " WiFi</string>\n";
    plist += "  <key>PayloadIdentifier</key><string>" + payloadId + "</string>\n";
    plist += "  <key>PayloadRemovalDisallowed</key><false/>\n";
    plist += "  <key>PayloadType</key><string>Configuration</string>\n";
    plist += "  <key>PayloadUUID</key><string>" + profileUuid + "</string>\n";
    plist += "  <key>PayloadVersion</key><integer>1</integer>\n";
    plist += "</dict>\n</plist>\n";
    return plist;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Table helpers
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
WifiManagerPanel::WifiConfig WifiManagerPanel::configFromForm() const
{
    WifiConfig cfg;
    cfg.location = m_location_input->text().trimmed();
    cfg.ssid     = m_ssid_input->text().trimmed();
    cfg.password = m_password_input->text();
    cfg.security = m_security_combo->currentText();
    cfg.hidden   = m_hidden_checkbox->isChecked();
    return cfg;
}

void WifiManagerPanel::loadConfigToForm(const WifiConfig& cfg)
{
    m_location_input->setText(cfg.location);
    m_ssid_input->setText(cfg.ssid);
    m_password_input->setText(cfg.password);
    m_hidden_checkbox->setChecked(cfg.hidden);
    const int idx = m_security_combo->findText(cfg.security);
    m_security_combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void WifiManagerPanel::addRowToTable(const WifiConfig& cfg)
{
    const int row = m_network_table->rowCount();
    m_network_table->insertRow(row);

    // Checkbox cell (COL_SELECT)
    auto* checkItem = new QTableWidgetItem();
    checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
    checkItem->setCheckState(Qt::Unchecked);
    m_network_table->setItem(row, COL_SELECT, checkItem);

    m_network_table->setItem(row, COL_LOCATION, new QTableWidgetItem(cfg.location));
    m_network_table->setItem(row, COL_SSID,     new QTableWidgetItem(cfg.ssid));

    // Password cell: store real value in item's UserRole; cell widget shows dots + eye toggle
    {
        const QString pwd  = cfg.password;
        const QString dots = pwd.isEmpty() ? QString{} : QString(pwd.length(), QChar(0x2022));

        // Item holds the real password for configFromRow() retrieval
        auto* pwItem = new QTableWidgetItem(QString{});
        pwItem->setData(Qt::UserRole, pwd);
        m_network_table->setItem(row, COL_PASSWORD, pwItem);

        // Visual widget: [dots label] [eye button]
        auto* container = new QWidget;
        container->setProperty("wifi_password", pwd);
        auto* hbox = new QHBoxLayout(container);
        hbox->setContentsMargins(4, 0, 2, 0);
        hbox->setSpacing(2);

        auto* lbl = new QLabel(dots, container);
        lbl->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

        auto* eyeBtn = new QToolButton(container);
        eyeBtn->setIcon(QIcon(":/icons/eye_open.svg"));
        eyeBtn->setIconSize(QSize(14, 14));
        eyeBtn->setCheckable(true);
        eyeBtn->setFixedSize(22, 22);
        eyeBtn->setToolTip("Show/hide password");
        eyeBtn->setAccessibleName(QStringLiteral("Toggle password visibility"));
        eyeBtn->setStyleSheet("QToolButton { border: none; background: transparent; }");
        eyeBtn->setEnabled(!pwd.isEmpty());

        QObject::connect(eyeBtn, &QToolButton::toggled, [lbl, eyeBtn, pwd](bool showing) {
            lbl->setText(showing ? pwd
                                 : (pwd.isEmpty() ? QString{} : QString(pwd.length(), QChar(0x2022))));
            eyeBtn->setIcon(QIcon(showing ? ":/icons/eye_closed.svg" : ":/icons/eye_open.svg"));
        });

        hbox->addWidget(lbl, 1);
        hbox->addWidget(eyeBtn);
        m_network_table->setCellWidget(row, COL_PASSWORD, container);
    }

    m_network_table->setItem(row, COL_SECURITY, new QTableWidgetItem(cfg.security));
    m_network_table->setItem(row, COL_HIDDEN,   new QTableWidgetItem(cfg.hidden ? "Yes" : "No"));
    m_network_table->scrollToBottom();
    onSelectionChanged();
}

WifiManagerPanel::WifiConfig WifiManagerPanel::configFromRow(int row) const
{
    auto text = [&](int col) -> QString {
        auto* item = m_network_table->item(row, col);
        return item ? item->text() : QString{};
    };
    WifiConfig cfg;
    cfg.location = text(COL_LOCATION);
    cfg.ssid     = text(COL_SSID);
    // Real password is stored in UserRole (display shows dots)
    auto* pwItem = m_network_table->item(row, COL_PASSWORD);
    cfg.password = pwItem ? pwItem->data(Qt::UserRole).toString() : QString{};
    cfg.security = text(COL_SECURITY);
    cfg.hidden   = text(COL_HIDDEN).compare("Yes", Qt::CaseInsensitive) == 0;
    return cfg;
}

QList<WifiManagerPanel::WifiConfig> WifiManagerPanel::allConfigs() const
{
    QList<WifiConfig> list;
    list.reserve(m_network_table->rowCount());
    for (int i = 0; i < m_network_table->rowCount(); ++i)
        list.append(configFromRow(i));
    return list;
}

QList<WifiManagerPanel::WifiConfig> WifiManagerPanel::checkedConfigs() const
{
    QList<WifiConfig> list;
    for (int r = 0; r < m_network_table->rowCount(); ++r) {
        auto* item = m_network_table->item(r, COL_SELECT);
        if (item && item->checkState() == Qt::Checked)
            list.append(configFromRow(r));
    }
    return list;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Search
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void WifiManagerPanel::updateSearchMatches(const QString& text)
{
    m_search_matches.clear();
    if (text.isEmpty()) return;
    for (int row = 0; row < m_network_table->rowCount(); ++row) {
        for (int col = COL_LOCATION; col < COL_COUNT; ++col) {  // skip COL_SELECT
            auto* item = m_network_table->item(row, col);
            if (item && item->text().contains(text, Qt::CaseInsensitive)) {
                m_search_matches.append(row);
                break;
            }
        }
    }
}

void WifiManagerPanel::highlightSearchMatches()
{
    for (int row = 0; row < m_network_table->rowCount(); ++row)
        for (int col = COL_LOCATION; col < COL_COUNT; ++col)  // skip COL_SELECT
            if (auto* item = m_network_table->item(row, col))
                item->setBackground(QBrush());

    for (int row : m_search_matches)
        for (int col = COL_LOCATION; col < COL_COUNT; ++col)
            if (auto* item = m_network_table->item(row, col))
                item->setBackground(QColor(255, 255, 150));
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Persistence
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void WifiManagerPanel::saveTableToJson(const QString& path)
{
    QJsonArray arr;
    for (int i = 0; i < m_network_table->rowCount(); ++i) {
        const WifiConfig cfg = configFromRow(i);
        QJsonObject obj;
        obj["location"] = cfg.location;
        obj["ssid"]     = cfg.ssid;
        obj["password"] = cfg.password;
        obj["security"] = cfg.security;
        obj["hidden"]   = cfg.hidden;
        arr.append(obj);
    }
    QJsonDocument doc(arr);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        sak::logWarning(("Could not open file for writing: " + path).toStdString());
        QMessageBox::warning(this, "Save Error", "Could not open file for writing:\n" + path);
        return;
    }
    f.write(doc.toJson());
    Q_EMIT statusMessage(QString("Saved %1 network(s) to %2").arg(arr.size()).arg(path), 5000);
}

void WifiManagerPanel::loadTableFromJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        sak::logWarning(("Could not open file: " + path).toStdString());
        QMessageBox::warning(this, "Load Error", "Could not open file:\n" + path);
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) {
        sak::logWarning("Load Error: Invalid JSON format (expected array).");
        QMessageBox::warning(this, "Load Error", "Invalid JSON format (expected array).");
        return;
    }
    m_network_table->setRowCount(0);
    for (const QJsonValue& val : doc.array()) {
        const QJsonObject obj = val.toObject();
        WifiConfig cfg;
        cfg.location = obj["location"].toString();
        cfg.ssid     = obj["ssid"].toString();
        cfg.password = obj["password"].toString();
        cfg.security = obj["security"].toString();
        cfg.hidden   = obj["hidden"].toBool();
        addRowToTable(cfg);
    }
    Q_EMIT statusMessage(
        QString("Loaded %1 network(s) from %2").arg(m_network_table->rowCount()).arg(path), 5000);
    onSelectionChanged();
}

}  // namespace sak

// Required for Q_OBJECT classes defined in .cpp files
#include "wifi_manager_panel.moc"
