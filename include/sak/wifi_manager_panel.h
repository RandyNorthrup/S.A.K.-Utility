// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QGroupBox>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QToolButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QImage>
#include <QModelIndex>
#include <QString>
#include <QList>

class QLabel;
class QStackedWidget;
class QVBoxLayout;

namespace sak {

class LogToggleSwitch;

/**
 * @brief WiFi Manager panel  --  generates scannable WiFi QR codes and network scripts
 *
 * Allows entry of one or more WiFi networks and exports QR codes via a mini
 * wizard (PNG / PDF / JPG / BMP), plus Windows netsh scripts and macOS profiles.
 *
 * WiFi payload format: WIFI:T:<security>;S:<ssid>;P:<password>;H:<hidden>;;
 * QR error correction: HIGH (30%)
 */
class WifiManagerPanel : public QWidget {
    Q_OBJECT

public:
    explicit WifiManagerPanel(QWidget* parent = nullptr);
    ~WifiManagerPanel() override;

    WifiManagerPanel(const WifiManagerPanel&) = delete;
    WifiManagerPanel& operator=(const WifiManagerPanel&) = delete;
    WifiManagerPanel(WifiManagerPanel&&) = delete;
    WifiManagerPanel& operator=(WifiManagerPanel&&) = delete;

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);

private Q_SLOTS:
    void onAddToTableClicked();
    void onDeleteSelectedClicked();
    void onGenerateQrClicked();
    void onExportWindowsScriptClicked();
    void onExportMacosProfileClicked();
    void onTableDoubleClicked(const QModelIndex& index);
    void onSearchChanged(const QString& text);
    void onFindNext();
    void onFindPrev();
    void onSecurityChanged(const QString& value);
    void onTogglePasswordVisibility();
    void onSaveTableClicked();
    void onLoadTableClicked();
    void onConnectWithPhoneClicked();
    void onScanNetworksClicked();
    void onAddToWindowsClicked();
    void onSelectionChanged();
    void onTableItemChanged(QTableWidgetItem* item);

private:
    // -------------------------------------------------------------------------
    // UI setup helpers
    // -------------------------------------------------------------------------
    void setupUi();
    void setupFormGroup();
    void setupTableGroup();
    void setupTableSearchRow(QVBoxLayout* layout);
    void setupNetworkTable(QVBoxLayout* layout);
    void setupTableActionButtons(QVBoxLayout* layout);
    void setupActionButtons();
    void connectSignals();

    // -------------------------------------------------------------------------
    // Data types
    // -------------------------------------------------------------------------
    struct WifiConfig {
        QString location;
        QString ssid;
        QString password;
        QString security;
        bool    hidden{false};
    };

    // -------------------------------------------------------------------------
    // WiFi payload helpers
    // -------------------------------------------------------------------------
    /** Escape special characters per WiFi QR spec: \ ; , : â†’ \\ \; \, \: */
    static QString escapeWifiField(const QString& value);

    /** Normalize security string for QR payload ("WPA/WPA2/WPA3" â†’ "WPA", etc.) */
    static QString normalizeSecurityForQr(const QString& security);

    /** Build the WIFI: URI payload from the current form values */
    QString buildWifiPayload() const;

    /** Build the WIFI: URI payload from an explicit WifiConfig struct */
    static QString buildWifiPayloadFromConfig(const WifiConfig& cfg);

    // -------------------------------------------------------------------------
    // QR generation
    // -------------------------------------------------------------------------
    /** Render a QR code for @p payload into a 640Ã—640 QImage (white background) */
    static QImage generateQrImage(const QString& payload);

    // -------------------------------------------------------------------------    // QR wizard decomposition helpers
    // -------------------------------------------------------------------------
    /** Bundle of widget pointers shared between QR-wizard page builders */
    struct QrWizardControls {
        QLabel*           previewLabel{};
        QCheckBox*        chkPng{};
        QCheckBox*        chkPdf{};
        QCheckBox*        chkJpg{};
        QCheckBox*        chkBmp{};
        LogToggleSwitch*  headerToggle{};
        QPushButton*      btnCancel0{};
        QPushButton*      btnNext{};
        QLineEdit*        dirEdit{};
        QLabel*           subLabel{};
        QPushButton*      btnBack{};
        QPushButton*      btnCancel1{};
        QPushButton*      btnBrowse{};
        QPushButton*      btnGenerate{};
    };

    /** Render a QR image with an optional location header banner */
    static QImage renderQrWithHeader(const QString& payload,
                                     const QString& location, bool showHeader);

    /** Write a QR image as a full-page A4 PDF; returns true on success */
    static bool exportQrToPdf(const QImage& image, const QString& path,
                              const QString& title);

    /** Show a QR dialog for a single WiFi network */
    void showSingleNetworkQrDialog(const WifiConfig& cfg);

    /** Show a navigable QR dialog for multiple WiFi networks */
    void showMultiNetworkQrDialog(const QList<WifiConfig>& sources);

    /** Show the single-network QR wizard dialog */
    void showSingleQrWizard(const WifiConfig& cfg);

    /** Build wizard page 0 (format + header selection) */
    QWidget* buildQrFormatPage(const QString& payload, const QString& location,
                               QrWizardControls& ctl);

    /** Build wizard page 1 (output directory selection) */
    QWidget* buildQrOutputPage(QrWizardControls& ctl);

    /** Wire all signals for the single-network QR wizard */
    void connectSingleQrWizard(QDialog* dlg, QStackedWidget* stack,
                               QrWizardControls ctl,
                               const QString& payload, const QString& ssid,
                               const QString& location, const QString& subName);

    /** Execute the single-network QR export (save selected formats) */
    void executeSingleQrExport(QDialog* dlg, QrWizardControls ctl,
                               const QString& payload, const QString& ssid,
                               const QString& location, const QString& subName);

    /** Show the batch (multi-network) QR export dialog */
    void showBatchQrDialog(const QList<WifiConfig>& sources);

    /** Execute the batch QR export loop for selected formats */
    void executeBatchQrExport(QDialog* dlg, const QList<WifiConfig>& sources,
                              const QString& baseDir, bool showHeader,
                              bool png, bool pdf, bool jpg, bool bmp);

    // -------------------------------------------------------------------------    // Export helpers
    // -------------------------------------------------------------------------
    /** Generate content of a Windows netsh .cmd script for a single network */
    static QString buildWindowsScript(const QString& ssid,
                                      const QString& password,
                                      const QString& security,
                                      bool hidden);

    /** Generate a macOS .mobileconfig plist for one or more networks */
    static QString buildMacosProfile(const QList<WifiConfig>& networks);

    // -------------------------------------------------------------------------
    // Table helpers
    // -------------------------------------------------------------------------
    WifiConfig configFromForm() const;
    void loadConfigToForm(const WifiConfig& cfg);
    void addRowToTable(const WifiConfig& cfg);
    WifiConfig configFromRow(int row) const;
    QList<WifiConfig> allConfigs() const;
    /** Returns WifiConfig for every checked table row; empty list if none checked */
    QList<WifiConfig> checkedConfigs() const;
    void updateSearchMatches(const QString& text);
    void highlightSearchMatches();
    /** Scan Windows known WiFi profile names via netsh */
    QStringList scanWindowsProfileNames() const;
    /** Parse a single Windows WiFi profile and return its config */
    WifiConfig parseWindowsWifiProfile(const QString& profileName) const;

    // -------------------------------------------------------------------------
    // Windows WiFi profile helpers
    // -------------------------------------------------------------------------
    /** Build WLAN profile XML for netsh import */
    static QString buildWlanProfileXml(const WifiConfig& cfg);

    /** Write XML to a temp file and install via netsh; returns true on success */
    static bool installWlanProfile(const QString& xml, int row);

    // -------------------------------------------------------------------------
    // Persistence
    // -------------------------------------------------------------------------
    void saveTableToJson(const QString& path);
    void loadTableFromJson(const QString& path);

    // =========================================================================
    // UI widgets
    // =========================================================================

    // -- Form group --
    QGroupBox*   m_form_group{nullptr};
    QLineEdit*   m_location_input{nullptr};
    QLineEdit*   m_ssid_input{nullptr};
    QLineEdit*   m_password_input{nullptr};
    QToolButton* m_password_toggle_btn{nullptr};
    QComboBox*   m_security_combo{nullptr};
    QCheckBox*   m_hidden_checkbox{nullptr};

    // -- Table group --
    QGroupBox*    m_table_group{nullptr};
    QTableWidget* m_network_table{nullptr};
    QLineEdit*    m_search_input{nullptr};
    QToolButton*  m_search_up_btn{nullptr};
    QToolButton*  m_search_down_btn{nullptr};
    QPushButton*  m_add_table_btn{nullptr};
    QPushButton*  m_delete_selected_btn{nullptr};
    QPushButton*  m_add_to_windows_btn{nullptr};
    QPushButton*  m_save_table_btn{nullptr};
    QPushButton*  m_load_table_btn{nullptr};

    // -- Action buttons --
    QPushButton* m_generate_qr_btn{nullptr};
    QPushButton* m_export_script_btn{nullptr};
    QPushButton* m_export_macos_btn{nullptr};
    QPushButton* m_connect_phone_btn{nullptr};
    QPushButton* m_scan_networks_btn{nullptr};

    // =========================================================================
    // State
    // =========================================================================
    QList<int> m_search_matches;
    int        m_search_index{-1};
    QString    m_save_path;
};

}  // namespace sak
